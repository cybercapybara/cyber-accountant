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
 * RBAC: `uploads` and `confirm-upload` create/modify a document row and go
 * through API_REQUIRE_ORG_PERM for `documents`/write against the §5.3
 * permission matrix (Tenancy::OrgPerm), which DENIES BY DEFAULT — an unknown
 * role, resource or action is a 403, so a role added later cannot fail open
 * the way the old `ctx.role == "viewer"` denylist let it. `download-url`
 * never writes to the database (it only mints a presigned URL for an object
 * that already has an s3_key), so it carries no WRITE gate — but it hands
 * out the document's bytes, which makes it a READ, and it is gated as one.
 *
 * The read side cannot use API_REQUIRE_ORG_PERM with a fixed resource,
 * because ONE table holds two §5.3 resources: кадровые documents
 * (doc_type='hr') are `hr_docs`, which the `hr` role may read, while all
 * other primary documents are `documents`, which it may not. Hence the two
 * private helpers below — resource_for(doc) and ensure_document_access() —
 * which every route that has already loaded a row must go through, and
 * `list`'s unconditional narrowing: a caller holding only `hr_docs`/read
 * gets doc_type='hr' forced onto BOTH the listing query and its count,
 * regardless of what ?type it did or did not send.
 *
 * Presigning needs Storage::S3Storage::presign(), which is deliberately NOT
 * part of the StorageBackend interface (LocalStorage has no query-signing
 * equivalent — see Storage.hpp's class comments) — the global accessor
 * Storage::get() returns the interface type, so both presigning handlers
 * dynamic_cast it down via s3_backend(); a non-S3 backend, or Storage not
 * initialized at all, answers 503 rather than throwing.
 *
 * s3_key-before-confirm (this task's designated repository extension):
 * DocumentRepository::set_version_file() requires checksum_sha256/mime/
 * size_bytes together, none of which are known until the client's PUT
 * actually finishes, but the presigned PUT URL and the document row both need
 * to agree on the SAME s3_key from the moment the upload starts (a client that
 * reloads the page, or retries confirm-upload, reads the key back off the
 * row rather than re-deriving it). The fix:
 * DocumentRepository::set_pending_upload() (new, see that file for the full
 * rationale) persists s3_key + mime right away; confirm-upload's later
 * set_version_file() call fills in checksum_sha256/size_bytes once
 * Storage::exists() has verified the object is actually there.
 *
 * P3 (migrations/018_document_versions.sql): those file fields live on a
 * document VERSION now, and a document reports the file of its CURRENT
 * version. An upload has no asynchronous render job to move that pointer
 * later, so set_pending_upload() publishes version 1 in the same transaction
 * it writes the key into — without that, `found->s3_key` in confirmUpload
 * would always be empty and every confirm-upload would answer 409
 * no_pending_upload.
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
 *
 * Fix round 2: `startUpload`/`confirmUpload` used to dump BOTH structural
 * (missing/wrong-type field — 400) and semantic (allowlist/format/range —
 * 422) errors into one `Errors` collector dispatched through a single
 * `Validation::response_400(errs)` call, which meant a semantic failure
 * (an unlisted `doc_type`, a traversal-shaped `filename`, a malformed
 * `checksum_sha256`) always came back 400 even though this file's own
 * comments (and the tests) already said 422. Each handler now runs two
 * SEPARATE `Errors` collectors — structural first (dispatched via
 * `Validation::response_400`, returning early), then semantic (dispatched
 * via the new `Validation::response_422(const Errors&)` multi-field
 * overload) — so a value read past phase 1 is already guaranteed
 * present + correctly typed, matching the split
 * `CounterpartiesController::validate_and_fill` already used for
 * `identifier`'s check-digit check.
 *
 * Final pre-merge fix: `confirmUpload` used to trust size_bytes/
 * checksum_sha256 for ANY document it could find, so calling it on a
 * source='generated' document (docgen's own reproducible checksum/mime/
 * size) — or on an already-'final' uploaded one — would let the client
 * overwrite that real audit metadata with unverified values. `confirmUpload`
 * now 409s (`invalid_state`) unless the document is `source='uploaded'` AND
 * `status='draft'`, checked before the s3_key/Storage::exists() checks
 * (a generated document already has a real s3_key and an existing object,
 * so those alone would not have caught this).
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
#include "tenancy/OrgPermissions.hpp"
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

        // Two §5.3 resources share this table (see the RBAC note in the file
        // header), so the gate is a pair of allows() calls, not one macro:
        // read on EITHER lets the caller see the registry at all.
        const bool may_read_all =
            Tenancy::OrgPerm::allows(ctx.role, Tenancy::OrgPerm::Resource::kDocuments, Tenancy::OrgPerm::Action::kRead);
        const bool may_read_hr =
            Tenancy::OrgPerm::allows(ctx.role, Tenancy::OrgPerm::Resource::kHrDocs, Tenancy::OrgPerm::Action::kRead);
        if (!may_read_all && !may_read_hr) {
            callback(ErrorResponse::forbidden("org_role_denied",
                                              "Your role in this organization is not allowed to read documents"));
            return;
        }

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
            // Query-param filters, not a JSON body, but the same semantic-
            // validation split as everywhere else in this file: an
            // unlisted type/status VALUE is a 422 (Validation::response_422),
            // not a 400 — there's no missing/wrong-type case here at all
            // since these are plain query strings, not a typed JSON body.
            callback(Validation::response_422(errs));
            return;
        }

        // The кадровик sees the registry, but only their own slice of it.
        // ?type is OVERWRITTEN unconditionally, not merely checked: the
        // narrowing has to be a property of the ROLE, not a consequence of
        // the client politely sending ?type=hr — otherwise an empty request
        // would hand back every invoice in the ledger. The assignment sits
        // AFTER the ?type parsing for exactly that reason, and the SAME
        // type_filter goes into list_filtered AND count_filtered below: a
        // narrowing applied to only one of the pair would still tell the
        // кадровик how many primary documents exist without showing them.
        if (!may_read_all)
            type_filter = std::string("hr");

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
            if (!ensure_document_access(callback, ctx, *found, Tenancy::OrgPerm::Action::kRead))
                return;
            callback(Response::ok({{"data", json(*found)}}));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/documents/{id}/download-url — presigned GET, TTL 300s.
    // POST, but semantically a READ: it writes nothing and hands out the
    // document's bytes, so it carries the same per-document read gate as
    // GET /documents/{id} — checked AFTER find_in_org, so a document that
    // belongs to another organization stays a 404 rather than becoming a
    // 403 that confirms its existence.
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
            if (!ensure_document_access(callback, ctx, *found, Tenancy::OrgPerm::Action::kRead))
                return;
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
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kDocuments, Tenancy::OrgPerm::Action::kWrite);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        // Phase 1 — structural (missing / wrong-type field): 400. Checked
        // BEFORE any semantic check touches these fields' VALUES, so a
        // caller that sends e.g. {"doc_type": 5} gets "not_string" (400),
        // never "not_allowed" (422) for the same field in the same request.
        Validation::Errors errs;
        Validation::require(errs, body, "filename");
        Validation::require(errs, body, "mime");
        Validation::require(errs, body, "doc_type");
        if (body.contains("filename") && !body["filename"].is_string())
            errs.add("filename", "not_string", "must be a string");
        if (body.contains("mime") && !body["mime"].is_string())
            errs.add("mime", "not_string", "must be a string");
        if (body.contains("doc_type") && !body["doc_type"].is_string())
            errs.add("doc_type", "not_string", "must be a string");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        // Phase 2 — semantic (right type, present, VALUE fails a domain
        // rule): 422. Every field read here is already confirmed present +
        // string-typed by phase 1, so `.get<std::string>()` is safe, and
        // one_of()'s own internal not_string branch can never fire again —
        // only its "not_allowed" branch is reachable from this point on.
        Validation::Errors semantic_errs;
        if (!is_plain_filename(body["filename"].get<std::string>()))
            semantic_errs.add("filename", "invalid_filename", "must be a plain file name");
        Validation::one_of(semantic_errs, body, "doc_type", allowed_doc_types());
        if (semantic_errs.any()) {
            callback(Validation::response_422(semantic_errs));
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
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kDocuments, Tenancy::OrgPerm::Action::kWrite);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed document id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        // Phase 1 — structural (missing / wrong-type field): 400. Same
        // split as startUpload() above — see this file's header and
        // Api::Validation::response_422's doc comment.
        Validation::Errors errs;
        Validation::require(errs, body, "size_bytes");
        Validation::require(errs, body, "checksum_sha256");
        if (body.contains("size_bytes") && !body["size_bytes"].is_number_integer())
            errs.add("size_bytes", "not_integer", "must be an integer");
        if (body.contains("checksum_sha256") && !body["checksum_sha256"].is_string())
            errs.add("checksum_sha256", "not_string", "must be a string");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        // Phase 2 — semantic (right type, present, VALUE fails a domain
        // rule): 422. size_bytes/checksum_sha256 are already confirmed
        // integer/string by phase 1, so int_range()/regex_match()'s own
        // internal type-check branches can never fire again here.
        Validation::Errors semantic_errs;
        // 10 GiB ceiling: a sane upper bound for accounting-document
        // attachments, not a spec'd limit — big enough to never legitimately
        // trip, small enough to reject an obviously wrong value.
        Validation::int_range(semantic_errs, body, "size_bytes", 0, 10LL * 1024 * 1024 * 1024);
        static const std::regex kSha256Hex(R"(^[0-9a-f]{64}$)");
        Validation::regex_match(semantic_errs, body, "checksum_sha256", kSha256Hex, "64 lowercase hex characters");
        if (semantic_errs.any()) {
            callback(Validation::response_422(semantic_errs));
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
            // Lifecycle guard: confirm-upload is only meaningful for a
            // document THIS endpoint's own startUpload() created — a
            // source='uploaded' row still sitting in 'draft'. Without this
            // check, calling confirm-upload on a source='generated' document
            // (docgen's own s3_key already set and its checksum/mime/size
            // already the true, reproducible values of the rendered PDF) or
            // on an already-'final' uploaded document would let the CLIENT's
            // reported size_bytes/checksum_sha256 below overwrite that real
            // audit metadata with unverified values — a document-integrity
            // hole, not just a wasted call. Checked BEFORE the s3_key/
            // Storage::exists() checks that follow, since a generated
            // document already has a (real) s3_key and an existing object,
            // so those checks alone would not catch this.
            if (found->source != "uploaded" || found->status != "draft") {
                callback(ErrorResponse::conflict("invalid_state",
                                                 "confirm-upload is only valid for draft uploaded documents"));
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
            // set_file(document_id, ...) is gone: file metadata lives on a
            // VERSION now (migrations/018_document_versions.sql). An uploaded
            // document has exactly one version and startUpload's
            // set_pending_upload() already made it current — which is also
            // why `found->s3_key` above is non-empty at all, since that field
            // is read through the current-version pointer.
            if (!found->current_version_id) {
                callback(ErrorResponse::conflict("no_pending_upload", "No upload was started for this document"));
                return;
            }
            repo.set_version_file(ctx.org_id, *found->current_version_id, *found->s3_key, checksum, mime, size_bytes);
            repo.set_status(ctx.org_id, id, "final");
            auto fresh = repo.find_in_org(id, ctx.org_id, /*from_primary=*/true);
            callback(Response::ok({{"data", json(fresh ? *fresh : *found)}}));
        });
    }

