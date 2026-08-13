/**
 * @file OrganizationsController.hpp
 * @brief Organizations (tenants) + membership management API — design spec
 *        §5: tenants are created by a system admin, not self-service signup.
 *
 * Routes (all under /api/v1):
 *   POST   /api/v1/orgs                        create a tenant (admin)
 *   GET    /api/v1/orgs                         list tenants (admin, paginated)
 *   GET    /api/v1/orgs/mine                    the caller's orgs + their role
 *   POST   /api/v1/orgs/{id}/switch              mint an access token scoped to {id}
 *   POST   /api/v1/orgs/{id}/members             add a member (admin or owner)
 *   PATCH  /api/v1/orgs/{id}/members/{user_id}   change a member's role (admin or owner)
 *   DELETE /api/v1/orgs/{id}/members/{user_id}   remove a member (admin or owner)
 *
 * Member-management gate ("admin or owner-of-this-org"): tries the caller's
 * org context first (the access token's `org` claim must already be {id} AND
 * the backing membership role must be "owner" — see
 * require_admin_or_org_owner() below), and falls back to full-admin
 * otherwise. Fail-closed like every other guard in this codebase: a caller
 * who is owner of {id} but hasn't switched into it (org claim points
 * elsewhere, or is empty) is NOT treated as authorized here — only as admin,
 * if they happen to also be one.
 */

#pragma once

