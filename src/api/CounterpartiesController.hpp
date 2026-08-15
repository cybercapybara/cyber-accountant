/**
 * @file CounterpartiesController.hpp
 * @brief Counterparty (contractor) CRUD API — org-scoped (design spec §6.3 /
 *        Task 12).
 *
 * Routes (all under /api/v1, every handler starts with
 * API_REQUIRE_ORG(req, callback, ctx) — mirrors OrganizationsController's
 * guard order: org context first, `is_valid_uuid` on any path id second):
 *   GET   /api/v1/counterparties        list, paginated (parse_page_params)
 *   POST  /api/v1/counterparties        create (accountant/owner only)
 *   GET   /api/v1/counterparties/{id}   fetch one
 *   PATCH /api/v1/counterparties/{id}   full replace (accountant/owner only)
 *
 * RBAC: EVERY route goes through API_REQUIRE_ORG_PERM against the §5.3
 * permission matrix (Tenancy::OrgPerm), which DENIES BY DEFAULT — an unknown
 * role, resource or action is a 403, so a role added later cannot fail open
 * the way the old `ctx.role == "viewer"` denylist let it. Mutations
 * (POST/PATCH) need `counterparties`/write, the two GETs `counterparties`/
 * read: "—" in that matrix means INVISIBLE, not read-only, so the `hr` role
 * gets a 403 on the list and on a single counterparty alike.
 * `org_id` is taken EXCLUSIVELY from `ctx.org_id` (the access token's
 * membership-backed org claim) — never from the path, body, or a query
 * param; that was the T7 review lesson behind `Files::org_key` trusting its
 * caller, and the same discipline applies here on the read/write side.
 *
 * `identifier` (БИН/ИИН) gets a check-digit pass via Ledger::is_valid_bin_iin
 * on top of CounterpartyRepository/the DB CHECK's 12-digit shape check — a
 * value that's the right shape but the wrong check digit is a 422, same
 * "syntactically valid JSON, semantically invalid value" split
 * OrganizationsController::createOrganization uses for `bin`.
 */

#pragma once

