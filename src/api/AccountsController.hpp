/**
 * @file AccountsController.hpp
 * @brief Chart-of-accounts API — system rows (org_id IS NULL, приказ МФ РК
 *        №185) plus a tenant's own subaccounts (design spec §6.1 / Task 12).
 *
 * Routes (all under /api/v1, every handler starts with
 * API_REQUIRE_ORG(req, callback, ctx)):
 *   GET  /api/v1/accounts   system + own accounts, UNPAGINATED — mirrors
 *                            AccountRepository::list_visible(org_id), which
 *                            takes no limit/offset (same "small, unpaginated
 *                            list" idiom as GET /api/v1/orgs/mine)
 *   POST /api/v1/accounts   create a subaccount (accountant/owner only)
 *
 * There is deliberately no GET /api/v1/accounts/{id} — the brief's Produces
 * list for this controller only calls out list_visible + create_subaccount,
 * and AccountRepository exposes no id-keyed single-row lookup for API
 * purposes (find_visible is keyed by `code`, used internally by
 * create_subaccount's parent check).
 *
 * POST validates the subaccount at the domain level via
 * AccountRepository::create_subaccount (parent visibility + code-extends-
 * parent), which throws Ledger::InvalidSubaccount — NOT a
 * Repositories::RepoError subclass (see that type's own doc comment in
 * AccountRepository.hpp for why) — so this handler catches it explicitly
 * instead of going through Api::with_repo_errors(), which only understands
 * Repositories::ConflictError/NotFoundError/std::exception.
 */

#pragma once

#include <functional>
#include <string>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "ledger/Account.hpp"
#include "ledger/AccountRepository.hpp"
#include "repositories/RepoErrors.hpp"
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrgPermissions.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class AccountsController : public HttpController<AccountsController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AccountsController::list, "/api/v1/accounts", Get);
    ADD_METHOD_TO(AccountsController::create, "/api/v1/accounts", Post);
    METHOD_LIST_END

    // -------------------------------------------------------------------
    // GET /api/v1/accounts — system rows + this org's own subaccounts.
    // -------------------------------------------------------------------
    void list(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);

        with_repo_errors(callback, "accounts list", [&] {
            Ledger::AccountRepository repo;
            auto rows = repo.list_visible(ctx.org_id);
            json data = json::array();
            for (const auto& a : rows)
                data.push_back(a);
            callback(Response::list(data));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/accounts — { code, name_ru, name_kk?, parent_code }.
    // Accountant/owner only. type/currency_tracked are NOT accepted — the
    // subaccount always inherits both from its parent (see
    // AccountRepository::create_subaccount's doc comment).
    // -------------------------------------------------------------------
    void create(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kJournal, Tenancy::OrgPerm::Action::kWrite);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "code");
        Validation::require(errs, body, "name_ru");
        Validation::require(errs, body, "parent_code");
        if (body.contains("code") && !body["code"].is_string())
            errs.add("code", "not_string", "must be a string");
        if (body.contains("name_ru") && !body["name_ru"].is_string())
            errs.add("name_ru", "not_string", "must be a string");
        if (body.contains("parent_code") && !body["parent_code"].is_string())
            errs.add("parent_code", "not_string", "must be a string");
        if (body.contains("name_kk") && !body["name_kk"].is_string())
            errs.add("name_kk", "not_string", "must be a string");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        const std::string code = body["code"].get<std::string>();
        const std::string name_ru = body["name_ru"].get<std::string>();
        const std::string name_kk = body.value("name_kk", std::string(""));
        const std::string parent_code = body["parent_code"].get<std::string>();

        // Not routed through Api::with_repo_errors() — see file header:
        // Ledger::InvalidSubaccount is a plain std::runtime_error, and that
        // helper's generic std::exception branch would otherwise flatten it
        // into a 500 instead of the 422 it actually is.
        Ledger::AccountRepository repo;
        try {
            auto created = repo.create_subaccount(ctx.org_id, code, name_ru, name_kk, parent_code);
            callback(Response::created({{"data", json(created)}}));
        } catch (const Ledger::InvalidSubaccount& e) {
            callback(ErrorResponse::unprocessable("invalid_subaccount", e.what()));
        } catch (const Repositories::ConflictError& e) {
            callback(ErrorResponse::conflict(e.code(), e.message()));
        } catch (const std::exception& e) {
            spdlog::error("accounts create failed: {}", e.what());
            callback(ErrorResponse::internal_error());
        }
    }
};

}  // namespace Api
