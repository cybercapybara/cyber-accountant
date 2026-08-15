/**
 * @file test_organizations_api.cpp
 * @brief Integration tests for OrganizationsController — tenant CRUD,
 *        membership management, and the org-switch access-token mint.
 *
 * Follows the direct-controller-invocation idiom of test_admin_flow.cpp
 * (stamp a hand-built AuthPrincipal onto a bare request, call the handler,
 * assert on the response) plus the membership-seeding idiom of
 * test_org_context.cpp / test_tenancy_repositories.cpp. SwitchIssuesOrgToken
 * additionally decodes the minted token with the real
 * Security::Auth::verify_hs256_jwt() to prove the `org` claim round-trips.
 */

#include <optional>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/OrganizationsController.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-organizations-api-padding";

class OrganizationsApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::OrganizationsController orgs_ctrl;

    std::string config_file_name() const override { return "organizations_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        // Cookie mode on, same as AuthFlowTest::config_overrides — switchOrg
        // must set the __Host-access cookie (fix round 1), so the suite
        // needs to run under the same cookie config production does.
        cfg["auth"]["cookies"]["enabled"] = true;
        cfg["auth"]["cookies"]["secure"] = false;  // no https in the test container
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // Centralized org-data wipe (TestHelpers::wipe_org_data(), in
        // test_helpers.hpp) — see its Doxygen comment for why it TRUNCATEs
        // the journal/document tables before a plain DELETE on organizations.
        // TRUNCATE users CASCADE stays local to this fixture (unaffected by
        // the centralization — no table on the accounts/organizations side
        // references users).
        TestHelpers::wipe_org_data();
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
    }

    /// Create a user with the named app role and return both the row and a
    /// pre-built principal — mirrors AdminFlowTest::seed_user in
    /// test_admin_flow.cpp. `org` is left empty; org-scoped tests set it
    /// via with_org() below once a membership row exists.
    struct Pair {
        Domain::User user;
        Security::Auth::AuthPrincipal principal;
    };

    Pair seed_user(const std::string& email, const std::string& role_name = "User") {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name(role_name);
        if (!role) {
            ADD_FAILURE() << "role " << role_name << " missing — seed migration?";
            throw std::runtime_error("seed role missing: " + role_name);
        }
        auto created = users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
        auto fresh = users.find(created.id);
        Pair p;
        p.user = fresh ? *fresh : created;
        p.principal.subject = p.user.id;
        if (p.user.role)
            p.principal.roles.push_back(p.user.role->name);
        // Mirrors what AuthController::mint_session would put in claims.
        p.principal.raw_claims = json{{"sub", p.user.id}, {"permissions", p.user.role ? p.user.role->permissions : 0u}};
        return p;
    }

    /// Stamp the `org` claim onto an already-built principal — the piece
    /// org_context_of() reads to resolve the caller's membership. Returns a
    /// copy so call sites can chain: `with_org(seed_user(...), org.id)`.
    static Security::Auth::AuthPrincipal with_org(Security::Auth::AuthPrincipal p, const std::string& org_id) {
        p.org = org_id;
        return p;
    }

    Tenancy::Organization seed_org(const std::string& bin, const std::string& name) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, name, "snr_simplified", false);
    }

    static HttpRequestPtr authed(const Security::Auth::AuthPrincipal& p, HttpMethod method = Get) {
        return TestHelpers::authed(p, method);
    }

    static HttpRequestPtr authed_json(const Security::Auth::AuthPrincipal& p,
                                      const json& body,
                                      HttpMethod method = Post) {
        return TestHelpers::authed_json(p, body, method);
    }
};

// ── Organization CRUD (admin-gated) ─────────────────────────────────────────

TEST_F(OrganizationsApiTest, AdminCreatesOrganization) {
    auto admin = seed_user("admin@example.com", "Administrator");
    auto req = authed_json(admin.principal, {{"bin", "111240000001"}, {"name", "Cyber Capybara LLP"}});
    HttpResponsePtr resp;
    orgs_ctrl.createOrganization(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k201Created);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["bin"].get<std::string>(), "111240000001");
    EXPECT_EQ(body["data"]["name"].get<std::string>(), "Cyber Capybara LLP");

    Tenancy::OrganizationRepository repo;
    auto found = repo.find(body["data"]["id"].get<std::string>());
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->bin, "111240000001");
}

