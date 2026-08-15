/**
 * @file EmployeesController.hpp
 * @brief Employee (сотрудник) CRUD API — org-scoped (design spec §7.2 /
 *        Task 11).
 *
 * Routes (all under /api/v1, every handler starts with
 * API_REQUIRE_ORG(req, callback, ctx) — same guard order as
 * CounterpartiesController/JournalController):
 *   GET   /api/v1/employees             list, paginated (parse_page_params)
 *   POST  /api/v1/employees             create (accountant/owner only)
 *   GET   /api/v1/employees/{id}        fetch one
 *   PATCH /api/v1/employees/{id}        patch editable fields (accountant/owner only)
 *   POST  /api/v1/employees/{id}/dismiss  {dismissed_on} -> status='dismissed' (accountant/owner only)
 *
 * RBAC: every mutating route (create/patch/dismiss) additionally goes through
 * API_REQUIRE_ORG_PERM for `employees`/write against the §5.3 permission
 * matrix (Tenancy::OrgPerm), which DENIES BY DEFAULT — an unknown role,
 * resource or action is a 403, so a role added later cannot fail open the way
 * the old `ctx.role == "viewer"` denylist let it. Reads are gated in a
 * follow-up. `org_id` comes EXCLUSIVELY from `ctx.org_id`, never the
 * path/body/a query param.
 *
 * Validation split (same 400-shape / 422-value line CounterpartiesController's
 * `identifier` and JournalController's `entry_date`/amount draw):
 *   - `iin`: present + string is a 400; a syntactically-12-digit-CHAR value
 *     that fails Ledger::is_valid_bin_iin's check digit is a 422.
 *   - `salary`: present + string is a 400; a value that fails
 *     Ledger::parse_tiyn() (JournalService.hpp — money is parsed the same
 *     way everywhere in this codebase, not re-implemented here) is a 422.
 *   - `hired_on` (create only) / `dismissed_on` (dismiss only): present +
 *     string is a 400; a value that fails Api::Validation::is_valid_date()
 *     (calendar-valid, not just shape) is a 422.
 *
 * PATCH semantics (design decision this task had to make —
 * Hr::EmployeeRepository::update() deliberately does NOT touch
 * hired_on/status/dismissed_on, see that file's doc comment): a PATCH body
 * that includes any of those three fields (non-null) is rejected with a
 * 422 naming that field, NOT silently ignored. Silently dropping a field the
 * client explicitly sent would let a caller believe it took effect (e.g. a
 * client that PATCHes `{..., "dismissed_on": "2026-01-01"}` expecting a
 * dismissal) when nothing happened — the same "explicit failure over silent
 * no-op" posture this codebase takes everywhere else (e.g. DocumentRepository
 * trusting-but-never-silently-dropping a status value). `hired_on` gets its
 * own code (`immutable_field` — there is no other endpoint that can ever
 * change it) while `status`/`dismissed_on` point the caller at the dismiss
 * endpoint (`use_dismiss_endpoint`) that owns that transition.
 */

#pragma once

