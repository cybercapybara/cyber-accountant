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

#include <optional>
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

    /**
     * @brief Calculations for @p org_id, newest period first (kOrderBy),
     *        optionally narrowed to one @p kind and/or one calendar @p year
     *        (matched against the year of `period_from`).
     *
     * Task 12 addition, mirroring Ledger::DocumentRepository::list_filtered:
     * `GET /api/v1/tax/calculations?kind=&year=` needs both filters applied
     * IN SQL so the paginated `total` (count_filtered below) describes the
     * same rows the page contains. @p year is an optional decimal STRING cast
     * in SQL (`$3::int`), the same "optional text parameter + cast" idiom the
     * `$2::text IS NULL` kind guard uses; the controller has already rejected
     * anything that isn't a plain integer, and allowlisted @p kind against
     * migrations/014_tax_calculations.sql's CHECK list.
     *
     * A period that straddles a year boundary is filtered by its START year
     * — every period this system produces (a half-year for СНР, a quarter for
     * НДС) lies wholly inside one calendar year, so the distinction is
     * currently unobservable; picking `period_from` explicitly (rather than
     * an OVERLAPS-style range test) keeps the semantics stated rather than
     * accidental if that ever stops holding.
     */
    std::vector<Calculation> list_filtered(const std::string& org_id,
                                           const std::optional<std::string>& kind,
                                           const std::optional<std::string>& year,
                                           int limit,
                                           int offset) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT " + std::string(kColumns) +
                                         " FROM tax_calculations WHERE org_id = $1 "
                                         "AND ($2::text IS NULL OR kind = $2) "
                                         "AND ($3::text IS NULL OR EXTRACT(YEAR FROM period_from)::int = $3::int) "
                                         "ORDER BY " +
                                         std::string(kOrderBy) + " LIMIT $4 OFFSET $5",
                                     org_id,
                                     kind,
                                     year,
                                     limit,
                                     offset);
            std::vector<Calculation> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Calculation::from_row(row));
            return out;
        });
    }

    /// Total row count for the same (@p kind, @p year) filter list_filtered()
    /// applies — kept as a matching pair so the list endpoint's `total` never
    /// disagrees with its page.
    long count_filtered(const std::string& org_id,
                        const std::optional<std::string>& kind,
                        const std::optional<std::string>& year) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT COUNT(*) FROM tax_calculations WHERE org_id = $1 "
                "AND ($2::text IS NULL OR kind = $2) "
                "AND ($3::text IS NULL OR EXTRACT(YEAR FROM period_from)::int = $3::int)",
                org_id,
                kind,
                year);
            return r.at(0).at(0).template as<long>();
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
