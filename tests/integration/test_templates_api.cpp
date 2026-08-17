/**
 * @file test_templates_api.cpp
 * @brief API конструктора шаблонов (спека §10) против настоящего Postgres.
 *
 * Проверяется не CRUD, а три вещи, которые дороже него:
 *   1. Права по ВИДУ документа: кадровик правит кадровые шаблоны и не трогает
 *      шаблоны счетов — то же деление, что kDocuments / kHrDocs.
 *   2. Негодные блоки отвергаются ПРИ СОЗДАНИИ, с номером блока, а не молча
 *      сохраняются, чтобы упасть при публикации.
 *   3. Опубликованную версию нельзя ни править, ни опубликовать снова.
 */

#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/TemplatesController.hpp"
#include "database/Database.hpp"
#include "docgen/DocumentTemplateRepository.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

using json = nlohmann::json;
using namespace drogon;

class TemplatesApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::TemplatesController ctrl;

    /// Подсистема задач ОБЯЗАНА быть включена: публикация ставит задачу, и без
    /// этого Jobs::submit честно отказывает 503. Мой тест на 202 сначала падал
    /// именно так — то есть он проверял путь ОШИБКИ, а не успеха, чего я и не
    /// заметил, пока сборка не сказала.
    void config_overrides(json& cfg) override {
        cfg["jobs"]["enabled"] = true;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("DELETE FROM document_templates WHERE status <> 'published'");
            txn.exec("UPDATE document_templates SET status = 'archived' WHERE status = 'published'");
            txn.exec("DELETE FROM document_templates WHERE status = 'archived'");
            return true;
        });
        TestHelpers::wipe_org_data();
    }

    Tenancy::Organization seed_org(const std::string& bin) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "Templates API Org " + bin, "snr_simplified", false);
    }

    Security::Auth::AuthPrincipal member(const std::string& email, const std::string& org_id, const std::string& role) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto base_role = roles.find_by_name("User");
        if (!base_role)
            throw std::runtime_error("seed role missing: User");
        auto user = users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, base_role->id, /*confirmed=*/true);
        Security::Auth::AuthPrincipal p;
        p.subject = user.id;
        p.raw_claims = json{{"sub", user.id}};
        Tenancy::OrgMemberRepository members;
        members.add(org_id, user.id, role);
        p.org = org_id;
        return p;
    }

    /// Минимальный годный набор блоков: заголовок даёт статическую подпись,
    /// без которой шаблон нельзя пропустить через гейт вовсе.
    static json good_blocks() {
        return json::array({json{{"type", "header"}, {"title", "Счёт на оплату"}},
                            json{{"type", "totals"}, {"label", "Итого к оплате"}}});
    }

    json create_template(const Security::Auth::AuthPrincipal& p, const std::string& slug, const json& blocks) {
        HttpResponsePtr resp;
        ctrl.createTemplate(TestHelpers::authed_json(p, json{{"slug", slug}, {"blocks", blocks}}),
                            [&](const HttpResponsePtr& r) { resp = r; });
        if (resp == nullptr)
            throw std::runtime_error("createTemplate did not answer");
        return json{{"status", static_cast<int>(resp->statusCode())}, {"body", json::parse(std::string(resp->body()))}};
    }
};

TEST_F(TemplatesApiTest, AccountantCreatesADraftAndItsSourceIsGenerated) {
    auto org = seed_org("666260000001");
    auto accountant = member("tpl-acc@example.com", org.id, "accountant");

    auto created = create_template(accountant, "my_invoice", good_blocks());
    ASSERT_EQ(created["status"], 201) << created["body"].dump();
    const json& data = created["body"]["data"];
    EXPECT_EQ(data["slug"], "my_invoice");
    EXPECT_EQ(data["status"], "draft");
    EXPECT_EQ(data["mode"], "blocks");
    EXPECT_EQ(data["version"], 1);
    // Исходник, схема и обязательные подписи ПОРОЖДЕНЫ, а не присланы.
    EXPECT_NE(data["source"].get<std::string>().find("json(\"input.json\")"), std::string::npos);
    EXPECT_NE(data["expected"].get<std::string>().find("Счёт на оплату"), std::string::npos);
    EXPECT_EQ(data["schema"]["properties"]["total_tiyn"]["type"], "integer");
    // В форме — целое в тиынах, а не строка суммы: её пишет сервер.
    EXPECT_EQ(data["form"]["fields"][0]["widget"], "money");
}

