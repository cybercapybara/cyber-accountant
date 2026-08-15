/**
 * @file HrController.hpp
 * @brief HR orders / labor contracts / vacations API — org-scoped (design
 *        spec §7.2 / Task 11), plus the two "generate the кадровый document
 *        from this record" convenience endpoints.
 *
 * Routes (all under /api/v1, every handler starts with
 * API_REQUIRE_ORG(req, callback, ctx)):
 *   GET  /api/v1/hr-orders                         list, paginated (optional
 *                                                     ?employee_id)
 *   POST /api/v1/hr-orders                          create (owner/accountant/hr)
 *   POST /api/v1/hr-orders/{id}/generate-document    -> hr_order docgen, 202
 *   GET  /api/v1/labor-contracts                    list (?employee_id REQUIRED
 *                                                     — Hr::HrRepository::
 *                                                     list_contracts takes no
 *                                                     "every employee" mode,
 *                                                     unlike list_orders/
 *                                                     list_vacations). Bounded
 *                                                     by one employee's own
 *                                                     contract history, so it
 *                                                     stays unpaginated.
 *   POST /api/v1/labor-contracts                    create (owner/accountant/hr)
 *   POST /api/v1/labor-contracts/{id}/generate-document -> labor_contract docgen, 202
 *   GET  /api/v1/vacations                          list, paginated (optional
 *                                                     ?employee_id)
 *   POST /api/v1/vacations                          create (owner/accountant/hr)
 *
 * RBAC: EVERY route goes through API_REQUIRE_ORG_PERM against the §5.3
 * permission matrix (Tenancy::OrgPerm), which DENIES BY DEFAULT — an unknown
 * role, resource or action is a 403, so a role added later cannot fail open
 * the way the old `ctx.role == "viewer"` denylist let it. The mutating routes
 * (create and generate-document) need `hr_docs`/write, the three list routes
 * `hr_docs`/read. `hr_docs` is one of the two resources the `hr` role holds
 * in full, so unlike the ledger controllers these routes read
 * "owner/accountant/hr", not "accountant/owner only". `org_id` comes
 * EXCLUSIVELY from `ctx.org_id`.
 *
 * Cross-org employee_id: HrRepository's create_order/create_contract/
 * create_vacation rely on migrations/012_hr.sql's composite FK
 * `(employee_id, org_id) -> employees(id, org_id)` to reject a foreign
 * employee — but letting that FK violation surface raw would be a 500 for
 * an entirely caller-correctable mistake (the exact gap DocgenController's
 * header documents for counterparty_id/link_entry_id). So every create*
 * handler here pre-checks `employee_id` with a plain
 * `EmployeeRepository::find_in_org` and reports a foreign employee as a 422
 * on that field (`foreign_employee`), never a 404 — same posture as
 * DocgenController's counterparty_id/link_entry_id: a bad body-field
 * reference is a validation failure of THIS request, not "resource not
 * found".
 *
 * generate-document design (both hr-orders and labor-contracts): the two
 * templates' schemas (templates/latex/hr_order/v1/schema.json,
 * .../labor_contract/v1/schema.json) require several free-text fields this
 * codebase has no column for at all — `director`, `reason`, `details`,
 * `employer.address`, `employee.address`, `work_schedule`,
 * `probation_months`. There is no organization "director" field
 * (Tenancy::Organization), so those cannot be auto-derived. The two amounts
 * in words (`salary_words`, `salary_words_kk`) USED to be in that list; P3
 * derives both from employees.salary_tiyn instead, so the contract's digits
 * and its two spelled-out amounts cannot disagree. Each generate-document
 * handler therefore: (1) builds a BASE input object from what IS on file (the
 * order/contract row, the referenced employee, the organization's
 * name/bin), (2) validates an OPTIONAL request body against an explicit
 * ALLOWLIST of exactly those free-text field names
 * (hr_order_allowed_extra_fields()/labor_contract_allowed_extra_fields())
 * and deep-merges it on top via `nlohmann::json::merge_patch` (RFC 7396) —
 * an empty/absent body is fine, it just means "use only what's on file" —
 * and (3) validates the merged result against the template's JSON Schema
 * exactly like `POST /documents/generate` (DocgenController) does — a
 * caller that omits a required free-text field gets the same
 * `422 schema_validation_failed` DocgenController already produces for a
 * malformed `input`, not a new error shape.
 *
 * Fix round 1 (security): the allowlist in step (2) is load-bearing, not
 * cosmetic — an earlier version of this handler deep-merged the request
 * body onto the base input with NO allowlist at all, so a caller could
 * override an authoritative, database-derived field (employee.iin,
 * salary_tenge, employer.name, ...) in what becomes a generated LEGAL HR
 * document. Any body key that is not on the allowlist (checked at its own
 * leaf via dotted paths, e.g. "employer.director" vs "employer.name") is
 * now rejected with a 422 naming that field — never silently dropped,
 * never silently merged — via Api::Validation::merge_allowed_extra()/
 * validate_extra_allowlist(), which live in api/Validation.hpp because
 * TaxController's ФНО filings and PayrollController's payslips need the
 * identical discipline (final fix round).
 *
 * `doc_type` for both generated documents is `"hr"` — migrations/
 * 010_documents.sql's CHECK list has one generic HR bucket, not
 * `"hr_order"`/`"labor_contract"` entries (those exist only as docgen
 * TEMPLATE slugs, in `documents.template_slug`, which has no CHECK at all).
 * Unlike DocgenController's generic endpoint, `doc_type` here is NOT simply
 * `template_slug` — this is the one place in this codebase where that
 * identity mapping (documented in DocgenController.hpp's header) does not
 * hold, so it is called out explicitly here instead of silently diverging.
 *
 * Only `POST /hr-orders/{id}/generate-document` attaches the resulting
 * document back onto its source row (`HrRepository::attach_document` sets
 * `hr_orders.document_id`) — `labor_contracts` has no `document_id` column
 * at all (migrations/012_hr.sql), so the labor-contract variant has nothing
 * to attach to and simply creates + enqueues, same as
 * `POST /documents/generate`.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
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
#include "database/Database.hpp"
#include "docgen/TemplateRegistry.hpp"
#include "hr/Employee.hpp"
#include "hr/EmployeeRepository.hpp"
#include "hr/HrDocuments.hpp"
#include "hr/HrRepository.hpp"
#include "jobs/Jobs.hpp"
#include "ledger/DocumentRepository.hpp"
#include "ledger/JournalService.hpp"
#include "money/AmountInWords.hpp"
#include "money/MoneyFormat.hpp"
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrgPermissions.hpp"
#include "tenancy/Organization.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {
using namespace drogon;
using json = nlohmann::json;

class HrController : public HttpController<HrController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(HrController::listOrders, "/api/v1/hr-orders", Get);
    ADD_METHOD_TO(HrController::createOrder, "/api/v1/hr-orders", Post);
    ADD_METHOD_TO(HrController::generateOrderDocument, "/api/v1/hr-orders/{1}/generate-document", Post);
    ADD_METHOD_TO(HrController::listContracts, "/api/v1/labor-contracts", Get);
    ADD_METHOD_TO(HrController::createContract, "/api/v1/labor-contracts", Post);
    ADD_METHOD_TO(HrController::generateContractDocument, "/api/v1/labor-contracts/{1}/generate-document", Post);
    ADD_METHOD_TO(HrController::listVacations, "/api/v1/vacations", Get);
    ADD_METHOD_TO(HrController::createVacation, "/api/v1/vacations", Post);
    METHOD_LIST_END

    /// Same job type DocgenController's `POST /documents/generate` enqueues
    /// — see that file's header for why the string is duplicated here
    /// rather than including the worker-side RenderJob.hpp.
    static constexpr const char* kRenderJobType = "docgen.render";

    // ===================================================================
    // hr-orders
    // ===================================================================

    // -------------------------------------------------------------------
    // GET /api/v1/hr-orders — PAGINATED (?limit/?offset, same 50/200
    // convention as every other list route in this branch), optional
    // ?employee_id filter. `hr_orders` grows for the life of an
    // organization, so this route used to be an unbounded org-wide SELECT
    // (final fix round): the envelope is now {data, total, limit, offset},
    // not the bare {data} it answered before.
    // -------------------------------------------------------------------
    void listOrders(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kHrDocs, Tenancy::OrgPerm::Action::kRead);
        std::optional<std::string> employee_id_filter;
        if (!filter_employee_id(req, employee_id_filter, callback))
            return;
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);

        with_repo_errors(callback, "hr orders list", [&] {
            Hr::HrRepository repo;
            auto rows = repo.list_orders(ctx.org_id, employee_id_filter, page.limit, page.offset);
            long total = repo.count_orders(ctx.org_id, employee_id_filter);
            json data = json::array();
            for (const auto& o : rows)
                data.push_back(o);
            callback(Response::paginated(data, total, page.limit, page.offset));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/hr-orders — owner/accountant/hr. Body: {employee_id,
    // kind, number, issued_on, effective_from, effective_to?, payload?}.
    // -------------------------------------------------------------------
    void createOrder(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kHrDocs, Tenancy::OrgPerm::Action::kWrite);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "employee_id");
        Validation::require(errs, body, "kind");
        Validation::require(errs, body, "number");
        Validation::require(errs, body, "issued_on");
        Validation::require(errs, body, "effective_from");
        if (body.contains("employee_id") &&
            (!body["employee_id"].is_string() || !is_valid_uuid(body["employee_id"].get<std::string>())))
            errs.add("employee_id", "bad_format", "must be a uuid");
        if (body.contains("kind") && !body["kind"].is_string())
            errs.add("kind", "not_string", "must be a string");
        if (body.contains("number") && !body["number"].is_string())
            errs.add("number", "not_string", "must be a string");
        if (body.contains("issued_on") && !body["issued_on"].is_string())
            errs.add("issued_on", "not_string", "must be a string");
        if (body.contains("effective_from") && !body["effective_from"].is_string())
            errs.add("effective_from", "not_string", "must be a string");
        if (body.contains("effective_to") && !body["effective_to"].is_null() && !body["effective_to"].is_string())
            errs.add("effective_to", "not_string", "must be a string");
        if (body.contains("payload") && !body["payload"].is_null() && !body["payload"].is_object())
            errs.add("payload", "not_object", "must be a JSON object");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        const std::string kind = body["kind"].get<std::string>();
        static const std::vector<std::string> kKinds = {
            "hire", "dismiss", "vacation", "business_trip", "salary_change"};
        if (std::find(kKinds.begin(), kKinds.end(), kind) == kKinds.end()) {
            callback(Validation::response_422(
                "kind", "not_allowed", "must be one of: hire, dismiss, vacation, business_trip, salary_change"));
            return;
        }
        const std::string issued_on = body["issued_on"].get<std::string>();
        if (!Validation::is_valid_date(issued_on)) {
            callback(Validation::response_422("issued_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
            return;
        }
        const std::string effective_from = body["effective_from"].get<std::string>();
        if (!Validation::is_valid_date(effective_from)) {
            callback(
                Validation::response_422("effective_from", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
            return;
        }
        std::optional<std::string> effective_to;
        if (body.contains("effective_to") && !body["effective_to"].is_null()) {
            effective_to = body["effective_to"].get<std::string>();
            if (!Validation::is_valid_date(*effective_to)) {
                callback(Validation::response_422(
                    "effective_to", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
                return;
            }
        }
        std::optional<json> payload;
        if (body.contains("payload") && !body["payload"].is_null())
            payload = body["payload"];

        const std::string employee_id = body["employee_id"].get<std::string>();
        if (!employee_belongs_to_org(employee_id, ctx.org_id)) {
            callback(Validation::response_422(
                "employee_id", "foreign_employee", "employee does not belong to this organization"));
            return;
        }

        const std::string number = body["number"].get<std::string>();
        with_repo_errors(callback, "hr orders create", [&] {
            Hr::HrRepository repo;
            auto created = repo.create_order(
                ctx.org_id, employee_id, kind, number, issued_on, effective_from, effective_to, payload);
            callback(Response::created({{"data", json(created)}}));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/hr-orders/{id}/generate-document — accountant/owner
    // only. See file header for the base-input + optional-body-merge design.
    // -------------------------------------------------------------------
    void generateOrderDocument(const HttpRequestPtr& req,
                               std::function<void(const HttpResponsePtr&)>&& callback,
                               const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kHrDocs, Tenancy::OrgPerm::Action::kWrite);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed HR order id"));
            return;
        }
        json extra;
        if (!Validation::parse_optional_body(req, extra, callback))
            return;

        Hr::HrRepository orders;
        auto order = orders.find_in_org(id, ctx.org_id);
        if (!order) {
            callback(ErrorResponse::not_found("hr order"));
            return;
        }

        auto resolved = lookup_employee_and_org(ctx.org_id, order->employee_id, "hr order", id, callback);
        if (!resolved)
            return;

        json input = {
            {"kind", order->kind},
            {"number", order->number},
            {"issued_on", iso_to_ddmmyyyy(order->issued_on)},
            {"employer", {{"name", resolved->organization.name}, {"bin", resolved->organization.bin}}},
            {"employee",
             {{"full_name", full_name(resolved->employee)},
              {"iin", resolved->employee.iin},
              {"position", resolved->employee.position}}},
            {"effective_from", iso_to_ddmmyyyy(order->effective_from)},
        };
        if (order->effective_to)
            input["effective_to"] = iso_to_ddmmyyyy(*order->effective_to);

        if (!Validation::merge_allowed_extra(input, extra, hr_order_allowed_extra_fields(), callback))
            return;

        finish_generate_document(callback, ctx.org_id, "hr_order", input, [&](const std::string& document_id) {
            if (!orders.attach_document(ctx.org_id, id, document_id)) {
                // Pre-checked above (find_in_org succeeded); only a
                // concurrent delete of the order between that check and here
                // could land here. Best-effort, same posture as
                // DocgenController's link_entry() call.
                spdlog::warn(
                    "hr orders generate-document: attach_document({}, {}) failed after pre-check", id, document_id);
            }
        });
    }

    // ===================================================================
    // labor-contracts
    // ===================================================================

    // -------------------------------------------------------------------
    // GET /api/v1/labor-contracts — ?employee_id is REQUIRED (see file
    // header: HrRepository::list_contracts has no "every employee" mode).
    // -------------------------------------------------------------------
    void listContracts(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kHrDocs, Tenancy::OrgPerm::Action::kRead);
        const std::string employee_id = req->getParameter("employee_id");
        if (employee_id.empty()) {
            callback(ErrorResponse::bad_request("missing_employee_id", "employee_id query parameter is required"));
            return;
        }
        if (!is_valid_uuid(employee_id)) {
            callback(ErrorResponse::bad_request("invalid_employee_id", "employee_id must be a uuid"));
            return;
        }

        with_repo_errors(callback, "labor contracts list", [&] {
            Hr::HrRepository repo;
            auto rows = repo.list_contracts(ctx.org_id, employee_id);
            json data = json::array();
            for (const auto& c : rows)
                data.push_back(c);
            callback(Response::list(data));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/labor-contracts — owner/accountant/hr. Body:
    // {employee_id, number, signed_on, starts_on, ends_on?, terms_json?}.
    // -------------------------------------------------------------------
    void createContract(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kHrDocs, Tenancy::OrgPerm::Action::kWrite);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "employee_id");
        Validation::require(errs, body, "number");
        Validation::require(errs, body, "signed_on");
        Validation::require(errs, body, "starts_on");
        if (body.contains("employee_id") &&
            (!body["employee_id"].is_string() || !is_valid_uuid(body["employee_id"].get<std::string>())))
            errs.add("employee_id", "bad_format", "must be a uuid");
        if (body.contains("number") && !body["number"].is_string())
            errs.add("number", "not_string", "must be a string");
        if (body.contains("signed_on") && !body["signed_on"].is_string())
            errs.add("signed_on", "not_string", "must be a string");
        if (body.contains("starts_on") && !body["starts_on"].is_string())
            errs.add("starts_on", "not_string", "must be a string");
        if (body.contains("ends_on") && !body["ends_on"].is_null() && !body["ends_on"].is_string())
            errs.add("ends_on", "not_string", "must be a string");
        if (body.contains("terms_json") && !body["terms_json"].is_null() && !body["terms_json"].is_object())
            errs.add("terms_json", "not_object", "must be a JSON object");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        const std::string signed_on = body["signed_on"].get<std::string>();
        if (!Validation::is_valid_date(signed_on)) {
            callback(Validation::response_422("signed_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
            return;
        }
        const std::string starts_on = body["starts_on"].get<std::string>();
        if (!Validation::is_valid_date(starts_on)) {
            callback(Validation::response_422("starts_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
            return;
        }
        std::optional<std::string> ends_on;
        if (body.contains("ends_on") && !body["ends_on"].is_null()) {
            ends_on = body["ends_on"].get<std::string>();
            if (!Validation::is_valid_date(*ends_on)) {
                callback(
                    Validation::response_422("ends_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
                return;
            }
        }
        std::optional<json> terms_json;
        if (body.contains("terms_json") && !body["terms_json"].is_null())
            terms_json = body["terms_json"];

        const std::string employee_id = body["employee_id"].get<std::string>();
        if (!employee_belongs_to_org(employee_id, ctx.org_id)) {
            callback(Validation::response_422(
                "employee_id", "foreign_employee", "employee does not belong to this organization"));
            return;
        }

        const std::string number = body["number"].get<std::string>();
        with_repo_errors(callback, "labor contracts create", [&] {
            Hr::HrRepository repo;
            auto created =
                repo.create_contract(ctx.org_id, employee_id, number, signed_on, starts_on, ends_on, terms_json);
            callback(Response::created({{"data", json(created)}}));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/labor-contracts/{id}/generate-document — accountant/
    // owner only. See file header for the base-input + optional-body-merge
    // design and why there is no attach_document step here.
    // -------------------------------------------------------------------
    void generateContractDocument(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& callback,
                                  const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kHrDocs, Tenancy::OrgPerm::Action::kWrite);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed labor contract id"));
            return;
        }
        json extra;
        if (!Validation::parse_optional_body(req, extra, callback))
            return;

        // labor_contracts has no find_in_org of its own (HrRepository's
        // OrgCrudBase parameterization is over hr_orders, not
        // labor_contracts — see HrRepository.hpp's header, and
        // find_contract()'s doc comment below) — resolve the contract's own
        // row directly.
        auto found_contract = find_contract(ctx.org_id, id);
        if (!found_contract) {
            callback(ErrorResponse::not_found("labor contract"));
            return;
        }

        auto resolved =
            lookup_employee_and_org(ctx.org_id, found_contract->employee_id, "labor contract", id, callback);
        if (!resolved)
            return;

        json input = {
            {"number", found_contract->number},
            {"signed_on", iso_to_ddmmyyyy(found_contract->signed_on)},
            {"employer", {{"name", resolved->organization.name}, {"bin", resolved->organization.bin}}},
            {"employee",
             {{"full_name", full_name(resolved->employee)},
              {"iin", resolved->employee.iin},
              {"position", resolved->employee.position}}},
            // Printed money: the HUMAN form («300 000,00»), not
            // Ledger::format_tiyn's machine «300000.00». The labour contract
            // is signed by an employee and read by a labour inspector; the
            // machine form belongs to employees.salary_tiyn's API
            // representation, which is not this. Both прописи below are
            // derived from the SAME integer, so the figure and the two texts
            // still cannot disagree.
            {"salary_tenge", Money::format_tiyn_ru(resolved->employee.salary_tiyn)},
            {"starts_on", iso_to_ddmmyyyy(found_contract->starts_on)},
        };
        if (found_contract->ends_on)
            input["ends_on"] = iso_to_ddmmyyyy(*found_contract->ends_on);

        // Обе прописи выводятся из ОДНОГО целого employees.salary_tiyn —
        // того же, из которого выше собран salary_tenge, — так что цифра и
        // два текста в договоре не могут разойтись. `salary_words_kk` —
        // обязательное поле схемы labor_contract (P3), без него
        // TemplateRegistry::validate отдал бы 422 на каждом договоре.
        //
        // try/catch не украшение: Ledger::parse_tiyn принимает до 16 цифр
        // целой части, то есть оклад МОЖЕТ быть сохранён выше
        // Money::kMaxTiyn, и тогда to_words_ru бросает std::out_of_range.
        // Без перехвата это был бы 500 на корректном по форме запросе.
        //
        // Ошибка БЕЗ поля (ErrorResponse::unprocessable, не
        // Validation::response_422): проблема в СОХРАНЁННОМ окладе
        // сотрудника, а в этом запросе нет ни одного поля, которое каллер
        // прислал бы и мог бы исправить, — назвать здесь `salary_tiyn`
        // значило бы указать на поле, которого в теле запроса нет. Форма
        // ответа та же самая, общая.
        try {
            input["salary_words"] = Money::to_words_ru(resolved->employee.salary_tiyn);
            input["salary_words_kk"] = Money::to_words_kk(resolved->employee.salary_tiyn);
        } catch (const std::out_of_range&) {
            callback(ErrorResponse::unprocessable(
                "amount_out_of_range",
                "the employee's stored salary cannot be spelled out in words: it exceeds the maximum supported " +
                    std::to_string(Money::kMaxTiyn) + " tiyn"));
            return;
        }

        if (!Validation::merge_allowed_extra(input, extra, labor_contract_allowed_extra_fields(), callback))
            return;

        // labor_contracts has no document_id column (see file header) — no
        // after_create hook needed here, unlike the hr-orders variant.
        finish_generate_document(callback, ctx.org_id, "labor_contract", input, nullptr);
    }

    // ===================================================================
    // vacations
    // ===================================================================

    // -------------------------------------------------------------------
    // GET /api/v1/vacations — PAGINATED, optional ?employee_id filter. Same
    // reasoning and same envelope change as listOrders() above (final fix
    // round): `vacations` also grows without bound.
    // -------------------------------------------------------------------
    void listVacations(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kHrDocs, Tenancy::OrgPerm::Action::kRead);
        std::optional<std::string> employee_id_filter;
        if (!filter_employee_id(req, employee_id_filter, callback))
            return;
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);

        with_repo_errors(callback, "vacations list", [&] {
            Hr::HrRepository repo;
            auto rows = repo.list_vacations(ctx.org_id, employee_id_filter, page.limit, page.offset);
            long total = repo.count_vacations(ctx.org_id, employee_id_filter);
            json data = json::array();
            for (const auto& v : rows)
                data.push_back(v);
            callback(Response::paginated(data, total, page.limit, page.offset));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/vacations — owner/accountant/hr. Body: {employee_id,
    // starts_on, ends_on, days, kind}.
    // -------------------------------------------------------------------
    void createVacation(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kHrDocs, Tenancy::OrgPerm::Action::kWrite);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "employee_id");
        Validation::require(errs, body, "starts_on");
        Validation::require(errs, body, "ends_on");
        Validation::require(errs, body, "days");
        Validation::require(errs, body, "kind");
        if (body.contains("employee_id") &&
            (!body["employee_id"].is_string() || !is_valid_uuid(body["employee_id"].get<std::string>())))
            errs.add("employee_id", "bad_format", "must be a uuid");
        if (body.contains("starts_on") && !body["starts_on"].is_string())
            errs.add("starts_on", "not_string", "must be a string");
        if (body.contains("ends_on") && !body["ends_on"].is_string())
            errs.add("ends_on", "not_string", "must be a string");
        if (body.contains("days") && !body["days"].is_number_integer())
            errs.add("days", "not_integer", "must be an integer");
        if (body.contains("kind") && !body["kind"].is_string())
            errs.add("kind", "not_string", "must be a string");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        const std::string starts_on = body["starts_on"].get<std::string>();
        if (!Validation::is_valid_date(starts_on)) {
            callback(Validation::response_422("starts_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
            return;
        }
        const std::string ends_on = body["ends_on"].get<std::string>();
        if (!Validation::is_valid_date(ends_on)) {
            callback(Validation::response_422("ends_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
            return;
        }
        // ISO YYYY-MM-DD strings compare lexicographically the same as
        // chronologically — pre-checked here (422) so a violation of
        // migrations/012_hr.sql's CHECK (ends_on >= starts_on) never reaches
        // the database as a raw, 500-mapped SQLSTATE 23514.
        if (ends_on < starts_on) {
            callback(Validation::response_422("ends_on", "before_starts_on", "ends_on must be on or after starts_on"));
            return;
        }
        const int days = body["days"].get<int>();
        if (days <= 0) {
            callback(Validation::response_422("days", "not_positive", "must be greater than 0"));
            return;
        }
        const std::string kind = body["kind"].get<std::string>();
        static const std::vector<std::string> kKinds = {"annual", "unpaid", "sick"};
        if (std::find(kKinds.begin(), kKinds.end(), kind) == kKinds.end()) {
            callback(Validation::response_422("kind", "not_allowed", "must be one of: annual, unpaid, sick"));
            return;
        }

        const std::string employee_id = body["employee_id"].get<std::string>();
        if (!employee_belongs_to_org(employee_id, ctx.org_id)) {
            callback(Validation::response_422(
                "employee_id", "foreign_employee", "employee does not belong to this organization"));
            return;
        }

        with_repo_errors(callback, "vacations create", [&] {
            Hr::HrRepository repo;
            auto created = repo.create_vacation(ctx.org_id, employee_id, starts_on, ends_on, days, kind);
            callback(Response::created({{"data", json(created)}}));
        });
    }

private:
    static bool employee_belongs_to_org(const std::string& employee_id, const std::string& org_id) {
        Hr::EmployeeRepository employees;
        return employees.find_in_org(employee_id, org_id).has_value();
    }

    /// Resolved employee + organization behind a generate-document request
    /// — see lookup_employee_and_org() below.
    struct EmployeeAndOrg {
        Hr::Employee employee;
        Tenancy::Organization organization;
    };

    /// Resolve the employee + organization behind a generate-document
    /// request. Both should always exist — the employee via the composite
    /// FK backing hr_orders/labor_contracts, the organization via ctx.org_id
    /// itself being live — so a miss here is logged and answered 500 (a
    /// defensive path, not a normal 404): @p source_label/@p source_id name
    /// the calling order/contract in the log line.
    static std::optional<EmployeeAndOrg> lookup_employee_and_org(
        const std::string& org_id,
        const std::string& employee_id,
        const char* source_label,
        const std::string& source_id,
        const std::function<void(const HttpResponsePtr&)>& callback) {
        Hr::EmployeeRepository employees;
        auto employee = employees.find_in_org(employee_id, org_id);
        if (!employee) {
            spdlog::error("hr generate-document: {} {} references missing employee {} in org {}",
                          source_label,
                          source_id,
                          employee_id,
                          org_id);
            callback(ErrorResponse::internal_error());
            return std::nullopt;
        }
        Tenancy::OrganizationRepository orgs;
        auto org = orgs.find(org_id);
        if (!org) {
            spdlog::error("hr generate-document: organization {} missing", org_id);
            callback(ErrorResponse::internal_error());
            return std::nullopt;
        }
        return EmployeeAndOrg{*employee, *org};
    }

    /// Free-text fields the hr_order template's schema
    /// (templates/latex/hr_order/v1/schema.json) needs that have no backing
    /// column anywhere in this codebase — see file header. This IS the
    /// allowlist: every other key in a caller-supplied generate-document
    /// body is rejected by Api::Validation::merge_allowed_extra(),
    /// specifically so a
    /// caller can never override an authoritative, database-derived value
    /// (kind, number, issued_on, effective_from, any employer.* or employee.* field) that
    /// ends up in a generated legal HR document (Fix round 1).
    static const std::vector<std::string>& hr_order_allowed_extra_fields() {
        static const std::vector<std::string> kAllowed = {"director", "reason", "details"};
        return kAllowed;
    }

    /// Same role as hr_order_allowed_extra_fields(), for the labor_contract
    /// template (templates/latex/labor_contract/v1/schema.json). Paths are
    /// dot-separated for fields nested under employer/employee — e.g.
    /// "employer.director" allows overriding ONLY that leaf, never the
    /// authoritative employer.name/employer.bin siblings.
    ///
    /// `salary_words` left this list in P3: it and `salary_words_kk` are
    /// both derived from employees.salary_tiyn (see
    /// generateContractDocument), so a caller supplying either now gets a
    /// 422 not_allowed_override.
    static const std::vector<std::string>& labor_contract_allowed_extra_fields() {
        static const std::vector<std::string> kAllowed = {
            "work_schedule", "probation_months", "employer.director", "employer.address", "employee.address"};
        return kAllowed;
    }

    /// Shared tail for both generate-document handlers, factored out after
    /// Fix round 1 (the two handlers used to duplicate ~80 lines of this).
    /// @p input must already have its allowlisted extras merged in (see
    /// Api::Validation::merge_allowed_extra()) — this only validates against
    /// @p slug's
    /// template schema, creates a draft document (doc_type="hr" — see file
    /// header for why this is NOT simply @p slug), best-effort enqueues
    /// docgen.render, and responds 202 — the same async contract as
    /// POST /documents/generate. @p after_create (may be an empty
    /// std::function) runs once the document exists but BEFORE the job is
    /// enqueued; only hr-orders' caller uses it, to attach_document() back
    /// onto the source order — labor_contracts has no document_id column.
    static void finish_generate_document(const std::function<void(const HttpResponsePtr&)>& callback,
                                         const std::string& org_id,
                                         const char* slug,
                                         const json& input,
                                         const std::function<void(const std::string& document_id)>& after_create) {
        Docgen::TemplateRegistry registry;
        auto info = registry.latest(slug);
        if (!info) {
            spdlog::error("hr generate-document: template '{}' not found on disk", slug);
            callback(ErrorResponse::internal_error());
            return;
        }
        if (auto err = Docgen::TemplateRegistry::validate(*info, input)) {
            callback(Validation::response_422("input", "schema_validation_failed", *err));
            return;
        }

        API_REQUIRE_JOBS_READY(callback);

        with_repo_errors(callback, "hr generate-document", [&] {
            Ledger::DocumentRepository documents;
            // doc_type is "hr" (the generic bucket), NOT the template slug —
            // see file header for why this is the one place that identity
            // mapping does not hold.
            auto created = documents.create(org_id,
                                            "hr",
                                            "generated",
                                            "draft",
                                            std::nullopt,
                                            info->slug,
                                            info->version_str,
                                            std::optional<nlohmann::json>{input});

            if (after_create)
                after_create(created.id);

            // version_id — see DocgenController::generate: the render lands on
            // the version the payload NAMES, never on "the newest one at the
            // time the worker got round to it".
            auto first_version = documents.latest_version(org_id, created.id, /*from_primary=*/true);
            json payload = {{"org_id", org_id},
                            {"document_id", created.id},
                            {"version_id", first_version ? first_version->id : std::string{}},
                            {"slug", std::string(slug)},
                            {"input", input}};
            bool render_queued = false;
            try {
                auto job = Jobs::get().submit(kRenderJobType, payload);
                spdlog::debug("hr generate-document: document {} enqueued as job {}", created.id, job.id);
                render_queued = true;
            } catch (const std::exception& e) {
                spdlog::error(
                    "hr generate-document: enqueue docgen.render for document {} failed: {}", created.id, e.what());
            }
            callback(Response::accepted({{"document_id", created.id}, {"render_queued", render_queued}}));
        });
    }

    /// `employee_id` query-param filter shared by listOrders()/
    /// listVacations() — nullopt (no filter) when absent, 400 when present
    /// but not uuid-shaped (same structural-vs-semantic split path ids use).
    static bool filter_employee_id(const HttpRequestPtr& req,
                                   std::optional<std::string>& out,
                                   const std::function<void(const HttpResponsePtr&)>& callback) {
        const std::string param = req->getParameter("employee_id");
        if (param.empty())
            return true;
        if (!is_valid_uuid(param)) {
            callback(ErrorResponse::bad_request("invalid_employee_id", "employee_id must be a uuid"));
            return false;
        }
        out = param;
        return true;
    }

    /// labor_contracts has no OrgCrudBase find_in_org (HrRepository's base
    /// parameterization reads hr_orders — see HrRepository.hpp's header);
    /// list_contracts(org_id, employee_id) needs employee_id up front, which
    /// generate-document doesn't have until AFTER it finds the contract. A
    /// tiny direct query is simpler than adding a new bespoke repository
    /// method used by exactly one caller.
    static std::optional<Hr::LaborContract> find_contract(const std::string& org_id, const std::string& id) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Hr::LaborContract> {
            auto r = txn.exec_params("SELECT " + std::string(Hr::HrRepository::kContractColumns) +
                                         " FROM labor_contracts WHERE id = $1 AND org_id = $2",
                                     id,
                                     org_id);
            if (r.empty())
                return std::nullopt;
            return Hr::LaborContract::from_row(r[0]);
        });
    }

    static std::string full_name(const Hr::Employee& e) {
        std::string name = e.last_name + " " + e.first_name;
        if (e.middle_name && !e.middle_name->empty())
            name += " " + *e.middle_name;
        return name;
    }

    /// `YYYY-MM-DD` (already calendar-validated on the way in, or DB-sourced
    /// and therefore already valid) -> `DD.MM.YYYY`, the format both docgen
    /// templates' schemas require for their date fields.
    static std::string iso_to_ddmmyyyy(const std::string& iso) {
        if (iso.size() != 10)
            return iso;
        return iso.substr(8, 2) + "." + iso.substr(5, 2) + "." + iso.substr(0, 4);
    }
};

}  // namespace Api
