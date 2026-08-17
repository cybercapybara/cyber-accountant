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
#include "repo_templates.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/BankAccountRepository.hpp"
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
        REQUIRE_REPO_TEMPLATE("invoice");

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

    /// A valid REQUEST input for templates/docs/invoice/v1's schema.json.
    /// P3: the caller supplies the integer `total_tiyn` ONLY — `total` and
    /// `total_words` are derived by the server, and sending either is a 422
    /// (GenerateRejectsClientSuppliedTotalWords). What the document ends up
    /// storing is expected_invoice_snapshot(org) below.
    static json valid_invoice_input() {
        return json{
            {"number", "1"},
            {"date", "14.08.2026"},
            {"buyer", {{"name", "Покупатель ТОО"}, {"identifier", "001338908381"}}},
            {"items",
             json::array({json{{"name", "Консультации"},
                               {"qty", "1"},
                               {"unit", "шт"},
                               {"price", "1 000,00"},
                               {"amount", "1 000,00"}}})},
            {"total_tiyn", 100000},
        };
    }

    /// valid_invoice_input() plus everything the SERVER writes: две строки,
    /// выведенные из `total_tiyn`, и реквизиты продавца, взятые из самой
    /// организации. Продавец здесь параметр, а не константа, потому что
    /// каждый тест заводит свою организацию — и снимок обязан содержать
    /// именно её реквизиты, иначе тест прошёл бы и на чужих.
    ///
    /// Банковских полей тут нет намеренно: у организации не назначен
    /// основной счёт, и незаполненные поля сервер не пишет вовсе (пустая
    /// строка напечатала бы в документе пустую строку реквизита вместо её
    /// отсутствия).
    static json expected_invoice_snapshot(const Tenancy::Organization& org) {
        json input = valid_invoice_input();
        input["seller"] = json{{"name", org.name}, {"identifier", org.bin}};
        input["total"] = "1 000,00";
        input["total_words"] = "Одна тысяча тенге 00 тиын";
        return input;
    }

    /// A valid REQUEST input for templates/docs/tax_invoice/v1 minus its
    /// `totals` object, which each test supplies itself — the three integer
    /// totals are the subject of those tests.
    static json tax_invoice_input_without_totals() {
        return json{
            {"number", "7"},
            {"date", "14.08.2026"},
            {"buyer", {{"name", "Покупатель ТОО"}, {"identifier", "001338908381"}}},
            {"items",
             json::array({json{{"name", "Консультации"},
                               {"unit", "шт"},
                               {"qty", "1"},
                               {"price", "90 000,00"},
                               {"amount", "90 000,00"},
                               {"vat_rate", "16"},
                               {"vat_amount", "14 400,00"},
                               {"total_with_vat", "104 400,00"}}})},
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
    // template_version/input_snapshot live on the document's VERSION now
    // (migrations/018_document_versions.sql), and the render that would
    // publish version 1 was only enqueued — so the document's own copies of
    // those fields (read through the current-version pointer) are still
    // empty and the snapshot is checked on the version itself.
    EXPECT_FALSE(doc->current_version_id.has_value());
    EXPECT_EQ(doc->latest_version_no, 1);
    auto version = documents.latest_version(org.id, doc->id);
    ASSERT_TRUE(version.has_value());
    EXPECT_EQ(version->template_version.value_or(""), "v1");
    ASSERT_TRUE(version->input_snapshot.has_value());
    EXPECT_EQ(*version->input_snapshot, expected_invoice_snapshot(org));

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
    // …and it NAMES the version it must render into (P3 task 10). Without
    // this key the worker would address whichever version is newest by the
    // time it runs, which an edit arriving first makes the wrong one.
    EXPECT_EQ(job->payload["version_id"], version->id);
    EXPECT_EQ(job->payload["slug"], "invoice");
    EXPECT_EQ(job->payload["input"], expected_invoice_snapshot(org));
}

TEST_F(DocgenApiTest, GenerateInvalidInputRejected) {
    auto org = seed_org("444260000003", "Generate Invalid Org LLP");
    auto accountant = member("accountant2@example.com", org.id, "accountant");

    // Missing every required field. P3 changed WHICH error comes out, and the
    // order cannot be reversed: the invoice schema requires `total` and
    // `total_words`, which the SERVER writes, so validating before deriving
    // would reject every legitimate request. Derivation therefore runs first
    // and reports the one thing the caller actually owes — the integer.
    auto req = authed_json(accountant, {{"template_slug", "invoice"}, {"input", json::object()}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "input.total_tiyn");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "missing");

    EXPECT_EQ(queue_depth(), 0);
}

// The schema check still bites for everything the derivation does not cover:
// a request carrying the required integer but nothing else fails on the
// template's own JSON Schema, with the original field/code.
TEST_F(DocgenApiTest, GenerateStillReportsSchemaFailuresAfterDerivation) {
    auto org = seed_org("444260000015", "Generate Schema Org LLP");
    auto accountant = member("schema@example.com", org.id, "accountant");

    auto req = authed_json(accountant, {{"template_slug", "invoice"}, {"input", {{"total_tiyn", 100000}}}});
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

// ── P3: the server derives every printed money string ───────────────────────
//
// The threat these four tests close: a document used to print an amount as
// digits AND spelled out in words, both taken from the request, so a crafted
// body could make the two disagree (the ФНО 300.00 hole, generalised). The
// caller now sends ONE integer per total; the server writes every string.

TEST_F(DocgenApiTest, GenerateDerivesTotalAndWordsFromTiyn) {
    auto org = seed_org("444260000010", "Derive Org LLP");
    auto accountant = member("derive@example.com", org.id, "accountant");

    // Only the total is overridden; the item line deliberately keeps its own
    // figure. That mismatch is ALLOWED and is the honest limit of this task:
    // the guarantee is about the document's total, not its line items.
    json input = valid_invoice_input();
    input["total_tiyn"] = 1234567;
    auto req = authed_json(accountant, {{"template_slug", "invoice"}, {"input", input}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted) << resp->body();
    const std::string document_id = json::parse(std::string(resp->body()))["document_id"].get<std::string>();

    Ledger::DocumentRepository documents;
    auto doc = documents.find_in_org(document_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(doc.has_value());
    // The snapshot rides on version 1 — nothing has published it yet.
    auto version = documents.latest_version(org.id, doc->id);
    ASSERT_TRUE(version.has_value());
    ASSERT_TRUE(version->input_snapshot.has_value());
    EXPECT_EQ((*version->input_snapshot)["total"].get<std::string>(), "12 345,67");
    EXPECT_EQ((*version->input_snapshot)["total_words"].get<std::string>(),
              "Двенадцать тысяч триста сорок пять тенге 67 тиын");
}

TEST_F(DocgenApiTest, GenerateRejectsClientSuppliedTotalWords) {
    auto org = seed_org("444260000011", "Words Org LLP");
    auto accountant = member("words@example.com", org.id, "accountant");

    json input = valid_invoice_input();
    input["total_words"] = "Один тенге 00 тиын";
    auto req = authed_json(accountant, {{"template_slug", "invoice"}, {"input", input}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "input.total_words");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "not_allowed_override");
    EXPECT_EQ(queue_depth(), 0);
}

TEST_F(DocgenApiTest, GenerateRejectsClientSuppliedSeller) {
    // Продавец — это САМА организация, поэтому он не может приходить от
    // клиента: иначе счёт печатался бы от чужого имени и с чужим счётом,
    // оставаясь при этом законным документом этой организации.
    auto org = seed_org("444260000020", "Seller Override Org LLP");
    auto accountant = member("seller-override@example.com", org.id, "accountant");

    json input = valid_invoice_input();
    input["seller"] = json{{"name", "Чужое ТОО"}, {"identifier", "999999999999"}};
    auto req = authed_json(accountant, {{"template_slug", "invoice"}, {"input", input}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "input.seller");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "not_allowed_override");
    EXPECT_EQ(queue_depth(), 0);
}

TEST_F(DocgenApiTest, GenerateFillsSellerFromTheOrganizationRequisitesAndPrimaryAccount) {
    // Ровно та боль, ради которой вводились реквизиты: раньше эти строки
    // бухгалтер набирал руками в каждом документе.
    auto org = seed_org("444260000021", "Requisites Org LLP");
    auto accountant = member("requisites@example.com", org.id, "accountant");

    Tenancy::OrganizationRepository orgs;
    ASSERT_TRUE(orgs.update_requisites(org.id, "г. Алматы, пр. Абая 1", "Тарасов М.", "Директор"));
    Tenancy::BankAccountRepository accounts;
    Tenancy::BankAccount account;
    account.iik = "KZ12345678901234567";
    account.bank_name = "АО «Банк ЦентрКредит»";
    account.bik = "KCJBKZKX";
    account.kbe = "17";
    account.is_primary = true;
    accounts.create(org.id, account);

    auto req = authed_json(accountant, {{"template_slug", "invoice"}, {"input", valid_invoice_input()}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted);
    auto body = json::parse(std::string(resp->body()));

    Ledger::DocumentRepository documents;
    auto doc = documents.find_in_org(body["document_id"].get<std::string>(), org.id, /*from_primary=*/true);
    ASSERT_TRUE(doc.has_value());
    auto version = documents.latest_version(org.id, doc->id);
    ASSERT_TRUE(version.has_value());
    ASSERT_TRUE(version->input_snapshot.has_value());
    const json& seller = (*version->input_snapshot)["seller"];
    EXPECT_EQ(seller["name"].get<std::string>(), "Requisites Org LLP");
    EXPECT_EQ(seller["identifier"].get<std::string>(), "444260000021");
    EXPECT_EQ(seller["address"].get<std::string>(), "г. Алматы, пр. Абая 1");
    EXPECT_EQ(seller["iik"].get<std::string>(), "KZ12345678901234567");
    EXPECT_EQ(seller["bank"].get<std::string>(), "АО «Банк ЦентрКредит»");
    EXPECT_EQ(seller["bik"].get<std::string>(), "KCJBKZKX");
    EXPECT_EQ(seller["kbe"].get<std::string>(), "17");
}

TEST_F(DocgenApiTest, GenerateOmitsUnfilledSellerFieldsInsteadOfPrintingBlanks) {
    // Организация, ещё не внёсшая адрес и счёт, обязана продолжать выпускать
    // документы. Пустых строк в снимке при этом быть не должно: пустая
    // строка напечатала бы в документе пустой реквизит вместо его отсутствия.
    auto org = seed_org("444260000022", "Bare Org LLP");
    auto accountant = member("bare@example.com", org.id, "accountant");

    auto req = authed_json(accountant, {{"template_slug", "invoice"}, {"input", valid_invoice_input()}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted);
    auto body = json::parse(std::string(resp->body()));

    Ledger::DocumentRepository documents;
    auto doc = documents.find_in_org(body["document_id"].get<std::string>(), org.id, /*from_primary=*/true);
    ASSERT_TRUE(doc.has_value());
    auto version = documents.latest_version(org.id, doc->id);
    ASSERT_TRUE(version.has_value());
    ASSERT_TRUE(version->input_snapshot.has_value());
    const json& seller = (*version->input_snapshot)["seller"];
    EXPECT_EQ(seller["name"].get<std::string>(), "Bare Org LLP");
    EXPECT_FALSE(seller.contains("address"));
    EXPECT_FALSE(seller.contains("iik"));
    EXPECT_FALSE(seller.contains("bik"));
}

TEST_F(DocgenApiTest, GenerateRejectsATaxInvoiceWhoseTotalsDoNotSum) {
    auto org = seed_org("444260000012", "Sum Org LLP");
    auto accountant = member("sum@example.com", org.id, "accountant");

    // 90 000,00 оборот + 14 400,00 НДС, а итог заявлен 104 000,00 —
    // счёт-фактура, печатающая три несходящихся числа, не должна
    // существовать вовсе.
    json input = tax_invoice_input_without_totals();
    input["totals"] = {{"amount_tiyn", 9000000}, {"vat_tiyn", 1440000}, {"with_vat_tiyn", 10400000}};
    auto req = authed_json(accountant, {{"template_slug", "tax_invoice"}, {"input", input}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "inconsistent_total");
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "input.totals.with_vat_tiyn");
    // Ни документа, ни джобы: проверка идёт до любого побочного эффекта.
    Ledger::DocumentRepository documents;
    EXPECT_EQ(documents.count_filtered(org.id, std::optional<std::string>("tax_invoice"), std::nullopt), 0);
    EXPECT_EQ(queue_depth(), 0);
}

TEST_F(DocgenApiTest, GenerateFormatsAllThreeTaxInvoiceTotalsFromIntegers) {
    auto org = seed_org("444260000013", "Sum2 Org LLP");
    auto accountant = member("sum2@example.com", org.id, "accountant");

    json input = tax_invoice_input_without_totals();
    input["totals"] = {{"amount_tiyn", 9000000}, {"vat_tiyn", 1440000}, {"with_vat_tiyn", 10440000}};
    auto req = authed_json(accountant, {{"template_slug", "tax_invoice"}, {"input", input}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted) << resp->body();

    Ledger::DocumentRepository documents;
    auto doc = documents.find_in_org(
        json::parse(std::string(resp->body()))["document_id"].get<std::string>(), org.id, /*from_primary=*/true);
    ASSERT_TRUE(doc.has_value());
    // The snapshot rides on version 1 — nothing has published it yet.
    auto version = documents.latest_version(org.id, doc->id);
    ASSERT_TRUE(version.has_value());
    ASSERT_TRUE(version->input_snapshot.has_value());
    const json& totals = (*version->input_snapshot)["totals"];
    EXPECT_EQ(totals["amount"].get<std::string>(), "90 000,00");
    EXPECT_EQ(totals["vat"].get<std::string>(), "14 400,00");
    EXPECT_EQ(totals["with_vat"].get<std::string>(), "104 400,00");
    EXPECT_EQ((*version->input_snapshot)["total_words"].get<std::string>(),
              "Сто четыре тысячи четыреста тенге 00 тиын");
}

// The invoice VAT hole, end to end. templates/docs/invoice/v1's template
// prints «НДС (<vat_rate>): <vat_amount> ₸» directly above «Итого к оплате:
// <total> ₸» — adjacent lines of the SAME issued PDF, in either engine's
// spelling of them. While vat_amount was client-authored, those two lines
// could be made to contradict each other outright.
TEST_F(DocgenApiTest, GenerateRejectsAClientSuppliedInvoiceVatAmount) {
    auto org = seed_org("444260000016", "Invoice Vat Org LLP");
    auto accountant = member("invoice-vat@example.com", org.id, "accountant");

    json input = valid_invoice_input();
    input["total_tiyn"] = 112000;
    input["vat_rate"] = "12%";
    input["vat_amount"] = "999 999,00";
    auto req = authed_json(accountant, {{"template_slug", "invoice"}, {"input", input}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "input.vat_amount");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "not_allowed_override");
    EXPECT_EQ(queue_depth(), 0);
}

TEST_F(DocgenApiTest, GenerateRejectsAnInvoiceWhoseVatExceedsItsTotal) {
    auto org = seed_org("444260000017", "Invoice Vat Bound Org LLP");
    auto accountant = member("invoice-vat-bound@example.com", org.id, "accountant");

    json input = valid_invoice_input();
    input["total_tiyn"] = 112000;
    input["vat_rate"] = "12%";
    input["vat_tiyn"] = 99999900;  // the same forgery, stated as an integer
    auto req = authed_json(accountant, {{"template_slug", "invoice"}, {"input", input}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "input.vat_tiyn");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "exceeds_total");
    EXPECT_EQ(queue_depth(), 0);
}

// The legitimate counterpart: one integer per figure in, three mutually
// consistent printed strings out.
TEST_F(DocgenApiTest, GenerateFormatsInvoiceVatAndTotalFromIntegers) {
    auto org = seed_org("444260000018", "Invoice Vat Ok Org LLP");
    auto accountant = member("invoice-vat-ok@example.com", org.id, "accountant");

    json input = valid_invoice_input();
    input["total_tiyn"] = 112000;
    input["vat_rate"] = "12%";
    input["vat_tiyn"] = 12000;
    auto req = authed_json(accountant, {{"template_slug", "invoice"}, {"input", input}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted) << resp->body();

    Ledger::DocumentRepository documents;
    auto doc = documents.find_in_org(
        json::parse(std::string(resp->body()))["document_id"].get<std::string>(), org.id, /*from_primary=*/true);
    ASSERT_TRUE(doc.has_value());
    // Read through the VERSION, like every other assertion in this suite: a
    // freshly generated document has no current version until its render job
    // succeeds, so the document row's own input_snapshot is deliberately null
    // here (migrations/018_document_versions.sql).
    auto version = documents.latest_version(org.id, doc->id);
    ASSERT_TRUE(version.has_value());
    ASSERT_TRUE(version->input_snapshot.has_value());
    const json& stored = *version->input_snapshot;
    EXPECT_EQ(stored["vat_amount"].get<std::string>(), "120,00");
    EXPECT_EQ(stored["total"].get<std::string>(), "1 120,00");
    EXPECT_EQ(stored["total_words"].get<std::string>(), "Одна тысяча сто двадцать тенге 00 тиын");
    // vat_rate is a RATE, not an amount — it stays the caller's own text.
    EXPECT_EQ(stored["vat_rate"].get<std::string>(), "12%");
}

// Third instance of the same forgery class this phase, and the subtlest: the
// LABEL, not the amount. `vat_rate` is caller text printed inside the VAT
// line's parentheses — «НДС (#d.vat_rate): #d.vat_amount ₸» — and nothing
// between the request and the page rewrites a '(', ')', ':', digit, space or
// '₸'. (Under the retired LaTeX path that was the escaper's deliberate
// choice; under Typst nothing transforms a value at all.) So a rate of
// "16%): 9 999 999,00 ₸ (" closed the parenthesis, printed a fabricated
// amount, and reopened it — a forged figure standing directly above the
// server-derived total. The schema pattern is what stops it: the rate may now
// contain only digits, one optional decimal separator and a percent sign, so
// it cannot express a parenthesis, a colon, a space or a currency sign at all.
TEST_F(DocgenApiTest, GenerateRejectsAVatRateThatEscapesItsLabel) {
    auto org = seed_org("444260000019", "Invoice Rate Org LLP");
    auto accountant = member("invoice-rate@example.com", org.id, "accountant");

    for (const auto* slug : {"invoice", "avr"}) {
        json input = valid_invoice_input();
        input["total_tiyn"] = 112000;
        input["vat_tiyn"] = 12000;
        input["vat_rate"] = "16%): 9 999 999,00 ₸ (";
        if (std::string(slug) == "avr")
            input["act_period"] = "август 2026";
        auto req = authed_json(accountant, {{"template_slug", slug}, {"input", input}});
        HttpResponsePtr resp;
        ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
        ASSERT_NE(resp, nullptr) << slug;
        EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity) << slug;
        auto body = json::parse(std::string(resp->body()));
        EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "input") << slug;
        EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "schema_validation_failed") << slug;
    }
    EXPECT_EQ(queue_depth(), 0);
}

// The schemas' `dependencies` block couples vat_rate and vat_amount both
// ways. Without it, a caller could send `vat_tiyn` and no rate: the server
// would write vat_amount, and the invoice template — whose guard is only
// `{% if vat_amount != "" %}` — would print «НДС (): 120,00 ₸» with an empty
// label. This proves the dependency actually fires rather than being inert
// JSON nobody exercises.
TEST_F(DocgenApiTest, GenerateRejectsAVatAmountWithNoRateToLabelIt) {
    auto org = seed_org("444260000020", "Invoice Rate Dep Org LLP");
    auto accountant = member("invoice-rate-dep@example.com", org.id, "accountant");

    json input = valid_invoice_input();
    input["total_tiyn"] = 112000;
    input["vat_tiyn"] = 12000;  // no vat_rate alongside it
    auto req = authed_json(accountant, {{"template_slug", "invoice"}, {"input", input}});
    HttpResponsePtr resp;
    ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "input");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "schema_validation_failed");
    EXPECT_EQ(queue_depth(), 0);
}

// Release defect v0.3.1: these five templates exist on disk and resolved
// through the registry, then went into documents.doc_type verbatim and blew
// up on documents_doc_type_check inside the INSERT — a 500 for a request the
// server could have refused up front. The transaction rolled back, so there
// was never an orphan row; the bug was purely the status code and the
// message the caller got.
TEST_F(DocgenApiTest, GenerateRejectsNonPrimarySlugWith422NotFiveHundred) {
    auto org = seed_org("444260000014", "Slug Org LLP");
    auto accountant = member("slug@example.com", org.id, "accountant");

    for (const auto* slug : {"payslip", "fno_910", "fno_300", "hr_order", "labor_contract"}) {
        auto req = authed_json(accountant, {{"template_slug", slug}, {"input", json::object()}});
        HttpResponsePtr resp;
        ctrl.generate(req, [&](const HttpResponsePtr& r) { resp = r; });
        ASSERT_NE(resp, nullptr) << slug;
        EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity) << slug;
        auto body = json::parse(std::string(resp->body()));
        EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "template_slug") << slug;
        EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "unsupported_template") << slug;
    }
    EXPECT_EQ(queue_depth(), 0);
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
