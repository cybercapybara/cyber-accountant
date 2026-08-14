/**
 * @file LedgerDocumentsController.hpp
 * @brief Document registry + presigned S3 download/upload API (design spec
 *        §6.4 / Task 12).
 *
 * Routes (all under /api/v1, every handler starts with
 * API_REQUIRE_ORG(req, callback, ctx)):
 *   GET  /api/v1/documents                    list, paginated, optional
 *                                               ?type=&status= filters
 *                                               (allowlisted against
 *                                               migrations/010_documents.sql's
 *                                               CHECK lists — an unlisted
 *                                               value is a 422, not a
 *                                               silently-ignored filter)
 *   GET  /api/v1/documents/{id}                fetch one
 *   POST /api/v1/documents/{id}/download-url   presigned GET, TTL 300s
 *   POST /api/v1/documents/uploads             start a client-driven
 *                                                upload: creates a draft
 *                                                document (source=uploaded)
 *                                                + a presigned PUT, TTL 600s
 *   POST /api/v1/documents/{id}/confirm-upload verify the object landed in
 *                                                S3, then finalize
 *
 * RBAC: `download-url` never writes to the database (it only mints a
 * presigned URL for an object that already has an s3_key) — this task's
 * brief scopes the viewer-mutation gate to routes that actually mutate, so
 * `download-url` does NOT reject viewers; `uploads` and `confirm-upload`
 * create/modify a document row and DO.
 *
 * Presigning needs Storage::S3Storage::presign(), which is deliberately NOT
 * part of the StorageBackend interface (LocalStorage has no query-signing
 * equivalent — see Storage.hpp's class comments) — the global accessor
 * Storage::get() returns the interface type, so both presigning handlers
 * dynamic_cast it down via s3_backend(); a non-S3 backend, or Storage not
 * initialized at all, answers 503 rather than throwing.
 *
 * s3_key-before-confirm (this task's designated repository extension):
 * DocumentRepository::set_file() requires checksum_sha256/mime/size_bytes
 * together, none of which are known until the client's PUT actually
 * finishes, but the presigned PUT URL and the document row both need to
 * agree on the SAME s3_key from the moment the upload starts (a client that
 * reloads the page, or retries confirm-upload, reads the key back off the
 * row rather than re-deriving it). The fix:
 * DocumentRepository::set_pending_upload() (new, see that file for the full
 * rationale) persists s3_key + mime right away; confirm-upload's later
 * set_file() call fills in checksum_sha256/size_bytes once
 * Storage::exists() has verified the object is actually there.
 *
 * Security note (uploads' `filename` field): Files::org_key() already
 * neutralizes an unsafe filename inside the S3 KEY itself (uuid-prefixed,
 * then sanitize_filename()'s [A-Za-z0-9._-] allowlist), and
 * Storage::key_is_safe() independently rejects ".." at the storage layer —
 * but this controller does NOT lean on either as its only line of defense.
 * `is_plain_filename()` below rejects an unsafe/malformed filename with a
 * clean 422 (not a 500 surfaced from deep in the storage layer, and not a
 * traversal-shaped string silently accepted and stored). The ORIGINAL
 * filename (pre-sanitization) is kept as document metadata for the UI/audit
 * trail — `documents` has no dedicated column for it, so it rides in
 * `input_snapshot` as `{"original_filename": ...}` for source='uploaded'
 * rows; see Document.hpp's doc comment for that column, updated to note
 * this one exception to its "docgen reproducibility only" rule.
 */

#pragma once