private:
    /// Ресурс матрицы §5.3 для КОНКРЕТНОГО документа: кадровые документы —
    /// hr_docs (кадровик их видит), вся остальная первичка — documents (не
    /// видит). Обе живут в одной таблице, поэтому ресурс определяется
    /// строкой, а не маршрутом.
    static const char* resource_for(const Ledger::Document& doc) {
        return doc.doc_type == "hr" ? Tenancy::OrgPerm::Resource::kHrDocs : Tenancy::OrgPerm::Resource::kDocuments;
    }

    /// Проверить право @p action на @p doc; при отказе ответить 403 и
    /// вернуть false (вызывающий делает `return;`). Функция, а не макрос
    /// вроде API_REQUIRE_ORG_PERM: решение зависит от УЖЕ ПРОЧИТАННОЙ
    /// строки, то есть от значения, а не только от ctx, и вызывается
    /// строго ПОСЛЕ find_in_org — «не в этой организации» обязано
    /// оставаться 404, а не превращаться в 403.
    ///
    /// Все маршруты над ОДНИМ документом обязаны ходить через неё, а не
    /// повторять условие у себя: сейчас их два (get, downloadUrl), задачи 9
    /// и 11 добавят ещё пять (listVersions, createVersion,
    /// versionDownloadUrl, remove, voidDocument), и семь копий одного
    /// условия разъедутся при первой же правке матрицы.
    static bool ensure_document_access(const std::function<void(const HttpResponsePtr&)>& callback,
                                       const Tenancy::OrgContext& ctx,
                                       const Ledger::Document& doc,
                                       const char* action) {
        if (Tenancy::OrgPerm::allows(ctx.role, resource_for(doc), action))
            return true;
        callback(ErrorResponse::forbidden(
            "org_role_denied",
            "Your role in this organization is not allowed to " + std::string(action) + " this document"));
        return false;
    }

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
