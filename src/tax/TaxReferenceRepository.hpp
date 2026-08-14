/**
 * @file TaxReferenceRepository.hpp
 * @brief All SQL touching `tax_rates` / `tax_constants` lives here.
 *
 * Neither table carries an `org_id` column. This is the SECOND documented
 * exception to "every domain table is org-scoped" (design spec §6.1 / Global
 * Constraints of the P1 plan) — the first is `accounts`
 * (src/ledger/AccountRepository.hpp). The reasoning is the same shape but
 * even stronger here: accounts.org_id IS NULL rows are a starting point a
 * tenant may extend with its OWN subaccounts, so that table at least HAS an
 * org_id column for the rows that need one. Tax rates and constants (НК РК /
 * Соцкодекс РК values — see migrations/011_tax_reference.sql for the full
 * source list) are facts about Kazakhstani law: no tenant has — or could
 * ever have — its own НДС rate or its own МРП. There is nothing to scope, so
 * this repository does NOT extend Tenancy::OrgCrudBase (src/tenancy/OrgScoped.hpp)
 * at all — that base's entire contract is "filter by org_id", which would be
 * dead weight (and a temptation to add a meaningless org_id column later) for
 * a table where every row is inherently global. Every read here takes an
 * effective-date instead of an org_id, because the actual source of
 * variation for tax data is TIME (rates change every fiscal year), not
 * tenant.
 *
 * `region` on `tax_rates` supports a future маслихат-level override (e.g. the
 * ±50% a local маслихат may apply to `snr_simplified`, НК РК ст.722) without
 * a schema change: a region-specific row for the same (kind, date) takes
 * priority over the NULL-region default because of `ORDER BY region NULLS
 * LAST` — see rate_on() below. No regional rows are seeded as of migration
 * 011 (no maslikhat decision list was available to seed against), but the
 * query shape already handles them correctly once they exist.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "tax/TaxRate.hpp"

namespace Tax {

class TaxReferenceRepository {
public:
    static constexpr const char* kRateColumns = "id, kind, rate_bp, region, effective_from, effective_to, source_note";
    static constexpr const char* kConstantColumns =
        "id, key, value_tiyn, value_units, effective_from, effective_to, source_note";

    /**
     * @brief The rate in force for @p kind on @p date, optionally narrowed to
     *        @p region_or_empty.
     *
     * Pass an empty string for @p region_or_empty when the caller has no
     * region context (the common case: most kinds — vat, ipn, opv, ... —
     * never vary by region at all). Because no seeded row uses region = ''
     * as a real value, `region IS NULL OR region = $3` with $3 = '' matches
     * only the republic-wide default row, exactly as intended. When a real
     * region is passed and BOTH a regional row and the NULL default exist
     * for the same (kind, date), `ORDER BY region NULLS LAST` picks the
     * regional row — it is more specific, so it wins.
     */
    std::optional<Rate> rate_on(const std::string& kind, const std::string& date, const std::string& region_or_empty) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Rate> {
            auto r = txn.exec_params("SELECT " + std::string(kRateColumns) +
                                         " FROM tax_rates"
                                         " WHERE kind = $1 AND effective_from <= $2::date"
                                         "   AND (effective_to IS NULL OR effective_to >= $2::date)"
                                         "   AND (region IS NULL OR region = $3)"
                                         " ORDER BY region NULLS LAST, effective_from DESC LIMIT 1",
                                     kind,
                                     date,
                                     region_or_empty);
            if (r.empty())
                return std::nullopt;
            return Rate::from_row(r[0]);
        });
    }

    /// The constant identified by @p key in force on @p date, or nullopt if
    /// @p key doesn't exist yet on that date (e.g. querying "mrp" before
    /// 2026-01-01, the first effective_from this migration seeds).
    std::optional<Constant> constant_on(const std::string& key, const std::string& date) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Constant> {
            auto r = txn.exec_params("SELECT " + std::string(kConstantColumns) +
                                         " FROM tax_constants"
                                         " WHERE key = $1 AND effective_from <= $2::date"
                                         "   AND (effective_to IS NULL OR effective_to >= $2::date)"
                                         " ORDER BY effective_from DESC LIMIT 1",
                                     key,
                                     date);
            if (r.empty())
                return std::nullopt;
            return Constant::from_row(r[0]);
        });
    }

    /// Every rate in force on @p date, one row per (kind, region) — the most
    /// recently effective vintage of each, mirroring the "latest applicable"
    /// semantics of rate_on() but for every kind/region at once instead of a
    /// single lookup. DISTINCT ON requires its columns to be a prefix of
    /// ORDER BY, which is why kind/region lead the ORDER BY here even though
    /// callers mostly care about effective_from DESC picking the winner.
    std::vector<Rate> list_rates_on(const std::string& date) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT DISTINCT ON (kind, region) " + std::string(kRateColumns) +
                                         " FROM tax_rates"
                                         " WHERE effective_from <= $1::date"
                                         "   AND (effective_to IS NULL OR effective_to >= $1::date)"
                                         " ORDER BY kind, region NULLS LAST, effective_from DESC",
                                     date);
            std::vector<Rate> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Rate::from_row(row));
            return out;
        });
    }

    /// Every constant in force on @p date, one row per key (latest vintage).
    std::vector<Constant> list_constants_on(const std::string& date) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT DISTINCT ON (key) " + std::string(kConstantColumns) +
                                         " FROM tax_constants"
                                         " WHERE effective_from <= $1::date"
                                         "   AND (effective_to IS NULL OR effective_to >= $1::date)"
                                         " ORDER BY key, effective_from DESC",
                                     date);
            std::vector<Constant> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Constant::from_row(row));
            return out;
        });
    }
};

}  // namespace Tax
