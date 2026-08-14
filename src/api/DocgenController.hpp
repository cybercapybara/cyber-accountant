/**
 * @file DocgenController.hpp
 * @brief Template discovery + document-generation API (design spec §6.4 /
 *        Task 13). The actual rendering never happens on the request
 *        thread: `generate()` only validates, creates a `draft` document,
 *        and enqueues a `docgen.render` job (Docgen::RenderJob.hpp,
 *        src/worker_main.cpp) — the client polls
 *        `GET /api/v1/documents/{id}` (LedgerDocumentsController, Task 12)
 *        for the `final`/rendered result.
 *
 * Routes (all under /api/v1, every handler starts with
 * API_REQUIRE_ORG(req, callback, ctx)):
 *   GET  /api/v1/doc-templates      registry scan: {slug, version, schema}
 *                                    for every template on disk — read-only,
 *                                    no viewer gate.
 *   POST /api/v1/documents/generate  body {template_slug, input,
 *                                    counterparty_id?, link_entry_id?} ->
 *                                    202 {document_id}. Accountant/owner
 *                                    only.
 *
 * `docgen.render`'s job type string is duplicated here as `kRenderJobType`
 * rather than pulling in `docgen/RenderJob.hpp` for its `kJobType` constant:
 * that header is the WORKER-side pipeline (LaTeX compile, `<sys/wait.h>`,
 * temp-dir scratch space) — src/worker_main.cpp is its only other includer.
 * Bringing that machinery into the API server binary just for one string
 * constant would be a needless coupling; the payload SHAPE contract
 * (`{org_id, document_id, slug, input}`) is what actually has to match, and
 * it's documented on both sides. Same posture Webhooks.hpp/AccountEmails.hpp
 * take: the module that ENQUEUES a job type owns that type's string where it
 * doesn't already share a file with the module that PROCESSES it.
 *
 * template_slug -> doc_type mapping is the identity function: every
 * template slug on disk (invoice/avr/waybill/tax_invoice/reconciliation) is
 * spelled identically in migrations/010_documents.sql's `doc_type` CHECK
 * list, so `doc_type = template_slug` needs no lookup table — but slug is
 * still ALWAYS resolved through Docgen::TemplateRegistry::latest() first
 * (its own allowlist + on-disk existence check), never used as a doc_type
 * on a bare say-so, so an unregistered or path-traversal-shaped slug can
 * never reach DocumentRepository::create() at all.
 *
 * Cross-org reference decision (counterparty_id / link_entry_id, both
 * optional body fields): TREATED IDENTICALLY, both a 422 keyed to their own
 * field name, not a 404. Rationale: unlike a URL resource id (GET
 * /documents/{id}, where "not visible to this org" and "doesn't exist" are
 * deliberately indistinguishable — see LedgerDocumentsController), these
 * are fields INSIDE a create-like request body, the same role
 * counterparty_id plays inside a journal line
 * (Ledger::JournalService::ForeignCounterparty, itself documented as a 422
 * for exactly this reason). A wrong/foreign reference here is a validation
 * failure of THIS request, not "the resource you asked for doesn't exist"
 * — so this controller pre-checks both via a plain org-scoped read BEFORE
 * creating anything (no document, no job) for a request that would fail
 * anyway. This also closes a real gap: `documents.counterparty_id` is a
 * PLAIN FK (`REFERENCES counterparties(id)`, migrations/010_documents.sql)
 * with no composite `(id, org_id)` pinning the way `document_entries`' FKs
 * do — so without this pre-check, a foreign org's counterparty_id would
 * satisfy the FK silently and leak a cross-tenant reference into this org's
 * document. `link_entry_id`, by contrast, IS protected by
 * `document_entries`' composite FKs (a cross-org id can never satisfy both
 * at once — see DocumentRepository::link_entry()'s doc comment) but this
 * controller pre-checks it anyway, for the same "don't create a document
 * and a render job for a request that's going to fail" reason, and for
 * symmetry with counterparty_id's field-level 422.
 */

#pragma once

