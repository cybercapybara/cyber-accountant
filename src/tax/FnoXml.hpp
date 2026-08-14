/**
 * @file FnoXml.hpp
 * @brief Shared, DB-free XML-building primitives for ФНО (форма налоговой
 *        отчётности) generators: XML-special-character escaping, whole-tenge
 *        amount formatting (tiyn rounded half-up), and the taxpayer-identity
 *        fields every KGD/СОНО "sheet" template carries.
 *
 * @par Step 1 finding (P2 task-7-brief.md), summarized here because both
 *      Fno910.hpp and every future Fno*.hpp built on this file share it —
 *      see Fno910.hpp's file header for the full research trail:
 *
 * kgd.gov.kz does NOT publish an XSD for any ФНО form's CONTENT. It DOES
 * publish (a) the real wire-format XML the СОНО client application sends —
 * confirmed by downloading and inspecting KGD's own official documentation
 * archive (see Fno910.hpp) — and (b) XSD schemas for the SOAP-ish
 * SONO_FNO_SEND / SONO_FNO_GET_STATUS *transport* API (ack/status messages),
 * which describe the envelope carrying a form, not the form's own field
 * grid. No content XSD exists to validate against, so every generator built
 * on this file is schema_validated=false by construction (see
 * Fno910::kSchemaValidated) — this is a documented, permanent property of
 * the KGD format, not a gap to "fix" later.
 *
 * Because of (a), `pugixml` is used ONLY in
 * tests/unit/test_fno910_xml.cpp, to parse a generator's output back and
 * assert well-formedness — production code here has zero XML-library
 * dependency; every function below is hand-rolled string assembly.
 *
 * Every function in this file is PURE (no I/O, no exceptions on
 * well-typed input, no allocation beyond the returned std::string) — the
 * P2 task-7 brief requires generators to be testable without a database.
 */

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace Tax {

/// Minimal organization/period context a ФНО generator needs. Deliberately
/// NOT Tenancy::Organization — this module must not depend on the tenancy
/// domain — and deliberately all-`std::string` (including the period
/// fields) so a caller building it from HTTP path/query params never has to
/// think about int parsing before calling a `build_xml`. `tax_period_half`
/// is "1" or "2"; `tax_period_year` is e.g. "2026". Defined here rather than
/// in Fno910.hpp because Task 8's ФНО generator reuses this exact struct.
struct OrgInfo {
    std::string bin;
    std::string name;
    std::string tax_period_year;
    std::string tax_period_half;
};

inline void to_json(nlohmann::json& j, const OrgInfo& o) {
    j = nlohmann::json{
        {"bin", o.bin},
        {"name", o.name},
        {"tax_period_year", o.tax_period_year},
        {"tax_period_half", o.tax_period_half},
    };
}

/// Common XML-building primitives shared by every Fno*::build_xml.
namespace FnoXml {

/// Escapes the five XML special characters for safe placement in EITHER
/// text content or a double-quoted attribute value — a strict superset of
/// what text content alone needs (just `&` and `<`), so callers never have
/// to reason about which of the two contexts a given value ends up in.
///
/// Single left-to-right pass over the input (never a sequence of
/// find/replace calls) so a `&` byte is never re-escaped by a later
/// substitution — an `&` -> `&amp;` followed by a naive second pass over
/// the RESULT is the classic way to double-escape into `&amp;amp;`.
inline std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

/// Rounds a signed tiyn amount to the nearest whole tenge, ties rounding
/// AWAY FROM ZERO ("half-up" per the brief, generalized to negative amounts
/// since a Calculation::total_tiyn can be negative — see
/// Tax::TaxService::calculate_vat's refund-position doc comment). 100 tiyn =
/// 1 tenge. Pure integer arithmetic — no floating point, so there is no
/// double-rounding risk between tiyn and tenge.
inline long long round_half_up_to_tenge(long long tiyn) {
    const long long sign = tiyn < 0 ? -1 : 1;
    const long long abs_tiyn = tiyn < 0 ? -tiyn : tiyn;
    return sign * ((abs_tiyn + 50) / 100);
}

/// `round_half_up_to_tenge` rendered as a decimal string — every amount a
/// ФНО XML carries is whole tenge, never tiyn, per the brief.
inline std::string tenge_amount(long long tiyn) {
    return std::to_string(round_half_up_to_tenge(tiyn));
}

/// One `<field name="...">value</field>` element, matching the real
/// `<sheet>`-scoped field grid confirmed in Fno910.hpp's file header.
/// `name` is always one of this codebase's own compile-time field-name
/// literals (never caller/user-supplied), so it is never escaped; `value`
/// is text content and IS escaped.
inline std::string field(const std::string& name, const std::string& value) {
    return "<field name=\"" + name + "\">" + escape(value) + "</field>";
}

/// Overload for an already-numeric value (e.g. `tenge_amount`'s output) —
/// digits and an optional leading '-' need no escaping, but routing every
/// call site through `field()` keeps them symmetrical and keeps the
/// `<field name="...">` boilerplate in exactly one place.
inline std::string field(const std::string& name, long long numeric_value) {
    return field(name, std::to_string(numeric_value));
}

/// The three identity fields every KGD/СОНО "sheet" template carries,
/// confirmed against the real form_910_00 v27 example XML (see
/// Fno910.hpp's file header for provenance): `iin` and `rnn` both hold the
/// taxpayer's 12-digit identifier — BIN for a legal entity, IIN for an
/// individual entrepreneur, the SAME field name is used for both in the
/// real format — and `payer_name1` the taxpayer's name.
///
/// The real format also has `payer_name2`/`payer_name3` for a legal name
/// that wraps onto a second/third printed line; OrgInfo carries a single
/// `name` string, so those two are deliberately OMITTED here rather than
/// filled with an empty line, which would misrepresent a genuine
/// multi-line name as intentionally blank.
inline std::string identity_fields(const OrgInfo& org) {
    return field("iin", org.bin) + field("rnn", org.bin) + field("payer_name1", org.name);
}

}  // namespace FnoXml
}  // namespace Tax
