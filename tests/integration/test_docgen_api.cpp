/**
 * @file test_docgen_api.cpp
 * @brief Integration tests for DocgenController — Task 13.
 *
 * Follows the direct-controller-invocation idiom of test_documents_api.cpp.
 * The actual PDF render is NEVER exercised here — `generate()` only
 * validates + creates a draft Document + enqueues a `docgen.render` job;
 * this suite asserts the job actually landed on the queue (same idiom as
 * test_email_jobs.cpp / test_jobs.cpp), not that it ran. See
 * tests/integration/test_render_job.cpp for the render pipeline itself.
 */

#include <filesystem>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/DocgenController.hpp"
#include "cache/Cache.hpp"
#include "database/Database.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "jobs/Jobs.hpp"
#include "ledger/Counterparty.hpp"
#include "ledger/CounterpartyRepository.hpp"
#include "ledger/DocumentRepository.hpp"
#include "ledger/JournalEntry.hpp"
#include "ledger/JournalService.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;
namespace fs = std::filesystem;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-docgen-api-padding";
constexpr const char* kRenderJobType = "docgen.render";

class DocgenApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::DocgenController ctrl;

    std::string config_file_name() const override { return "docgen_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["jobs"]["enabled"] = true;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        if (!fs::exists("templates/latex/invoice/v1/schema.json"))
            GTEST_SKIP() << "repo templates not reachable from this working directory";

        // Centralized org-data wipe (TestHelpers::wipe_org_data(), in
        // test_helpers.hpp) — see its Doxygen comment for why the journal/
        // document tables are TRUNCATEd before organizations is plain
        // DELETEd. TRUNCATE users CASCADE stays local to this fixture.
        TestHelpers::wipe_org_data();
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
        drain_queue();
    }

    void TearDown() override {
        if (!::testing::Test::IsSkipped() && Cache::is_initialized())
            drain_queue();
        TestHelpers::CoreBackedTest::TearDown();
    }

    static void drain_queue() { TestHelpers::drain_jobs({kRenderJobType}); }

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

    Security::Auth::AuthPrincipal member(const std::string& email, const std::string& org_id, const std::string& role) {
        auto user = seed_user(email);
        Security::Auth::AuthPrincipal p;
        p.subject = user.id;
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

    /// A valid input for templates/latex/invoice/v1's schema.json — same
    /// fixture as test_render_job.cpp's valid_invoice_input().
    static json valid_invoice_input() {
        return json{
            {"number", "1"},
            {"date", "14.08.2026"},
            {"seller", {{"name", "Cyber Capybara ТОО"}, {"identifier", "104332181962"}}},
            {"buyer", {{"name", "Покупатель ТОО"}, {"identifier", "001338908381"}}},
            {"items",
             json::array({json{{"name", "Консультации"},
                               {"qty", "1"},
                               {"unit", "шт"},
                               {"price", "1000.00"},
                               {"amount", "1000.00"}}})},
            {"total", "1000.00"},
            {"total_words", "Одна тысяча тенге 00 тиын"},
        };
    }

    static long queue_depth() {
        return static_cast<long>(Cache::get().get_client().llen(Jobs::queue_key(kRenderJobType)));
    }
};

// ── GET /api/v1/doc-templates ────────────────────────────────────────────────