#include <functional>
#include <stdexcept>
#include <string>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "hr/Employee.hpp"
#include "hr/EmployeeRepository.hpp"
#include "ledger/JournalService.hpp"
#include "ledger/KzIdentifiers.hpp"
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrgPermissions.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class EmployeesController : public HttpController<EmployeesController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(EmployeesController::list, "/api/v1/employees", Get);
    ADD_METHOD_TO(EmployeesController::create, "/api/v1/employees", Post);
    ADD_METHOD_TO(EmployeesController::get, "/api/v1/employees/{1}", Get);
    ADD_METHOD_TO(EmployeesController::patch, "/api/v1/employees/{1}", Patch);
    ADD_METHOD_TO(EmployeesController::dismiss, "/api/v1/employees/{1}/dismiss", Post);
    METHOD_LIST_END

    // -------------------------------------------------------------------
    // GET /api/v1/employees — paginated, org-scoped. Includes dismissed
    // employees: this is the full roster (OrgCrudBase::list_in_org), which
    // is also the only roster query EmployeeRepository still exposes for
    // "right now" — a client that only wants active staff filters
    // client-side, or a future ?status= filter can be layered on without
    // changing this shape.
    // -------------------------------------------------------------------
    void list(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);

        with_repo_errors(callback, "employees list", [&] {
            Hr::EmployeeRepository repo;
            auto rows = repo.list_in_org(ctx.org_id, page.limit, page.offset);
            long total = repo.count_in_org(ctx.org_id);
            json data = json::array();
            for (const auto& e : rows)
                data.push_back(e);
            callback(Response::paginated(data, total, page.limit, page.offset));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/employees — accountant/owner only.
    // -------------------------------------------------------------------
    void create(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kEmployees, Tenancy::OrgPerm::Action::kWrite);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Hr::Employee draft;
        if (!validate_and_fill_create(body, draft, callback))
            return;

        with_repo_errors(callback, "employees create", [&] {
            Hr::EmployeeRepository repo;
            auto created = repo.create(ctx.org_id, draft);
            callback(Response::created({{"data", json(created)}}));
        });
    }

    // -------------------------------------------------------------------
    // GET /api/v1/employees/{id}
    // -------------------------------------------------------------------
    void get(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed employee id"));
            return;
        }

        with_repo_errors(callback, "employees get", [&] {
            Hr::EmployeeRepository repo;
            auto found = repo.find_in_org(id, ctx.org_id);
            if (!found) {
                callback(ErrorResponse::not_found("employee"));
                return;
            }
            callback(Response::ok({{"data", json(*found)}}));
        });
    }

    // -------------------------------------------------------------------
    // PATCH /api/v1/employees/{id} — patches every column
    // EmployeeRepository::update() allows (everything except
    // hired_on/status/dismissed_on — see file header), accountant/owner only.
    // -------------------------------------------------------------------
    void patch(const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kEmployees, Tenancy::OrgPerm::Action::kWrite);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed employee id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Hr::Employee draft;
        if (!validate_and_fill_update(body, draft, callback))
            return;

        with_repo_errors(callback, "employees patch", [&] {
            Hr::EmployeeRepository repo;
            auto updated = repo.update(ctx.org_id, id, draft);
            if (!updated) {
                callback(ErrorResponse::not_found("employee"));
                return;
            }
            callback(Response::ok({{"data", json(*updated)}}));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/employees/{id}/dismiss — {dismissed_on}, accountant/owner
    // only. Hr::EmployeeRepository::dismiss() returns nullopt for a wrong
    // org, a missing id, AND an already-dismissed employee alike (see that
    // method's doc comment) — all three map to the same 404 here.
    // -------------------------------------------------------------------
    void dismiss(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kEmployees, Tenancy::OrgPerm::Action::kWrite);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed employee id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "dismissed_on");
        if (body.contains("dismissed_on") && !body["dismissed_on"].is_string())
            errs.add("dismissed_on", "not_string", "must be a string");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        const std::string dismissed_on = body["dismissed_on"].get<std::string>();
        if (!Validation::is_valid_date(dismissed_on)) {
            callback(
                Validation::response_422("dismissed_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
            return;
        }

        with_repo_errors(callback, "employees dismiss", [&] {
            Hr::EmployeeRepository repo;
            auto updated = repo.dismiss(ctx.org_id, id, dismissed_on);
            if (!updated) {
                callback(ErrorResponse::not_found("employee"));
                return;
            }
            callback(Response::ok({{"data", json(*updated)}}));
        });
    }

private:
    /// Fields shared by create()/patch() type-checking — everything except
    /// hired_on (create-only; immutable afterwards, see file header).
    static void check_common_shape(Validation::Errors& errs, const json& body) {
        if (body.contains("iin") && !body["iin"].is_string())
            errs.add("iin", "not_string", "must be a string");
        if (body.contains("last_name") && !body["last_name"].is_string())
            errs.add("last_name", "not_string", "must be a string");
        if (body.contains("first_name") && !body["first_name"].is_string())
            errs.add("first_name", "not_string", "must be a string");
        if (body.contains("middle_name") && !body["middle_name"].is_null() && !body["middle_name"].is_string())
            errs.add("middle_name", "not_string", "must be a string");
        if (body.contains("position") && !body["position"].is_string())
            errs.add("position", "not_string", "must be a string");
        if (body.contains("salary") && !body["salary"].is_string())
            errs.add("salary", "not_string", "must be a decimal string, e.g. \"300000.00\"");
        if (body.contains("ipn_deduction_claimed") && !body["ipn_deduction_claimed"].is_boolean())
            errs.add("ipn_deduction_claimed", "not_boolean", "must be a boolean");
        if (body.contains("opvr_exempt") && !body["opvr_exempt"].is_boolean())
            errs.add("opvr_exempt", "not_boolean", "must be a boolean");
        if (body.contains("payout_iik") && !body["payout_iik"].is_string())
            errs.add("payout_iik", "not_string", "must be a string");
    }

    /// Shared 422 semantic checks (iin check digit, salary parse) — same for
    /// create and patch. @return false (already replied) on failure.
    static bool check_common_semantics(const json& body,
                                       Hr::Employee& out,
                                       const std::function<void(const HttpResponsePtr&)>& callback) {
        const std::string iin = body["iin"].get<std::string>();
        if (!Ledger::is_valid_bin_iin(iin)) {
            callback(Validation::response_422("iin", "invalid_iin", "IIN check digit is invalid"));
            return false;
        }
        long long salary_tiyn = 0;
        try {
            salary_tiyn = Ledger::parse_tiyn(body["salary"].get<std::string>());
        } catch (const std::invalid_argument& e) {
            callback(Validation::response_422("salary", "invalid_salary", e.what()));
            return false;
        }

        out.iin = iin;
        out.last_name = body["last_name"].get<std::string>();
        out.first_name = body["first_name"].get<std::string>();
        if (body.contains("middle_name") && !body["middle_name"].is_null())
            out.middle_name = body["middle_name"].get<std::string>();
        out.position = body["position"].get<std::string>();
        out.salary_tiyn = salary_tiyn;
        out.ipn_deduction_claimed = body.value("ipn_deduction_claimed", false);
        out.opvr_exempt = body.value("opvr_exempt", false);
        out.payout_iik = body.value("payout_iik", std::string(""));
        return true;
    }

    /// POST /employees body: {iin, last_name, first_name, middle_name?,
    /// position, salary, hired_on, ipn_deduction_claimed?, opvr_exempt?,
    /// payout_iik?}.
    static bool validate_and_fill_create(const json& body,
                                         Hr::Employee& out,
                                         const std::function<void(const HttpResponsePtr&)>& callback) {
        Validation::Errors errs;
        Validation::require(errs, body, "iin");
        Validation::require(errs, body, "last_name");
        Validation::require(errs, body, "first_name");
        Validation::require(errs, body, "position");
        Validation::require(errs, body, "salary");
        Validation::require(errs, body, "hired_on");
        check_common_shape(errs, body);
        if (body.contains("hired_on") && !body["hired_on"].is_string())
            errs.add("hired_on", "not_string", "must be a string");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return false;
        }

        if (!check_common_semantics(body, out, callback))
            return false;

        const std::string hired_on = body["hired_on"].get<std::string>();
        if (!Validation::is_valid_date(hired_on)) {
            callback(Validation::response_422("hired_on", "invalid_date", "must be a calendar-valid YYYY-MM-DD date"));
            return false;
        }
        out.hired_on = hired_on;
        return true;
    }

    /// PATCH /employees/{id} body: same as create minus hired_on — and
    /// hired_on/status/dismissed_on are actively REJECTED (422) if present,
    /// not silently dropped. See file header for the rationale.
    static bool validate_and_fill_update(const json& body,
                                         Hr::Employee& out,
                                         const std::function<void(const HttpResponsePtr&)>& callback) {
        if (body.contains("hired_on") && !body["hired_on"].is_null()) {
            callback(Validation::response_422(
                "hired_on", "immutable_field", "hired_on is fixed at hire time and cannot be changed"));
            return false;
        }
        if (body.contains("status") && !body["status"].is_null()) {
            callback(Validation::response_422(
                "status", "use_dismiss_endpoint", "status changes only through POST /employees/{id}/dismiss"));
            return false;
        }
        if (body.contains("dismissed_on") && !body["dismissed_on"].is_null()) {
            callback(Validation::response_422("dismissed_on",
                                              "use_dismiss_endpoint",
                                              "dismissed_on changes only through POST /employees/{id}/dismiss"));
            return false;
        }

        Validation::Errors errs;
        Validation::require(errs, body, "iin");
        Validation::require(errs, body, "last_name");
        Validation::require(errs, body, "first_name");
        Validation::require(errs, body, "position");
        Validation::require(errs, body, "salary");
        check_common_shape(errs, body);
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return false;
        }

        return check_common_semantics(body, out, callback);
    }
};

}  // namespace Api