// Regression for the "org creator becomes its owner" hotfix: an admin who
// provisions a tenant must land in org_members as "owner" for it, or they're
// stuck outside the org they just created — unable to reach any of the
// owner-gated member-management routes for it. Checks both surfaces a
// frontend would use to notice: GET /orgs/mine (role annotation) and
// GET /orgs/{id}/members (roster), the latter exercised via the same
// admin-fallback path require_admin_or_org_owner() falls back to.
TEST_F(OrganizationsApiTest, AdminCreatingOrgBecomesOwner) {
    auto admin = seed_user("owner-admin@example.com", "Administrator");
    auto req = authed_json(admin.principal, {{"bin", "111240000012"}, {"name", "Newly Owned LLP"}});
    HttpResponsePtr create_resp;
    orgs_ctrl.createOrganization(req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_NE(create_resp, nullptr);
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto create_body = json::parse(std::string(create_resp->body()));
    const std::string org_id = create_body["data"]["id"].get<std::string>();

    // The membership row exists directly.
    Tenancy::OrgMemberRepository members;
    auto membership = members.find_membership(org_id, admin.user.id);
    ASSERT_TRUE(membership.has_value());
    EXPECT_EQ(membership->role, "owner");

    // GET /orgs/mine surfaces it with the right role.
    HttpResponsePtr mine_resp;
    orgs_ctrl.mine(authed(admin.principal), [&](const HttpResponsePtr& r) { mine_resp = r; });
    ASSERT_NE(mine_resp, nullptr);
    ASSERT_EQ(mine_resp->statusCode(), k200OK);
    auto mine_body = json::parse(std::string(mine_resp->body()));
    ASSERT_TRUE(mine_body["data"].is_array());
    bool found_in_mine = false;
    for (const auto& item : mine_body["data"]) {
        if (item["id"].get<std::string>() == org_id) {
            EXPECT_EQ(item["role"].get<std::string>(), "owner");
            found_in_mine = true;
        }
    }
    EXPECT_TRUE(found_in_mine);

    // GET /orgs/{id}/members shows the admin as owner of the org they made
    // (admin-fallback path, since the admin never switched into this org).
    HttpResponsePtr roster_resp;
    orgs_ctrl.listMembers(
        authed(admin.principal), [&](const HttpResponsePtr& r) { roster_resp = r; }, org_id);
    ASSERT_NE(roster_resp, nullptr);
    ASSERT_EQ(roster_resp->statusCode(), k200OK);
    auto roster_body = json::parse(std::string(roster_resp->body()));
    ASSERT_TRUE(roster_body["data"].is_array());
    ASSERT_EQ(roster_body["data"].size(), 1U);
    EXPECT_EQ(roster_body["data"][0]["user_id"].get<std::string>(), admin.user.id);
    EXPECT_EQ(roster_body["data"][0]["role"].get<std::string>(), "owner");
}

TEST_F(OrganizationsApiTest, NonAdminCannotCreate) {
    auto user = seed_user("user@example.com", "User");
    auto req = authed_json(user.principal, {{"bin", "111240000002"}, {"name", "Nope LLP"}});
    HttpResponsePtr resp;
    orgs_ctrl.createOrganization(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(OrganizationsApiTest, InvalidBinRejected) {
    auto admin = seed_user("admin2@example.com", "Administrator");
    auto req = authed_json(admin.principal, {{"bin", "12345"}, {"name", "Bad Bin LLP"}});
    HttpResponsePtr resp;
    orgs_ctrl.createOrganization(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "validation_failed");
    ASSERT_TRUE(body["errors"].is_array());
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "bin");
}

TEST_F(OrganizationsApiTest, DuplicateBinConflict) {
    auto admin = seed_user("admin3@example.com", "Administrator");
    auto req1 = authed_json(admin.principal, {{"bin", "111240000003"}, {"name", "First LLP"}});
    HttpResponsePtr resp1;
    orgs_ctrl.createOrganization(req1, [&](const HttpResponsePtr& r) { resp1 = r; });
    ASSERT_EQ(resp1->statusCode(), k201Created);

    auto req2 = authed_json(admin.principal, {{"bin", "111240000003"}, {"name", "Second LLP"}});
    HttpResponsePtr resp2;
    orgs_ctrl.createOrganization(req2, [&](const HttpResponsePtr& r) { resp2 = r; });
    ASSERT_NE(resp2, nullptr);
    EXPECT_EQ(resp2->statusCode(), k409Conflict);
    auto body = json::parse(std::string(resp2->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "bin_taken");
}

// ── /orgs/mine ───────────────────────────────────────────────────────────────

TEST_F(OrganizationsApiTest, MineListsMembershipsWithRole) {
    auto user = seed_user("member@example.com", "User");
    auto org = seed_org("111240000004", "Member Org LLP");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, user.user.id, "accountant");

    HttpResponsePtr resp;
    orgs_ctrl.mine(authed(user.principal), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    ASSERT_TRUE(body["data"].is_array());
    ASSERT_EQ(body["data"].size(), 1U);
    EXPECT_EQ(body["data"][0]["id"].get<std::string>(), org.id);
    EXPECT_EQ(body["data"][0]["role"].get<std::string>(), "accountant");
}

// ── /orgs/{id}/switch ────────────────────────────────────────────────────────

TEST_F(OrganizationsApiTest, SwitchIssuesOrgToken) {
    auto user = seed_user("switcher@example.com", "User");
    auto org = seed_org("111240000005", "Switch Org LLP");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, user.user.id, "viewer");

    auto req = authed(user.principal, Post);
    HttpResponsePtr resp;
    orgs_ctrl.switchOrg(
        req, [&](const HttpResponsePtr& r) { resp = r; }, org.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    ASSERT_TRUE(body.contains("access"));

    std::string err;
    auto claims = Security::Auth::verify_hs256_jwt(body["access"].get<std::string>(), kSecret, err);
    ASSERT_TRUE(claims.has_value()) << "decode failed: " << err;
    EXPECT_EQ((*claims).value("org", ""), org.id);
    EXPECT_EQ((*claims).value("sub", ""), user.user.id);
    EXPECT_EQ((*claims).value("typ", ""), "access");

    // Fix round 1: cookie-mode clients read the access token from the
    // HttpOnly __Host-access cookie, not the JSON body — switchOrg must set
    // it or a browser session under cookies.enabled=true keeps sending the
    // OLD access cookie and the switch silently doesn't take effect. Mirrors
    // AuthFlowTest::loginSucceedsAndSetsCookies' idiom (resp->cookies(), one
    // Set-Cookie line per entry via addCookie()). Exactly one cookie — the
    // refresh cookie must stay untouched (empty refresh_token passed to
    // set_session_cookies).
    const auto& cookies = resp->cookies();
    ASSERT_EQ(cookies.size(), 1u);
    const auto& access_cookie_name = Security::Auth::get().config().cookies.access_name;
    auto cookie_it = cookies.find(access_cookie_name);
    ASSERT_NE(cookie_it, cookies.end()) << "missing cookie " << access_cookie_name;
    EXPECT_EQ(cookie_it->second.value(), body["access"].get<std::string>());
    EXPECT_TRUE(cookie_it->second.isHttpOnly());
}

TEST_F(OrganizationsApiTest, SwitchForbiddenForNonMember) {
    auto user = seed_user("outsider@example.com", "User");
    auto org = seed_org("111240000006", "Outsider Org LLP");

    auto req = authed(user.principal, Post);
    HttpResponsePtr resp;
    orgs_ctrl.switchOrg(
        req, [&](const HttpResponsePtr& r) { resp = r; }, org.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

// ── Member management (admin or owner) ──────────────────────────────────────

TEST_F(OrganizationsApiTest, OwnerManagesMembers) {
    auto org = seed_org("111240000007", "Owner-Managed Org LLP");
    auto owner = seed_user("owner@example.com", "User");
    auto target = seed_user("target@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, owner.user.id, "owner");
    auto owner_principal = with_org(owner.principal, org.id);

    // Add.
    auto add_req = authed_json(owner_principal, {{"user_id", target.user.id}, {"role", "viewer"}}, Post);
    HttpResponsePtr add_resp;
    orgs_ctrl.addMember(
        add_req, [&](const HttpResponsePtr& r) { add_resp = r; }, org.id);
    ASSERT_NE(add_resp, nullptr);
    EXPECT_EQ(add_resp->statusCode(), k201Created);

    // Change role.
    auto patch_req = authed_json(owner_principal, {{"role", "accountant"}}, Patch);
    HttpResponsePtr patch_resp;
    orgs_ctrl.updateMemberRole(
        patch_req, [&](const HttpResponsePtr& r) { patch_resp = r; }, org.id, target.user.id);
    ASSERT_NE(patch_resp, nullptr);
    EXPECT_EQ(patch_resp->statusCode(), k200OK);
    auto patch_body = json::parse(std::string(patch_resp->body()));
    EXPECT_EQ(patch_body["data"]["role"].get<std::string>(), "accountant");

    // Remove.
    auto del_req = authed(owner_principal, Delete);
    HttpResponsePtr del_resp;
    orgs_ctrl.removeMember(
        del_req, [&](const HttpResponsePtr& r) { del_resp = r; }, org.id, target.user.id);
    ASSERT_NE(del_resp, nullptr);
    EXPECT_EQ(del_resp->statusCode(), k200OK);
    EXPECT_FALSE(members.find_membership(org.id, target.user.id).has_value());
}

TEST_F(OrganizationsApiTest, ViewerCannotManageMembers) {
    auto org = seed_org("111240000008", "Viewer Org LLP");
    auto viewer = seed_user("viewer@example.com", "User");
    auto target = seed_user("target2@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, viewer.user.id, "viewer");
    auto viewer_principal = with_org(viewer.principal, org.id);

    auto req = authed_json(viewer_principal, {{"user_id", target.user.id}, {"role", "viewer"}}, Post);
    HttpResponsePtr resp;
    orgs_ctrl.addMember(
        req, [&](const HttpResponsePtr& r) { resp = r; }, org.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(OrganizationsApiTest, LastOwnerNotRemovable) {
    auto org = seed_org("111240000009", "Sole Owner Org LLP");
    auto owner = seed_user("soleowner@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, owner.user.id, "owner");
    auto owner_principal = with_org(owner.principal, org.id);

    // Cannot remove the last owner.
    auto del_req = authed(owner_principal, Delete);
    HttpResponsePtr del_resp;
    orgs_ctrl.removeMember(
        del_req, [&](const HttpResponsePtr& r) { del_resp = r; }, org.id, owner.user.id);
    ASSERT_NE(del_resp, nullptr);
    EXPECT_EQ(del_resp->statusCode(), k409Conflict);
    auto del_body = json::parse(std::string(del_resp->body()));
    EXPECT_EQ(del_body["error"].get<std::string>(), "last_owner");
    EXPECT_TRUE(members.find_membership(org.id, owner.user.id).has_value());

    // Cannot demote the last owner either.
    auto patch_req = authed_json(owner_principal, {{"role", "viewer"}}, Patch);
    HttpResponsePtr patch_resp;
    orgs_ctrl.updateMemberRole(
        patch_req, [&](const HttpResponsePtr& r) { patch_resp = r; }, org.id, owner.user.id);
    ASSERT_NE(patch_resp, nullptr);
    EXPECT_EQ(patch_resp->statusCode(), k409Conflict);
    auto patch_body = json::parse(std::string(patch_resp->body()));
    EXPECT_EQ(patch_body["error"].get<std::string>(), "last_owner");
    auto still = members.find_membership(org.id, owner.user.id);
    ASSERT_TRUE(still.has_value());
    EXPECT_EQ(still->role, "owner");
}

// ── Member roster (GET) + add-by-email ──────────────────────────────────────

TEST_F(OrganizationsApiTest, ListMembersReturnsRoster) {
    auto org = seed_org("111240000010", "Roster Org LLP");
    auto owner = seed_user("roster-owner@example.com", "User");
    auto accountant = seed_user("roster-accountant@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, owner.user.id, "owner");
    members.add(org.id, accountant.user.id, "accountant");
    auto owner_principal = with_org(owner.principal, org.id);

    HttpResponsePtr resp;
    orgs_ctrl.listMembers(
        authed(owner_principal), [&](const HttpResponsePtr& r) { resp = r; }, org.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    ASSERT_TRUE(body["data"].is_array());
    ASSERT_EQ(body["data"].size(), 2U);

    bool found_owner = false;
    bool found_accountant = false;
    for (const auto& row : body["data"]) {
        ASSERT_TRUE(row.contains("user_id"));
        ASSERT_TRUE(row.contains("email"));
        ASSERT_TRUE(row.contains("role"));
        ASSERT_TRUE(row.contains("created_at"));
        if (row["user_id"].get<std::string>() == owner.user.id) {
            EXPECT_EQ(row["email"].get<std::string>(), "roster-owner@example.com");
            EXPECT_EQ(row["role"].get<std::string>(), "owner");
            found_owner = true;
        } else if (row["user_id"].get<std::string>() == accountant.user.id) {
            EXPECT_EQ(row["email"].get<std::string>(), "roster-accountant@example.com");
            EXPECT_EQ(row["role"].get<std::string>(), "accountant");
            found_accountant = true;
        }
    }
    EXPECT_TRUE(found_owner);
    EXPECT_TRUE(found_accountant);
}

TEST_F(OrganizationsApiTest, ListMembersForbiddenForViewer) {
    auto org = seed_org("111240000011", "Roster Viewer Org LLP");
    auto viewer = seed_user("roster-viewer@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, viewer.user.id, "viewer");
    auto viewer_principal = with_org(viewer.principal, org.id);

    HttpResponsePtr resp;
    orgs_ctrl.listMembers(
        authed(viewer_principal), [&](const HttpResponsePtr& r) { resp = r; }, org.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(OrganizationsApiTest, ListMembersForbiddenForOutsiderOwner) {
    auto org_a = seed_org("111240000012", "Roster Org A LLP");
    auto org_b = seed_org("111240000013", "Roster Org B LLP");
    auto owner_of_b = seed_user("roster-owner-b@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org_b.id, owner_of_b.user.id, "owner");
    // Session is scoped to org B (owner there), not org A — the gate is
    // fail-closed per require_admin_or_org_owner's contract: being an
    // owner elsewhere doesn't authorize org A.
    auto owner_b_principal = with_org(owner_of_b.principal, org_b.id);

    HttpResponsePtr resp;
    orgs_ctrl.listMembers(
        authed(owner_b_principal), [&](const HttpResponsePtr& r) { resp = r; }, org_a.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(OrganizationsApiTest, AddMemberByEmail) {
    auto org = seed_org("111240000014", "Add By Email Org LLP");
    auto owner = seed_user("add-by-email-owner@example.com", "User");
    auto target = seed_user("add-by-email-target@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, owner.user.id, "owner");
    auto owner_principal = with_org(owner.principal, org.id);

    auto req = authed_json(owner_principal, {{"email", target.user.email}, {"role", "accountant"}}, Post);
    HttpResponsePtr resp;
    orgs_ctrl.addMember(
        req, [&](const HttpResponsePtr& r) { resp = r; }, org.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k201Created);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["user_id"].get<std::string>(), target.user.id);
    EXPECT_EQ(body["data"]["role"].get<std::string>(), "accountant");
    EXPECT_TRUE(members.find_membership(org.id, target.user.id).has_value());
}

TEST_F(OrganizationsApiTest, AddMemberByUnknownEmail) {
    auto org = seed_org("111240000015", "Unknown Email Org LLP");
    auto owner = seed_user("unknown-email-owner@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, owner.user.id, "owner");
    auto owner_principal = with_org(owner.principal, org.id);

    auto req = authed_json(owner_principal, {{"email", "nobody@example.com"}, {"role", "viewer"}}, Post);
    HttpResponsePtr resp;
    orgs_ctrl.addMember(
        req, [&](const HttpResponsePtr& r) { resp = r; }, org.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "user_not_found");
}

TEST_F(OrganizationsApiTest, AddMemberByEmailAndUserIdRejected) {
    auto org = seed_org("111240000016", "Ambiguous Add Org LLP");
    auto owner = seed_user("ambiguous-owner@example.com", "User");
    auto target = seed_user("ambiguous-target@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, owner.user.id, "owner");
    auto owner_principal = with_org(owner.principal, org.id);

    auto req = authed_json(
        owner_principal, {{"user_id", target.user.id}, {"email", target.user.email}, {"role", "viewer"}}, Post);
    HttpResponsePtr resp;
    orgs_ctrl.addMember(
        req, [&](const HttpResponsePtr& r) { resp = r; }, org.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    EXPECT_FALSE(members.find_membership(org.id, target.user.id).has_value());
}

// ── The `hr` role (migrations/017_hr_role.sql) ──────────────────────────────

TEST_F(OrganizationsApiTest, HrIsAnAcceptedMemberRole) {
    // The role has to survive BOTH gates it passes on the way in:
    // Tenancy::is_valid_role in the controller, and org_members' CHECK
    // constraint in the database (widened by migrations/017_hr_role.sql).
    // A test that only exercised is_valid_role would still pass against an
    // un-migrated database and blow up as a 500 in production.
    auto org = seed_org("111240000017", "Hr Role Org LLP");
    auto owner = seed_user("hrrole-owner@example.com", "User");
    auto target = seed_user("hrrole-target@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, owner.user.id, "owner");
    auto owner_principal = with_org(owner.principal, org.id);

    auto req = authed_json(owner_principal, {{"user_id", target.user.id}, {"role", "hr"}}, Post);
    HttpResponsePtr resp;
    orgs_ctrl.addMember(
        req, [&](const HttpResponsePtr& r) { resp = r; }, org.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k201Created);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["role"].get<std::string>(), "hr");
    auto stored = members.find_membership(org.id, target.user.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->role, "hr");
}

TEST_F(OrganizationsApiTest, UnknownMemberRoleStillRejected) {
    // Widening the CHECK to four values must not turn the role field into a
    // free-text column: anything outside the roster is still a 400, and the
    // message names all four.
    auto org = seed_org("111240000018", "Bad Role Org LLP");
    auto owner = seed_user("badrole-owner@example.com", "User");
    auto target = seed_user("badrole-target@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, owner.user.id, "owner");
    auto owner_principal = with_org(owner.principal, org.id);

    auto req = authed_json(owner_principal, {{"user_id", target.user.id}, {"role", "manager"}}, Post);
    HttpResponsePtr resp;
    orgs_ctrl.addMember(
        req, [&](const HttpResponsePtr& r) { resp = r; }, org.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "not_allowed");
    EXPECT_NE(body["errors"][0]["message"].get<std::string>().find("hr"), std::string::npos);
    EXPECT_FALSE(members.find_membership(org.id, target.user.id).has_value());
}

TEST_F(OrganizationsApiTest, LastOwnerNotDemotableToHr) {
    // Last-owner protection counts `role == "owner"` rows, so a fourth role
    // must not become a loophole: demoting the sole owner to кадровик is
    // the same 409 as demoting them to viewer.
    auto org = seed_org("111240000019", "Sole Owner To Hr Org LLP");
    auto owner = seed_user("soleowner-hr@example.com", "User");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, owner.user.id, "owner");
    // A second member who is `hr` — proof the count is of OWNERS, not of
    // members: with this row present the org has two members and one owner.
    auto helper = seed_user("hr-helper@example.com", "User");
    members.add(org.id, helper.user.id, "hr");
    auto owner_principal = with_org(owner.principal, org.id);

    auto req = authed_json(owner_principal, {{"role", "hr"}}, Patch);
    HttpResponsePtr resp;
    orgs_ctrl.updateMemberRole(
        req, [&](const HttpResponsePtr& r) { resp = r; }, org.id, owner.user.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "last_owner");
    auto still = members.find_membership(org.id, owner.user.id);
    ASSERT_TRUE(still.has_value());
    EXPECT_EQ(still->role, "owner");
}

}  // namespace
