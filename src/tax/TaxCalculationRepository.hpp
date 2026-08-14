/**
 * @file TaxCalculationRepository.hpp
 * @brief All SQL touching `tax_calculations` lives here.
 *
 * Org-scoped (design spec §5), so this extends Tenancy::OrgCrudBase for
 * find_in_org/list_in_org/count_in_org — mirrors Ledger::JournalRepository /
 * Hr::EmployeeRepository's overall shape. The one bespoke write, `upsert()`,
 * is deliberately the ONLY way this repository ever writes a row: unlike
 * DocumentRepository::create() (a plain INSERT, since documents don't have a
 * natural "same request" key), Tax::TaxService's whole point is that
 * calling calculate_snr/calculate_vat again for the SAME
 * (org_id, kind, period_from, period_to) REPLACES the previous result rather
 * than accumulating a second row next to it (design spec §7.2, "повторный
 * расчёт того же периода обновляет строку, не плодит") — so there is no
 * separate plain `create()` for callers to misuse instead of upsert().
 */

#pragma once

#include <pqxx/pqxx>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "tax/TaxCalculation.hpp"
#include "tenancy/OrgScoped.hpp"

namespace Tax {

class TaxCalculationRepository : public Tenancy::OrgCrudBase<TaxCalculationRepository, Calculation, std::string> {
public:
    // OrgCrudBase contract — supplies find_in_org(id,org_id)/
    // list_in_org(org_id,limit,offset)/count_in_org(org_id) against
    // `tax_calculations`.
    static constexpr const char* kTable = "tax_calculations";
    static constexpr const char* kColumns =
        "id, org_id, kind, period_from, period_to, computed_at, input_snapshot, result_snapshot, total_tiyn";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "period_from DESC, kind";
    static constexpr const char* kOrgColumn = "org_id";

    /**
     * @brief Insert a fresh calculation, or replace the existing row for the
     *        same (org_id, kind, period_from, period_to) — the "recalculate
     *        overwrites, doesn't duplicate" contract migrations/014's UNIQUE
     *        constraint exists to back. `computed_at` is refreshed to `now()`
     *        on every call (including an overwrite), so it always reflects
     *        when the returned figures were actually produced, not when the
     *        row was first created.
     */
    Calculation upsert(const std::string& org_id,
                       const std::string& kind,
                       const std::string& period_from,
                       const std::string& period_to,
                       const nlohmann::json& input_snapshot,
                       const nlohmann::json& result_snapshot,
                       long long total_tiyn) {
        return Database::get().execute_write([&](auto& txn) -> Calculation {
            auto r = txn.exec_params(
                "INSERT INTO tax_calculations (org_id, kind, period_from, period_to, input_snapshot, "
                "result_snapshot, total_tiyn) "
                "VALUES ($1, $2, $3, $4, $5::jsonb, $6::jsonb, $7) "
                "ON CONFLICT (org_id, kind, period_from, period_to) DO UPDATE SET "
                "  computed_at = now(), "
                "  input_snapshot = EXCLUDED.input_snapshot, "
                "  result_snapshot = EXCLUDED.result_snapshot, "
                "  total_tiyn = EXCLUDED.total_tiyn "
                "RETURNING " +
                    std::string(kColumns),
                org_id,
                kind,
                period_from,
                period_to,
                input_snapshot.dump(),
                result_snapshot.dump(),
                total_tiyn);
            return Calculation::from_row(r[0]);
        });
    }

    /// The stored calculation for exactly one (org_id, kind, period_from,
    /// period_to), if one has ever been computed — the same key upsert()
    /// writes against. For a caller that wants "the last SNR calculation for
    /// H1 2026" without listing everything for the org.
    ///
    /// @p from_primary mirrors OrgCrudBase::find_in_org's parameter of the
    /// same name: pass true right after a write (e.g. confirming the row
    /// upsert() just returned, or any other read-after-write caller) so a
    /// lagging replica can't return a stale/missing row — same
    /// read-after-write posture Tax::TaxService::sum_line_amount_tiyn takes
    /// for its own aggregate reads.
    std::optional<Calculation> find_by_period(const std::string& org_id,
                                              const std::string& kind,
                                              const std::string& period_from,
                                              const std::string& period_to,
                                              bool from_primary = false) {
        auto query = [&](auto& txn) -> std::optional<Calculation> {
            auto r = txn.exec_params("SELECT " + std::string(kColumns) +
                                         " FROM tax_calculations "
                                         "WHERE org_id = $1 AND kind = $2 AND period_from = $3 AND period_to = $4",
                                     org_id,
                                     kind,
                                     period_from,
                                     period_to);
            if (r.empty())
                return std::nullopt;
            return Calculation::from_row(r[0]);
        };
        return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
    }
};

}  // namespace Tax
