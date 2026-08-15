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
 *   GET  /api/v1/documents/{id}/versions       the document's history,
 *                                                oldest first
 *   POST /api/v1/documents/{id}/versions       EDIT: append a version and
 *                                                queue its render (202)
 *   POST /api/v1/documents/{id}/versions/{version_no}/download-url
 *                                              presigned GET for ONE
 *                                                historical version, TTL 300s
 *   DELETE /api/v1/documents/{id}              physically delete a document
 *                                                NOT linked to a posted (or
 *                                                reversed) journal entry — 204
 *   POST /api/v1/documents/{id}/void           mark it void ({reason}); the
 *                                                row, the file and the whole
 *                                                version history stay
 *
 * Delete versus void (P3 task 11) — the rule and why it is shaped this way.
 * The ledger is insert-only and is corrected only by storno, so a document
 * that is the basis of a POSTED (or REVERSED) entry can never be destroyed:
 * it is voided instead, and the entry is fixed by its own mechanism. The
 * condition is therefore the LINK, not the status — an earlier draft keyed
 * deletion on `status='draft'`, which only source='generated' rows ever carry,
 * so an uploaded or emailed scan (inbox -> recognized -> linked -> archived)
 * could never have been deleted at all. A link to a DRAFT entry does not
 * block deletion: document_entries cascades, the draft is left without its
 * basis, and that fact is written to the audit log. A document nothing has
 * posted against but which an HR order or a tax filing points at
 * (hr_orders.document_id / tax_filings.document_id, both NO ACTION) is a 409
 * `document_referenced`, translated from SQLSTATE 23503 in
 * DocumentRepository::remove() — never a 500. Versions are never deletable
 * individually; both routes act on the document as a whole. Objects in S3 are
 * removed by NEITHER path — a stated retention policy, see the header of
 * migrations/019_document_voiding.sql.
 *
 * Editing (P3 task 9) — why the body is `{input}` and not a snapshot.
 * An accounting document is evidence: overwriting one in place silently
 * rewrites a file someone may already have cited. So an edit APPENDS a
 * version, the previous PDF stays in object storage, and `/versions` is what
 * keeps it reachable. The security property that makes this safe is that the
 * edit body goes through the SAME allowlist as creation
 * (Docgen::InputPolicy), never through the stored snapshot:
 *   - a caller-authored slug (invoice/avr/waybill/tax_invoice/reconciliation
 *     — nothing in the database stands behind it) takes the whole object,
 *     exactly like POST /documents/generate, and runs the same money
 *     derivation, so a client-supplied `total`/`total_words` is still a 422;
 *   - every other slug (fno_910, fno_300, payslip, hr_order,
 *     labor_contract) accepts ONLY Docgen::InputPolicy::editable_fields(slug)
 *     merged over the PREVIOUS version's snapshot, so each server-derived
 *     figure is carried forward unchanged from the version that was rendered
 *     out of authoritative rows.
 * `input_snapshot` is precisely what the render job renders, so accepting it
 * wholesale would reopen the forgery hole P2 closed (a declaration whose PDF
 * claimed a 1 ₸ balance while the XML of the same filing told the truth).
 * A document whose `source` is `uploaded` or `email`, or which has no
 * `template_slug`, has no snapshot by construction and is 409 `not_editable`
 * — there is nothing to re-render.
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
 * regardless of what ?type it did or did not send. That helper carries the
 * WRITE gate too, not just reads: `createVersion` is a mutation, but WHICH
 * resource it mutates still depends on the row it just loaded, so it calls
 * ensure_document_access(..., kWrite) rather than API_REQUIRE_ORG_PERM with
 * a fixed resource — a кадровик may edit an hr document and may not even see
 * an invoice, and one gate has to express both.
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
#include "docgen/InputPolicy.hpp"
#include "docgen/TemplateRegistry.hpp"
#include "files/FileKeys.hpp"
#include "jobs/Jobs.hpp"
#include "ledger/Document.hpp"
#include "ledger/DocumentRepository.hpp"
#include "ledger/DocumentVersion.hpp"
#include "security/Audit.hpp"
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
    ADD_METHOD_TO(LedgerDocumentsController::listVersions, "/api/v1/documents/{1}/versions", Get);
    ADD_METHOD_TO(LedgerDocumentsController::createVersion, "/api/v1/documents/{1}/versions", Post);
    // Formatting is suppressed for the next registration only: it is two
    // columns over the limit, and scripts/check-routes-registered.sh scans
    // ADD_METHOD_TO LINE BY LINE — a wrapped registration is invisible to the
    // triple-sync gate, so the route would ship undocumented. Same hazard
    // PayrollController::generatePayslip's comment records; there the fix was
    // a shorter handler name, here it is the PATH that is long.
    // clang-format off
    ADD_METHOD_TO(LedgerDocumentsController::versionDownloadUrl, "/api/v1/documents/{1}/versions/{2}/download-url", Post);
    // clang-format on
    ADD_METHOD_TO(LedgerDocumentsController::remove, "/api/v1/documents/{1}", Delete);
    ADD_METHOD_TO(LedgerDocumentsController::voidDocument, "/api/v1/documents/{1}/void", Post);
    METHOD_LIST_END

    /// TTL of every presigned GET this controller mints — the document's
    /// current file and any one historical version alike.
    static constexpr int kDownloadTtlSec = 300;

    /// The same job type DocgenController::generate enqueues (that file's
    /// header explains why the string is duplicated rather than pulled in
    /// from the worker-side docgen/RenderJob.hpp).
    static constexpr const char* kRenderJobType = "docgen.render";

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
    //
    // Accepted exposure, stated rather than left implicit: a presign issued
    // before the document was voided, or before an edit superseded the
    // version it points at, keeps working until its TTL (300s) runs out.
    // That is the deal S3 query-signing makes, not a defect here — revoking
    // an already-issued link would require proxying the download through
    // this service, and there is no such path in the system.
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
            const std::string url = s3->presign(*found->s3_key, "GET", kDownloadTtlSec);
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

    // -------------------------------------------------------------------
    // GET /api/v1/documents/{id}/versions — the document's history, oldest
    // first. A READ of the document, gated exactly like GET /documents/{id}
    // (the same per-row helper), because it is the same evidence seen from
    // a different angle. `input_snapshot` is NOT in the payload — see
    // Ledger::to_json(DocumentVersion).
    // -------------------------------------------------------------------
    void listVersions(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed document id"));
            return;
        }

        with_repo_errors(callback, "documents listVersions", [&] {
            Ledger::DocumentRepository repo;
            auto found = repo.find_in_org(id, ctx.org_id);
            if (!found) {
                callback(ErrorResponse::not_found("document"));
                return;
            }
            if (!ensure_document_access(callback, ctx, *found, Tenancy::OrgPerm::Action::kRead))
                return;
            json data = json::array();
            for (const auto& v : repo.list_versions(ctx.org_id, id))
                data.push_back(json(v));
            callback(Response::list(data));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/documents/{id}/versions — правка документа: новая
    // версия + новый рендер, предыдущий PDF остаётся. Тело — {input},
    // и это НЕ снапшот: принимается ровно тот же набор полей, что и при
    // создании документа этого шаблона (см. Docgen::InputPolicy), всё
    // остальное переносится из предыдущей версии. Приём снапшота целиком
    // воспроизвёл бы дыру подделки, закрытую в P2.
    // -------------------------------------------------------------------
    void createVersion(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed document id"));
            return;
        }
        // parse_optional_body, NOT parse_body (fix round 1): `contains()` is
        // false on a non-object json, so `{}`-guard-then-`body.value(...)`
        // sailed past the 400 below and threw nlohmann type_error.306 for a
        // body of `null`, `[]`, `"x"` or `5` — one line BEFORE
        // with_repo_errors, so nothing mapped it and the caller got a 500 for
        // a malformed request. This helper answers 400 `invalid_json` for a
        // present-but-non-object body and treats an ABSENT body as `{}`,
        // which is the honest reading here: "edit nothing" is a legitimate
        // request (a faithful re-render of the stored input), the same
        // posture the .../generate-document endpoints take.
        json body;
        if (!Validation::parse_optional_body(req, body, callback))
            return;
        if (body.contains("input") && !body["input"].is_object()) {
            Validation::Errors errs;
            errs.add("input", "not_object", "must be a JSON object");
            callback(Validation::response_400(errs));
            return;
        }
        const json client_input = body.value("input", json::object());

        with_repo_errors(callback, "documents createVersion", [&] {
            Ledger::DocumentRepository repo;
            auto found = repo.find_in_org(id, ctx.org_id);
            if (!found) {
                callback(ErrorResponse::not_found("document"));
                return;
            }
            // Хелпер задачи 7 — единственная точка, где решается, какой
            // ресурс матрицы §5.3 у этого документа.
            if (!ensure_document_access(callback, ctx, *found, Tenancy::OrgPerm::Action::kWrite))
                return;
            // Загруженные и пришедшие почтой документы не редактируются: у
            // них input_snapshot пуст по построению, редактировать нечего.
            if (found->source != "generated" || !found->template_slug || found->template_slug->empty()) {
                callback(
                    ErrorResponse::conflict("not_editable", "Only documents generated from a template can be edited"));
                return;
            }
            // Аннулированный документ не редактируется: новая версия и её
            // рендер вернули бы ему живой файл поверх пометки «недействителен»
            // — ровно то, что аннулирование обязано исключить. Проверка
            // появилась только сейчас, вместе с колонкой
            // (migrations/019_document_voiding.sql); её второй, серверный
            // близнец — ветка kVoided в DocumentRepository::version_render_state,
            // которая гасит уже стоящую в очереди джобу.
            if (found->voided_at) {
                callback(ErrorResponse::conflict("document_voided", "A voided document cannot be edited"));
                return;
            }
            const std::string slug = *found->template_slug;

            auto previous = repo.latest_version(ctx.org_id, id);
            if (!previous) {
                spdlog::error("documents createVersion: document {} has no versions", id);
                callback(ErrorResponse::internal_error());
                return;
            }

            json input;
            if (Docgen::InputPolicy::input_is_caller_authored(slug)) {
                // Первичка: весь объект авторский, ровно как в
                // POST /documents/generate — и та же деривация суммы.
                input = client_input;
                std::string bad_field, bad_code, bad_message;
                if (!Docgen::InputPolicy::apply_derived_amount(slug, input, bad_field, bad_code, bad_message)) {
                    callback(Validation::response_422(bad_field, bad_code, bad_message));
                    return;
                }
            } else {
                // Серверная форма: база — снапшот предыдущей версии,
                // сверху ложатся ТОЛЬКО allowlisted-ключи. Любой другой —
                // 422 not_allowed_override, тем же механизмом, что при
                // создании.
                input = previous->input_snapshot ? *previous->input_snapshot : json::object();
                if (!Validation::merge_allowed_extra(
                        input, client_input, Docgen::InputPolicy::editable_fields(slug), callback))
                    return;
            }

            Docgen::TemplateRegistry registry;
            auto info = registry.latest(slug);
            if (!info) {
                spdlog::error("documents createVersion: template '{}' not found on disk", slug);
                callback(ErrorResponse::internal_error());
                return;
            }
            if (auto err = Docgen::TemplateRegistry::validate(*info, input)) {
                callback(Validation::response_422("input", "schema_validation_failed", *err));
                return;
            }
            API_REQUIRE_JOBS_READY(callback);

            auto version = repo.add_version(ctx.org_id,
                                            id,
                                            std::optional<nlohmann::json>{input},
                                            std::optional<std::string>{info->version_str},
                                            std::optional<std::string>{ctx.user_id});

            // Best-effort enqueue — та же посадка, что у
            // DocgenController::generate: версия уже существует, и сбой
            // Redis не должен превращать её в 500 без номера, по которому
            // клиент мог бы опрашивать состояние.
            json payload = {{"org_id", ctx.org_id},
                            {"document_id", id},
                            {"version_id", version.id},
                            {"slug", slug},
                            {"input", input}};
            bool render_queued = false;
            try {
                auto job = Jobs::get().submit(kRenderJobType, payload);
                spdlog::debug("documents createVersion: version {} enqueued as job {}", version.id, job.id);
                render_queued = true;
            } catch (const std::exception& e) {
                spdlog::error(
                    "documents createVersion: enqueue docgen.render for version {} failed: {}", version.id, e.what());
            }
            const json response_body = {{"document_id", id},
                                        {"version_id", version.id},
                                        {"version_no", version.version_no},
                                        {"render_queued", render_queued}};
            callback(Response::accepted(response_body));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/documents/{id}/versions/{version_no}/download-url —
    // presigned GET for ONE historical version, TTL 300s. Same read gate,
    // same accepted TTL exposure as downloadUrl() above; the point of the
    // route is that superseding a document does not take its earlier
    // evidence away.
    // -------------------------------------------------------------------
    void versionDownloadUrl(const HttpRequestPtr& req,
                            std::function<void(const HttpResponsePtr&)>&& callback,
                            const std::string& id,
                            const std::string& version_no_param) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed document id"));
            return;
        }
        int version_no = 0;
        if (!parse_version_no(version_no_param, version_no)) {
            callback(ErrorResponse::bad_request("invalid_version", "Version number must be a positive integer"));
            return;
        }

        with_repo_errors(callback, "documents versionDownloadUrl", [&] {
            Ledger::DocumentRepository repo;
            auto found = repo.find_in_org(id, ctx.org_id);
            if (!found) {
                callback(ErrorResponse::not_found("document"));
                return;
            }
            if (!ensure_document_access(callback, ctx, *found, Tenancy::OrgPerm::Action::kRead))
                return;
            auto version = repo.find_version(ctx.org_id, id, version_no);
            if (!version) {
                callback(ErrorResponse::not_found("document_version"));
                return;
            }
            if (!version->s3_key || version->s3_key->empty()) {
                callback(ErrorResponse::conflict("no_file", "This version has no stored file yet"));
                return;
            }
            auto* s3 = s3_backend();
            if (!s3) {
                callback(ErrorResponse::service_unavailable("presign_unsupported",
                                                            "Presigned URLs require the S3 storage backend"));
                return;
            }
            const std::string url = s3->presign(*version->s3_key, "GET", kDownloadTtlSec);
            callback(Response::ok({{"url", url}}));
        });
    }

    // -------------------------------------------------------------------
    // DELETE /api/v1/documents/{id} — физическое удаление, и только для
    // документа, НЕ связанного с проведённой (или сторнированной)
    // проводкой. Ключ — связь, а не статус: 'draft' бывает только у
    // source='generated', и по статусу ошибочно загруженный скан не
    // удалился бы никогда, потому что он живёт в цикле inbox -> recognized
    // -> linked -> archived. Всё, что уже стало основанием проводки,
    // удалению не подлежит вовсе — журнал append-only, правится сторно, а
    // документ аннулируется (POST .../void ниже).
    //
    // Объекты в S3 при удалении НЕ трогаются — принятая политика хранения,
    // записанная в заголовке migrations/019_document_voiding.sql: удаляются
    // только метаданные, сборщика объектов в P3 нет.
    // -------------------------------------------------------------------
    void remove(const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback,
                const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed document id"));
            return;
        }

        with_repo_errors(callback, "documents remove", [&] {
            Ledger::DocumentRepository repo;
            auto found = repo.find_in_org(id, ctx.org_id);
            if (!found) {
                callback(ErrorResponse::not_found("document"));
                return;
            }
            // Тот же хелпер задачи 7, что и у остальных маршрутов над одним
            // документом: кадровый документ — ресурс hr_docs, вся прочая
            // первичка — documents. Своей копии условия по doc_type здесь
            // быть не должно.
            if (!ensure_document_access(callback, ctx, *found, Tenancy::OrgPerm::Action::kWrite))
                return;

            switch (repo.remove(ctx.org_id, id)) {
                case Ledger::DeleteOutcome::kDeleted: {
                    // Факт удаления пишется в аудит, а не только в лог: связь
                    // с ЧЕРНОВОЙ проводкой каскадится молча (document_entries
                    // — ON DELETE CASCADE), черновик остаётся без основания, и
                    // единственный след этого события живёт здесь.
                    Security::Audit::record(ctx.user_id,
                                            "document.delete",
                                            "document",
                                            id,
                                            {{"org_id", ctx.org_id}, {"doc_type", found->doc_type}});
                    // 204 строится вручную: в Response:: хелпера без тела
                    // нет, а заводить его ради одного вызова — лишняя
                    // публичная поверхность. Тела у ответа нет вовсе, так
                    // что setContentTypeCode не нужен.
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k204NoContent);
                    callback(resp);
                    return;
                }
                case Ledger::DeleteOutcome::kNotFound:
                    callback(ErrorResponse::not_found("document"));
                    return;
                case Ledger::DeleteOutcome::kHasPostedEntries:
                    callback(ErrorResponse::conflict(
                        "document_has_posted_entries",
                        "This document is linked to a posted journal entry — void it instead of deleting it"));
                    return;
                case Ledger::DeleteOutcome::kReferenced:
                    callback(ErrorResponse::conflict(
                        "document_referenced",
                        "This document is referenced by an HR order or a tax filing — void it instead of deleting it"));
                    return;
            }
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/documents/{id}/void — тело {reason}. Аннулирование: строка,
    // файл и вся история версий остаются на месте, документ лишь помечен
    // тремя колонками (voided_at/voided_by_user_id/void_reason). В `status`
    // ничего не пишется — иначе аудит потерял бы, был документ 'final' или
    // 'sent'. Повторное аннулирование — 409: важнее ПЕРВОЕ решение и его
    // автор, а не последнее.
    // -------------------------------------------------------------------
    void voidDocument(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed document id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        // Фаза 1 — структура (нет поля / не тот тип): 400. Та же двухфазная
        // разбивка, что у startUpload/confirmUpload выше.
        Validation::Errors errs;
        Validation::require(errs, body, "reason");
        if (body.contains("reason") && !body["reason"].is_string())
            errs.add("reason", "not_string", "must be a string");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        // Фаза 2 — значение: 422. Причина из одних пробелов — это НЕ
        // причина: аннулирование без объяснения превращает запись аудита в
        // пустую строку, а именно она и есть весь смысл этих колонок.
        const std::string reason = trimmed(body["reason"].get<std::string>());
        if (reason.empty()) {
            callback(Validation::response_422("reason", "blank", "must not be blank"));
            return;
        }

        with_repo_errors(callback, "documents voidDocument", [&] {
            Ledger::DocumentRepository repo;
            auto found = repo.find_in_org(id, ctx.org_id);
            if (!found) {
                callback(ErrorResponse::not_found("document"));
                return;
            }
            if (!ensure_document_access(callback, ctx, *found, Tenancy::OrgPerm::Action::kWrite))
                return;
            if (found->voided_at) {
                callback(ErrorResponse::conflict("already_voided", "This document is already voided"));
                return;
            }
            // Гонка «двое аннулируют одновременно» решается не этой
            // проверкой, а условием `voided_at IS NULL` в самом UPDATE:
            // проигравший получает false и тот же 409, что и выше.
            if (!repo.void_document(ctx.org_id, id, ctx.user_id, reason)) {
                callback(ErrorResponse::conflict("already_voided", "This document is already voided"));
                return;
            }
            Security::Audit::record(ctx.user_id, "document.void", "document", id, {{"reason", reason}});
            auto fresh = repo.find_in_org(id, ctx.org_id, /*from_primary=*/true);
            if (!fresh) {
                // Документ был только что прочитан и обновлён — исчезнуть он
                // мог только под конкурентным удалением, и это не 500.
                callback(ErrorResponse::not_found("document"));
                return;
            }
            callback(Response::ok({{"data", json(*fresh)}}));
        });
    }

private:
    /// Обрезать ASCII-пробелы по краям. Тела запросов здесь бывают на
    /// кириллице, поэтому режутся ТОЛЬКО пробельные ASCII-байты — они не
    /// могут оказаться продолжением многобайтового символа UTF-8 (у тех
    /// старший бит всегда выставлен).
    static std::string trimmed(const std::string& s) {
        const std::string::size_type a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            return {};
        const std::string::size_type b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    /// Parse the `{version_no}` path segment. Digits only, no sign, no
    /// whitespace, at most 9 of them (so std::stoi below cannot throw or
    /// overflow), and strictly positive — version numbers start at 1
    /// (migrations/018_document_versions.sql's CHECK). Anything else is a
    /// malformed REQUEST SHAPE, i.e. a 400, not a 422: "abc" is not a
    /// version number that happens to be wrong, it is not a version number.
    static bool parse_version_no(const std::string& s, int& out) {
        if (s.empty() || s.size() > 9)
            return false;
        for (const unsigned char c : s) {
            if (c < '0' || c > '9')
                return false;
        }
        const int value = std::stoi(s);
        if (value <= 0)
            return false;
        out = value;
        return true;
    }

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
    /// повторять условие у себя: сейчас их семь (get, downloadUrl, три
    /// маршрута версий из задачи 9 и remove/voidDocument из задачи 11), и
    /// семь копий одного условия разъехались бы при первой же правке
    /// матрицы. @p action здесь не только kRead: createVersion —
    /// запись, и та же функция решает, по какому ресурсу её проверять.
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