#include <functional>
#include <optional>
#include <string>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "docgen/TemplateRegistry.hpp"
#include "jobs/Jobs.hpp"
#include "ledger/CounterpartyRepository.hpp"
#include "ledger/DocumentRepository.hpp"
#include "ledger/JournalRepository.hpp"
#include "tenancy/OrgContext.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class DocgenController : public HttpController<DocgenController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(DocgenController::listTemplates, "/api/v1/doc-templates", Get);
    ADD_METHOD_TO(DocgenController::generate, "/api/v1/documents/generate", Post);
    METHOD_LIST_END

    /// Job type `docgen.render`'s worker (Docgen::RenderJob.hpp) expects —
    /// see file header for why the string is duplicated here instead of
    /// including that header.
    static constexpr const char* kRenderJobType = "docgen.render";

    // -------------------------------------------------------------------
    // GET /api/v1/doc-templates — registry scan. Read-only, no viewer gate.
    // -------------------------------------------------------------------
    void listTemplates(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        (void)ctx;  // no per-org data here — the guard only establishes that the caller has org access at all

        Docgen::TemplateRegistry registry;
        json data = json::array();
        for (const auto& info : registry.list())
            data.push_back({{"slug", info.slug}, {"version", info.version_str}, {"schema", info.schema}});
        callback(Response::list(data));
    }

    // -------------------------------------------------------------------
    // POST /api/v1/documents/generate — accountant/owner only. Body:
    // {template_slug, input, counterparty_id?, link_entry_id?}.
    // -------------------------------------------------------------------
    void generate(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (ctx.role == "viewer") {
            callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot generate documents"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "template_slug");
        if (body.contains("template_slug") && !body["template_slug"].is_string())
            errs.add("template_slug", "not_string", "must be a string");
        if (body.contains("input") && !body["input"].is_object())
            errs.add("input", "not_object", "must be a JSON object");
        if (body.contains("counterparty_id") && !body["counterparty_id"].is_null()) {
            if (!body["counterparty_id"].is_string() || !is_valid_uuid(body["counterparty_id"].get<std::string>()))
                errs.add("counterparty_id", "bad_format", "must be a uuid");
        }
        if (body.contains("link_entry_id") && !body["link_entry_id"].is_null()) {
            if (!body["link_entry_id"].is_string() || !is_valid_uuid(body["link_entry_id"].get<std::string>()))
                errs.add("link_entry_id", "bad_format", "must be a uuid");
        }
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        const std::string template_slug = body["template_slug"].get<std::string>();
        const json input = body.value("input", json::object());
        const std::optional<std::string> counterparty_id =
            (body.contains("counterparty_id") && !body["counterparty_id"].is_null())
                ? std::optional<std::string>(body["counterparty_id"].get<std::string>())
                : std::nullopt;
        const std::optional<std::string> link_entry_id =
            (body.contains("link_entry_id") && !body["link_entry_id"].is_null())
                ? std::optional<std::string>(body["link_entry_id"].get<std::string>())
                : std::nullopt;

        // Resolve + schema-validate the template BEFORE creating anything —
        // an unregistered/traversal-shaped slug and a schema mismatch both
        // surface as one 422 here, never reaching DocumentRepository.
        Docgen::TemplateRegistry registry;
        auto info = registry.latest(template_slug);
        if (!info) {
            callback(Validation::response_422(
                "template_slug", "unknown_template", "no template found for slug '" + template_slug + "'"));
            return;
        }
        if (auto err = Docgen::TemplateRegistry::validate(*info, input)) {
            callback(Validation::response_422("input", "schema_validation_failed", *err));
            return;
        }

        // Cross-org reference pre-checks — see file header for why both are
        // 422s (not 404s) and why both run BEFORE anything is created.
        if (counterparty_id) {
            Ledger::CounterpartyRepository counterparties;
            if (!counterparties.find_in_org(*counterparty_id, ctx.org_id)) {
                callback(Validation::response_422(
                    "counterparty_id", "foreign_counterparty", "counterparty does not belong to this organization"));
                return;
            }
        }
        if (link_entry_id) {
            Ledger::JournalRepository journal;
            if (!journal.find_in_org(*link_entry_id, ctx.org_id)) {
                callback(Validation::response_422(
                    "link_entry_id", "foreign_journal_entry", "journal entry does not belong to this organization"));
                return;
            }
        }

        // Infra readiness checked last: everything about THIS request is
        // already known to be valid at this point, so a 503 here means
        // "try again", never "fix your request".
        API_REQUIRE_JOBS_READY(callback);

        with_repo_errors(callback, "documents generate", [&] {
            Ledger::DocumentRepository documents;
            auto created = documents.create(ctx.org_id,
                                            template_slug,  // doc_type == slug — see file header
                                            "generated",
                                            "draft",
                                            counterparty_id,
                                            info->slug,
                                            info->version_str,
                                            input);
            if (link_entry_id && !documents.link_entry(ctx.org_id, created.id, *link_entry_id)) {
                // Pre-checked above; only a concurrent delete of the entry
                // between that check and here could land here. Best-effort:
                // the document and its render job still proceed unlinked
                // rather than discarding already-committed work over a race
                // this narrow.
                spdlog::warn("documents generate: link_entry({}, {}) failed after passing the pre-check",
                             created.id,
                             *link_entry_id);
            }

            json payload = {
                {"org_id", ctx.org_id}, {"document_id", created.id}, {"slug", template_slug}, {"input", input}};
            auto job = Jobs::get().submit(kRenderJobType, payload);
            spdlog::debug("documents generate: document {} enqueued as job {}", created.id, job.id);
            callback(Response::accepted({{"document_id", created.id}}));
        });
    }
};

}  // namespace Api
