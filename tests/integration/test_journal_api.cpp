/**
 * @file test_journal_api.cpp
 * @brief Integration tests for JournalController — Task 13.
 *
 * Follows the direct-controller-invocation idiom of test_documents_api.cpp /
 * test_organizations_api.cpp for everything auth/org-context related: no
 * real HTTP server, the controller's handler methods are called directly
 * with a stamped principal.
 */

#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/JournalController.hpp"
#include "database/Database.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "ledger/JournalEntry.hpp"
#include "ledger/JournalRepository.hpp"
#include "ledger/JournalService.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-journal-api-padding";

class JournalApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::JournalController ctrl;

    std::string config_file_name() const override { return "journal_api_test_config.json"; }

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
        // Same idiom (and rationale) as test_journal_service.cpp's SetUp:
        // TRUNCATE journal_lines/journal_entries FIRST (bypasses the
        // journal_entries_immutability trigger a posted/reversed row would
        // otherwise trip on a plain DELETE), then clear organizations/users.
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE journal_lines, journal_entries CASCADE");
            txn.exec("DELETE FROM organizations");
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
    }

    Tenancy::Organization seed_org(const std::string& bin, const std::string& name) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, name, "snr_simplified", false);
    }

    Domain::User seed_user(const std::string& email) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name("User");
        if (!role) {
            ADD_FAILURE() << "role 'User' missing — seed migration?";
            throw std::runtime_error("seed role missing: User");
        }
        return users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
    }

    static Security::Auth::AuthPrincipal with_org(Security::Auth::AuthPrincipal p, const std::string& org_id) {
        p.org = org_id;
        return p;
    }

    /// Seed a user, add them to @p org_id with @p role, and return an
    /// org-scoped principal ready for authed()/authed_json().
    Security::Auth::AuthPrincipal member(const std::string& email, const std::string& org_id, const std::string& role) {
        auto user = seed_user(email);
        Security::Auth::AuthPrincipal p;
        p.subject = user.id;
        p.roles.push_back("User");
        p.raw_claims = json{{"sub", user.id}};
        Tenancy::OrgMemberRepository members;
        members.add(org_id, user.id, role);
        return with_org(p, org_id);
    }

    static HttpRequestPtr authed(const Security::Auth::AuthPrincipal& p, HttpMethod method = Get) {
        return TestHelpers::authed(p, method);
    }

    static HttpRequestPtr authed_json(const Security::Auth::AuthPrincipal& p,
                                      const json& body,
                                      HttpMethod method = Post) {
        return TestHelpers::authed_json(p, body, method);
    }

    /// A balanced 2-line entry: cash in (1030) against sales income (6010),
    /// 1000.00 each side — both are system chart-of-accounts codes
    /// (migrations/008_accounts.sql), visible to every org.
    static json balanced_lines_body() {
        return json::array({
            json{{"account_code", "1030"}, {"side", "debit"}, {"amount", "1000.00"}},
            json{{"account_code", "6010"}, {"side", "credit"}, {"amount", "1000.00"}},
        });
    }
};

// ── Full lifecycle: create -> post -> reverse -> list ──────────────────────

