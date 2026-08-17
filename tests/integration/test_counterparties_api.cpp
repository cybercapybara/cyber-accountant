/**
 * @file test_counterparties_api.cpp
 * @brief Integration tests for CounterpartiesController — Task 12.
 *
 * Follows the direct-controller-invocation idiom of test_organizations_api.cpp
 * (stamp a hand-built AuthPrincipal + org claim onto a bare request, call the
 * handler, assert on the response) plus the membership-seeding idiom of
 * test_org_context.cpp. Covers, per route: happy path, the viewer-mutation
 * 403, cross-org 404, and the identifier check-digit 422.
 *
 * Valid BIN/IIN test fixtures below all pass Ledger::is_valid_bin_iin (the
 * check-digit algorithm, not just the 12-digit shape a plain regex would
 * accept) — generated offline against that exact algorithm so
 * CreateCounterpartySucceeds etc. don't spuriously 422.
 */

#include <optional>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/CounterpartiesController.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "ledger/CounterpartyRepository.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-counterparties-api-padding";

class CounterpartiesApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::CounterpartiesController ctrl;

    std::string config_file_name() const override { return "counterparties_api_test_config.json"; }

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
        // Centralized org-data wipe (TestHelpers::wipe_org_data(), in
        // test_helpers.hpp) — see its Doxygen comment for why it TRUNCATEs
        // journal_lines/journal_entries/document_entries/documents before a
        // plain DELETE on organizations. Users cleanup stays local to this
        // fixture (untouched by the centralization).
        TestHelpers::wipe_org_data();
        Database::get().execute_write([](auto& txn) {
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
        if (role)
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

// ── POST /api/v1/counterparties ─────────────────────────────────────────────

TEST_F(CounterpartiesApiTest, CreateCounterpartySucceeds) {
    auto org = seed_org("222240000001", "Create Org LLP");
    auto accountant = member("accountant1@example.com", org.id, "accountant");

    auto req = authed_json(
        accountant, {{"identifier", "111240000001"}, {"name", "Acme LLP"}, {"contact_email", "ap@acme.example"}});
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k201Created);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["identifier"].get<std::string>(), "111240000001");
    EXPECT_EQ(body["data"]["name"].get<std::string>(), "Acme LLP");
    EXPECT_EQ(body["data"]["contact_email"].get<std::string>(), "ap@acme.example");
    EXPECT_TRUE(body["data"]["is_resident"].get<bool>());
    EXPECT_FALSE(body["data"]["vat_payer"].get<bool>());
    EXPECT_EQ(body["data"]["address"].get<std::string>(), "");

    Ledger::CounterpartyRepository repo;
    auto found = repo.find_in_org(body["data"]["id"].get<std::string>(), org.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->identifier, "111240000001");
}

TEST_F(CounterpartiesApiTest, CreateViewerForbidden) {
    auto org = seed_org("222240000002", "Viewer Org LLP");
    auto viewer = member("viewer1@example.com", org.id, "viewer");

    auto req = authed_json(viewer, {{"identifier", "111240000011"}, {"name", "Nope LLP"}});
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(CounterpartiesApiTest, CreateInvalidIdentifierRejected) {
    auto org = seed_org("222240000003", "Bad Id Org LLP");
    auto accountant = member("accountant2@example.com", org.id, "accountant");

    auto req = authed_json(accountant, {{"identifier", "123"}, {"name", "Bad Id LLP"}});
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "validation_failed");
    ASSERT_TRUE(body["errors"].is_array());
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "identifier");
}

