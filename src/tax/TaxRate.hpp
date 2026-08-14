/**
 * @file TaxRate.hpp
 * @brief Domain rows for the tax/constant reference (migrations/011_tax_reference.sql
 *        — design spec §7.1), seeded with НК РК / Соцкодекс РК values effective
 *        2026-01-01 (see the migration's header comment for the full source list
 *        and how the Step-1 official-source reconciliation resolved every
 *        contradiction in the initial draft).
 *
 * Domain-only — no SQL here; persistence lives in
 * src/tax/TaxReferenceRepository.hpp. Follows the same from_row/to_json idioms
 * as src/ledger/Account.hpp: from_row is a templated static factory (works
 * with any pqxx row-like type), and to_json is a free function found via ADL.
 *
 * Money is always in TIYN (1 ₸ = 100 tiyn) and rates are always in BASIS
 * POINTS (1 bp = 0.01%, so 16% = 1600, 3.5% = 350) — both integral, so no
 * repository or caller ever needs floating-point arithmetic to answer "how
 * much tax is this". Values that are multiples of МРП/МЗП rather than fixed
 * money (e.g. "50 МЗП", "30 МРП") live in value_units instead, left as a
 * whole number the caller multiplies by the current mrp/mzp constant.
 */

#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace Tax {

/// The closed set of `tax_rates.kind` values enforced by the migration's
/// CHECK constraint. Plain string constants (not an enum class) because the
/// column — and every repository query against it — is TEXT, matching the
/// Ledger::Account::type idiom (a documented string, not a wire-format enum).
namespace RateKind {
inline constexpr const char* kVat = "vat";
inline constexpr const char* kSnrSimplified = "snr_simplified";
inline constexpr const char* kIpn = "ipn";
inline constexpr const char* kOpv = "opv";
inline constexpr const char* kOpvr = "opvr";
inline constexpr const char* kSo = "so";
inline constexpr const char* kOsms = "osms";
inline constexpr const char* kVosms = "vosms";
inline constexpr const char* kSocialTax = "social_tax";
}  // namespace RateKind

/// One row of `tax_rates`: a percentage (in basis points) for a given
/// `kind`, valid over [effective_from, effective_to] (NULL effective_to =
/// still in force). `region` is NULL for the republic-wide default rate;
/// a non-NULL region row (e.g. a маслихат's ±50% adjustment to
/// snr_simplified) takes priority over the NULL-region row for the same
/// kind/date — see TaxReferenceRepository::rate_on.
struct Rate {
    std::string id;
    std::string kind;
    long long rate_bp = 0;
    std::optional<std::string> region;
    std::string effective_from;
    std::optional<std::string> effective_to;  // empty/nullopt = бессрочно
    std::string source_note;

    template <typename Row>
    static Rate from_row(const Row& row) {
        Rate r;
        r.id = row["id"].template as<std::string>();
        r.kind = row["kind"].template as<std::string>();
        r.rate_bp = row["rate_bp"].template as<long long>();
        if (!row["region"].is_null())
            r.region = row["region"].template as<std::string>();
        r.effective_from = row["effective_from"].template as<std::string>();
        if (!row["effective_to"].is_null())
            r.effective_to = row["effective_to"].template as<std::string>();
        r.source_note = row["source_note"].template as<std::string>();
        return r;
    }
};

inline void to_json(nlohmann::json& j, const Rate& r) {
    j = nlohmann::json{
        {"id", r.id},
        {"kind", r.kind},
        {"rate_bp", r.rate_bp},
        {"region", r.region ? nlohmann::json(*r.region) : nlohmann::json(nullptr)},
        {"effective_from", r.effective_from},
        {"effective_to", r.effective_to ? nlohmann::json(*r.effective_to) : nlohmann::json(nullptr)},
        {"source_note", r.source_note},
    };
}

/// One row of `tax_constants`: either a money amount (value_tiyn, e.g. "mrp",
/// "mzp") or a multiple of МРП/МЗП (value_units, e.g. "ipn_deduction_mrp" =
/// 30, meaning 30×МРП) or, for a few rows that are legally defined as an МРП
/// multiple but conventionally quoted in tenge (vat_threshold_tenge), both:
/// value_tiyn holds the tenge equivalent for THIS effective period and
/// value_units the underlying МРП multiplier, so a future migration that
/// re-seeds a new МРП only has to add a new effective_from row, not rename
/// anything.
struct Constant {
    std::string id;
    std::string key;
    long long value_tiyn = 0;
    std::optional<long long> value_units;
    std::string effective_from;
    std::optional<std::string> effective_to;
    std::string source_note;

    template <typename Row>
    static Constant from_row(const Row& row) {
        Constant c;
        c.id = row["id"].template as<std::string>();
        c.key = row["key"].template as<std::string>();
        c.value_tiyn = row["value_tiyn"].template as<long long>();
        if (!row["value_units"].is_null())
            c.value_units = row["value_units"].template as<long long>();
        c.effective_from = row["effective_from"].template as<std::string>();
        if (!row["effective_to"].is_null())
            c.effective_to = row["effective_to"].template as<std::string>();
        c.source_note = row["source_note"].template as<std::string>();
        return c;
    }
};

inline void to_json(nlohmann::json& j, const Constant& c) {
    j = nlohmann::json{
        {"id", c.id},
        {"key", c.key},
        {"value_tiyn", c.value_tiyn},
        {"value_units", c.value_units ? nlohmann::json(*c.value_units) : nlohmann::json(nullptr)},
        {"effective_from", c.effective_from},
        {"effective_to", c.effective_to ? nlohmann::json(*c.effective_to) : nlohmann::json(nullptr)},
        {"source_note", c.source_note},
    };
}

}  // namespace Tax
