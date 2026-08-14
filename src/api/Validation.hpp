/**
 * @file Validation.hpp
 * @brief Composable request-body validators.
 * @details Controllers accumulate field errors into a @c Validation::Errors
 *          collector and emit a single 400 response with a structured error
 *          list. Keeps validation logic out of each controller's imperative
 *          tree of `if (…) return Response::ok(err, 400);`.
 *
 *          Response shape:
 *          @code
 *          {
 *            "error": "validation_failed",
 *            "errors": [
 *              {"field": "email", "code": "missing", "message": "required"},
 *              {"field": "username", "code": "too_short", "message": "min 1"}
 *            ]
 *          }
 *          @endcode
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <nlohmann/json.hpp>

#include "utils/ErrorResponse.hpp"

namespace Api::Validation {

using json = nlohmann::json;

// Password length bounds — single source for register / reset-password /
// change-password / admin-create. Mirrored by the zod schema on the frontend
// (frontend/src/lib/schemas/auth.ts).
inline constexpr std::size_t kPasswordMinLen = 8;
inline constexpr std::size_t kPasswordMaxLen = 128;

struct Error {
    std::string field;
    std::string code;
    std::string message;
};

/**
 * @brief Accumulator that each validator appends to.
 */
class Errors {
public:
    void add(std::string field, std::string code, std::string message) {
        items_.push_back({std::move(field), std::move(code), std::move(message)});
    }
    bool any() const { return !items_.empty(); }
    const std::vector<Error>& items() const { return items_; }

    // Build the `errors` array only — the full response body is assembled by
    // response_400() using the shared ErrorResponse shape.
    json errors_json() const {
        json arr = json::array();
        for (const auto& e : items_) {
            arr.push_back({{"field", e.field}, {"code", e.code}, {"message", e.message}});
        }
        return arr;
    }

private:
    std::vector<Error> items_;
};

// ---------------------------------------------------------------------------
// Field validators — each inspects body[field] and appends on failure.
// Every validator is a no-op on a field that doesn't exist (use require()
// first to enforce presence).
// ---------------------------------------------------------------------------

/**
 * @brief Require a field to be present AND a non-null value.
 */
inline bool require(Errors& errs, const json& body, const std::string& field) {
    if (!body.contains(field) || body[field].is_null()) {
        errs.add(field, "missing", "required");
        return false;
    }
    return true;
}

/**
 * @brief Require a string field within [min, max] length. Non-strings fail
 *        with code "not_string". Missing fields are no-op (pair with require).
 */
inline void string_length(Errors& errs, const json& body, const std::string& field, size_t min_len, size_t max_len) {
    if (!body.contains(field) || body[field].is_null())
        return;
    if (!body[field].is_string()) {
        errs.add(field, "not_string", "must be a string");
        return;
    }
    const auto& s = body[field].get_ref<const std::string&>();
    if (s.size() < min_len) {
        errs.add(field, "too_short", "min length " + std::to_string(min_len));
    } else if (s.size() > max_len) {
        errs.add(field, "too_long", "max length " + std::to_string(max_len));
    }
}

/**
 * @brief Require a string field to match the given regex.
 */
inline void regex_match(
    Errors& errs, const json& body, const std::string& field, const std::regex& re, const std::string& format_hint) {
    if (!body.contains(field) || body[field].is_null())
        return;
    if (!body[field].is_string()) {
        errs.add(field, "not_string", "must be a string");
        return;
    }
    const auto& s = body[field].get_ref<const std::string&>();
    if (!std::regex_match(s, re)) {
        errs.add(field, "bad_format", "expected format: " + format_hint);
    }
}

/**
 * @brief Require an integer field within [min, max].
 */
inline void int_range(Errors& errs, const json& body, const std::string& field, long long min_val, long long max_val) {
    if (!body.contains(field) || body[field].is_null())
        return;
    if (!body[field].is_number_integer()) {
        errs.add(field, "not_integer", "must be an integer");
        return;
    }
    auto v = body[field].get<long long>();
    if (v < min_val) {
        errs.add(field, "below_min", "min " + std::to_string(min_val));
    } else if (v > max_val) {
        errs.add(field, "above_max", "max " + std::to_string(max_val));
    }
}

/**
 * @brief Require a string field to be one of a fixed set of values.
 */
inline void one_of(Errors& errs, const json& body, const std::string& field, const std::vector<std::string>& allowed) {
    if (!body.contains(field) || body[field].is_null())
        return;
    if (!body[field].is_string()) {
        errs.add(field, "not_string", "must be a string");
        return;
    }
    const auto& s = body[field].get_ref<const std::string&>();
    for (const auto& a : allowed)
        if (s == a)
            return;
    std::string msg = "must be one of:";
    for (const auto& a : allowed) {
        msg += " '" + a + "'";
    }
    errs.add(field, "not_allowed", std::move(msg));
}