TEST_F(DocgenApiTest, ListTemplatesReturnsAtLeastFive) {
    auto org = seed_org("444260000001", "Templates Org LLP");
    auto viewer = member("viewer1@example.com", org.id, "viewer");

    HttpResponsePtr resp;
    ctrl.listTemplates(authed(viewer), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    ASSERT_TRUE(body["data"].is_array());
    EXPECT_GE(body["data"].size(), 5U);

    bool found_invoice = false;
    for (const auto& tpl : body["data"]) {
        EXPECT_FALSE(tpl["slug"].get<std::string>().empty());
        EXPECT_FALSE(tpl["version"].get<std::string>().empty());
        EXPECT_TRUE(tpl["schema"].is_object());
        if (tpl["slug"].get<std::string>() == "invoice")
            found_invoice = true;
    }
    EXPECT_TRUE(found_invoice);
}

// ── POST /api/v1/documents/generate ─────────────────────────────────────────

TEST_F(DocgenApiTest, GenerateValidInputAcceptedAndEnqueues) {
    auto org = seed_org("444260000002", "Generate Org LLP");
    auto accountant = member("accountant1@example.com", org.id, "accountant");

    auto req = authed_json(accountant, {{"template_slug", "invoice"}, {"input", valid_invoice_input()}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted);
    auto body = json::parse(std::string(resp->body()));
    const std::string document_id = body["document_id"].get<std::string>();
    ASSERT_FALSE(document_id.empty());
    // render_queued reflects the best-effort Jobs::submit() outcome (Fix
    // round 1) — Redis is up in this fixture, so the enqueue must succeed.
    EXPECT_TRUE(body["render_queued"].get<bool>());

    // The document exists, draft, with the exact input snapshotted.
    Ledger::DocumentRepository documents;
    auto doc = documents.find_in_org(document_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->status, "draft");
    EXPECT_EQ(doc->source, "generated");
    EXPECT_EQ(doc->doc_type, "invoice");
    EXPECT_EQ(doc->template_slug.value_or(""), "invoice");
    EXPECT_EQ(doc->template_version.value_or(""), "v1");
    ASSERT_TRUE(doc->input_snapshot.has_value());
    EXPECT_EQ(*doc->input_snapshot, valid_invoice_input());

    // A docgen.render job was enqueued — NOT executed.
    ASSERT_EQ(queue_depth(), 1);
    std::vector<std::string> ids;
    Cache::get().get_client().lrange(Jobs::queue_key(kRenderJobType), 0, -1, std::back_inserter(ids));
    ASSERT_EQ(ids.size(), 1u);
    auto job = Jobs::get().get_status(ids[0]);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->type, kRenderJobType);
    EXPECT_EQ(job->status, "pending");
    EXPECT_EQ(job->payload["org_id"], org.id);
    EXPECT_EQ(job->payload["document_id"], document_id);
    EXPECT_EQ(job->payload["slug"], "invoice");
    EXPECT_EQ(job->payload["input"], valid_invoice_input());
}

TEST_F(DocgenApiTest, GenerateInvalidInputRejected) {
    auto org = seed_org("444260000003", "Generate Invalid Org LLP");
    auto accountant = member("accountant2@example.com", org.id, "accountant");

    // Missing every required field.
    auto req = authed_json(accountant, {{"template_slug", "invoice"}, {"input", json::object()}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "input");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "schema_validation_failed");

    EXPECT_EQ(queue_depth(), 0);
}

TEST_F(DocgenApiTest, GenerateUnknownTemplateRejected) {
    auto org = seed_org("444260000004", "Generate Unknown Org LLP");
    auto accountant = member("accountant3@example.com", org.id, "accountant");

    auto req = authed_json(accountant, {{"template_slug", "not_a_template"}, {"input", json::object()}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "template_slug");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "unknown_template");
}

// Semantics decision (see DocgenController.hpp's file header): a foreign
// link_entry_id is a 422 on that field, not a 404 — it is a body field
// inside a create-like request, not a URL resource id, so a bad reference
// is a validation failure of THIS request, the same posture
// Ledger::JournalService::ForeignCounterparty already takes for a foreign
// counterparty_id inside a journal line.
TEST_F(DocgenApiTest, GenerateForeignLinkEntryRejected) {
    auto org_a = seed_org("444260000005", "Docgen Org A LLP");
    auto org_b = seed_org("444260000006", "Docgen Org B LLP");
    auto accountant_b = member("accountant4@example.com", org_b.id, "accountant");

    // A draft entry in org_a that org_b has no access to — link_entry_id's
    // pre-check doesn't care about entry status, only org membership.
    auto user_a = seed_user("owner_a@example.com");
    Ledger::JournalService svc;
    Ledger::JournalLine debit;
    debit.account_code = "1030";
    debit.side = "debit";
    debit.amount = "500.00";
    Ledger::JournalLine credit;
    credit.account_code = "6010";
    credit.side = "credit";
    credit.amount = "500.00";
    auto foreign_entry = svc.create_draft(org_a.id, user_a.id, "2026-01-10", "Org A entry", {debit, credit});

    auto req = authed_json(
        accountant_b,
        {{"template_slug", "invoice"}, {"input", valid_invoice_input()}, {"link_entry_id", foreign_entry.id}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "link_entry_id");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "foreign_journal_entry");

    // Nothing was created for a request that was going to fail anyway.
    EXPECT_EQ(queue_depth(), 0);
}

TEST_F(DocgenApiTest, GenerateForeignCounterpartyRejected) {
    auto org_a = seed_org("444260000007", "Docgen CP Org A LLP");
    auto org_b = seed_org("444260000008", "Docgen CP Org B LLP");
    auto accountant_b = member("accountant5@example.com", org_b.id, "accountant");

    Ledger::CounterpartyRepository counterparties;
    Ledger::Counterparty cp;
    cp.identifier = "123456789012";
    cp.name = "Org A's counterparty";
    auto created_cp = counterparties.create(org_a.id, cp);

    auto req = authed_json(
        accountant_b,
        {{"template_slug", "invoice"}, {"input", valid_invoice_input()}, {"counterparty_id", created_cp.id}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "counterparty_id");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "foreign_counterparty");
}

TEST_F(DocgenApiTest, GenerateViewerForbidden) {
    auto org = seed_org("444260000009", "Generate Viewer Org LLP");
    auto viewer = member("viewer2@example.com", org.id, "viewer");

    auto req = authed_json(viewer, {{"template_slug", "invoice"}, {"input", valid_invoice_input()}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);

    EXPECT_EQ(queue_depth(), 0);
}

}  // namespace
