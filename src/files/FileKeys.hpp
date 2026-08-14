/**
 * @file FileKeys.hpp
 * @brief Object-storage key layout for org-scoped files (generated reports,
 *        inbox uploads, bank statements, …) — one place so every producer
 *        (a controller, a job) lands objects under the same tree instead of
 *        each inventing its own prefix.
 *
 * Layout: `org/{org_id}/{kind}/{uuid}-{sanitized-filename}`. The uuid makes
 * the key unique even when two uploads share a filename; keeping the
 * sanitized original name in the tail is purely for human debugging (S3
 * consoles, `mc ls`) — callers must not parse it back out.
 */

#pragma once

#include <string>

#include "jobs/Job.hpp"  // Jobs::generate_uuid() — the template's one UUID v4 utility

namespace Files {

/// Reduce an arbitrary, possibly hostile filename to the safe subset
/// `[A-Za-z0-9._-]`; anything else (spaces, path separators, unicode, …)
/// becomes '_'. An empty input becomes the literal "file" rather than
/// producing a trailing '-' with nothing after it in org_key().
inline std::string sanitize_filename(const std::string& filename) {
    if (filename.empty())
        return "file";
    std::string out;
    out.reserve(filename.size());
    for (unsigned char c : filename) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                          c == '_' || c == '-';
        out.push_back(safe ? static_cast<char>(c) : '_');
    }
    return out;
}

/// Build the storage key for an org-scoped file.
/// @param org_id   Owning organization id (caller-validated; not sanitized
///                  here — pass a trusted id, e.g. from the auth principal).
/// @param kind     Sub-tree under the org, e.g. "generated" | "inbox" |
///                  "statements". Any caller-chosen tag works; the three
///                  above are what this codebase currently produces.
/// @param filename Original filename as the client supplied it — sanitized
///                  before use, never trusted verbatim.
/// @return "org/{org_id}/{kind}/{uuid}-{sanitized-filename}"
inline std::string org_key(const std::string& org_id, const std::string& kind, const std::string& filename) {
    return "org/" + org_id + "/" + kind + "/" + Jobs::generate_uuid() + "-" + sanitize_filename(filename);
}

}  // namespace Files