TEST_F(JournalApiTest, LifecycleThroughApi) {
    auto org = seed_org("444250000001", "Journal Lifecycle Org LLP");
    auto accountant = member("accountant1@example.com", org.id, "accountant");

    // 1. Create a draft entry.
    auto create_req = authed_json(
        accountant, {{"entry_date", "2026-01-15"}, {"description", "Cash sale"}, {"lines", balanced_lines_body()}});
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_NE(create_resp, nullptr);
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto create_body = json::parse(std::string(create_resp->body()));
    const std::string entry_id = create_body["data"]["id"].get<std::string>();
    EXPECT_EQ(create_body["data"]["status"].get<std::string>(), "draft");
    ASSERT_EQ(create_body["data"]["lines"].size(), 2U);

    // 2. Post it.
    HttpResponsePtr post_resp;
    ctrl.post(
        authed(accountant, Post), [&](const HttpResponsePtr& r) { post_resp = r; }, entry_id);
    ASSERT_NE(post_resp, nullptr);
    ASSERT_EQ(post_resp->statusCode(), k200OK);
    auto post_body = json::parse(std::string(post_resp->body()));
    EXPECT_EQ(post_body["data"]["status"].get<std::string>(), "posted");

    // 3. Reverse it — returns the NEW storno entry, posted, linked back.
    HttpResponsePtr reverse_resp;
    ctrl.reverse(
        authed(accountant, Post), [&](const HttpResponsePtr& r) { reverse_resp = r; }, entry_id);
    ASSERT_NE(reverse_resp, nullptr);
    ASSERT_EQ(reverse_resp->statusCode(), k200OK);
    auto reverse_body = json::parse(std::string(reverse_resp->body()));
    const std::string storno_id = reverse_body["data"]["id"].get<std::string>();
    EXPECT_EQ(reverse_body["data"]["status"].get<std::string>(), "posted");
    EXPECT_EQ(reverse_body["data"]["reverses_entry_id"].get<std::string>(), entry_id);
    EXPECT_NE(storno_id, entry_id);

    // 4. The list contains BOTH entries, the storno linked to the original.
    HttpResponsePtr list_resp;
    ctrl.list(authed(accountant), [&](const HttpResponsePtr& r) { list_resp = r; });
    ASSERT_NE(list_resp, nullptr);
    ASSERT_EQ(list_resp->statusCode(), k200OK);
    auto list_body = json::parse(std::string(list_resp->body()));
    EXPECT_EQ(list_body["total"].get<long>(), 2);
    bool found_original = false;
    bool found_storno = false;
    for (const auto& row : list_body["data"]) {
        if (row["id"].get<std::string>() == entry_id) {
            found_original = true;
            EXPECT_EQ(row["status"].get<std::string>(), "reversed");
        }
        if (row["id"].get<std::string>() == storno_id) {
            found_storno = true;
            EXPECT_EQ(row["reverses_entry_id"].get<std::string>(), entry_id);
        }
    }
    EXPECT_TRUE(found_original);
    EXPECT_TRUE(found_storno);

    // 5. GET the original by id now returns 'reversed' with its own (frozen)
    // lines, not the storno's.
    HttpResponsePtr get_resp;
    ctrl.get(
        authed(accountant), [&](const HttpResponsePtr& r) { get_resp = r; }, entry_id);
    ASSERT_EQ(get_resp->statusCode(), k200OK);
    auto get_body = json::parse(std::string(get_resp->body()));
    EXPECT_EQ(get_body["data"]["status"].get<std::string>(), "reversed");
    ASSERT_EQ(get_body["data"]["lines"].size(), 2U);
}

// ── POST /api/v1/journal-entries — validation ───────────────────────────────

TEST_F(JournalApiTest, CreateUnbalancedEntryRejected) {
    auto org = seed_org("444250000002", "Unbalanced Org LLP");
    auto accountant = member("accountant2@example.com", org.id, "accountant");

    json unbalanced_lines = json::array({
        json{{"account_code", "1030"}, {"side", "debit"}, {"amount", "1000.00"}},
        json{{"account_code", "6010"}, {"side", "credit"}, {"amount", "999.00"}},
    });
    auto req = authed_json(accountant,
                           {{"entry_date", "2026-01-15"}, {"description", "Bad entry"}, {"lines", unbalanced_lines}});
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "lines");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "unbalanced_entry");
}

TEST_F(JournalApiTest, CreateUnknownAccountRejected) {
    auto org = seed_org("444250000003", "Unknown Account Org LLP");
    auto accountant = member("accountant3@example.com", org.id, "accountant");

    json bad_lines = json::array({
        json{{"account_code", "9999999"}, {"side", "debit"}, {"amount", "1000.00"}},
        json{{"account_code", "6010"}, {"side", "credit"}, {"amount", "1000.00"}},
    });
    auto req =
        authed_json(accountant, {{"entry_date", "2026-01-15"}, {"description", "Bad account"}, {"lines", bad_lines}});
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "lines");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "unknown_account");
}