#include <functional>
#include <string>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "ledger/Counterparty.hpp"
#include "ledger/CounterpartyRepository.hpp"
#include "ledger/KzIdentifiers.hpp"
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrgPermissions.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class CounterpartiesController : public HttpController<CounterpartiesController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(CounterpartiesController::list, "/api/v1/counterparties", Get);
    ADD_METHOD_TO(CounterpartiesController::create, "/api/v1/counterparties", Post);
    ADD_METHOD_TO(CounterpartiesController::get, "/api/v1/counterparties/{1}", Get);
    ADD_METHOD_TO(CounterpartiesController::patch, "/api/v1/counterparties/{1}", Patch);
    METHOD_LIST_END

    // -------------------------------------------------------------------
    // GET /api/v1/counterparties — paginated, org-scoped.
    // -------------------------------------------------------------------
    void list(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(
            callback, ctx, Tenancy::OrgPerm::Resource::kCounterparties, Tenancy::OrgPerm::Action::kRead);
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);

        with_repo_errors(callback, "counterparties list", [&] {
            Ledger::CounterpartyRepository repo;
            auto rows = repo.list_in_org(ctx.org_id, page.limit, page.offset);
            long total = repo.count_in_org(ctx.org_id);
            json data = json::array();
            for (const auto& c : rows)
                data.push_back(c);
            callback(Response::paginated(data, total, page.limit, page.offset));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/counterparties — accountant/owner only.
    // -------------------------------------------------------------------
    void create(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(
            callback, ctx, Tenancy::OrgPerm::Resource::kCounterparties, Tenancy::OrgPerm::Action::kWrite);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Ledger::Counterparty draft;
        if (!validate_and_fill(body, draft, callback))
            return;

        with_repo_errors(callback, "counterparties create", [&] {
            Ledger::CounterpartyRepository repo;
            auto created = repo.create(ctx.org_id, draft);
            callback(Response::created({{"data", json(created)}}));
        });
    }

    // -------------------------------------------------------------------
    // GET /api/v1/counterparties/{id}
    // -------------------------------------------------------------------
    void get(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(
            callback, ctx, Tenancy::OrgPerm::Resource::kCounterparties, Tenancy::OrgPerm::Action::kRead);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed counterparty id"));
            return;
        }

        with_repo_errors(callback, "counterparties get", [&] {
            Ledger::CounterpartyRepository repo;
            auto found = repo.find_in_org(id, ctx.org_id);
            if (!found) {
                callback(ErrorResponse::not_found("counterparty"));
                return;
            }
            callback(Response::ok({{"data", json(*found)}}));
        });
    }

    // -------------------------------------------------------------------
    // PATCH /api/v1/counterparties/{id} — full replace (CounterpartyRepository::
    // update() patches every editable column in one statement — there is no
    // partial-field PATCH here), accountant/owner only.
    // -------------------------------------------------------------------
    void patch(const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(
            callback, ctx, Tenancy::OrgPerm::Resource::kCounterparties, Tenancy::OrgPerm::Action::kWrite);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed counterparty id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Ledger::Counterparty draft;
        if (!validate_and_fill(body, draft, callback))
            return;

        with_repo_errors(callback, "counterparties patch", [&] {
            Ledger::CounterpartyRepository repo;
            auto updated = repo.update(ctx.org_id, id, draft);
            if (!updated) {
                callback(ErrorResponse::not_found("counterparty"));
                return;
            }
            callback(Response::ok({{"data", json(*updated)}}));
        });
    }

private:
    /**
     * @brief Shared POST/PATCH body validation. `identifier` and `name` are
     *        required; `address`/`iik`/`bik`/`kbe`/`contact_email` default
     *        to "" and `is_resident`/`vat_payer` to Counterparty's own
     *        defaults (true/false) when absent — same `body.value(field,
     *        default)` idiom OrganizationsController::createOrganization
     *        uses for `tax_regime`/`vat_payer`.
     *
     *        `identifier`'s check-digit failure is reported as a SEPARATE
     *        422 (`Validation::response_422(field, code, message)` — the
     *        single-field semantic-validation helper) rather than folded
     *        into the 400 `errs` collector above — mirrors
     *        OrganizationsController::createOrganization's `bin` handling:
     *        missing/wrong-type is a 400, a syntactically fine value that
     *        fails a semantic check is a 422.
     *
     * @return false (and already replied via @p callback) on any validation
     *         failure; true with @p out filled otherwise.
     */
    static bool validate_and_fill(const json& body,
                                  Ledger::Counterparty& out,
                                  const std::function<void(const HttpResponsePtr&)>& callback) {
        Validation::Errors errs;
        Validation::require(errs, body, "identifier");
        Validation::require(errs, body, "name");
        if (body.contains("identifier") && !body["identifier"].is_string())
            errs.add("identifier", "not_string", "must be a string");
        if (body.contains("name") && !body["name"].is_string())
            errs.add("name", "not_string", "must be a string");
        for (const char* f : {"address", "iik", "bik", "kbe", "contact_email"}) {
            if (body.contains(f) && !body[f].is_string())
                errs.add(f, "not_string", "must be a string");
        }
        if (body.contains("is_resident") && !body["is_resident"].is_boolean())
            errs.add("is_resident", "not_boolean", "must be a boolean");
        if (body.contains("vat_payer") && !body["vat_payer"].is_boolean())
            errs.add("vat_payer", "not_boolean", "must be a boolean");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return false;
        }

        const std::string identifier = body["identifier"].get<std::string>();
        if (!Ledger::is_valid_bin_iin(identifier)) {
            callback(Validation::response_422("identifier", "invalid_identifier", "BIN/IIN check digit is invalid"));
            return false;
        }

        out.identifier = identifier;
        out.name = body["name"].get<std::string>();
        out.address = body.value("address", std::string(""));
        out.iik = body.value("iik", std::string(""));
        out.bik = body.value("bik", std::string(""));
        out.kbe = body.value("kbe", std::string(""));
        out.is_resident = body.value("is_resident", true);
        out.vat_payer = body.value("vat_payer", false);
        out.contact_email = body.value("contact_email", std::string(""));
        return true;
    }
};

}  // namespace Api
