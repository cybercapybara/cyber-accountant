/**
 * @file PayrollController.hpp
 * @brief Payroll-run REST surface over Payroll::PayrollService (design spec
 *        §7.2 / Task 12) — the HTTP layer only; every rule about WHAT a
 *        payroll run may do lives in that service.
 *
 * Routes (all under /api/v1, every handler starts with
 * API_REQUIRE_ORG(req, callback, ctx)):
 *   GET  /api/v1/payroll-runs                 list, paginated, optional ?year
 *   POST /api/v1/payroll-runs                 {year, month} -> calculate (or
 *                                               RECALCULATE) that period,
 *                                               200 with the run AND its
 *                                               payslips
 *   POST /api/v1/payroll-runs/{id}/approve            draft -> approved
 *   POST /api/v1/payroll-runs/{id}/post-to-journal    -> {entry_id}
 *   GET  /api/v1/payroll-runs/{id}/payslips           the run's payslips
 *   POST /api/v1/payroll-runs/{id}/payslips/{employee_id}/generate-document
 *                                             -> `payslip` docgen, 202
 *
 * RBAC: every mutating route rejects `ctx.role == "viewer"` with 403, the
 * rule every ledger/HR route follows. `org_id` comes EXCLUSIVELY from
 * `ctx.org_id` — never a body field, a query param or a path segment.
 *
 * `POST /payroll-runs` answers 200, not 201: PayrollService::calculate_run is
 * an UPSERT over `payroll_runs`' UNIQUE(org_id, period_year, period_month)
 * (a recalculation replaces the draft's payslips in place — see that file's
 * header), so a second call for the same period does not create a second
 * resource and "201 Created" would be a lie half the time. Same reasoning
 * makes `POST /tax/calculations` a 200 in TaxController.
 *
 * Error semantics (deliberate, per route):
 *   - 400 — malformed request SHAPE: a non-uuid path id, a missing or
 *     wrong-typed body field.
 *   - 422 — well-formed but semantically invalid VALUE: `month` outside
 *     1..12, `year` outside the supported range, `?year` that isn't an
 *     integer.
 *   - 409 — STATE conflict, and this is the one that matters here.
 *     PayrollService::InvalidRunState derives from
 *     Repositories::ConflictError, so `with_repo_errors` already maps it to
 *     409 with the service's own code (`invalid_run_state`) — that covers
 *     recalculating an approved run, approving a non-draft run, posting a
 *     run that is not approved yet, and posting a run that already has a
 *     `journal_entry_id`. None of those may surface as a 500.
 *     `post-to-journal` additionally PRE-CHECKS the "no payslips yet" case
 *     and answers 409 `empty_run` itself, because PayrollService signals that
 *     one with a plain std::runtime_error (a 500 through with_repo_errors) —
 *     it is a state conflict for the caller, not a server fault, so it is
 *     caught here rather than left to the generic handler.
 *   - 404 — no such run visible to this org (a foreign org's run is
 *     indistinguishable from a missing one, per OrgCrudBase::find_in_org).
 *
 *   - 422 also covers a period whose rates have not been seeded at all:
 *     PayrollService::calculate_run throws Payroll::MissingPayrollReference
 *     (fix round 1 — it used to be a bare std::runtime_error, i.e. a 500)
 *     and `calculate` renders it as a 422 naming the missing rate/constant
 *     and the exact effective date. No "earliest supported period" constant
 *     is hardcoded here to detect it — the throw site already knows the
 *     lookup came back empty, so only the exception TYPE changed; rates and
 *     thresholds still come exclusively from `tax_rates`/`tax_constants`.
 *
 * generate-document: same base-input + ALLOWLISTED-body-merge design
 * HrController documents at length — templates/latex/payslip/v1/schema.json
 * requires `net_words` (the net amount spelled out in Russian), and this
 * codebase has no money-to-words converter anywhere, so that field CANNOT be
 * derived. The handler builds every OTHER field the schema requires from the
 * payslip + employee + organization, validates an OPTIONAL request body
 * against payslip_allowed_extra_fields() — which holds exactly `net_words`,
 * the one field with no authoritative source — and only then deep-merges it
 * on top (RFC 7396 `merge_patch`) and validates against the template's JSON
 * Schema exactly like POST /documents/generate. A caller that omits
 * `net_words` gets the same `422 schema_validation_failed`, never a broken
 * PDF.
 *
 * Final fix round (security): that allowlist is load-bearing, not cosmetic.
 * This handler used to merge_patch the raw request body with no allowlist at
 * all, so a caller could rewrite `gross_tenge`, `net`, or `employee.iin` in
 * the PDF while the payroll run, the payslip row and the journal entry for
 * the same period kept saying something else — the identical hole already
 * found and fixed in HrController. Any body key outside the allowlist is now
 * rejected with a 422 `not_allowed_override` naming that field, BEFORE any
 * merge touches the input.
 *
 * Money in that input is rendered with Ledger::format_tiyn ("300000.00"),
 * the canonical form everywhere in this codebase. The template's own
 * fixtures use a prettier locale form ("300 000,00"); that is a presentation
 * difference only, and it is NOT something a caller may fix by overriding
 * the amount through the merge body (every amount here is derived from the
 * stored payslip and is therefore off the allowlist) — a second money
 * formatter, or a template-side change, is the honest way to get it.
 *
 * `doc_type` for the generated payslip is `"hr"` — migrations/010_documents.sql's
 * CHECK list has one generic HR bucket and no `payslip` entry ("payslip"
 * exists only as a docgen TEMPLATE slug, in `documents.template_slug`, which
 * has no CHECK at all). Same one exception to DocgenController's
 * doc_type == template_slug identity that HrController already documents.
 */

