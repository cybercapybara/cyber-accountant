/**
 * @file Strings.hpp
 * @brief Small string helpers used in multiple modules.
 */

#pragma once

#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Utils::Strings {

/**
 * @brief Paths that every middleware treats as never-authenticated and
 *        never-rate-limited. Read once from `api.public_paths` config /
 *        `API_PUBLIC_PATHS` env and reused by all security modules to
 *        avoid skew between per-module overrides.
 *
 * Entries are exact-match, except a trailing `*` matches by prefix — needed
 * for the token-bearing account routes (confirm / reset / change-email),
 * which carry the token as a path segment and so can't be matched exactly.
 * Those flows MUST be reachable without a session (the user clicking an email
 * link isn't logged in), so they ship public by default. Note the static
 * `*-request` / `confirm-resend` routes are deliberately NOT here:
 * change-email-request and confirm-resend require an authenticated principal.
 */
inline constexpr const char* kDefaultPublicPathsCsv =
    "/,/healthz,/ready,/health,/metrics,"
    "/api/v1/docs,/api/v1/openapi.yaml,"
    "/api/v1/auth/login,/api/v1/auth/register,/api/v1/auth/refresh,"
    "/api/v1/account/confirm/*,/api/v1/account/reset-password-request,"
    "/api/v1/account/reset-password/*,/api/v1/account/change-email/*,"
    "/api/v1/account/join-from-invite/*";

/**
 * @brief Public endpoints that must STILL be rate-limited despite being
 *        auth-public. These are the brute-force / mail-bombing surfaces:
 *        login & register (credential stuffing), refresh (token churn),
 *        reset-password-request (mail bomb), and the token-bearing links
 *        (reset / confirm / change-email / invite — guessable-token attempts).
 *
 * This is the auth/account subset of kDefaultPublicPathsCsv minus the infra
 * and static surface (`/`, `/healthz`, `/ready`, `/health`, `/metrics`,
 * `/api/v1/docs`, `/api/v1/openapi.yaml`), which we never want to throttle. The
 * general limiter skips everything in api.public_paths; without this list the
 * auth surface would be skipped too, leaving it wide open.
 * Matched the same way as public paths (exact, or trailing `*` prefix).
 */
inline constexpr const char* kDefaultProtectedPathsCsv =
    "/api/v1/auth/login,/api/v1/auth/register,/api/v1/auth/refresh,"
    "/api/v1/account/confirm/*,/api/v1/account/reset-password-request,"
    "/api/v1/account/reset-password/*,/api/v1/account/change-email/*,"
    "/api/v1/account/join-from-invite/*";

/**
 * @brief True if @p path is covered by @p public_paths — exact match, or a
 *        prefix match for an entry ending in `*`. Shared by Auth / RateLimit /
 *        Idempotency so they can't disagree about what's public.
 */
inline bool path_is_public(const std::unordered_set<std::string>& public_paths, const std::string& path) {
    if (public_paths.count(path) > 0)
        return true;
    for (const auto& p : public_paths) {
        if (!p.empty() && p.back() == '*') {
            const std::string_view prefix(p.data(), p.size() - 1);
            if (path.size() >= prefix.size() && path.compare(0, prefix.size(), prefix) == 0)
                return true;
        }
    }
    return false;
}

/// Characters `trim()` (and the CSV splitter below) treat as surrounding
/// whitespace. ASCII only, and deliberately so: every byte here is < 0x80, so
/// none of them can be a continuation byte of a multi-byte UTF-8 sequence —
/// trimming a Cyrillic string can never cut a character in half.
inline constexpr const char* kTrimChars = " \t\r\n";

/**
 * @brief Strip leading/trailing whitespace. Returns "" for an all-whitespace
 *        (or empty) input.
 *
 * One shared implementation because this idiom had reached three copies —
 * split_csv_vec() below, RateLimit's X-Forwarded-For hop splitter, and
 * LedgerDocumentsController's void `reason` check — and a fourth was about to
 * appear. Callers that must reject "blank" input compare the RESULT to empty
 * rather than re-deriving their own notion of blankness.
 */
inline std::string trim(const std::string& s) {
    const std::string::size_type a = s.find_first_not_of(kTrimChars);
    if (a == std::string::npos)
        return {};
    const std::string::size_type b = s.find_last_not_of(kTrimChars);
    return s.substr(a, b - a + 1);
}

/**
 * @brief Split @p csv on commas, dropping empty components.
 */
inline std::vector<std::string> split_csv_vec(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string piece;
    while (std::getline(ss, piece, ',')) {
        // Trim surrounding whitespace so "a, b" yields {"a","b"} not {"a"," b"}
        // — public-path / whitelist / CORS configs are routinely written with
        // spaces after commas.
        const std::string trimmed = trim(piece);
        if (trimmed.empty())
            continue;  // all-whitespace / empty
        out.push_back(trimmed);
    }
    return out;
}

/// CSV → unordered_set wrapper for callers that need set semantics (auth /
/// rate-limit public paths). Built on top of split_csv_vec — single source.
inline std::unordered_set<std::string> split_csv_set(const std::string& csv) {
    auto v = split_csv_vec(csv);
    return {v.begin(), v.end()};
}

/// Partially redact an email for logs: keep the first local char and the full
/// domain, mask the rest ("john.doe@example.com" -> "j***@example.com"). A
/// single-char (or empty) local part is fully masked so it can't be recovered.
/// Inputs without '@' are treated as the local part (defensive — not a real
/// address). Use everywhere an address would otherwise land in a log line; raw
/// PII in logs is inherited by every fork by default.
inline std::string mask_email(const std::string& email) {
    if (email.empty())
        return email;
    const auto at = email.find('@');
    const std::string local = (at == std::string::npos) ? email : email.substr(0, at);
    const std::string domain = (at == std::string::npos) ? std::string() : email.substr(at);  // includes '@'
    const std::string masked = (local.size() > 1) ? (std::string(1, local[0]) + "***") : "***";
    return masked + domain;
}

}  // namespace Utils::Strings
