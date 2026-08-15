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
 *                                    gated as `documents`/read.
 *   POST /api/v1/documents/generate  body {template_slug, input,
 *                                    counterparty_id?, link_entry_id?} ->
 *                                    202 {document_id}.
 *
 * RBAC: BOTH routes go through API_REQUIRE_ORG_PERM against the §5.3
 * permission matrix (Tenancy::OrgPerm), which DENIES BY DEFAULT — an unknown
 * role, resource or action is a 403, so a role added later cannot fail open
 * the way the old `ctx.role == "viewer"` denylist let it.
 * `documents/generate` needs `documents`/write; `doc-templates` needs
 * `documents`/read even though it touches no per-org row — the template
 * registry IS the shape of the primary documents a role cannot see, and "—"
 * in §5.3 means INVISIBLE, so the `hr` role gets a 403 rather than a catalog
 * of the invoice/АВР/накладная forms it may never generate or read.
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
 * P3: that identity is only SAFE for the five primary-document slugs, and
 * the templates on disk are not only those five. `payslip`, `fno_910`,
 * `fno_300`, `hr_order` and `labor_contract` all resolve through the
 * registry and used to pass straight through as a `doc_type` that
 * `documents_doc_type_check` rejects — an INSERT-time constraint violation
 * the caller saw as a 500 (the transaction rolls back, so no orphan row was
 * ever left behind). `generate()` now gates the slug against
 * Docgen::InputPolicy::generate_slugs() and answers 422
 * `unsupported_template`, naming the endpoint that owns that template.
 *
 * That gate runs AFTER the registry lookup, deliberately: `unknown_template`
 * ("no such slug anywhere") and `unsupported_template` ("real template, wrong
 * endpoint") are different answers to the caller — a typo versus a wrong
 * address — and collapsing them into one code would make diagnosis worse.
 * The ordering is not a security property: the 500 came from the doc_type
 * CHECK at INSERT time, not from the registry, and nothing between the two
 * checks touches the database.
 *
 * P3, money: every printed money string on these documents is derived by
 * the SERVER from an integer tiyn field (`total_tiyn` for invoice/avr/
 * waybill, `totals.{amount,vat,with_vat}_tiyn` for tax_invoice) via
 * Docgen::InputPolicy::apply_derived_amount(), and a client-supplied
 * `total`/`total_words`/`totals.*` string is a 422 `not_allowed_override`.
 * Honest scope: this makes the document's TOTAL line self-consistent (and,
 * for a счёт-фактура, forces amount + vat == with_vat). The per-line
 * `items[]` figures stay free-form and are NOT reconciled against that
 * total — that is separate work, outside P3.
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
 *
 * Fix round 1 (best-effort enqueue): Jobs::get().submit() runs AFTER the
 * document row already exists in the same way link_entry()'s own race note
 * two paragraphs up does — one side effect already committed, only the
 * enqueue itself can still fail. Wrapped in its own try/catch, the same
 * idiom AccountEmails.hpp::dispatch() / Webhooks.hpp::send() already use
 * for exactly this shape: a transient Redis blip must not turn an
 * already-real document into a 500 with no document_id for the client to
 * poll (a naive retry on 500 would just mint another orphaned draft). On
 * failure this logs spdlog::error with the document_id (an operator needs
 * to find and re-enqueue it — there is no automatic fallback path here,
 * unlike AccountEmails' inline-send fallback, because there is no
 * synchronous rendering path to fall back to) and still answers 202 with
 * render_queued: false in the body, so the client can tell "the document
 * exists but nothing is rendering it yet" apart from a real success.
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
#include "docgen/InputPolicy.hpp"
#include "docgen/TemplateRegistry.hpp"
#include "jobs/Jobs.hpp"
#include "ledger/CounterpartyRepository.hpp"
#include "ledger/DocumentRepository.hpp"
#include "ledger/JournalRepository.hpp"
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrgPermissions.hpp"
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
    // GET /api/v1/doc-templates — registry scan. Reads no per-org row, but
    // still requires `documents`/read: see the RBAC note in this file's
    // header for why the catalog itself is part of what "—" hides.
    // -------------------------------------------------------------------
    void listTemplates(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kDocuments, Tenancy::OrgPerm::Action::kRead);

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
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kDocuments, Tenancy::OrgPerm::Action::kWrite);
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
        const json client_input = body.value("input", json::object());
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
        //
        // Порядок двух проверок значащий и НЕ переставляется: сначала
        // «существует ли такой шаблон вообще», потом «порождается ли он
        // этим эндпоинтом». Это два разных ответа каллеру — опечатка в
        // слаге и обращение не по адресу, — и слив их в один код сделал бы
        // диагностику хуже. Дыры это не открывает: 500 приходил не из
        // реестра, а из documents_doc_type_check на INSERT (см. ниже).
        Docgen::TemplateRegistry registry;
        auto info = registry.latest(template_slug);
        if (!info) {
            callback(Validation::response_422(
                "template_slug", "unknown_template", "no template found for slug '" + template_slug + "'"));
            return;
        }

        // Шаблон существует, но первичкой не является: слаг идёт в
        // documents.doc_type дословно (см. шапку файла), а
        // migrations/010_documents.sql разрешает там только пять значений.
        // Без этой проверки payslip/fno_910/fno_300/hr_order/labor_contract
        // проходили дальше и валились на documents_doc_type_check уже внутри
        // INSERT — клиент получал 500 вместо внятного 422 (дефект, найденный
        // при релизе v0.3.1). У каждого из них есть свой эндпоинт-владелец,
        // и сообщение называет именно его, а не «какой-то другой».
        if (!Docgen::InputPolicy::input_is_caller_authored(template_slug)) {
            // Пустая строка возможна только у шаблона, который положили на
            // диск, но ни к какому эндпоинту не приписали, — тогда честнее
            // сказать «владельца нет», чем назвать пустой маршрут.
            const std::string owner = Docgen::InputPolicy::owning_endpoint(template_slug);
            const std::string where = owner.empty()
                                          ? std::string("no endpoint generates it")
                                          : std::string("use ") + owner + ", which holds the authoritative data for it";
            callback(Validation::response_422(
                "template_slug",
                "unsupported_template",
                "template '" + template_slug + "' is not generated through this endpoint — " + where));
            return;
        }

        // Прописи и строковая сумма выводятся сервером из целого числа
        // тиын и подставляются в `input` ДО schema-валидации: у этих
        // шаблонов allowlist'а нет, весь объект приходит от клиента, и без
        // деривации цифра и текст в одном документе могли разойтись.
        // Гарантия ровно про ИТОГОВЫЕ строки — цифры позиций items[]
        // остаются свободным текстом и с итогом не сверяются.
        json input = client_input;
        {
            std::string bad_field, bad_code, bad_message;
            if (!Docgen::InputPolicy::apply_derived_amount(template_slug, input, bad_field, bad_code, bad_message)) {
                callback(Validation::response_422(bad_field, bad_code, bad_message));
                return;
            }
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
            // doc_type == slug is safe here because the slug gate above
            // already restricted it to the five values documents_doc_type_check
            // accepts (P3 — that check used to be the only thing catching a
            // non-primary slug, and it did so as a 500).
            // input_snapshot is std::optional<nlohmann::json> — wrapped
            // explicitly (not passed as a bare `input`) because nlohmann::json's
            // own greedy converting-constructor template makes the implicit
            // json -> optional<json> conversion ambiguous under GCC (Fix
            // round 2: this failed -Werror in CI, unlike the single-element
            // json{...} temporaries LedgerDocumentsController passes for the
            // same parameter — a NAMED json lvalue is what triggers it).
            auto created = documents.create(ctx.org_id,
                                            template_slug,  // doc_type == slug — see file header
                                            "generated",
                                            "draft",
                                            counterparty_id,
                                            info->slug,
                                            info->version_str,
                                            std::optional<nlohmann::json>{input});
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

            // Best-effort enqueue — see file header's "Fix round 1" note.
            // The document already exists; a submit() failure must not turn
            // it into a 500 the client would just retry into another
            // orphaned draft.
            json payload = {
                {"org_id", ctx.org_id}, {"document_id", created.id}, {"slug", template_slug}, {"input", input}};
            bool render_queued = false;
            try {
                auto job = Jobs::get().submit(kRenderJobType, payload);
                spdlog::debug("documents generate: document {} enqueued as job {}", created.id, job.id);
                render_queued = true;
            } catch (const std::exception& e) {
                spdlog::error(
                    "documents generate: enqueue docgen.render for document {} failed: {}", created.id, e.what());
            }
            const json response_body = {{"document_id", created.id}, {"render_queued", render_queued}};
            callback(Response::accepted(response_body));
        });
    }
};

}  // namespace Api
