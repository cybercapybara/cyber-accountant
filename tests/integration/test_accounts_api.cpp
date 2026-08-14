/**
 * @file test_accounts_api.cpp
 * @brief Integration tests for AccountsController — Task 12.
 *
 * Follows the direct-controller-invocation idiom of test_organizations_api.cpp
 * and the seeding idioms of test_accounts.cpp (AccountRepository's own
 * integration suite). There is no GET /api/v1/accounts/{id} route (see
 * AccountsController.hpp's file header), so the "cross-org isolation" case
 * this task's brief asks for is expressed as list_visible() NOT leaking
 * another org's subaccount, rather than a 404 on a single-object fetch.
 */

#include <optional>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/AccountsController.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "ledger/AccountRepository.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-accounts-api-padding";

class AccountsApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::AccountsController ctrl;

    std::string config_file_name() const override { return "accounts_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // Same idiom (and rationale) as test_accounts.cpp's SetUp: a plain
        // "DELETE FROM organizations" cascade would also wipe the org_id IS
        // NULL system chart-of-accounts seed rows if TRUNCATE CASCADE were
        // used instead — clear tenant subaccounts explicitly first.
        Database::get().execute_write([](auto& txn) {
            txn.exec("DELETE FROM accounts WHERE org_id IS NOT NULL");
            txn.exec("DELETE FROM organizations");
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
    }

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
        Pair p;
        p.user = created;
        p.principal.subject = created.id;
        p.principal.roles.push_back(role->name);
        p.principal.raw_claims = json{{"sub", created.id}, {"permissions", role->permissions}};
        return p;
    }

    static Security::Auth::AuthPrincipal with_org(Security::Auth::AuthPrincipal p, const std::string& org_id) {
        p.org = org_id;
        return p;
    }

    Tenancy::Organization seed_org(const std::string& bin, const std::string& name) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, name, "snr_simplified", false);
    }

    /// Seed a user, add them to @p org_id with @p role, and return an
    /// org-scoped principal ready for authed()/authed_json().
    Security::Auth::AuthPrincipal member(const std::string& email, const std::string& org_id, const std::string& role) {
        auto seeded = seed_user(email);
        Tenancy::OrgMemberRepository members;
        members.add(org_id, seeded.user.id, role);
        return with_org(seeded.principal, org_id);
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

// ── GET /api/v1/accounts ─────────────────────────────────────────────────────

TEST_F(AccountsApiTest, ListIncludesSystemAndOwnSubaccounts) {
    auto org = seed_org("333240000001", "List Accounts Org LLP");
    auto accountant = member("accountant1@example.com", org.id, "accountant");

    auto create_req =
        authed_json(accountant, {{"code", "1030.1"}, {"name_ru", "Текущий счет в KZT"}, {"parent_code", "1030"}});
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);

    // Viewers can read — the mutation gate only applies to POST.
    auto viewer = member("viewer1@example.com", org.id, "viewer");
    HttpResponsePtr resp;
    ctrl.list(authed(viewer), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    ASSERT_TRUE(body["data"].is_array());
    // migration 008 seeds >=36 system rows, plus this org's one subaccount.
    EXPECT_GE(body["data"].size(), 37U);

    bool found_system = false;
    bool found_sub = false;
    for (const auto& a : body["data"]) {
        if (a["code"].get<std::string>() == "1030" && a["org_id"].is_null())
            found_system = true;
        if (a["code"].get<std::string>() == "1030.1" && a["org_id"].get<std::string>() == org.id)
            found_sub = true;
    }
    EXPECT_TRUE(found_system);
    EXPECT_TRUE(found_sub);
}

// ── POST /api/v1/accounts ────────────────────────────────────────────────────

TEST_F(AccountsApiTest, CreateSubaccountSucceeds) {
    auto org = seed_org("333240000002", "Create Account Org LLP");
    auto accountant = member("accountant2@example.com", org.id, "accountant");

    auto req = authed_json(accountant, {{"code", "1030.2"}, {"name_ru", "Валютный счет"}, {"parent_code", "1030"}});
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k201Created);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["code"].get<std::string>(), "1030.2");
    EXPECT_EQ(body["data"]["name_ru"].get<std::string>(), "Валютный счет");
    EXPECT_EQ(body["data"]["org_id"].get<std::string>(), org.id);
    EXPECT_EQ(body["data"]["parent_code"].get<std::string>(), "1030");
    // Inherited from the parent (1030 is an asset, not currency-tracked).
    EXPECT_EQ(body["data"]["type"].get<std::string>(), "asset");
    EXPECT_FALSE(body["data"]["currency_tracked"].get<bool>());

    Ledger::AccountRepository repo;
    auto found = repo.find_visible(org.id, "1030.2");
    ASSERT_TRUE(found.has_value());
}

TEST_F(AccountsApiTest, CreateViewerForbidden) {
    auto org = seed_org("333240000003", "Viewer Account Org LLP");
    auto viewer = member("viewer2@example.com", org.id, "viewer");

    auto req = authed_json(viewer, {{"code", "1030.3"}, {"name_ru", "Nope"}, {"parent_code", "1030"}});
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(AccountsApiTest, CreateInvalidSubaccountRejected) {
    auto org = seed_org("333240000004", "Invalid Sub Org LLP");
    auto accountant = member("accountant3@example.com", org.id, "accountant");

    // "9999" does not extend parent "1030" — Ledger::InvalidSubaccount, 422.
    auto req = authed_json(accountant, {{"code", "9999"}, {"name_ru", "Not a child"}, {"parent_code", "1030"}});
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "invalid_subaccount");
}

TEST_F(AccountsApiTest, CreateDuplicateCodeConflict) {
    auto org = seed_org("333240000005", "Dup Account Org LLP");
    auto accountant = member("accountant4@example.com", org.id, "accountant");

    // "1030" is already a reserved system code — shadowing it under itself
    // as a subaccount must 409, not silently create a second org-scoped row.
    auto req = authed_json(accountant, {{"code", "1030"}, {"name_ru", "Shadow attempt"}, {"parent_code", "1030"}});
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "account_code_taken");
}

TEST_F(AccountsApiTest, SubaccountsIsolatedBetweenOrgs) {
    auto org_a = seed_org("333240000006", "Org A Accounts LLP");
    auto org_b = seed_org("333240000007", "Org B Accounts LLP");
    auto accountant_a = member("accountant5@example.com", org_a.id, "accountant");
    auto accountant_b = member("accountant6@example.com", org_b.id, "accountant");

    auto req = authed_json(accountant_a, {{"code", "1030.9"}, {"name_ru", "A's account"}, {"parent_code", "1030"}});
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k201Created);

    HttpResponsePtr list_b_resp;
    ctrl.list(authed(accountant_b), [&](const HttpResponsePtr& r) { list_b_resp = r; });
    ASSERT_EQ(list_b_resp->statusCode(), k200OK);
    auto list_b = json::parse(std::string(list_b_resp->body()));
    for (const auto& a : list_b["data"])
        EXPECT_NE(a["code"].get<std::string>(), "1030.9");

    HttpResponsePtr list_a_resp;
    ctrl.list(authed(accountant_a), [&](const HttpResponsePtr& r) { list_a_resp = r; });
    ASSERT_EQ(list_a_resp->statusCode(), k200OK);
    auto list_a = json::parse(std::string(list_a_resp->body()));
    bool found = false;
    for (const auto& a : list_a["data"])
        if (a["code"].get<std::string>() == "1030.9")
            found = true;
    EXPECT_TRUE(found);
}

}  // namespace
