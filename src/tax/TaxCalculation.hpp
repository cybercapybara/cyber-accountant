/**
 * @file TaxCalculation.hpp
 * @brief Domain row for `tax_calculations` (migrations/014_tax_calculations.sql
 *        — design spec §7.2), the persisted, reproducible output of
 *        Tax::TaxService::calculate_snr / calculate_vat.
 *
 * Domain-only — no SQL here; persistence lives in
 * src/tax/TaxCalculationRepository.hpp. Follows the same from_row/to_json
 * idioms as src/ledger/Document.hpp (the closest existing JSONB-carrying
 * row): from_row is a templated static factory, to_json is a free function
 * found via ADL, and the two JSONB columns are parsed with
 * nlohmann::json::parse and fall back to an empty object on a corrupt value
 * — they are NOT std::optional here (unlike Document::input_snapshot)
 * because migrations/014_tax_calculations.sql declares both NOT NULL: every
 * row this repository ever reads was written by
 * Tax::TaxService with both snapshots populated, so "absent" is not a state
 * this type needs to represent.
 *
 * Money is always TIYN (total_tiyn, and every money field folded into the
 * snapshots) — integral throughout, no floating point.
 */

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace Tax {

/// The closed set of `tax_calculations.kind` values enforced by the
/// migration's CHECK constraint — mirrors Tax::RateKind's naming for the two
/// kinds TaxService actually computes (RateKind additionally knows about the
/// payroll-only kinds ipn/opv/... that never appear here).
namespace CalculationKind {
inline constexpr const char* kSnrSimplified = "snr_simplified";
inline constexpr const char* kVat = "vat";
}  // namespace CalculationKind

/// One row of `tax_calculations`: a single org/kind/period's computed tax,
/// with both the inputs (rates, constants, account sets, period bounds) and
/// the outputs (every intermediate and final figure) captured as JSONB for
/// reproducibility (design spec §7.2) — see TaxService.hpp for what each
/// snapshot actually contains per kind.
struct Calculation {
    std::string id;
    std::string org_id;
    std::string kind;  // 'snr_simplified' | 'vat'
    std::string period_from;
    std::string period_to;
    std::string computed_at;
    nlohmann::json input_snapshot = nlohmann::json::object();
    nlohmann::json result_snapshot = nlohmann::json::object();
    long long total_tiyn = 0;

    template <typename Row>
    static Calculation from_row(const Row& row) {
        Calculation c;
        c.id = row["id"].template as<std::string>();
        c.org_id = row["org_id"].template as<std::string>();
        c.kind = row["kind"].template as<std::string>();
        c.period_from = row["period_from"].template as<std::string>();
        c.period_to = row["period_to"].template as<std::string>();
        c.computed_at = row["computed_at"].template as<std::string>();
        try {
            c.input_snapshot = nlohmann::json::parse(row["input_snapshot"].template as<std::string>());
        } catch (...) {
            c.input_snapshot = nlohmann::json::object();
        }
        try {
            c.result_snapshot = nlohmann::json::parse(row["result_snapshot"].template as<std::string>());
        } catch (...) {
            c.result_snapshot = nlohmann::json::object();
        }
        c.total_tiyn = row["total_tiyn"].template as<long long>();
        return c;
    }
};

inline void to_json(nlohmann::json& j, const Calculation& c) {
    j = nlohmann::json{
        {"id", c.id},
        {"org_id", c.org_id},
        {"kind", c.kind},
        {"period_from", c.period_from},
        {"period_to", c.period_to},
        {"computed_at", c.computed_at},
        {"input_snapshot", c.input_snapshot},
        {"result_snapshot", c.result_snapshot},
        {"total_tiyn", c.total_tiyn},
    };
}

}  // namespace Tax