#pragma once

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
#include "hr/Employee.hpp"
#include "hr/EmployeeRepository.hpp"
#include "jobs/Jobs.hpp"
#include "ledger/DocumentRepository.hpp"
#include "ledger/JournalService.hpp"
#include "payroll/PayrollRepository.hpp"
#include "payroll/PayrollService.hpp"
#include "payroll/Payslip.hpp"
#include "tenancy/OrgContext.hpp"
#include "tenancy/Organization.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class PayrollController : public HttpController<PayrollController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(PayrollController::list, "/api/v1/payroll-runs", Get);
    ADD_METHOD_TO(PayrollController::calculate, "/api/v1/payroll-runs", Post);
    ADD_METHOD_TO(PayrollController::approve, "/api/v1/payroll-runs/{1}/approve", Post);
    ADD_METHOD_TO(PayrollController::postToJournal, "/api/v1/payroll-runs/{1}/post-to-journal", Post);
    ADD_METHOD_TO(PayrollController::listPayslips, "/api/v1/payroll-runs/{1}/payslips", Get);
    // Handler name kept short on purpose: scripts/check-routes-registered.sh
    // scans ADD_METHOD_TO line by line, so a wrapped registration would be
    // silently skipped by the triple-sync gate. It generates the payslip
    // DOCUMENT (docgen slug `payslip`), same as HrController's
    // generateOrderDocument.
    ADD_METHOD_TO(PayrollController::generatePayslip, "/api/v1/payroll-runs/{1}/payslips/{2}/generate-document", Post);
    METHOD_LIST_END

    /// Same job type DocgenController's `POST /documents/generate` enqueues —
    /// see that file's header for why the string is duplicated per enqueueing
    /// module rather than pulling in the worker-side RenderJob.hpp.
    static constexpr const char* kRenderJobType = "docgen.render";

    /// Docgen template slug for a payslip (templates/latex/payslip/v1/).
    static constexpr const char* kPayslipSlug = "payslip";

    /// The ONLY field templates/latex/payslip/v1/schema.json requires that
    /// this codebase cannot derive: the net amount spelled out in Russian
    /// (there is no money-to-words converter anywhere here). Every other
    /// field in that schema — period_label, employer.name/bin,
    /// employee.full_name/iin/position, and all nine money figures — comes
    /// from the payroll run, the employee record or the organization, so
    /// none of them may be overridden by the caller: this IS the allowlist
    /// Api::Validation::merge_allowed_extra() enforces, and anything else in
    /// the body is a 422 `not_allowed_override` (see file header).
    static const std::vector<std::string>& payslip_allowed_extra_fields() {
        static const std::vector<std::string> kAllowed = {"net_words"};
        return kAllowed;
    }

    /// Accepted `period_year` bounds. Not a tax value (those come only from
    /// `tax_rates`/`tax_constants`) — purely a sanity window that keeps a
    /// typo like `year: 20261` from reaching Postgres and the rate lookup.
    static constexpr int kMinYear = 2000;
    static constexpr int kMaxYear = 2100;

    // -------------------------------------------------------------------
    // GET /api/v1/payroll-runs — paginated, org-scoped, optional ?year.
    // Header rows only (PayrollRun::from_row never populates `payslips`);
    // GET /payroll-runs/{id}/payslips is the per-run detail view.
    // -------------------------------------------------------------------
    void list(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        std::optional<std::string> year_filter;
        if (!parse_year_filter(req, year_filter, callback))
            return;
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);

        with_repo_errors(callback, "payroll runs list", [&] {
            Payroll::PayrollRepository repo;
            auto rows = repo.list_filtered(ctx.org_id, year_filter, page.limit, page.offset);
            long total = repo.count_filtered(ctx.org_id, year_filter);
            json data = json::array();
            for (const auto& r : rows)
                data.push_back(r);
            callback(Response::paginated(data, total, page.limit, page.offset));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/payroll-runs — body {year, month}. Calculates or
    // RECALCULATES the org's run for that period and answers 200 with the
    // run plus every payslip it produced (PayrollRun::to_json embeds them).
    // Accountant/owner only.
    // -------------------------------------------------------------------
    void calculate(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (ctx.role == "viewer") {
            callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot calculate payroll"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        // Phase 1 — structural (missing / wrong-type): 400.
        Validation::Errors errs;
        Validation::require(errs, body, "year");
        Validation::require(errs, body, "month");
        if (body.contains("year") && !body["year"].is_number_integer())
            errs.add("year", "not_integer", "must be an integer");
        if (body.contains("month") && !body["month"].is_number_integer())
            errs.add("month", "not_integer", "must be an integer");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        // Phase 2 — semantic (right type, present, VALUE out of range): 422.
        const int year = body["year"].get<int>();
        const int month = body["month"].get<int>();
        Validation::Errors semantic_errs;
        if (year < kMinYear || year > kMaxYear)
            semantic_errs.add("year",
                              "out_of_range",
                              "must be between " + std::to_string(kMinYear) + " and " + std::to_string(kMaxYear));
        if (month < 1 || month > 12)
            semantic_errs.add("month", "out_of_range", "must be between 1 and 12");
        if (semantic_errs.any()) {
            callback(Validation::response_422(semantic_errs));
            return;
        }

        // InvalidRunState (recalculating an approved run) is a
        // Repositories::ConflictError — with_repo_errors turns it into a 409
        // with the service's own code, no ad-hoc catch needed here.
        with_repo_errors(callback, "payroll runs calculate", [&] {
            Payroll::PayrollService svc;
            try {
                auto run = svc.calculate_run(ctx.org_id, year, month);
                callback(Response::ok({{"data", json(run)}}));
            } catch (const Payroll::MissingPayrollReference& e) {
                // The period the caller asked for has no seeded rate/constant
                // — a reference-data gap, correctable by adding a row to
                // `tax_rates`/`tax_constants`, so a 422 naming the exact
                // missing row rather than the 500 with_repo_errors would give
                // an unrecognized std::exception (fix round 1). Same posture
                // TaxController takes for Tax::MissingTaxReference.
                //
                // Keyed to `year` because that is the field of the two that
                // actually decides whether the period falls inside the seeded
                // window (migration 011 starts at 2026-01-01); the message
                // carries the rate/constant name and the exact effective date
                // so an operator can act on the response alone.
                callback(Validation::response_422("year",
                                                  "missing_tax_reference",
                                                  "no tax " + e.kind + " '" + e.reference + "' is in force on " +
                                                      e.effective_on + " (the last day of the requested period)"));
            }
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/payroll-runs/{id}/approve — draft -> approved.
    // Accountant/owner only.
    // -------------------------------------------------------------------
    void approve(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (ctx.role == "viewer") {
            callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot approve payroll runs"));
            return;
        }
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed payroll run id"));
            return;
        }

        with_repo_errors(callback, "payroll runs approve", [&] {
            Payroll::PayrollService svc;
            // nullopt = no such run in THIS org (404); a run that exists but
            // is not 'draft' throws InvalidRunState -> 409.
            auto approved = svc.approve(ctx.org_id, id);
            if (!approved) {
                callback(ErrorResponse::not_found("payroll run"));
                return;
            }
            callback(Response::ok({{"data", json(*approved)}}));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/payroll-runs/{id}/post-to-journal — posts the whole run's
    // withholdings/accruals as ONE balanced journal entry and answers its id.
    // Accountant/owner only.
    // -------------------------------------------------------------------
    void postToJournal(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (ctx.role == "viewer") {
            callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot post payroll to the journal"));
            return;
        }
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed payroll run id"));
            return;
        }

        with_repo_errors(callback, "payroll runs post-to-journal", [&] {
            Payroll::PayrollRepository repo;
            auto run = repo.find_in_org(id, ctx.org_id, /*from_primary=*/true);
            if (!run) {
                callback(ErrorResponse::not_found("payroll run"));
                return;
            }
            // Pre-check the one failure PayrollService signals with a plain
            // std::runtime_error (which with_repo_errors would render as a
            // 500): an approved run whose payslips were never calculated.
            // For the caller that is a state conflict, so it gets a 409 with
            // its own code here. Every OTHER guard — not approved yet,
            // already posted — is InvalidRunState and is left to the service
            // (and its compare-and-swap), which is the authority on those.
            if (repo.list_payslips(*run, /*from_primary=*/true).empty()) {
                callback(
                    ErrorResponse::conflict("empty_run", "This payroll run has no payslips yet — calculate it first"));
                return;
            }

            Payroll::PayrollService svc;
            const std::string entry_id = svc.post_to_journal(ctx.org_id, id, ctx.user_id);
            callback(Response::ok({{"entry_id", entry_id}}));
        });
    }

    // -------------------------------------------------------------------
    // GET /api/v1/payroll-runs/{id}/payslips — unpaginated (one payslip per
    // active employee; a run is bounded by headcount, not by history).
    // -------------------------------------------------------------------
    void listPayslips(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed payroll run id"));
            return;
        }

        with_repo_errors(callback, "payroll runs payslips", [&] {
            Payroll::PayrollRepository repo;
            // The run itself is resolved first so a missing/foreign run is a
            // 404 rather than an empty array — PayrollService::payslips_of
            // deliberately collapses both into "nothing to show", which is
            // the wrong answer for a URL that names the run.
            auto run = repo.find_in_org(id, ctx.org_id);
            if (!run) {
                callback(ErrorResponse::not_found("payroll run"));
                return;
            }
            json data = json::array();
            for (const auto& p : repo.list_payslips(*run))
                data.push_back(p);
            callback(Response::list(data));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/payroll-runs/{id}/payslips/{employee_id}/generate-document
    // — accountant/owner only. See file header for the base-input +
    // optional-body-merge design and why `net_words` must come from the body.
    // -------------------------------------------------------------------
    void generatePayslip(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback,
                         const std::string& id,
                         const std::string& employee_id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (ctx.role == "viewer") {
            callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot generate documents"));
            return;
        }
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed payroll run id"));
            return;
        }
        if (!is_valid_uuid(employee_id)) {
            callback(ErrorResponse::bad_request("invalid_employee_id", "Malformed employee id"));
            return;
        }
        json extra;
        if (!Validation::parse_optional_body(req, extra, callback))
            return;

        Payroll::PayrollRepository payroll_repo;
        auto run = payroll_repo.find_in_org(id, ctx.org_id);
        if (!run) {
            callback(ErrorResponse::not_found("payroll run"));
            return;
        }
        std::optional<Payroll::Payslip> payslip;
        for (const auto& p : payroll_repo.list_payslips(*run)) {
            if (p.employee_id == employee_id) {
                payslip = p;
                break;
            }
        }
        if (!payslip) {
            callback(ErrorResponse::not_found("payslip"));
            return;
        }

        Hr::EmployeeRepository employees;
        auto employee = employees.find_in_org(employee_id, ctx.org_id);
        if (!employee) {
            // payslips' composite FK (employee_id, org_id) -> employees
            // (id, org_id) makes this unreachable short of a concurrent
            // delete; same posture as HrController's equivalent branch.
            spdlog::error("payroll generate-document: payslip {} references missing employee {} in org {}",
                          payslip->id,
                          employee_id,
                          ctx.org_id);
            callback(ErrorResponse::internal_error());
            return;
        }

        Tenancy::OrganizationRepository orgs;
        auto org = orgs.find(ctx.org_id);
        if (!org) {
            spdlog::error("payroll generate-document: organization {} missing", ctx.org_id);
            callback(ErrorResponse::internal_error());
            return;
        }

        json input = {
            {"period_label", period_label_ru(run->period_year, run->period_month)},
            {"employer", {{"name", org->name}, {"bin", org->bin}}},
            {"employee",
             {{"full_name", full_name(*employee)}, {"iin", employee->iin}, {"position", employee->position}}},
            {"gross_tenge", Ledger::format_tiyn(payslip->gross_tiyn)},
            {"opv", Ledger::format_tiyn(payslip->opv)},
            {"vosms", Ledger::format_tiyn(payslip->vosms)},
            {"ipn", Ledger::format_tiyn(payslip->ipn)},
            {"net", Ledger::format_tiyn(payslip->net)},
            {"opvr", Ledger::format_tiyn(payslip->opvr)},
            {"so", Ledger::format_tiyn(payslip->so)},
            {"osms", Ledger::format_tiyn(payslip->osms)},
            {"social_tax", Ledger::format_tiyn(payslip->social_tax)},
        };
        // `net_words` is deliberately absent from the base — see file header
        // — and is the only key the caller may supply. Everything above is
        // authoritative (payslip/employee/organization columns), so an
        // attempt to override any of it is rejected here with a 422 naming
        // the field, BEFORE the merge mutates `input`.
        if (!Validation::merge_allowed_extra(input, extra, payslip_allowed_extra_fields(), callback))
            return;

        Docgen::TemplateRegistry registry;
        auto info = registry.latest(kPayslipSlug);
        if (!info) {
            spdlog::error("payroll generate-document: template '{}' not found on disk", kPayslipSlug);
            callback(ErrorResponse::internal_error());
            return;
        }
        if (auto err = Docgen::TemplateRegistry::validate(*info, input)) {
            callback(Validation::response_422("input", "schema_validation_failed", *err));
            return;
        }

        API_REQUIRE_JOBS_READY(callback);

        with_repo_errors(callback, "payroll generate-document", [&] {
            Ledger::DocumentRepository documents;
            // doc_type is "hr" (the generic bucket), NOT the template slug —
            // see file header.
            auto created = documents.create(ctx.org_id,
                                            "hr",
                                            "generated",
                                            "draft",
                                            std::nullopt,
                                            info->slug,
                                            info->version_str,
                                            std::optional<nlohmann::json>{input});

            json payload = {
                {"org_id", ctx.org_id}, {"document_id", created.id}, {"slug", kPayslipSlug}, {"input", input}};
            bool render_queued = false;
            try {
                auto job = Jobs::get().submit(kRenderJobType, payload);
                spdlog::debug("payroll generate-document: document {} enqueued as job {}", created.id, job.id);
                render_queued = true;
            } catch (const std::exception& e) {
                // Best-effort enqueue, same posture as DocgenController: the
                // document already exists, so a Redis blip must not turn it
                // into a 500 the client would retry into another orphan.
                spdlog::error("payroll generate-document: enqueue docgen.render for document {} failed: {}",
                              created.id,
                              e.what());
            }
            const json response_body = {{"document_id", created.id}, {"render_queued", render_queued}};
            callback(Response::accepted(response_body));
        });
    }

private:
    /// `?year=` filter shared by list(): nullopt (no filter) when absent, a
    /// 422 when present but not a plain in-range integer. A query-param VALUE
    /// that fails a domain rule is a 422 here, matching
    /// LedgerDocumentsController::list's type/status filters (there is no
    /// missing/wrong-type case to report as 400 — a query string is always a
    /// string). Kept as a string because
    /// PayrollRepository::list_filtered casts it in SQL.
    static bool parse_year_filter(const HttpRequestPtr& req,
                                  std::optional<std::string>& out,
                                  const std::function<void(const HttpResponsePtr&)>& callback) {
        const std::string param = req->getParameter("year");
        if (param.empty())
            return true;
        if (!is_plain_integer(param)) {
            callback(Validation::response_422("year", "not_integer", "must be a 4-digit calendar year"));
            return false;
        }
        const int year = parse_int(param, 0);
        if (year < kMinYear || year > kMaxYear) {
            callback(Validation::response_422(
                "year",
                "out_of_range",
                "must be between " + std::to_string(kMinYear) + " and " + std::to_string(kMaxYear)));
            return false;
        }
        out = param;
        return true;
    }

    /// Digits only, 1..9 of them — deliberately rejects a leading '+'/'-' and
    /// any whitespace, so `parse_int` below can never silently truncate
    /// something like "2026abc" into 2026.
    static bool is_plain_integer(const std::string& s) {
        if (s.empty() || s.size() > 9)
            return false;
        for (unsigned char c : s) {
            if (c < '0' || c > '9')
                return false;
        }
        return true;
    }

    static std::string full_name(const Hr::Employee& e) {
        std::string name = e.last_name + " " + e.first_name;
        if (e.middle_name && !e.middle_name->empty())
            name += " " + *e.middle_name;
        return name;
    }

    /// "Август 2026" — the human-readable period label
    /// templates/latex/payslip/v1/schema.json wants (its fixtures use exactly
    /// this shape). Its own table rather than reusing Tax::TaxCalendar's:
    /// that one is a private implementation detail of the deadline expander
    /// and labels a period ENDING in a month, not a payroll month.
    static std::string period_label_ru(int year, int month) {
        static const char* kMonthsRu[] = {"",
                                          "Январь",
                                          "Февраль",
                                          "Март",
                                          "Апрель",
                                          "Май",
                                          "Июнь",
                                          "Июль",
                                          "Август",
                                          "Сентябрь",
                                          "Октябрь",
                                          "Ноябрь",
                                          "Декабрь"};
        if (month < 1 || month > 12)
            return std::to_string(month) + "." + std::to_string(year);
        return std::string(kMonthsRu[month]) + " " + std::to_string(year);
    }
};

}  // namespace Api