TEST_F(TemplatesApiTest, BadBlocksAreRefusedAtCreationNamingTheBlock) {
    auto org = seed_org("666260000002");
    auto accountant = member("tpl-bad@example.com", org.id, "accountant");

    auto bad = json::array({json{{"type", "header"}, {"title", "Счёт"}}, json{{"type", "qr_code"}}});
    auto created = create_template(accountant, "my_invoice", bad);
    ASSERT_EQ(created["status"], 422) << created["body"].dump();
    EXPECT_EQ(created["body"]["errors"][0]["code"], "unknown_block_type");
    // Номер блока в сообщении: «где-то в шаблоне» не помогает автору.
    EXPECT_NE(created["body"]["errors"][0]["message"].get<std::string>().find("block 1"), std::string::npos);
}

TEST_F(TemplatesApiTest, AnUnknownVariableIsRefusedRatherThanPrintedEmpty) {
    auto org = seed_org("666260000003");
    auto accountant = member("tpl-var@example.com", org.id, "accountant");

    auto blocks =
        json::array({json{{"type", "header"}, {"title", "Счёт"}},
                     json{{"type", "fields"},
                          {"rows", json::array({json{{"label", "Директор"}, {"variable", "org.nonexistent"}}})}}});
    auto created = create_template(accountant, "my_invoice", blocks);
    ASSERT_EQ(created["status"], 422) << created["body"].dump();
    EXPECT_EQ(created["body"]["errors"][0]["code"], "unknown_variable");
}

TEST_F(TemplatesApiTest, HrMayEditHrTemplatesButNotInvoiceTemplates) {
    // Ровно то деление, что у kDocuments / kHrDocs: кадровик ведёт приказы, но
    // счета не его дело.
    auto org = seed_org("666260000004");
    auto hr = member("tpl-hr@example.com", org.id, "hr");

    auto hr_tpl = create_template(hr, "hr_order_custom", good_blocks());
    EXPECT_EQ(hr_tpl["status"], 201) << hr_tpl["body"].dump();

    auto invoice_tpl = create_template(hr, "my_invoice", good_blocks());
    EXPECT_EQ(invoice_tpl["status"], 403) << invoice_tpl["body"].dump();
}

TEST_F(TemplatesApiTest, ViewerMayReadButNotCreate) {
    auto org = seed_org("666260000005");
    auto viewer = member("tpl-viewer@example.com", org.id, "viewer");

    auto created = create_template(viewer, "my_invoice", good_blocks());
    EXPECT_EQ(created["status"], 403);

    HttpResponsePtr list;
    ctrl.listTemplates(TestHelpers::authed(viewer), [&](const HttpResponsePtr& r) { list = r; });
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->statusCode(), k200OK);
}

TEST_F(TemplatesApiTest, EditingADraftRegeneratesEverythingFromTheBlocks) {
    auto org = seed_org("666260000006");
    auto accountant = member("tpl-edit@example.com", org.id, "accountant");
    auto created = create_template(accountant, "my_invoice", good_blocks());
    ASSERT_EQ(created["status"], 201);
    const std::string id = created["body"]["data"]["id"].get<std::string>();

    auto blocks = json::array({json{{"type", "header"}, {"title", "Другой заголовок"}}});
    HttpResponsePtr resp;
    ctrl.patchTemplate(
        TestHelpers::authed_json(accountant, json{{"blocks", blocks}}, Patch),
        [&](const HttpResponsePtr& r) { resp = r; },
        id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK) << std::string(resp->body());
    auto body = json::parse(std::string(resp->body()));
    // Блоки — источник правды: обязательные подписи пересобраны под них.
    EXPECT_NE(body["data"]["expected"].get<std::string>().find("Другой заголовок"), std::string::npos);
    EXPECT_EQ(body["data"]["expected"].get<std::string>().find("Счёт на оплату"), std::string::npos);
}

TEST_F(TemplatesApiTest, APublishedVersionCannotBeEditedThroughTheApi) {
    auto org = seed_org("666260000007");
    auto accountant = member("tpl-pub@example.com", org.id, "accountant");
    auto created = create_template(accountant, "my_invoice", good_blocks());
    ASSERT_EQ(created["status"], 201);
    const std::string id = created["body"]["data"]["id"].get<std::string>();

    Docgen::DocumentTemplateRepository repo;
    ASSERT_TRUE(repo.set_status(org.id, id, Docgen::TemplateStatus::kPublished).has_value());

    HttpResponsePtr resp;
    ctrl.patchTemplate(
        TestHelpers::authed_json(accountant, json{{"blocks", good_blocks()}}, Patch),
        [&](const HttpResponsePtr& r) { resp = r; },
        id);
    ASSERT_NE(resp, nullptr);
    // Запрет держит БАЗА, а контроллер лишь переводит его в понятный 409.
    EXPECT_EQ(resp->statusCode(), k409Conflict) << std::string(resp->body());
}