/**
 * @brief Pull an optional string field. Returns nullopt when the field is
 *        absent or not a string — collapses the
 *        `if (body.contains(f) && body[f].is_string()) x = body[f]...`
 *        boilerplate that recurs across the auth/admin controllers.
 */
inline std::optional<std::string> opt_string(const json& body, const std::string& field) {
    if (body.contains(field) && body[field].is_string())
        return body[field].get<std::string>();
    return std::nullopt;
}

inline void email(Errors& errs, const json& body, const std::string& field) {
    // Pragmatic email regex — not a spec-compliant RFC 5322 parser, but
    // rejects the obvious bad inputs without blocking valid ones.
    static const std::regex re(R"(^[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}$)");
    regex_match(errs, body, field, re, "email");
}

inline void uuid(Errors& errs, const json& body, const std::string& field) {
    static const std::regex re("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    regex_match(errs, body, field, re, "uuid");
}

/**
 * @brief `YYYY-MM-DD`, calendar-valid (not just shape/range) — days-in-month
 *        table + the standard Gregorian leap-year rule (divisible by 4,
 *        except centuries not divisible by 400), so e.g. "2026-02-30" fails
 *        here instead of only surfacing later as a raw pqxx `::date`-cast
 *        error (a 500) once it reaches the database.
 *
 * Moved here (Task 11) from JournalController's private `is_valid_date` so
 * Employees/HrController can share it without duplicating the table —
 * JournalController::list()/create() now call this same function; the
 * calendar-validation semantics are unchanged.
 */
inline bool is_valid_date(const std::string& s) {
    static const std::regex re(R"(^(\d{4})-(\d{2})-(\d{2})$)");
    std::smatch m;
    if (!std::regex_match(s, m, re))
        return false;
    const int year = std::stoi(m[1].str());
    const int month = std::stoi(m[2].str());
    const int day = std::stoi(m[3].str());
    if (month < 1 || month > 12 || day < 1)
        return false;
    static const int kDaysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap_year = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
    int days_in_this_month = kDaysInMonth[month - 1];
    if (month == 2 && leap_year)
        days_in_this_month = 29;
    return day <= days_in_this_month;
}

// ---------------------------------------------------------------------------
// Response helper
// ---------------------------------------------------------------------------

inline drogon::HttpResponsePtr response_400(const Errors& errs) {
    return ErrorResponse::bad_request("validation_failed", "", json{{"errors", errs.errors_json()}});
}

/**
 * @brief Single-field 422 — the semantic-validation counterpart to
 *        response_400(): @p field is syntactically fine (right type,
 *        present), but its VALUE fails a domain rule (a typed service
 *        exception, a JSON-Schema mismatch, a cross-org reference, ...).
 *        Same split CounterpartiesController::validate_and_fill's
 *        identifier check-digit branch and LedgerDocumentsController::list's
 *        type/status filter checks already draw ad hoc, each building the
 *        `{"errors": [...]}` body directly via ErrorResponse::make; Task
 *        13's Journal/Docgen controllers are the first callers that need
 *        this shape more than once each, so it moves here instead of
 *        getting copy-pasted a third and fourth time.
 */
inline drogon::HttpResponsePtr response_422(const std::string& field,
                                            const std::string& code,
                                            const std::string& message) {
    Errors errs;
    errs.add(field, code, message);
    return ErrorResponse::make(
        {drogon::k422UnprocessableEntity, "validation_failed", "", json{{"errors", errs.errors_json()}}});
}

/**
 * @brief Multi-field 422 — the semantic-validation counterpart to
 *        response_400(const Errors&) above, for a handler that runs several
 *        semantic checks (an allowlist, a regex format, a numeric range, ...)
 *        into ONE @c Errors collector and needs to report all of them at
 *        once rather than one field at a time (response_422(field, code,
 *        message) covers that single-field case). Same body shape as
 *        response_400() — only the status code differs — so callers that
 *        run structural checks (require/type) into one collector and
 *        semantic checks into a second, separate collector can dispatch
 *        each with the matching one of these two functions (Task 12 fix
 *        round 2: LedgerDocumentsController::startUpload/confirmUpload,
 *        CounterpartiesController::validate_and_fill's identifier check,
 *        LedgerDocumentsController::list's type/status filter checks).
 */
inline drogon::HttpResponsePtr response_422(const Errors& errs) {
    return ErrorResponse::make(
        {drogon::k422UnprocessableEntity, "validation_failed", "", json{{"errors", errs.errors_json()}}});
}

/**
 * @brief Parse the request body as JSON. Returns true on success and fills
 *        @p out; on failure invokes @p cb with a 400 response and returns false.
 *        Eliminates the 5-line try/catch boilerplate in every mutating handler.
 *
 * Usage:
 * @code
 *   json body;
 *   if (!Validation::parse_body(req, body, callback)) return;
 * @endcode
 */
inline bool parse_body(const drogon::HttpRequestPtr& req,
                       json& out,
                       std::function<void(const drogon::HttpResponsePtr&)>& cb) {
    try {
        out = json::parse(std::string(req->body()));
        return true;
    } catch (...) {
        cb(ErrorResponse::bad_request("invalid_json", "Invalid JSON body"));
        return false;
    }
}

/**
 * @brief Parse an OPTIONAL JSON **object** body: an absent/empty body yields
 *        an empty object and success, rather than the `invalid_json` 400
 *        parse_body() (correctly) gives it.
 *
 * For the `.../generate-document` endpoints, whose body carries only the
 * free-text fields a docgen template needs that the database cannot hold, and
 * which is deep-merged (RFC 7396 `merge_patch`) over an auto-derived base —
 * "send nothing" is a legitimate request there, not a malformed one. A body
 * that IS present but isn't a JSON object is still a 400, since merge_patch
 * over a non-object would silently REPLACE the whole derived input.
 *
 * Lives here (Task 12, fix round 1) rather than in each controller: it was a
 * verbatim third copy across HrController's two generate-document routes and
 * PayrollController's payslip one — the same consolidation is_valid_date got
 * in Task 11.
 *
 * @return false (having already replied through @p cb) on a malformed body.
 */
inline bool parse_optional_body(const drogon::HttpRequestPtr& req,
                                json& out,
                                std::function<void(const drogon::HttpResponsePtr&)>& cb) {
    if (req->body().empty()) {
        out = json::object();
        return true;
    }
    if (!parse_body(req, out, cb))
        return false;
    if (!out.is_object()) {
        cb(ErrorResponse::bad_request("invalid_json", "body must be a JSON object"));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// generate-document override allowlist
// ---------------------------------------------------------------------------

/**
 * @brief Recursively check every LEAF key path in @p extra against @p allowed
 *        (dot-separated paths, e.g. "employer.director") — an object value is
 *        walked one level deeper with its key appended to @p prefix, so a
 *        nested override is checked at its own leaf, not permitted merely
 *        because its PARENT key (e.g. "employer") is mentioned in @p allowed.
 *
 * The companion to parse_optional_body() above, and load-bearing for exactly
 * the same endpoints: the `.../generate-document` bodies are deep-merged
 * (RFC 7396 `merge_patch`) over an input the server derived from stored
 * records, so without an allowlist a caller can overwrite an AUTHORITATIVE
 * figure — an employee's iin, a payslip's net, a ФНО's balance — in what
 * becomes a legal document or a tax filing, while the XML/journal for the
 * same row keeps saying something else.
 *
 * @return false (already replied with a 422 naming the offending field) on
 *         the first disallowed key; true if every key in @p extra is on the
 *         allowlist (an empty/absent body trivially passes).
 */
inline bool validate_extra_allowlist(const json& extra,
                                     const std::vector<std::string>& allowed,
                                     const std::function<void(const drogon::HttpResponsePtr&)>& callback,
                                     const std::string& prefix = "") {
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        const std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (it.value().is_object()) {
            if (!validate_extra_allowlist(it.value(), allowed, callback, path))
                return false;
            continue;
        }
        if (std::find(allowed.begin(), allowed.end(), path) == allowed.end()) {
            callback(response_422(
                path, "not_allowed_override", "this field is derived from stored data and cannot be overridden"));
            return false;
        }
    }
    return true;
}

/**
 * @brief Validate @p extra against @p allowed (see validate_extra_allowlist())
 *        and, only if EVERY key passes, deep-merge it onto @p input via
 *        nlohmann::json::merge_patch (RFC 7396).
 *
 * @return false (already replied) if @p extra contains any field outside
 *         @p allowed — @p input is left unmodified in that case, so nothing
 *         downstream ever sees a partially-merged input.
 *
 * Lives here (final fix round) rather than in a controller: HrController
 * grew it first, then TaxController's ФНО filings and PayrollController's
 * payslips needed the identical discipline — the same consolidation
 * is_valid_date and parse_optional_body already got. The per-template
 * allowlists themselves stay in their controllers, next to the base input
 * they guard.
 */
inline bool merge_allowed_extra(json& input,
                                const json& extra,
                                const std::vector<std::string>& allowed,
                                const std::function<void(const drogon::HttpResponsePtr&)>& callback) {
    if (!validate_extra_allowlist(extra, allowed, callback))
        return false;
    input.merge_patch(extra);
    return true;
}

}  // namespace Api::Validation