#include <optional>
#include <regex>
#include <string>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Audit.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrgMember.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/Organization.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "utils/ErrorResponse.hpp"
#include "utils/Time.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class OrganizationsController : public HttpController<OrganizationsController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(OrganizationsController::createOrganization, "/api/v1/orgs", Post);
    ADD_METHOD_TO(OrganizationsController::listOrganizations, "/api/v1/orgs", Get);
    ADD_METHOD_TO(OrganizationsController::mine, "/api/v1/orgs/mine", Get);
    ADD_METHOD_TO(OrganizationsController::switchOrg, "/api/v1/orgs/{1}/switch", Post);
    ADD_METHOD_TO(OrganizationsController::addMember, "/api/v1/orgs/{1}/members", Post);
    ADD_METHOD_TO(OrganizationsController::updateMemberRole, "/api/v1/orgs/{1}/members/{2}", Patch);
    ADD_METHOD_TO(OrganizationsController::removeMember, "/api/v1/orgs/{1}/members/{2}", Delete);
    METHOD_LIST_END

    // ---------------------------------------------------------------------
    // POST /api/v1/orgs
    //
    // Body: { bin, name, tax_regime?, vat_payer? }. Admin-gated — design
    // spec §5: tenants are provisioned by a system admin. BIN validation in
    // v1 is format-only (exactly 12 digits); the check-digit algorithm is
    // P1 work shared with ledger/counterparties.
    // ---------------------------------------------------------------------
    void createOrganization(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "bin");
        Validation::require(errs, body, "name");
        if (body.contains("bin") && !body["bin"].is_string())
            errs.add("bin", "not_string", "must be a string");
        if (body.contains("vat_payer") && !body["vat_payer"].is_boolean())
            errs.add("vat_payer", "not_boolean", "must be a boolean");
        Validation::one_of(errs, body, "tax_regime", {"snr_simplified", "standard"});
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        // BIN format: exactly 12 digits (^[0-9]{12}$). A syntactically-valid
        // JSON string that doesn't match the shape is a 422 (semantically
        // invalid value), not a 400 (malformed request) — kept distinct from
        // the missing/wrong-type checks above via a dedicated Errors/422
        // response rather than Validation::response_400.
        static const std::regex kBinFormat(R"(^[0-9]{12}$)");
        const std::string bin = body["bin"].get<std::string>();
        if (!std::regex_match(bin, kBinFormat)) {
            Validation::Errors bin_errs;
            bin_errs.add("bin", "invalid_bin", "BIN must be exactly 12 digits");
            // ErrorResponse::unprocessable() takes no extra-fields param — go
            // through make() directly so the `errors` array (built via
            // Api::Validation, per this task's decision) rides along on the 422.
            callback(ErrorResponse::make(
                {drogon::k422UnprocessableEntity, "validation_failed", "", json{{"errors", bin_errs.errors_json()}}}));
            return;
        }

        const std::string tax_regime = body.value("tax_regime", std::string("snr_simplified"));
        const bool vat_payer = body.value("vat_payer", false);

        with_repo_errors(callback, "orgs createOrganization", [&] {
            Tenancy::OrganizationRepository orgs;
            auto created = orgs.create(bin, body["name"].get<std::string>(), tax_regime, vat_payer);
            Security::Audit::record(actor_of(req), "org.create", "organization", created.id, {{"bin", created.bin}});
            callback(Response::created({{"data", json(created)}}));
        });
    }

    // ---------------------------------------------------------------------
    // GET /api/v1/orgs — admin-gated, paginated (same limit/offset contract
    // as AdminController::listUsers).
    // ---------------------------------------------------------------------
    void listOrganizations(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ADMIN(req, callback);
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);

        with_repo_errors(callback, "orgs listOrganizations", [&] {
            Tenancy::OrganizationRepository repo;
            auto orgs = repo.list(page.limit, page.offset);
            long total = repo.count();
            json data = json::array();
            for (const auto& o : orgs)
                data.push_back(o);
            callback(Response::paginated(data, total, page.limit, page.offset));
        });
    }

    // ---------------------------------------------------------------------
    // GET /api/v1/orgs/mine — any authenticated user; the organizations they
    // belong to, each annotated with their role in it.
    // ---------------------------------------------------------------------
    void mine(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_PRINCIPAL(req, callback, principal);

        with_repo_errors(callback, "orgs mine", [&] {
            Tenancy::OrgMemberRepository members;
            auto memberships = members.list_for_user(principal->subject);
            Tenancy::OrganizationRepository orgs;
            json data = json::array();
            for (const auto& m : memberships) {
                auto org = orgs.find(m.org_id);
                if (!org)
                    continue;  // FK guarantees this can't happen; stay defensive anyway
                json item = json(*org);
                item["role"] = m.role;
                data.push_back(item);
            }
            callback(Response::list(data));
        });
    }

    // ---------------------------------------------------------------------
    // POST /api/v1/orgs/{id}/switch
    //
    // Any authenticated member of {id} gets a freshly-minted access token
    // whose claims are IDENTICAL in shape to AuthController::mint_session's
    // access token (sub/iat/exp/typ/confirmed/permissions/roles-claim/iss/
    // aud), plus `org: {id}`. This is a deliberately SEPARATE mint path, not
    // a call into mint_session — mint_session's single-membership heuristic
    // decides the org claim for login/refresh only; here the org is
    // explicit and membership-checked against the path id. The refresh
    // token/cookie is never touched by this endpoint.
    //
    // Consequence documented for the frontend (Task 8): after the next
    // POST /api/v1/auth/refresh, the active org resets to mint_session's
    // single-membership default (empty claim if the user belongs to 0 or
    // >1 orgs) — a client with multiple organizations must repeat /switch
    // after every refresh to stay scoped to a non-default org.
    // ---------------------------------------------------------------------
    void switchOrg(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   const std::string& id) {
        API_REQUIRE_PRINCIPAL(req, callback, principal);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed organization id"));
            return;
        }

        with_repo_errors(callback, "orgs switchOrg", [&] {
            Tenancy::OrgMemberRepository members;
            if (!members.find_membership(id, principal->subject)) {
                callback(ErrorResponse::forbidden("not_a_member", "You are not a member of this organization"));
                return;
            }

            Repositories::UserRepository users;
            auto user = users.find(principal->subject);
            if (!user) {
                callback(ErrorResponse::unauthorized("user_gone"));
                return;
            }

            const auto& cfg = Security::Auth::get().config();
            if (cfg.jwt_secret.empty()) {
                spdlog::error("orgs switchOrg: JWT_SECRET unset");
                callback(ErrorResponse::service_unavailable("session_unavailable", "Could not mint session"));
                return;
            }
            const long now = Utils::Time::now_epoch_seconds();

            json roles_array = json::array();
            if (user->role)
                roles_array.push_back(user->role->name);
            const std::uint32_t perm_bits = user->role ? user->role->permissions : 0u;

            // Same claim set as AuthController::mint_session's access token
            // (see the file comment above for why this isn't a shared call).
            json access_claims = {
                {"sub", user->id},
                {"iat", now},
                {"exp", now + cfg.cookies.access_ttl_sec},
                {"typ", "access"},
                {"confirmed", user->confirmed},
                {"permissions", perm_bits},
                {cfg.jwt_roles_claim, roles_array},
                {"org", id},
            };
            if (!cfg.jwt_issuer.empty())
                access_claims["iss"] = cfg.jwt_issuer;
            if (!cfg.jwt_audience.empty())
                access_claims["aud"] = cfg.jwt_audience;

            const std::string access = Security::Auth::issue_hs256_jwt(access_claims, cfg.jwt_secret);
            Security::Audit::record(actor_of(req), "org.switch", "organization", id);
            callback(Response::ok({{"access", access}}));
        });
    }

    // ---------------------------------------------------------------------
    // POST /api/v1/orgs/{id}/members — { user_id, role }.
    // ---------------------------------------------------------------------
    void addMember(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   const std::string& id) {
        if (auto err = require_admin_or_org_owner(req, id)) {
            callback(err);
            return;
        }
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed organization id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "user_id");
        Validation::require(errs, body, "role");
        Validation::uuid(errs, body, "user_id");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        if (!body["role"].is_string() || !Tenancy::is_valid_role(body["role"].get<std::string>())) {
            Validation::Errors role_errs;
            role_errs.add("role", "not_allowed", "must be one of: 'owner' 'accountant' 'viewer'");
            callback(Validation::response_400(role_errs));
            return;
        }

        with_repo_errors(callback, "orgs addMember", [&] {
            Tenancy::OrgMemberRepository members;
            auto created = members.add(id, body["user_id"].get<std::string>(), body["role"].get<std::string>());
            Security::Audit::record(actor_of(req),
                                    "org.member.add",
                                    "organization",
                                    id,
                                    {{"user_id", created.user_id}, {"role", created.role}});
            callback(Response::created({{"data", json(created)}}));
        });
    }

    // ---------------------------------------------------------------------
    // PATCH /api/v1/orgs/{id}/members/{user_id} — { role }.
    //
    // Last-owner protection: demoting the organization's sole owner is a
    // 409, counted via a fresh list_members() scan right before the write.
    // Race window: this count-then-mutate is NOT wrapped in one transaction
    // — OrgMemberRepository (Task 4) gives each call its own transaction and
    // doesn't expose a combined read+write one, so two concurrent demotions
    // of two different "last" owners of a two-owner org could both observe
    // count()==2 and both succeed, leaving zero owners. Closing this fully
    // needs a repository-level change (e.g. a single SQL statement with a
    // subquery guard); tracked as a known gap rather than blocking this task.
    // ---------------------------------------------------------------------
    void updateMemberRole(const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback,
                          const std::string& id,
                          const std::string& user_id) {
        if (auto err = require_admin_or_org_owner(req, id)) {
            callback(err);
            return;
        }
        if (!is_valid_uuid(id) || !is_valid_uuid(user_id)) {
            callback(ErrorResponse::bad_request("invalid_id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "role");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        if (!body["role"].is_string() || !Tenancy::is_valid_role(body["role"].get<std::string>())) {
            Validation::Errors role_errs;
            role_errs.add("role", "not_allowed", "must be one of: 'owner' 'accountant' 'viewer'");
            callback(Validation::response_400(role_errs));
            return;
        }
        const std::string new_role = body["role"].get<std::string>();

        with_repo_errors(callback, "orgs updateMemberRole", [&] {
            Tenancy::OrgMemberRepository members;
            auto target = members.find_membership(id, user_id);
            if (!target) {
                callback(ErrorResponse::not_found("member"));
                return;
            }
            if (target->role == "owner" && new_role != "owner" && count_owners(id) <= 1) {
                callback(ErrorResponse::conflict("last_owner", "Cannot demote the last owner of an organization"));
                return;
            }
            members.set_role(id, user_id, new_role);
            auto fresh = members.find_membership(id, user_id);
            Security::Audit::record(actor_of(req),
                                    "org.member.role_change",
                                    "organization",
                                    id,
                                    {{"user_id", user_id}, {"role", new_role}});
            callback(Response::ok({{"data", json(*fresh)}}));
        });
    }

    // ---------------------------------------------------------------------
    // DELETE /api/v1/orgs/{id}/members/{user_id}
    //
    // Last-owner protection — same race-window caveat as updateMemberRole
    // above (count_owners() then remove(), two separate transactions).
    // ---------------------------------------------------------------------
    void removeMember(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& id,
                      const std::string& user_id) {
        if (auto err = require_admin_or_org_owner(req, id)) {
            callback(err);
            return;
        }
        if (!is_valid_uuid(id) || !is_valid_uuid(user_id)) {
            callback(ErrorResponse::bad_request("invalid_id"));
            return;
        }

        with_repo_errors(callback, "orgs removeMember", [&] {
            Tenancy::OrgMemberRepository members;
            auto target = members.find_membership(id, user_id);
            if (!target) {
                callback(ErrorResponse::not_found("member"));
                return;
            }
            if (target->role == "owner" && count_owners(id) <= 1) {
                callback(ErrorResponse::conflict("last_owner", "Cannot remove the last owner of an organization"));
                return;
            }
            members.remove(id, user_id);
            Security::Audit::record(actor_of(req), "org.member.remove", "organization", id, {{"user_id", user_id}});
            callback(Response::ok({{"message", "Member removed"}}));
        });
    }

private:
    /// Acting principal's subject for the audit trail ("" when auth off).
    static std::string actor_of(const HttpRequestPtr& req) {
        auto p = Security::Auth::principal_of(req);
        return p ? p->subject : std::string{};
    }

    /// Member-management gate: the caller's org context must already point
    /// at {org_id} with role "owner" (see the file-level comment), else fall
    /// back to full-admin. Returns nullptr when authorized, an error
    /// response otherwise — same contract as Security::Auth::require_admin.
    static drogon::HttpResponsePtr require_admin_or_org_owner(const HttpRequestPtr& req, const std::string& org_id) {
        auto ctx = Tenancy::org_context_of(req);
        if (ctx && ctx->org_id == org_id && ctx->role == "owner")
            return {};
        return Security::Auth::require_admin(req);
    }

    /// Count of members holding "owner" in {org_id} — used by the
    /// last-owner protections above. See their comments for the race-window
    /// caveat: this is a plain read, not part of the caller's write transaction.
    static int count_owners(const std::string& org_id) {
        Tenancy::OrgMemberRepository members;
        int count = 0;
        for (const auto& m : members.list_members(org_id)) {
            if (m.role == "owner")
                ++count;
        }
        return count;
    }
};

}  // namespace Api
