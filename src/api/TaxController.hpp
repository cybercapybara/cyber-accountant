/**
 * @file TaxController.hpp
 * @brief Tax REST surface (design spec §7.1/§7.2, Task 12): the reference
 *        tables a UI needs for hints, the two persisted calculations
 *        (Tax::TaxService), the threshold alerts and deadline calendar, and
 *        the ФНО filing pipeline (XML into object storage + a printable form
 *        through docgen).
 *
 * Routes (all under /api/v1, every handler starts with
 * API_REQUIRE_ORG(req, callback, ctx)):
 *   GET  /api/v1/tax/rates                    ?on=YYYY-MM-DD — rates AND
 *                                               constants in force on that
 *                                               date (UI hints)
 *   POST /api/v1/tax/calculations             {kind, period_from, period_to}
 *                                               -> the calculation with both
 *                                               reproducibility snapshots
 *   GET  /api/v1/tax/calculations             ?kind=&year=, paginated
 *   GET  /api/v1/tax/alerts                   ?on= — registration-threshold
 *                                               warnings
 *   GET  /api/v1/tax/deadlines                ?on=&horizon_days=90
 *   POST /api/v1/tax/filings                  {kind, calculation_id,
 *                                               document_input?} -> 202
 *                                               {filing_id, xml_ready,
 *                                               render_queued}
 *   GET  /api/v1/tax/filings                  ?kind=, paginated
 *   GET  /api/v1/tax/filings/{id}
 *   POST /api/v1/tax/filings/{id}/download-url  ?artifact=xml|pdf ->
 *                                               presigned GET, TTL 300s
 *
 * RBAC: `POST /tax/calculations` and `POST /tax/filings` reject
 * `ctx.role == "viewer"` with 403 — both write. `download-url` does NOT (it
 * mints a URL and writes nothing), exactly like
 * LedgerDocumentsController::downloadUrl. `org_id` comes EXCLUSIVELY from
 * `ctx.org_id`.
 *
 * `GET /tax/rates` and `GET /tax/deadlines` return SYSTEM data — `tax_rates`/
 * `tax_constants` and `tax_deadlines` have no org_id at all (the documented
 * exceptions #2 and #3 to org-scoping; see TaxReferenceRepository.hpp /
 * TaxCalendar.hpp). They still sit behind API_REQUIRE_ORG: the guard
 * establishes that the caller is a member of SOME organization at all, the
 * same posture DocgenController::listTemplates takes for the equally
 * org-independent template registry.
 *
 * `POST /tax/calculations` answers 200, not 201: TaxService's whole contract
 * is that recalculating the same (org, kind, period) REPLACES the stored row
 * rather than adding one (TaxCalculationRepository::upsert), so a second call
 * creates nothing. Same reasoning as PayrollController's own POST.
 *
 * Error semantics (deliberate, per route):
 *   - 400 — malformed SHAPE: a non-uuid path id, a missing/wrong-typed body
 *     field.
 *   - 422 — well-formed but semantically invalid VALUE: an unlisted `kind`,
 *     a non-calendar `period_from`/`period_to`/`?on`, `period_to` before
 *     `period_from`, a `?year`/`?artifact` value outside its allowlist, a
 *     `calculation_id` belonging to another organization (a body-field
 *     reference, so 422 `foreign_calculation` rather than 404 — the same
 *     line DocgenController draws for counterparty_id/link_entry_id), a
 *     `kind` that disagrees with the referenced calculation's own kind, and
 *     a `document_input` that fails the ФНО template's JSON Schema.
 *   - 409 — STATE conflict: asking for a download URL for an artifact this
 *     filing does not have yet (no XML, no printable document, or a document
 *     whose render has not produced a file).
 *   - 404 — no such calculation/filing visible to this org.
 *   - 503 — object storage or the job queue is not available.
 *
 * `Tax::MissingTaxReference` (no rate/constant in force on the queried date)
 * is mapped to 422 on the offending field rather than left to
 * with_repo_errors' 500: for `POST /tax/calculations` and `GET /tax/alerts`
 * the caller chose the date, so "there is no СНР rate in force on
 * 2019-06-30" is a bad VALUE, not a server fault. Note the contrast with
 * PayrollController, where the equivalent condition arrives as an
 * undifferentiated std::runtime_error from PayrollService and stays a 500 —
 * TaxService is the one that gives it a distinguishable type.
 *
 * ── Filing pipeline (`POST /tax/filings`) ──────────────────────────────────
 * `kind` is a FORM CODE ("910.00"/"300.00"), a different vocabulary from the
 * calculation's kind ('snr_simplified'/'vat') — see migrations/016's header.
 * The handler enforces the pairing itself (422 `kind_mismatch`) so a НДС
 * calculation can never be rendered as a СНР declaration.
 *
 * Ordering is deliberate: everything that can reject the request runs FIRST
 * (guards, allowlists, the calculation lookup, the template's JSON Schema
 * over the merged document input), and only then are side effects performed
 * — XML into storage, then the document row, then the filing row, then the
 * render job. A caller that forgot `director` therefore leaves no orphaned
 * object in S3 and no orphaned document row behind.
 *
 * `xml_ready` and `render_queued` are both BEST-EFFORT flags, and both can
 * come back false in a 202:
 *   - a Storage::put failure leaves the filing in status 'draft' with a NULL
 *     xml_s3_key and `xml_ready: false` — the filing row is still the durable
 *     record that this ФНО was requested, and re-POSTing produces a fresh
 *     one (there is no UNIQUE key to collide with; see migration 016);
 *   - an enqueue failure leaves `render_queued: false` with the document row
 *     already created, exactly the posture DocgenController documents.
 *
 * `schema_validated` is persisted as false for every filing: KGD publishes no
 * content XSD for ANY ФНО form (see src/tax/FnoXml.hpp's Step-1 finding), so
 * there is nothing to validate the generated XML against. It is stored per
 * row rather than derived from `kind` so that, if an XSD ever appears, the
 * flag stays attached to the file that was actually produced.
 *
 * The printable form goes through the SAME base-input + optional-body-merge
 * design HrController documents: templates/latex/fno_910/v1/schema.json and
 * fno_300/v1/schema.json both require free-text fields this codebase has no
 * column for — `director`, `accountant`, and the amount spelled out in words
 * (`tax_words`/`balance_words`; there is no money-to-words converter
 * anywhere here). fno_300 additionally requires `sales_tenge`, the revenue
 * TURNOVER behind the VAT: Tax::TaxService::calculate_vat sums `vat_amount`
 * only and never records the underlying base, so that figure genuinely is
 * not on file either. All of those must arrive in `document_input`, which is
 * deep-merged (RFC 7396 `merge_patch`) over the derived base before the
 * schema check; omitting one yields the same `422 schema_validation_failed`
 * DocgenController already produces.
 *
 * Money in that input is rendered with Ledger::format_tiyn — and for a НДС
 * refund position (`balance_tiyn < 0`, a normal outcome per
 * TaxService::calculate_vat) the ABSOLUTE value is formatted and the sign is
 * carried by the template's own `balance_kind` enum ("to_pay"/"to_refund"),
 * because format_tiyn rejects negatives by contract.
 */