TEST_F(TemplatesApiTest, PublishingIsQueuedRatherThanDoneInline) {
    // 202, а не 200, и это не оптимизация: гейт рендерит НАСТОЯЩИМ движком, а
    // Typst есть только в образе воркера.
    auto org = seed_org("666260000008");
    auto accountant = member("tpl-queue@example.com", org.id, "accountant");
    auto created = create_template(accountant, "my_invoice", good_blocks());
    ASSERT_EQ(created["status"], 201);
    const std::string id = created["body"]["data"]["id"].get<std::string>();

    HttpResponsePtr resp;
    ctrl.publishTemplate(
        TestHelpers::authed_json(accountant, json::object()), [&](const HttpResponsePtr& r) { resp = r; }, id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted) << std::string(resp->body());
    auto body = json::parse(std::string(resp->body()));
    EXPECT_TRUE(body.contains("job_id"));
    EXPECT_EQ(body["template_id"], id);

    // Шаблон ОСТАЁТСЯ черновиком, пока проверка не прошла.
    Docgen::DocumentTemplateRepository repo;
    auto still = repo.find_in_org(org.id, id);
    ASSERT_TRUE(still.has_value());
    EXPECT_EQ(still->status, "draft");
}

TEST_F(TemplatesApiTest, PublishingAnAlreadyPublishedVersionIsRefused) {
    auto org = seed_org("666260000009");
    auto accountant = member("tpl-repub@example.com", org.id, "accountant");
    auto created = create_template(accountant, "my_invoice", good_blocks());
    ASSERT_EQ(created["status"], 201);
    const std::string id = created["body"]["data"]["id"].get<std::string>();

    Docgen::DocumentTemplateRepository repo;
    ASSERT_TRUE(repo.set_status(org.id, id, Docgen::TemplateStatus::kPublished).has_value());

    HttpResponsePtr resp;
    ctrl.publishTemplate(
        TestHelpers::authed_json(accountant, json::object()), [&](const HttpResponsePtr& r) { resp = r; }, id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    EXPECT_EQ(json::parse(std::string(resp->body()))["errors"][0]["code"], "not_a_draft");
}

TEST_F(TemplatesApiTest, DeleteArchivesAndKeepsTheRowReadable) {
    // Удаление стёрло бы версию, на которую ссылаются выпущенные документы.
    auto org = seed_org("666260000010");
    auto accountant = member("tpl-arch@example.com", org.id, "accountant");
    auto created = create_template(accountant, "my_invoice", good_blocks());
    ASSERT_EQ(created["status"], 201);
    const std::string id = created["body"]["data"]["id"].get<std::string>();

    HttpResponsePtr resp;
    ctrl.archiveTemplate(
        TestHelpers::authed(accountant, Delete), [&](const HttpResponsePtr& r) { resp = r; }, id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK) << std::string(resp->body());
    EXPECT_EQ(json::parse(std::string(resp->body()))["data"]["status"], "archived");

    Docgen::DocumentTemplateRepository repo;
    EXPECT_TRUE(repo.find_in_org(org.id, id).has_value());
}

TEST_F(TemplatesApiTest, AnotherOrgsTemplateIsNotFoundRatherThanForbidden) {
    auto mine = seed_org("666260000011");
    auto theirs = seed_org("666260000012");
    auto my_accountant = member("tpl-mine@example.com", mine.id, "accountant");
    auto their_accountant = member("tpl-theirs@example.com", theirs.id, "accountant");

    auto created = create_template(their_accountant, "my_invoice", good_blocks());
    ASSERT_EQ(created["status"], 201);
    const std::string id = created["body"]["data"]["id"].get<std::string>();

    HttpResponsePtr resp;
    ctrl.patchTemplate(
        TestHelpers::authed_json(my_accountant, json{{"blocks", good_blocks()}}, Patch),
        [&](const HttpResponsePtr& r) { resp = r; },
        id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

TEST_F(TemplatesApiTest, TheVariableCatalogueIsServedToAnyoneWhoMayReadTemplates) {
    auto org = seed_org("666260000013");
    auto viewer = member("tpl-cat@example.com", org.id, "viewer");

    HttpResponsePtr resp;
    ctrl.listVariables(TestHelpers::authed(viewer), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    ASSERT_TRUE(body["data"].is_array());
    bool found_vat = false;
    for (const auto& v : body["data"]) {
        if (v["id"] == "tax.vat_rate")
            found_vat = true;
        // Источник значения наружу не отдаётся.
        EXPECT_FALSE(v.contains("source"));
    }
    EXPECT_TRUE(found_vat);
}

}  // namespace