#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "files/FileKeys.hpp"
#include "ledger/Document.hpp"
#include "ledger/DocumentRepository.hpp"
#include "storage/Storage.hpp"
#include "tenancy/OrgContext.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class LedgerDocumentsController : public HttpController<LedgerDocumentsController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(LedgerDocumentsController::list, "/api/v1/documents", Get);
    ADD_METHOD_TO(LedgerDocumentsController::get, "/api/v1/documents/{1}", Get);
    ADD_METHOD_TO(LedgerDocumentsController::downloadUrl, "/api/v1/documents/{1}/download-url", Post);
    ADD_METHOD_TO(LedgerDocumentsController::startUpload, "/api/v1/documents/uploads", Post);
    ADD_METHOD_TO(LedgerDocumentsController::confirmUpload, "/api/v1/documents/{1}/confirm-upload", Post);
    METHOD_LIST_END

    /// doc_type allowlist — mirrors migrations/010_documents.sql's CHECK
    /// constraint byte-for-byte. DocumentRepository does not validate this
    /// itself (see that file's header: it trusts its caller on CHECK-shaped
    /// values), so this controller is the one place it's enforced.
    static const std::vector<std::string>& allowed_doc_types() {
        static const std::vector<std::string> v = {"invoice",
                                                   "avr",
                                                   "waybill",
                                                   "tax_invoice",
                                                   "reconciliation",
                                                   "power_of_attorney",
                                                   "incoming",
                                                   "bank_statement",
                                                   "hr",
                                                   "fno",
                                                   "other"};
        return v;
    }

    /// status allowlist — same CHECK constraint, the union of both the
    /// generated (draft/final/sent) and inbound (inbox/recognized/linked/
    /// archived) lifecycles (see Document.hpp / that migration's header).
    static const std::vector<std::string>& allowed_statuses() {
        static const std::vector<std::string> v = {
            "inbox", "recognized", "linked", "archived", "draft", "final", "sent"};
        return v;
    }

    // -------------------------------------------------------------------
    // GET /api/v1/documents — paginated, optional ?type=&status= filters.
    // -------------------------------------------------------------------
    void list(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);

        Validation::Errors errs;
        std::optional<std::string> type_filter;
        std::optional<std::string> status_filter;
        const std::string type_param = req->getParameter("type");
        const std::string status_param = req->getParameter("status");
        if (!type_param.empty()) {
            if (!is_allowed(type_param, allowed_doc_types()))
                errs.add("type", "not_allowed", "must be one of the registered document types");
            else
                type_filter = type_param;
        }
        if (!status_param.empty()) {
            if (!is_allowed(status_param, allowed_statuses()))
                errs.add("status", "not_allowed", "must be one of the registered document statuses");
            else
                status_filter = status_param;
        }
        if (errs.any()) {
            // Query-param filters, not a JSON body — same 422
            // Api::Validation shape as the identifier checks elsewhere in
            // this task, built directly via ErrorResponse::make (there is
            // no request body for Validation::response_400 to describe).
            callback(ErrorResponse::make(
                {drogon::k422UnprocessableEntity, "validation_failed", "", json{{"errors", errs.errors_json()}}}));
            return;
        }

        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);
        with_repo_errors(callback, "documents list", [&] {
            Ledger::DocumentRepository repo;
            auto rows = repo.list_filtered(ctx.org_id, type_filter, status_filter, page.limit, page.offset);
            long total = repo.count_filtered(ctx.org_id, type_filter, status_filter);
            json data = json::array();
            for (const auto& d : rows)
                data.push_back(d);
            callback(Response::paginated(data, total, page.limit, page.offset));
        });
    }

    // -------------------------------------------------------------------
    // GET /api/v1/documents/{id}
    // -------------------------------------------------------------------
    void get(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed document id"));
            return;
        }

        with_repo_errors(callback, "documents get", [&] {
            Ledger::DocumentRepository repo;
            auto found = repo.find_in_org(id, ctx.org_id);
            if (!found) {
                callback(ErrorResponse::not_found("document"));
                return;
            }
            callback(Response::ok({{"data", json(*found)}}));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/documents/{id}/download-url — presigned GET, TTL 300s.
    // Read-only (mints a URL, writes nothing) — no viewer gate.
    // -------------------------------------------------------------------
    void downloadUrl(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed document id"));
            return;
        }

        with_repo_errors(callback, "documents downloadUrl", [&] {
            Ledger::DocumentRepository repo;
            auto found = repo.find_in_org(id, ctx.org_id);
            if (!found) {
                callback(ErrorResponse::not_found("document"));
                return;
            }
            if (!found->s3_key || found->s3_key->empty()) {
                callback(ErrorResponse::conflict("no_file", "This document has no stored file yet"));
                return;
            }
            auto* s3 = s3_backend();
            if (!s3) {
                callback(ErrorResponse::service_unavailable("presign_unsupported",
                                                            "Presigned URLs require the S3 storage backend"));
                return;
            }
            const std::string url = s3->presign(*found->s3_key, "GET", 300);
            callback(Response::ok({{"url", url}}));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/documents/uploads — body { filename, mime, doc_type }.
    // Creates a draft document (source=uploaded, status=draft) + mints a
    // presigned PUT, TTL 600s. Accountant/owner only.
    // -------------------------------------------------------------------
    void startUpload(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (ctx.role == "viewer") {
            callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot upload documents"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "filename");
        Validation::require(errs, body, "mime");
        Validation::require(errs, body, "doc_type");
        if (body.contains("filename") && !body["filename"].is_string())
            errs.add("filename", "not_string", "must be a string");
        else if (body.contains("filename") && !is_plain_filename(body["filename"].get<std::string>()))
            errs.add("filename", "invalid_filename", "must be a plain file name");
        if (body.contains("mime") && !body["mime"].is_string())
            errs.add("mime", "not_string", "must be a string");
        Validation::one_of(errs, body, "doc_type", allowed_doc_types());
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        auto* s3 = s3_backend();
        if (!s3) {
            callback(ErrorResponse::service_unavailable("presign_unsupported",
                                                        "Presigned URLs require the S3 storage backend"));
            return;
        }

        const std::string filename = body["filename"].get<std::string>();
        const std::string mime = body["mime"].get<std::string>();
        const std::string doc_type = body["doc_type"].get<std::string>();
        // "inbox" — user-driven uploads land under org/{org_id}/inbox/... in
        // the Files::org_key layout (T7); "generated"/"statements" are the
        // other two kinds that layout documents, produced by docgen/bank-
        // import jobs rather than this endpoint. is_plain_filename() above
        // already rejected anything traversal-shaped, so org_key()'s own
        // uuid-prefix + sanitize_filename() allowlist is defense-in-depth
        // here, not the only check.
        const std::string s3_key = Files::org_key(ctx.org_id, "inbox", filename);

        with_repo_errors(callback, "documents startUpload", [&] {
            Ledger::DocumentRepository repo;
            // Original (pre-sanitization) filename kept as metadata — see
            // this file's header and Document.hpp's input_snapshot comment
            // for why it rides in input_snapshot rather than a dedicated
            // column.
            auto created = repo.create(ctx.org_id,
                                       doc_type,
                                       "uploaded",
                                       "draft",
                                       /*counterparty_id=*/std::nullopt,
                                       /*template_slug=*/std::nullopt,
                                       /*template_version=*/std::nullopt,
                                       /*input_snapshot=*/json{{"original_filename", filename}});
            repo.set_pending_upload(ctx.org_id, created.id, s3_key, mime);
            auto fresh = repo.find_in_org(created.id, ctx.org_id, /*from_primary=*/true);
            const std::string upload_url = s3->presign(s3_key, "PUT", 600);
            callback(Response::created({{"data", json(fresh ? *fresh : created)}, {"upload_url", upload_url}}));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/documents/{id}/confirm-upload — body { size_bytes,
    // checksum_sha256 }. Verifies the object actually landed in S3 before
    // finalizing. Accountant/owner only.
    // -------------------------------------------------------------------
    void confirmUpload(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (ctx.role == "viewer") {
            callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot confirm uploads"));
            return;
        }
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed document id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "size_bytes");
        Validation::require(errs, body, "checksum_sha256");
        // 10 GiB ceiling: a sane upper bound for accounting-document
        // attachments, not a spec'd limit — big enough to never legitimately
        // trip, small enough to reject an obviously wrong value.
        Validation::int_range(errs, body, "size_bytes", 0, 10LL * 1024 * 1024 * 1024);
        static const std::regex kSha256Hex(R"(^[0-9a-f]{64}$)");
        Validation::regex_match(errs, body, "checksum_sha256", kSha256Hex, "64 lowercase hex characters");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        if (!Storage::is_initialized()) {
            callback(ErrorResponse::service_unavailable("storage_unavailable", "Object storage is not configured"));
            return;
        }

        const long long size_bytes = body["size_bytes"].get<long long>();
        const std::string checksum = body["checksum_sha256"].get<std::string>();

        with_repo_errors(callback, "documents confirmUpload", [&] {
            Ledger::DocumentRepository repo;
            auto found = repo.find_in_org(id, ctx.org_id, /*from_primary=*/true);
            if (!found) {
                callback(ErrorResponse::not_found("document"));
                return;
            }
            if (!found->s3_key || found->s3_key->empty()) {
                callback(ErrorResponse::conflict("no_pending_upload", "No upload was started for this document"));
                return;
            }
            // Storage::exists() is on the StorageBackend interface (unlike
            // presign()) — this check works against ANY configured backend,
            // not just S3.
            if (!Storage::get().exists(*found->s3_key)) {
                callback(ErrorResponse::conflict("object_missing", "The uploaded object was not found in storage"));
                return;
            }
            const std::string mime = found->mime.value_or("");
            repo.set_file(ctx.org_id, id, *found->s3_key, checksum, mime, size_bytes);
            repo.set_status(ctx.org_id, id, "final");
            auto fresh = repo.find_in_org(id, ctx.org_id, /*from_primary=*/true);
            callback(Response::ok({{"data", json(fresh ? *fresh : *found)}}));
        });
    }

private:
    static bool is_allowed(const std::string& v, const std::vector<std::string>& allowed) {
        for (const auto& a : allowed)
            if (v == a)
                return true;
        return false;
    }

    /// Reject a `filename` that isn't a plain, single-segment file name —
    /// checked BEFORE Files::org_key() ever sees it (see this file's header
    /// Security note). Empty, over 255 bytes, a leading '.' (hidden file /
    /// "." or ".."), any '/' or '\\' (path separator on either platform),
    /// any ".." substring (traversal, even embedded — "a/../../etc"), or a
    /// control character (< 0x20) are all rejected. Everything else is
    /// allowed, including non-ASCII (e.g. Cyrillic) and spaces/parentheses —
    /// those are perfectly valid file names, just not safe S3 KEY
    /// characters, which is exactly what org_key()'s own sanitize_filename()
    /// handles downstream.
    static bool is_plain_filename(const std::string& filename) {
        if (filename.empty() || filename.size() > 255)
            return false;
        if (filename.front() == '.')
            return false;
        if (filename.find("..") != std::string::npos)
            return false;
        for (unsigned char c : filename) {
            if (c < 0x20 || c == '/' || c == '\\')
                return false;
        }
        return true;
    }

    /// The presign-capable backend, or nullptr when Storage isn't
    /// initialized or the configured backend isn't S3 (e.g. local-disk dev
    /// storage) — presign() is not part of the StorageBackend interface
    /// (see Storage.hpp's class comments), only Storage::S3Storage
    /// implements it.
    static Storage::S3Storage* s3_backend() {
        if (!Storage::is_initialized())
            return nullptr;
        return dynamic_cast<Storage::S3Storage*>(&Storage::get());
    }
};

}  // namespace Api