TEST_F(CounterpartiesApiTest, CreateDuplicateIdentifierConflict) {
    auto org = seed_org("222240000004", "Dup Org LLP");
    auto accountant = member("accountant3@example.com", org.id, "accountant");

    auto req1 = authed_json(accountant, {{"identifier", "111240000021"}, {"name", "First LLP"}});
    HttpResponsePtr resp1;
    ctrl.create(req1, [&](const HttpResponsePtr& r) { resp1 = r; });
    ASSERT_EQ(resp1->statusCode(), k201Created);

    auto req2 = authed_json(accountant, {{"identifier", "111240000021"}, {"name", "Second LLP"}});
    HttpResponsePtr resp2;
    ctrl.create(req2, [&](const HttpResponsePtr& r) { resp2 = r; });
    ASSERT_NE(resp2, nullptr);
    EXPECT_EQ(resp2->statusCode(), k409Conflict);
    auto body = json::parse(std::string(resp2->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "counterparty_identifier_taken");
}

// ── GET /api/v1/counterparties ──────────────────────────────────────────────

TEST_F(CounterpartiesApiTest, ListCounterpartiesPaginated) {
    auto org = seed_org("222240000005", "List Org LLP");
    auto accountant = member("accountant4@example.com", org.id, "accountant");

    for (const std::string& identifier : {std::string{"111240000031"}, std::string{"111240000041"}}) {
        auto req = authed_json(accountant, {{"identifier", identifier}, {"name", "Vendor " + identifier}});
        HttpResponsePtr resp;
        ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
        ASSERT_EQ(resp->statusCode(), k201Created);
    }

    // Viewers can read — the mutation gate only applies to POST/PATCH.
    auto viewer = member("viewer2@example.com", org.id, "viewer");
    HttpResponsePtr list_resp;
    ctrl.list(authed(viewer), [&](const HttpResponsePtr& r) { list_resp = r; });
    ASSERT_NE(list_resp, nullptr);
    ASSERT_EQ(list_resp->statusCode(), k200OK);
    auto body = json::parse(std::string(list_resp->body()));
    EXPECT_EQ(body["total"].get<long>(), 2);
    ASSERT_TRUE(body["data"].is_array());
    EXPECT_EQ(body["data"].size(), 2U);
}

// ── GET /api/v1/counterparties/{id} ─────────────────────────────────────────

TEST_F(CounterpartiesApiTest, GetCounterpartySucceeds) {
    auto org = seed_org("222240000006", "Get Org LLP");
    auto accountant = member("accountant5@example.com", org.id, "accountant");
    auto create_req = authed_json(accountant, {{"identifier", "111240000051"}, {"name", "Fetchable LLP"}});
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    HttpResponsePtr resp;
    ctrl.get(authed(accountant), [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["id"].get<std::string>(), created_id);
    EXPECT_EQ(body["data"]["name"].get<std::string>(), "Fetchable LLP");
}

TEST_F(CounterpartiesApiTest, GetCounterpartyCrossOrgNotFound) {
    auto org_a = seed_org("222240000007", "Org A LLP");
    auto org_b = seed_org("222240000008", "Org B LLP");
    auto accountant_a = member("accountant6@example.com", org_a.id, "accountant");
    auto accountant_b = member("accountant7@example.com", org_b.id, "accountant");

    auto create_req = authed_json(accountant_a, {{"identifier", "111240000061"}, {"name", "Org A's LLP"}});
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    HttpResponsePtr resp;
    ctrl.get(authed(accountant_b), [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

// ── PATCH /api/v1/counterparties/{id} ───────────────────────────────────────

TEST_F(CounterpartiesApiTest, PatchCounterpartySucceeds) {
    auto org = seed_org("222240000009", "Patch Org LLP");
    auto accountant = member("accountant8@example.com", org.id, "accountant");
    auto create_req = authed_json(accountant, {{"identifier", "111240000071"}, {"name", "Before Patch LLP"}});
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    auto patch_req = authed_json(accountant,
                                 {{"identifier", "111240000071"},
                                  {"name", "After Patch LLP"},
                                  {"address", "Astana, Turan 5"},
                                  {"vat_payer", true}},
                                 Patch);
    HttpResponsePtr resp;
    ctrl.patch(patch_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["name"].get<std::string>(), "After Patch LLP");
    EXPECT_EQ(body["data"]["address"].get<std::string>(), "Astana, Turan 5");
    EXPECT_TRUE(body["data"]["vat_payer"].get<bool>());
}

TEST_F(CounterpartiesApiTest, PatchViewerForbidden) {
    auto org = seed_org("222240000010", "Patch Viewer Org LLP");
    auto accountant = member("accountant9@example.com", org.id, "accountant");
    auto viewer = member("viewer3@example.com", org.id, "viewer");
    auto create_req = authed_json(accountant, {{"identifier", "111240000081"}, {"name", "Immutable LLP"}});
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    auto patch_req = authed_json(viewer, {{"identifier", "111240000081"}, {"name", "Hijacked LLP"}}, Patch);
    HttpResponsePtr resp;
    ctrl.patch(patch_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(CounterpartiesApiTest, PatchCrossOrgNotFound) {
    auto org_a = seed_org("222240000011", "Patch A LLP");
    auto org_b = seed_org("222240000012", "Patch B LLP");
    auto accountant_a = member("accountant10@example.com", org_a.id, "accountant");
    auto accountant_b = member("accountant11@example.com", org_b.id, "accountant");

    auto create_req = authed_json(accountant_a, {{"identifier", "111240000091"}, {"name", "Org A's Patch LLP"}});
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    auto patch_req = authed_json(accountant_b, {{"identifier", "111240000091"}, {"name", "Stolen LLP"}}, Patch);
    HttpResponsePtr resp;
    ctrl.patch(patch_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

TEST_F(CounterpartiesApiTest, PatchInvalidIdentifierRejected) {
    auto org = seed_org("222240000013", "Patch Invalid Org LLP");
    auto accountant = member("accountant12@example.com", org.id, "accountant");
    auto create_req = authed_json(accountant, {{"identifier", "111240000100"}, {"name", "Valid LLP"}});
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    auto patch_req = authed_json(accountant, {{"identifier", "123"}, {"name", "Valid LLP"}}, Patch);
    HttpResponsePtr resp;
    ctrl.patch(patch_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "identifier");
}

}  // namespace
