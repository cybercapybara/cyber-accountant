/**
 * @file HrController.hpp
 * @brief HR orders / labor contracts / vacations API — org-scoped (design
 *        spec §7.2 / Task 11), plus the two "generate the кадровый document
 *        from this record" convenience endpoints.
 *
 * Routes (all under /api/v1, every handler starts with
 * API_REQUIRE_ORG(req, callback, ctx)):
 *   GET  /api/v1/hr-orders                         list (optional ?employee_id)
 *   POST /api/v1/hr-orders                          create (accountant/owner)
 *   POST /api/v1/hr-orders/{id}/generate-document    -> hr_order docgen, 202
 *   GET  /api/v1/labor-contracts                    list (?employee_id REQUIRED
 *                                                     — Hr::HrRepository::
 *                                                     list_contracts takes no
 *                                                     "every employee" mode,
 *                                                     unlike list_orders/
 *                                                     list_vacations)
 *   POST /api/v1/labor-contracts                    create (accountant/owner)
 *   POST /api/v1/labor-contracts/{id}/generate-document -> labor_contract docgen, 202
 *   GET  /api/v1/vacations                          list (optional ?employee_id)
 *   POST /api/v1/vacations                          create (accountant/owner)
 *
 * RBAC: every mutating route (create*/generate-document) additionally
 * rejects `ctx.role == "viewer"` with 403. `org_id` comes EXCLUSIVELY from
 * `ctx.org_id`.
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
 * `employer.address`, `employee.address`, `salary_words`, `work_schedule`,
 * `probation_months`. There is no organization "director" field
 * (Tenancy::Organization) and no money-to-words converter anywhere in this
 * codebase, so those cannot be auto-derived. Each generate-document handler
 * therefore: (1) builds a BASE input object from what IS on file (the
 * order/contract row, the referenced employee, the organization's
 * name/bin), (2) deep-merges an OPTIONAL request body on top via
 * `nlohmann::json::merge_patch` (RFC 7396) — an empty/absent body is fine,
 * it just means "use only what's on file" — so a caller supplies exactly
 * the handful of free-text fields the schema needs and nothing else, and
 * (3) validates the merged result against the template's JSON Schema
 * exactly like `POST /documents/generate` (DocgenController) does — a
 * caller that omits a required free-text field gets the same
 * `422 schema_validation_failed` DocgenController already produces for a
 * malformed `input`, not a new error shape.
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
#include "tenancy/OrgContext.hpp"
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
        // GET /api/v1/hr-orders — unpaginated (HrRepository::list_orders has no
        // limit/offset — same "bespoke non-CrudBase query" shape as
        // DocgenController::listTemplates), optional ?employee_id filter.
        // -------------------------------------------------------------------
        void listOrders(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            API_REQUIRE_ORG(req, callback, ctx);
            std::optional<std::string> employee_id_filter;
            if (!filter_employee_id(req, employee_id_filter, callback))
                return;

            with_repo_errors(callback, "hr orders list", [&] {
                Hr::HrRepository repo;
                auto rows = repo.list_orders(ctx.org_id, employee_id_filter);
                json data = json::array();
                for (const auto& o : rows)
                    data.push_back(o);
                callback(Response::list(data));
            });
        }

        // -------------------------------------------------------------------
        // POST /api/v1/hr-orders — accountant/owner only. Body: {employee_id,
        // kind, number, issued_on, effective_from, effective_to?, payload?}.
        // -------------------------------------------------------------------
        void createOrder(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            API_REQUIRE_ORG(req, callback, ctx);
            if (ctx.role == "viewer") {
                callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot create HR orders"));
                return;
            }
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
                callback(
                    Validation::response_422("issued_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
                return;
            }
            const std::string effective_from = body["effective_from"].get<std::string>();
            if (!Validation::is_valid_date(effective_from)) {
                callback(Validation::response_422(
                    "effective_from", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
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
            if (ctx.role == "viewer") {
                callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot generate documents"));
                return;
            }
            if (!is_valid_uuid(id)) {
                callback(ErrorResponse::bad_request("invalid_id", "Malformed HR order id"));
                return;
            }
            json extra;
            if (!parse_optional_body(req, extra, callback))
                return;

            Hr::HrRepository orders;
            auto order = orders.find_in_org(id, ctx.org_id);
            if (!order) {
                callback(ErrorResponse::not_found("hr order"));
                return;
            }

            Hr::EmployeeRepository employees;
            auto employee = employees.find_in_org(order->employee_id, ctx.org_id);
            if (!employee) {
                spdlog::error("hr-orders generate-document: order {} references missing employee {} in org {}",
                              id,
                              order->employee_id,
                              ctx.org_id);
                callback(ErrorResponse::internal_error());
                return;
            }

            Tenancy::OrganizationRepository orgs;
            auto org = orgs.find(ctx.org_id);
            if (!org) {
                spdlog::error("hr-orders generate-document: organization {} missing", ctx.org_id);
                callback(ErrorResponse::internal_error());
                return;
            }

            json input = {
                {"kind", order->kind},
                {"number", order->number},
                {"issued_on", iso_to_ddmmyyyy(order->issued_on)},
                {"employer", {{"name", org->name}, {"bin", org->bin}}},
                {"employee",
                 {{"full_name", full_name(*employee)}, {"iin", employee->iin}, {"position", employee->position}}},
                {"effective_from", iso_to_ddmmyyyy(order->effective_from)},
            };
            if (order->effective_to)
                input["effective_to"] = iso_to_ddmmyyyy(*order->effective_to);
            input.merge_patch(extra);

            static const char* kSlug = "hr_order";
            Docgen::TemplateRegistry registry;
            auto info = registry.latest(kSlug);
            if (!info) {
                spdlog::error("hr-orders generate-document: template '{}' not found on disk", kSlug);
                callback(ErrorResponse::internal_error());
                return;
            }
            if (auto err = Docgen::TemplateRegistry::validate(*info, input)) {
                callback(Validation::response_422("input", "schema_validation_failed", *err));
                return;
            }

            API_REQUIRE_JOBS_READY(callback);

            with_repo_errors(callback, "hr orders generate-document", [&] {
                Ledger::DocumentRepository documents;
                // doc_type is "hr" (the generic bucket), NOT the template slug —
                // see file header for why this is the one place that identity
                // mapping does not hold.
                auto created = documents.create(ctx.org_id,
                                                "hr",
                                                "generated",
                                                "draft",
                                                std::nullopt,
                                                info->slug,
                                                info->version_str,
                                                std::optional<nlohmann::json>{input});

                if (!orders.attach_document(ctx.org_id, id, created.id)) {
                    // Pre-checked above (find_in_org succeeded); only a
                    // concurrent delete of the order between that check and here
                    // could land here. Best-effort, same posture as
                    // DocgenController's link_entry() call.
                    spdlog::warn(
                        "hr orders generate-document: attach_document({}, {}) failed after pre-check", id, created.id);
                }

                json payload = {{"org_id", ctx.org_id}, {"document_id", created.id}, {"slug", kSlug}, {"input", input}};
                bool render_queued = false;
                try {
                    auto job = Jobs::get().submit(kRenderJobType, payload);
                    spdlog::debug("hr orders generate-document: document {} enqueued as job {}", created.id, job.id);
                    render_queued = true;
                } catch (const std::exception& e) {
                    spdlog::error("hr orders generate-document: enqueue docgen.render for document {} failed: {}",
                                  created.id,
                                  e.what());
                }
                callback(Response::accepted({{"document_id", created.id}, {"render_queued", render_queued}}));
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
        // POST /api/v1/labor-contracts — accountant/owner only. Body:
        // {employee_id, number, signed_on, starts_on, ends_on?, terms_json?}.
        // -------------------------------------------------------------------
        void createContract(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            API_REQUIRE_ORG(req, callback, ctx);
            if (ctx.role == "viewer") {
                callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot create labor contracts"));
                return;
            }
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
                callback(
                    Validation::response_422("signed_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
                return;
            }
            const std::string starts_on = body["starts_on"].get<std::string>();
            if (!Validation::is_valid_date(starts_on)) {
                callback(
                    Validation::response_422("starts_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
                return;
            }
            std::optional<std::string> ends_on;
            if (body.contains("ends_on") && !body["ends_on"].is_null()) {
                ends_on = body["ends_on"].get<std::string>();
                if (!Validation::is_valid_date(*ends_on)) {
                    callback(Validation::response_422(
                        "ends_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
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
            if (ctx.role == "viewer") {
                callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot generate documents"));
                return;
            }
            if (!is_valid_uuid(id)) {
                callback(ErrorResponse::bad_request("invalid_id", "Malformed labor contract id"));
                return;
            }
            json extra;
            if (!parse_optional_body(req, extra, callback))
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

            Hr::EmployeeRepository employees;
            auto employee = employees.find_in_org(found_contract->employee_id, ctx.org_id);
            if (!employee) {
                spdlog::error("labor-contracts generate-document: contract {} references missing employee {} in org {}",
                              id,
                              found_contract->employee_id,
                              ctx.org_id);
                callback(ErrorResponse::internal_error());
                return;
            }

            Tenancy::OrganizationRepository orgs;
            auto org = orgs.find(ctx.org_id);
            if (!org) {
                spdlog::error("labor-contracts generate-document: organization {} missing", ctx.org_id);
                callback(ErrorResponse::internal_error());
                return;
            }

            json input = {
                {"number", found_contract->number},
                {"signed_on", iso_to_ddmmyyyy(found_contract->signed_on)},
                {"employer", {{"name", org->name}, {"bin", org->bin}}},
                {"employee",
                 {{"full_name", full_name(*employee)}, {"iin", employee->iin}, {"position", employee->position}}},
                {"salary_tenge", Ledger::format_tiyn(employee->salary_tiyn)},
                {"starts_on", iso_to_ddmmyyyy(found_contract->starts_on)},
            };
            if (found_contract->ends_on)
                input["ends_on"] = iso_to_ddmmyyyy(*found_contract->ends_on);
            input.merge_patch(extra);

            static const char* kSlug = "labor_contract";
            Docgen::TemplateRegistry registry;
            auto info = registry.latest(kSlug);
            if (!info) {
                spdlog::error("labor-contracts generate-document: template '{}' not found on disk", kSlug);
                callback(ErrorResponse::internal_error());
                return;
            }
            if (auto err = Docgen::TemplateRegistry::validate(*info, input)) {
                callback(Validation::response_422("input", "schema_validation_failed", *err));
                return;
            }

            API_REQUIRE_JOBS_READY(callback);

            with_repo_errors(callback, "labor contracts generate-document", [&] {
                Ledger::DocumentRepository documents;
                auto created = documents.create(ctx.org_id,
                                                "hr",
                                                "generated",
                                                "draft",
                                                std::nullopt,
                                                info->slug,
                                                info->version_str,
                                                std::optional<nlohmann::json>{input});

                json payload = {{"org_id", ctx.org_id}, {"document_id", created.id}, {"slug", kSlug}, {"input", input}};
                bool render_queued = false;
                try {
                    auto job = Jobs::get().submit(kRenderJobType, payload);
                    spdlog::debug(
                        "labor contracts generate-document: document {} enqueued as job {}", created.id, job.id);
                    render_queued = true;
                } catch (const std::exception& e) {
                    spdlog::error("labor contracts generate-document: enqueue docgen.render for document {} failed: {}",
                                  created.id,
                                  e.what());
                }
                callback(Response::accepted({{"document_id", created.id}, {"render_queued", render_queued}}));
            });
        }

        // ===================================================================
        // vacations
        // ===================================================================

        // -------------------------------------------------------------------
        // GET /api/v1/vacations — unpaginated, optional ?employee_id filter.
        // -------------------------------------------------------------------
        void listVacations(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            API_REQUIRE_ORG(req, callback, ctx);
            std::optional<std::string> employee_id_filter;
            if (!filter_employee_id(req, employee_id_filter, callback))
                return;

            with_repo_errors(callback, "vacations list", [&] {
                Hr::HrRepository repo;
                auto rows = repo.list_vacations(ctx.org_id, employee_id_filter);
                json data = json::array();
                for (const auto& v : rows)
                    data.push_back(v);
                callback(Response::list(data));
            });
        }

        // -------------------------------------------------------------------
        // POST /api/v1/vacations — accountant/owner only. Body: {employee_id,
        // starts_on, ends_on, days, kind}.
        // -------------------------------------------------------------------
        void createVacation(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
            API_REQUIRE_ORG(req, callback, ctx);
            if (ctx.role == "viewer") {
                callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot create vacations"));
                return;
            }
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
                callback(
                    Validation::response_422("starts_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
                return;
            }
            const std::string ends_on = body["ends_on"].get<std::string>();
            if (!Validation::is_valid_date(ends_on)) {
                callback(
                    Validation::response_422("ends_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
                return;
            }
            // ISO YYYY-MM-DD strings compare lexicographically the same as
            // chronologically — pre-checked here (422) so a violation of
            // migrations/012_hr.sql's CHECK (ends_on >= starts_on) never reaches
            // the database as a raw, 500-mapped SQLSTATE 23514.
            if (ends_on < starts_on) {
                callback(
                    Validation::response_422("ends_on", "before_starts_on", "ends_on must be on or after starts_on"));
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

        /// Parse an OPTIONAL JSON object body for the generate-document
        /// endpoints — an empty body means "no extra fields to merge", not an
        /// error (unlike Validation::parse_body, which treats an empty body as
        /// invalid JSON).
        static bool parse_optional_body(const HttpRequestPtr& req,
                                        json& out,
                                        std::function<void(const HttpResponsePtr&)>& callback) {
            if (req->body().empty()) {
                out = json::object();
                return true;
            }
            if (!Validation::parse_body(req, out, callback))
                return false;
            if (!out.is_object()) {
                callback(ErrorResponse::bad_request("invalid_json", "body must be a JSON object"));
                return false;
            }
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