TEST_F(JournalApiTest, CreateViewerForbidden) {
    auto org = seed_org("444250000004", "Create Viewer Org LLP");
    auto viewer = member("viewer1@example.com", org.id, "viewer");

    auto req = authed_json(
        viewer, {{"entry_date", "2026-01-15"}, {"description", "Cash sale"}, {"lines", balanced_lines_body()}});
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

// ── POST /api/v1/journal-entries/{id}/post ──────────────────────────────────

TEST_F(JournalApiTest, PostViewerForbidden) {
    auto org = seed_org("444250000005", "Post Viewer Org LLP");
    auto accountant_user = seed_user("accountant4@example.com");

    Ledger::JournalService svc;
    Ledger::JournalLine debit;
    debit.account_code = "1030";
    debit.side = "debit";
    debit.amount = "1000.00";
    Ledger::JournalLine credit;
    credit.account_code = "6010";
    credit.side = "credit";
    credit.amount = "1000.00";
    auto entry = svc.create_draft(org.id, accountant_user.id, "2026-01-15", "Cash sale", {debit, credit});

    auto viewer = member("viewer2@example.com", org.id, "viewer");
    HttpResponsePtr resp;
    ctrl.post(
        authed(viewer, Post), [&](const HttpResponsePtr& r) { resp = r; }, entry.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(JournalApiTest, PostAlreadyPostedConflict) {
    auto org = seed_org("444250000006", "Post Twice Org LLP");
    auto accountant = member("accountant5@example.com", org.id, "accountant");

    auto create_req = authed_json(
        accountant, {{"entry_date", "2026-01-15"}, {"description", "Cash sale"}, {"lines", balanced_lines_body()}});
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    const std::string entry_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    HttpResponsePtr first_post;
    ctrl.post(
        authed(accountant, Post), [&](const HttpResponsePtr& r) { first_post = r; }, entry_id);
    ASSERT_EQ(first_post->statusCode(), k200OK);

    HttpResponsePtr second_post;
    ctrl.post(
        authed(accountant, Post), [&](const HttpResponsePtr& r) { second_post = r; }, entry_id);
    ASSERT_NE(second_post, nullptr);
    EXPECT_EQ(second_post->statusCode(), k409Conflict);
    auto body = json::parse(std::string(second_post->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "invalid_entry_state");
}

// ── GET /api/v1/journal-entries/{id} — cross-org ────────────────────────────

TEST_F(JournalApiTest, GetCrossOrgNotFound) {
    auto org_a = seed_org("444250000007", "Journal Org A LLP");
    auto org_b = seed_org("444250000008", "Journal Org B LLP");
    auto accountant_a = member("accountant6@example.com", org_a.id, "accountant");
    auto viewer_b = member("viewer3@example.com", org_b.id, "viewer");

    auto create_req = authed_json(
        accountant_a, {{"entry_date", "2026-01-15"}, {"description", "Cash sale"}, {"lines", balanced_lines_body()}});
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    const std::string entry_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    HttpResponsePtr resp;
    ctrl.get(
        authed(viewer_b), [&](const HttpResponsePtr& r) { resp = r; }, entry_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

// ── GET /api/v1/journal-entries — invalid date filter ───────────────────────

TEST_F(JournalApiTest, ListInvalidDateFilterRejected) {
    auto org = seed_org("444250000009", "Bad Date Filter Org LLP");
    auto viewer = member("viewer4@example.com", org.id, "viewer");

    auto req = authed(viewer);
    req->setParameter("from", "15-01-2026");
    HttpResponsePtr resp;
    ctrl.list(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "from");
}

}  // namespace