#pragma once

#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
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
#include "docgen/TemplateRegistry.hpp"
#include "files/FileKeys.hpp"
#include "jobs/Jobs.hpp"
#include "ledger/Document.hpp"
#include "ledger/DocumentRepository.hpp"
#include "ledger/JournalService.hpp"
#include "storage/Storage.hpp"
#include "tax/Fno300.hpp"
#include "tax/Fno910.hpp"
#include "tax/FnoXml.hpp"
#include "tax/TaxCalculation.hpp"
#include "tax/TaxCalculationRepository.hpp"
#include "tax/TaxCalendar.hpp"
#include "tax/TaxFiling.hpp"
#include "tax/TaxFilingRepository.hpp"
#include "tax/TaxRate.hpp"
#include "tax/TaxReferenceRepository.hpp"
#include "tax/TaxService.hpp"
#include "tenancy/OrgContext.hpp"
#include "tenancy/Organization.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class TaxController : public HttpController<TaxController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(TaxController::listRates, "/api/v1/tax/rates", Get);
    ADD_METHOD_TO(TaxController::createCalculation, "/api/v1/tax/calculations", Post);
    ADD_METHOD_TO(TaxController::listCalculations, "/api/v1/tax/calculations", Get);
    ADD_METHOD_TO(TaxController::listAlerts, "/api/v1/tax/alerts", Get);
    ADD_METHOD_TO(TaxController::listDeadlines, "/api/v1/tax/deadlines", Get);
    ADD_METHOD_TO(TaxController::createFiling, "/api/v1/tax/filings", Post);
    ADD_METHOD_TO(TaxController::listFilings, "/api/v1/tax/filings", Get);
    ADD_METHOD_TO(TaxController::getFiling, "/api/v1/tax/filings/{1}", Get);
    ADD_METHOD_TO(TaxController::filingDownloadUrl, "/api/v1/tax/filings/{1}/download-url", Post);
    METHOD_LIST_END

    /// Same job type DocgenController's `POST /documents/generate` enqueues.
    static constexpr const char* kRenderJobType = "docgen.render";

    /// Presigned-URL lifetimes, matching LedgerDocumentsController's own
    /// download-url (300s is long enough for a browser to start the transfer,
    /// short enough that a leaked URL expires quickly).
    static constexpr long kDownloadTtlSec = 300;

    /// No content XSD exists for ANY ФНО form — see this file's header and
    /// src/tax/FnoXml.hpp. Named here rather than read off one generator's
    /// own constant so both form kinds record the same, correct value.
    static constexpr bool kSchemaValidated = false;

    static const std::vector<std::string>& allowed_calculation_kinds() {
        static const std::vector<std::string> v = {Tax::CalculationKind::kSnrSimplified, Tax::CalculationKind::kVat};
        return v;
    }

    static const std::vector<std::string>& allowed_filing_kinds() {
        static const std::vector<std::string> v = {Tax::FilingKind::kFno910, Tax::FilingKind::kFno300};
        return v;
    }

    // ===================================================================
    // Reference data
    // ===================================================================

    // -------------------------------------------------------------------
    // GET /api/v1/tax/rates — every rate AND constant in force on ?on
    // (default: today). System data, no org filter — see file header.
    // -------------------------------------------------------------------
    void listRates(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        (void)ctx;  // system-wide reference data; the guard only proves org access
        std::string on;
        if (!parse_on_date(req, on, callback))
            return;

        with_repo_errors(callback, "tax rates list", [&] {
            Tax::TaxReferenceRepository repo;
            json rates = json::array();
            for (const auto& r : repo.list_rates_on(on))
                rates.push_back(r);
            json constants = json::array();
            for (const auto& c : repo.list_constants_on(on))
                constants.push_back(c);
            // Built as a named object first: a doubly-nested brace-init
            // straight into Response::ok's `const json&` is exactly the shape
            // nlohmann's initializer-list constructor resolves ambiguously.
            const json payload = {{"on", on}, {"rates", rates}, {"constants", constants}};
            callback(Response::ok({{"data", payload}}));
        });
    }

    // -------------------------------------------------------------------
    // GET /api/v1/tax/deadlines — ?on (default today), ?horizon_days
    // (default 90, clamped to 1..3650). System data — see file header.
    // -------------------------------------------------------------------
    void listDeadlines(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        (void)ctx;
        std::string on;
        if (!parse_on_date(req, on, callback))
            return;
        // Clamped rather than 422'd: horizon_days is a VIEW WIDTH, not a
        // domain value — an out-of-range window has an obvious, harmless
        // interpretation (the widest/narrowest supported), the same
        // semantics parse_page_params gives limit/offset everywhere else.
        const int horizon_days = clamp_int(req->getParameter("horizon_days"), 90, 1, 3650);

        with_repo_errors(callback, "tax deadlines", [&] {
            Tax::TaxCalendar calendar;
            json data = json::array();
            for (const auto& d : calendar.upcoming(on, horizon_days))
                data.push_back(d);
            callback(Response::list(data));
        });
    }

    // -------------------------------------------------------------------
    // GET /api/v1/tax/alerts — registration-threshold warnings for THIS org
    // as of ?on (default today). Org-scoped (it reads the org's own posted
    // income), unlike rates/deadlines above.
    // -------------------------------------------------------------------
    void listAlerts(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        std::string on;
        if (!parse_on_date(req, on, callback))
            return;

        with_repo_errors(callback, "tax alerts", [&] {
            Tax::TaxService svc;
            std::vector<Tax::Alert> alerts;
            try {
                alerts = svc.threshold_alerts(ctx.org_id, on);
            } catch (const Tax::MissingTaxReference& e) {
                callback(Validation::response_422("on", "missing_tax_reference", e.what()));
                return;
            }
            json data = json::array();
            for (const auto& a : alerts)
                data.push_back(a);
            callback(Response::list(data));
        });
    }

    // ===================================================================
    // Calculations
    // ===================================================================

    // -------------------------------------------------------------------
    // POST /api/v1/tax/calculations — body {kind, period_from, period_to}.
    // Recalculating the same period REPLACES its row, so this is a 200, not
    // a 201 (see file header). Accountant/owner only.
    // -------------------------------------------------------------------
    void createCalculation(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (ctx.role == "viewer") {
            callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot run tax calculations"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        // Phase 1 — structural (missing / wrong-type): 400.
        Validation::Errors errs;
        Validation::require(errs, body, "kind");
        Validation::require(errs, body, "period_from");
        Validation::require(errs, body, "period_to");
        if (body.contains("kind") && !body["kind"].is_string())
            errs.add("kind", "not_string", "must be a string");
        if (body.contains("period_from") && !body["period_from"].is_string())
            errs.add("period_from", "not_string", "must be a string");
        if (body.contains("period_to") && !body["period_to"].is_string())
            errs.add("period_to", "not_string", "must be a string");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        // Phase 2 — semantic: 422. Every field below is already present and
        // string-typed, so one_of()'s internal not_string branch is dead here.
        const std::string kind = body["kind"].get<std::string>();
        const std::string period_from = body["period_from"].get<std::string>();
        const std::string period_to = body["period_to"].get<std::string>();
        Validation::Errors semantic_errs;
        Validation::one_of(semantic_errs, body, "kind", allowed_calculation_kinds());
        if (!Validation::is_valid_date(period_from))
            semantic_errs.add("period_from", "invalid_date", "must be a calendar-valid YYYY-MM-DD date");
        if (!Validation::is_valid_date(period_to))
            semantic_errs.add("period_to", "invalid_date", "must be a calendar-valid YYYY-MM-DD date");
        // ISO YYYY-MM-DD strings order lexicographically the same as
        // chronologically — checked here (422) so migration 014's
        // CHECK (period_to >= period_from) never surfaces as a raw 23514/500.
        // Only meaningful once both dates parsed, hence the guard.
        if (Validation::is_valid_date(period_from) && Validation::is_valid_date(period_to) && period_to < period_from)
            semantic_errs.add("period_to", "before_period_from", "period_to must be on or after period_from");
        if (semantic_errs.any()) {
            callback(Validation::response_422(semantic_errs));
            return;
        }

        with_repo_errors(callback, "tax calculations create", [&] {
            Tax::TaxService svc;
            try {
                const Tax::Calculation calc = (kind == Tax::CalculationKind::kSnrSimplified)
                                                  ? svc.calculate_snr(ctx.org_id, period_from, period_to)
                                                  : svc.calculate_vat(ctx.org_id, period_from, period_to);
                callback(Response::ok({{"data", json(calc)}}));
            } catch (const Tax::MissingTaxReference& e) {
                // The caller picked a period with no rate in force — a bad
                // VALUE, not a server fault. See file header.
                callback(Validation::response_422("period_to", "missing_tax_reference", e.what()));
            }
        });
    }

    // -------------------------------------------------------------------
    // GET /api/v1/tax/calculations — paginated, optional ?kind and ?year
    // filters (both allowlisted/validated; an unlisted value is a 422, never
    // a silently-ignored filter — same posture as
    // LedgerDocumentsController::list).
    // -------------------------------------------------------------------
    void listCalculations(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);

        Validation::Errors errs;
        std::optional<std::string> kind_filter;
        std::optional<std::string> year_filter;
        const std::string kind_param = req->getParameter("kind");
        const std::string year_param = req->getParameter("year");
        if (!kind_param.empty()) {
            if (!is_allowed(kind_param, allowed_calculation_kinds()))
                errs.add("kind", "not_allowed", "must be one of: snr_simplified, vat");
            else
                kind_filter = kind_param;
        }
        if (!year_param.empty()) {
            if (!is_year(year_param))
                errs.add("year", "not_integer", "must be a 4-digit calendar year");
            else
                year_filter = year_param;
        }
        if (errs.any()) {
            callback(Validation::response_422(errs));
            return;
        }

        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);
        with_repo_errors(callback, "tax calculations list", [&] {
            Tax::TaxCalculationRepository repo;
            auto rows = repo.list_filtered(ctx.org_id, kind_filter, year_filter, page.limit, page.offset);
            long total = repo.count_filtered(ctx.org_id, kind_filter, year_filter);
            json data = json::array();
            for (const auto& c : rows)
                data.push_back(c);
            callback(Response::paginated(data, total, page.limit, page.offset));
        });
    }

    // ===================================================================
    // Filings
    // ===================================================================

    // -------------------------------------------------------------------
    // POST /api/v1/tax/filings — body {kind, calculation_id,
    // document_input?}. See file header for the full ordering and the
    // meaning of xml_ready/render_queued. Accountant/owner only.
    // -------------------------------------------------------------------
    void createFiling(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (ctx.role == "viewer") {
            callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot generate tax filings"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        // Phase 1 — structural: 400.
        Validation::Errors errs;
        Validation::require(errs, body, "kind");
        Validation::require(errs, body, "calculation_id");
        if (body.contains("kind") && !body["kind"].is_string())
            errs.add("kind", "not_string", "must be a string");
        if (body.contains("calculation_id") &&
            (!body["calculation_id"].is_string() || !is_valid_uuid(body["calculation_id"].get<std::string>())))
            errs.add("calculation_id", "bad_format", "must be a uuid");
        if (body.contains("document_input") && !body["document_input"].is_null() && !body["document_input"].is_object())
            errs.add("document_input", "not_object", "must be a JSON object");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        // Phase 2 — semantic: 422.
        const std::string kind = body["kind"].get<std::string>();
        if (!is_allowed(kind, allowed_filing_kinds())) {
            callback(Validation::response_422("kind", "not_allowed", "must be one of: 910.00, 300.00"));
            return;
        }
        const std::string calculation_id = body["calculation_id"].get<std::string>();
        json extra = json::object();
        if (body.contains("document_input") && !body["document_input"].is_null())
            extra = body["document_input"];

        Tax::TaxCalculationRepository calculations;
        auto calc = calculations.find_in_org(calculation_id, ctx.org_id, /*from_primary=*/true);
        if (!calc) {
            // A body-field reference, not a URL resource — 422 keyed to the
            // field, matching DocgenController's counterparty_id posture.
            callback(Validation::response_422(
                "calculation_id", "foreign_calculation", "calculation does not belong to this organization"));
            return;
        }
        if (calc->kind != expected_calculation_kind(kind)) {
            callback(Validation::response_422("kind",
                                              "kind_mismatch",
                                              "form " + kind + " is filed from a '" +
                                                  std::string(expected_calculation_kind(kind)) +
                                                  "' calculation, but this one is '" + calc->kind + "'"));
            return;
        }

        Tenancy::OrganizationRepository orgs;
        auto org = orgs.find(ctx.org_id);
        if (!org) {
            spdlog::error("tax filings: organization {} missing", ctx.org_id);
            callback(ErrorResponse::internal_error());
            return;
        }

        const std::string slug = (kind == Tax::FilingKind::kFno910) ? "fno_910" : "fno_300";
        json input = build_form_input(kind, *calc, *org);
        input.merge_patch(extra);

        Docgen::TemplateRegistry registry;
        auto info = registry.latest(slug);
        if (!info) {
            spdlog::error("tax filings: template '{}' not found on disk", slug);
            callback(ErrorResponse::internal_error());
            return;
        }
        if (auto err = Docgen::TemplateRegistry::validate(*info, input)) {
            callback(Validation::response_422("document_input", "schema_validation_failed", *err));
            return;
        }

        if (!Storage::is_initialized()) {
            callback(ErrorResponse::service_unavailable("storage_unavailable", "Object storage is not configured"));
            return;
        }
        API_REQUIRE_JOBS_READY(callback);

        // Everything that can reject the request has run — side effects start
        // here (see file header for the ordering rationale).
        const std::string xml = build_form_xml(kind, *calc, *org);
        const std::string xml_key = Files::org_key(
            ctx.org_id,
            "generated",
            "fno_" + form_slug_fragment(kind) + "_" + calc->period_from + "_" + calc->period_to + ".xml");
        bool xml_ready = false;
        try {
            Storage::get().put(xml_key, xml, "application/xml");
            xml_ready = true;
        } catch (const std::exception& e) {
            // Best-effort: the filing row is still created (status 'draft',
            // no xml key) so the attempt is recorded rather than vanishing.
            spdlog::error(
                "tax filings: storing XML for calculation {} at '{}' failed: {}", calculation_id, xml_key, e.what());
        }

        with_repo_errors(callback, "tax filings create", [&] {
            Ledger::DocumentRepository documents;
            // doc_type "fno" IS in migrations/010_documents.sql's CHECK list,
            // so unlike the HR templates this one keeps DocgenController's
            // doc_type/template_slug distinction explicit but harmless:
            // the slug (fno_910/fno_300) stays in template_slug.
            auto document = documents.create(ctx.org_id,
                                             "fno",
                                             "generated",
                                             "draft",
                                             std::nullopt,
                                             info->slug,
                                             info->version_str,
                                             std::optional<nlohmann::json>{input});

            // Both branches of the key ternary are spelled as the SAME
            // std::optional<std::string> type — `... : std::nullopt` has no
            // common type with std::optional<std::string> and would not
            // compile.
            const std::optional<std::string> stored_key =
                xml_ready ? std::optional<std::string>(xml_key) : std::optional<std::string>{};
            Tax::TaxFilingRepository filings;
            auto filing = filings.create(ctx.org_id,
                                         kind,
                                         calc->period_from,
                                         calc->period_to,
                                         xml_ready ? Tax::FilingStatus::kGenerated : Tax::FilingStatus::kDraft,
                                         calculation_id,
                                         stored_key,
                                         std::optional<std::string>(document.id),
                                         kSchemaValidated);

            json payload = {{"org_id", ctx.org_id}, {"document_id", document.id}, {"slug", slug}, {"input", input}};
            bool render_queued = false;
            try {
                auto job = Jobs::get().submit(kRenderJobType, payload);
                spdlog::debug("tax filings: document {} enqueued as job {}", document.id, job.id);
                render_queued = true;
            } catch (const std::exception& e) {
                spdlog::error("tax filings: enqueue docgen.render for document {} failed: {}", document.id, e.what());
            }
            const json response_body = {
                {"filing_id", filing.id}, {"xml_ready", xml_ready}, {"render_queued", render_queued}};
            callback(Response::accepted(response_body));
        });
    }

    // -------------------------------------------------------------------
    // GET /api/v1/tax/filings — paginated, optional ?kind filter.
    // -------------------------------------------------------------------
    void listFilings(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);

        std::optional<std::string> kind_filter;
        const std::string kind_param = req->getParameter("kind");
        if (!kind_param.empty()) {
            if (!is_allowed(kind_param, allowed_filing_kinds())) {
                callback(Validation::response_422("kind", "not_allowed", "must be one of: 910.00, 300.00"));
                return;
            }
            kind_filter = kind_param;
        }

        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);
        with_repo_errors(callback, "tax filings list", [&] {
            Tax::TaxFilingRepository repo;
            auto rows = repo.list_filtered(ctx.org_id, kind_filter, page.limit, page.offset);
            long total = repo.count_filtered(ctx.org_id, kind_filter);
            json data = json::array();
            for (const auto& f : rows)
                data.push_back(f);
            callback(Response::paginated(data, total, page.limit, page.offset));
        });
    }

    // -------------------------------------------------------------------
    // GET /api/v1/tax/filings/{id}
    // -------------------------------------------------------------------
    void getFiling(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed filing id"));
            return;
        }

        with_repo_errors(callback, "tax filings get", [&] {
            Tax::TaxFilingRepository repo;
            auto found = repo.find_in_org(id, ctx.org_id);
            if (!found) {
                callback(ErrorResponse::not_found("tax filing"));
                return;
            }
            callback(Response::ok({{"data", json(*found)}}));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/tax/filings/{id}/download-url — ?artifact=xml|pdf
    // (default xml). Read-only (mints a URL, writes nothing) — no viewer
    // gate, same as POST /documents/{id}/download-url.
    //
    // The two artifacts live in DIFFERENT places and therefore yield
    // different keys: the XML is this filing's own `xml_s3_key` (written by
    // createFiling), the PDF is the linked document's `s3_key` (written by
    // the docgen.render worker once it finishes). Asking for one that does
    // not exist yet is a 409, not a 404 — the filing itself is right there.
    // -------------------------------------------------------------------
    void filingDownloadUrl(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& callback,
                           const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed filing id"));
            return;
        }
        const std::string artifact_param = req->getParameter("artifact");
        const std::string artifact = artifact_param.empty() ? std::string("xml") : artifact_param;
        if (artifact != "xml" && artifact != "pdf") {
            callback(Validation::response_422("artifact", "not_allowed", "must be one of: xml, pdf"));
            return;
        }

        with_repo_errors(callback, "tax filings downloadUrl", [&] {
            Tax::TaxFilingRepository filings;
            auto filing = filings.find_in_org(id, ctx.org_id);
            if (!filing) {
                callback(ErrorResponse::not_found("tax filing"));
                return;
            }

            std::string key;
            if (artifact == "xml") {
                if (!filing->xml_s3_key || filing->xml_s3_key->empty()) {
                    callback(ErrorResponse::conflict("no_xml", "This filing has no stored XML artifact"));
                    return;
                }
                key = *filing->xml_s3_key;
            } else {
                if (!filing->document_id) {
                    callback(ErrorResponse::conflict("no_document", "This filing has no printable document"));
                    return;
                }
                Ledger::DocumentRepository documents;
                auto document = documents.find_in_org(*filing->document_id, ctx.org_id);
                if (!document) {
                    callback(ErrorResponse::conflict("no_document", "This filing has no printable document"));
                    return;
                }
                if (!document->s3_key || document->s3_key->empty()) {
                    callback(ErrorResponse::conflict("not_rendered",
                                                     "The printable document has not been rendered "
                                                     "yet"));
                    return;
                }
                key = *document->s3_key;
            }

            auto* s3 = s3_backend();
            if (!s3) {
                callback(ErrorResponse::service_unavailable("presign_unsupported",
                                                            "Presigned URLs require the S3 storage backend"));
                return;
            }
            const std::string url = s3->presign(key, "GET", kDownloadTtlSec);
            callback(Response::ok({{"url", url}, {"artifact", artifact}, {"key", key}}));
        });
    }

private:
    static bool is_allowed(const std::string& v, const std::vector<std::string>& allowed) {
        for (const auto& a : allowed)
            if (v == a)
                return true;
        return false;
    }

    /// Exactly four ASCII digits — a calendar year for the ?year filter.
    static bool is_year(const std::string& s) {
        if (s.size() != 4)
            return false;
        for (unsigned char c : s) {
            if (c < '0' || c > '9')
                return false;
        }
        return true;
    }

    /// Shared `?on=YYYY-MM-DD` parsing for rates/deadlines/alerts: absent
    /// means today, a present-but-not-calendar-valid value is a 422 (a query
    /// param has no "missing/wrong type" case to report as 400 — it is
    /// always a string).
    static bool parse_on_date(const HttpRequestPtr& req,
                              std::string& out,
                              const std::function<void(const HttpResponsePtr&)>& callback) {
        const std::string param = req->getParameter("on");
        if (param.empty()) {
            out = today_iso();
            return true;
        }
        if (!Validation::is_valid_date(param)) {
            callback(Validation::response_422("on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
            return false;
        }
        out = param;
        return true;
    }

    /// Today in UTC as "YYYY-MM-DD". Pure std::chrono (C++20 calendar types),
    /// same formatting shape Tax::TaxCalendar uses internally — no locale, no
    /// std::localtime.
    static std::string today_iso() {
        // Fully qualified on purpose: this file already does
        // `using namespace drogon;` at namespace scope, and an unqualified
        // `floor<days>(...)` here would be resolved against whatever else is
        // visible rather than unambiguously against std::chrono.
        const std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
        char buf[11];
        std::snprintf(buf,
                      sizeof(buf),
                      "%04d-%02u-%02u",
                      static_cast<int>(ymd.year()),
                      static_cast<unsigned>(ymd.month()),
                      static_cast<unsigned>(ymd.day()));
        return std::string(buf);
    }

    /// Which `tax_calculations.kind` a given FORM is filed from — the pairing
    /// createFiling enforces with a 422 rather than letting a НДС calculation
    /// be rendered as a СНР declaration (which Fno910::build_xml would refuse
    /// with std::invalid_argument, i.e. a 500).
    static const char* expected_calculation_kind(const std::string& filing_kind) {
        return filing_kind == Tax::FilingKind::kFno910 ? Tax::CalculationKind::kSnrSimplified
                                                       : Tax::CalculationKind::kVat;
    }

    /// "910" / "300" — the S3 object-name fragment for a filing's XML.
    static std::string form_slug_fragment(const std::string& filing_kind) {
        return filing_kind == Tax::FilingKind::kFno910 ? "910" : "300";
    }

    /// The taxpayer/period context both ФНО generators consume.
    ///
    /// `tax_period_half` carries the HALF-YEAR ("1"/"2") for form 910.00,
    /// whose reporting period is a полугодие (НК РК ст.722/727), and the
    /// QUARTER ("1".."4") for form 300.00, whose period is a квартал
    /// (ст.504-506): Tax::FnoXml::OrgInfo has exactly one period-ordinal
    /// slot, deliberately shared by both generators (see FnoXml.hpp — "Task
    /// 8's ФНО generator reuses this exact struct"), so the ordinal it holds
    /// is the one that form's own period is numbered by.
    static Tax::OrgInfo org_info_for(const std::string& filing_kind,
                                     const Tax::Calculation& calc,
                                     const Tenancy::Organization& org) {
        Tax::OrgInfo info;
        info.bin = org.bin;
        info.name = org.name;
        info.tax_period_year = calc.period_from.substr(0, 4);
        info.tax_period_half = (filing_kind == Tax::FilingKind::kFno910) ? std::to_string(half_of(calc.period_from))
                                                                         : std::to_string(quarter_of(calc.period_from));
        return info;
    }

    static std::string build_form_xml(const std::string& filing_kind,
                                      const Tax::Calculation& calc,
                                      const Tenancy::Organization& org) {
        const Tax::OrgInfo info = org_info_for(filing_kind, calc, org);
        if (filing_kind == Tax::FilingKind::kFno910)
            return Tax::Fno910::build_xml(calc, info);
        return Tax::Fno300::build_xml(calc, info);
    }

    /// Everything the ФНО print template can be derived from. Fields the
    /// database genuinely does not hold (director/accountant/*_words, and
    /// fno_300's sales_tenge) are deliberately ABSENT so the template's own
    /// JSON Schema demands them from `document_input` — see file header.
    static json build_form_input(const std::string& filing_kind,
                                 const Tax::Calculation& calc,
                                 const Tenancy::Organization& org) {
        const std::string year = calc.period_from.substr(0, 4);
        json input = {
            {"org", {{"bin", org.bin}, {"name", org.name}}},
            {"signed_on", iso_to_ddmmyyyy(today_iso())},
        };
        if (filing_kind == Tax::FilingKind::kFno910) {
            const long long income_tiyn = calc.result_snapshot.value("income_tiyn", 0LL);
            const long long rate_bp = calc.result_snapshot.value("rate_bp", 0LL);
            input["period"] = {{"year", year}, {"half", std::to_string(half_of(calc.period_from))}};
            input["income_tenge"] = Ledger::format_tiyn(income_tiyn);
            input["rate_percent"] = format_bp_percent(rate_bp);
            input["tax_tenge"] = Ledger::format_tiyn(calc.total_tiyn);
            return input;
        }
        const long long accrued_tiyn = calc.result_snapshot.value("accrued_tiyn", 0LL);
        const long long deductible_tiyn = calc.result_snapshot.value("deductible_tiyn", 0LL);
        const long long balance_tiyn = calc.result_snapshot.value("balance_tiyn", 0LL);
        input["period"] = {{"year", year}, {"quarter", std::to_string(quarter_of(calc.period_from))}};
        input["vat_charged_tenge"] = Ledger::format_tiyn(accrued_tiyn);
        input["vat_credited_tenge"] = Ledger::format_tiyn(deductible_tiyn);
        // format_tiyn rejects negatives by contract, and a negative balance is
        // a normal refund position — the magnitude goes in the amount, the
        // direction in balance_kind (the template's own enum).
        input["balance_tenge"] = Ledger::format_tiyn(balance_tiyn < 0 ? -balance_tiyn : balance_tiyn);
        input["balance_kind"] = balance_tiyn < 0 ? "to_refund" : "to_pay";
        return input;
    }

    /// Basis points -> a percent string with no trailing noise: 400 -> "4",
    /// 350 -> "3.5", 1625 -> "16.25". Pure integer arithmetic — no float ever
    /// touches a rate.
    static std::string format_bp_percent(long long rate_bp) {
        const long long whole = rate_bp / 100;
        long long frac = rate_bp % 100;
        if (frac < 0)
            frac = -frac;
        if (frac == 0)
            return std::to_string(whole);
        std::string out = std::to_string(whole) + ".";
        if (frac < 10)
            out += "0";
        std::string frac_str = std::to_string(frac);
        while (frac_str.size() > 1 && frac_str.back() == '0')
            frac_str.pop_back();
        return out + frac_str;
    }

    /// 1 for January-June, 2 for July-December (form 910.00's полугодие).
    static int half_of(const std::string& iso_date) { return month_of(iso_date) <= 6 ? 1 : 2; }

    /// 1..4 (form 300.00's квартал).
    static int quarter_of(const std::string& iso_date) { return (month_of(iso_date) - 1) / 3 + 1; }

    /// Month number of an already-validated (DB-sourced) "YYYY-MM-DD".
    static int month_of(const std::string& iso_date) {
        if (iso_date.size() < 7)
            return 1;
        return parse_int(iso_date.substr(5, 2), 1);
    }

    /// "YYYY-MM-DD" -> "DD.MM.YYYY", the format both ФНО templates' schemas
    /// require for `signed_on` (same helper HrController needs for its own
    /// templates).
    static std::string iso_to_ddmmyyyy(const std::string& iso) {
        if (iso.size() != 10)
            return iso;
        return iso.substr(8, 2) + "." + iso.substr(5, 2) + "." + iso.substr(0, 4);
    }

    /// The presign-capable backend, or nullptr when Storage isn't initialized
    /// or the configured backend isn't S3 — presign() is not part of the
    /// StorageBackend interface (see Storage.hpp), only Storage::S3Storage
    /// implements it. Same helper LedgerDocumentsController defines.
    static Storage::S3Storage* s3_backend() {
        if (!Storage::is_initialized())
            return nullptr;
        return dynamic_cast<Storage::S3Storage*>(&Storage::get());
    }
};

}  // namespace Api
