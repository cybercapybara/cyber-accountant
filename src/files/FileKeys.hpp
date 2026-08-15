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

/**
 * @brief Key for a file that belongs to ONE document version.
 * @param org_id     Owning organization id — same trusted-id contract as
 *                    org_key(): it comes from the JWT claim / the job payload
 *                    the org-scoped SQL has already matched a row against.
 * @param kind       Sub-tree under the org (the render job passes "generated").
 * @param version_id `document_versions.id` — the version these bytes ARE.
 * @param filename   Human tail, sanitized (never trusted verbatim).
 * @return "org/{org_id}/{kind}/{version_id}/{sanitized-filename}"
 *
 * Why not org_key(): its random uuid makes every CALL unique, which is the
 * right property for an upload (two uploads of "scan.pdf" must not collide)
 * but the wrong one for a render. A version's PDF is evidence — it is
 * addressed by the version it belongs to, so the key itself says which
 * version's bytes these are, an object can be traced back to its row without
 * a database lookup, and version N+1's render lands in its OWN directory
 * instead of anywhere near version N's object. Determinism is safe here
 * precisely because RenderJob renders a given version at most once
 * (DocumentRepository::version_render_state's kAlreadyRendered branch); the
 * random component would only paper over a re-render that must not happen at
 * all, and would leak the superseded object.
 *
 * Tenant safety: the prefix is pinned to @p org_id, so one tenant's objects
 * are never under another's tree; @p version_id is a v4 UUID (unguessable)
 * and is only ever learned through an org-scoped read; and the bucket stays
 * private — the only way to any object is a short-lived presigned URL minted
 * after that org-scoped read. @p version_id is sanitized too, so even a
 * hostile job payload cannot smuggle a '/' into it and climb out of the org's
 * prefix.
 */
inline std::string version_key(const std::string& org_id,
                               const std::string& kind,
                               const std::string& version_id,
                               const std::string& filename) {
    return "org/" + org_id + "/" + kind + "/" + sanitize_filename(version_id) + "/" + sanitize_filename(filename);
}

}  // namespace Files
