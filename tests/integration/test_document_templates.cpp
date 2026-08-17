/**
 * @file test_document_templates.cpp
 * @brief Хранилище пользовательских шаблонов против настоящего Postgres
 *        (migrations/027_document_templates.sql).
 *
 * Проверяется не CRUD, а четыре свойства, которые дороже него:
 *   1. Опубликованная версия НЕИЗМЕНЯЕМА, и держит это БАЗА, а не C++:
 *      документ, выпущенный вчера, ссылается на версию шаблона, и правка
 *      молча переписала бы прошлое.
 *   2. Единственный разрешённый переход опубликованной версии — в архив.
 *   3. Шаблон одной организации недостижим из другой, а шаблон площадки
 *      виден всем.
 *   4. Своя версия побеждает общую при разрешении.
 */

#include <exception>
#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "docgen/DocumentTemplateRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

using nlohmann::json;

class DocumentTemplatesTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("DELETE FROM document_templates WHERE status <> 'published'");
            // Опубликованные строки триггер удалить не даёт — снимаем их в
            // архив и убираем уже архивные. Ровно тот путь, которым обязан
            // пользоваться и продакшн.
            txn.exec("UPDATE document_templates SET status = 'archived' WHERE status = 'published'");
            txn.exec("DELETE FROM document_templates WHERE status = 'archived'");
            return true;
        });
        TestHelpers::wipe_org_data();
    }

    std::string make_org(const std::string& bin) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "Template Org " + bin, "snr_simplified", false).id;
    }

    static Docgen::DocumentTemplate draft(const std::optional<std::string>& org_id,
                                          const std::string& slug,
                                          int version,
                                          const std::string& status = "draft") {
        Docgen::DocumentTemplate t;
        t.org_id = org_id;
        t.slug = slug;
        t.version = version;
        t.mode = Docgen::TemplateMode::kBlocks;
        t.blocks = json::array({json{{"type", "header"}, {"title", "Счёт"}}});
        t.source = "#let d = json(\"input.json\")\n= Счёт\n";
        t.schema = json{{"type", "object"}};
        t.form = json{{"fields", json::array()}};
        t.expected = "Счёт\nmargin 18mm\n";
        t.status = status;
        return t;
    }
};

TEST_F(DocumentTemplatesTest, CreateAndFindInOrg) {
    Docgen::DocumentTemplateRepository repo;
    auto org_id = make_org("555260000001");

    auto created = repo.create(draft(org_id, "my-invoice", 1), std::nullopt);
    EXPECT_FALSE(created.is_platform_template());
    EXPECT_EQ(created.status, "draft");

    auto found = repo.find_in_org(org_id, created.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->slug, "my-invoice");
    ASSERT_TRUE(found->blocks.has_value());
    EXPECT_EQ((*found->blocks)[0]["type"], "header");
}

TEST_F(DocumentTemplatesTest, TheSameSlugAndVersionTwiceIsAConflict) {
    Docgen::DocumentTemplateRepository repo;
    auto org_id = make_org("555260000002");
    repo.create(draft(org_id, "my-invoice", 1), std::nullopt);

    EXPECT_THROW(repo.create(draft(org_id, "my-invoice", 1), std::nullopt), Docgen::DuplicateTemplateVersion);
}

TEST_F(DocumentTemplatesTest, PlatformTemplatesAlsoGetTheVersionConstraint) {
    // Ловушка обычного UNIQUE: две строки с NULL в org_id считались бы
    // РАЗНЫМИ, и у шаблонов площадки ограничение молча не работало бы вовсе.
    // Поэтому в миграции NULLS NOT DISTINCT — тест закрепляет именно это.
    Docgen::DocumentTemplateRepository repo;
    repo.create(draft(std::nullopt, "platform-invoice", 1), std::nullopt);

    EXPECT_THROW(repo.create(draft(std::nullopt, "platform-invoice", 1), std::nullopt),
                 Docgen::DuplicateTemplateVersion);
}

TEST_F(DocumentTemplatesTest, PublishedVersionsCannotBeEdited) {
    Docgen::DocumentTemplateRepository repo;
    auto org_id = make_org("555260000003");
    auto published = repo.create(draft(org_id, "my-invoice", 1, "published"), std::nullopt);

    auto patch = published;
    patch.source = "#let d = json(\"input.json\")\n= Подделка\n";
    EXPECT_THROW(repo.update_draft(org_id, published.id, patch), Docgen::PublishedTemplateIsImmutable);

    auto reloaded = repo.find_in_org(org_id, published.id);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->source, published.source);
}

TEST_F(DocumentTemplatesTest, PublishedVersionsCannotBeDeletedEvenBypassingTheRepository) {
    // Гарантию держит БАЗА. Если бы она жила только в C++, любой другой путь
    // записи — миграция данных, ручная правка, будущий сервис — стёр бы
    // версию, на которую ссылаются уже выпущенные документы.
    Docgen::DocumentTemplateRepository repo;
    auto org_id = make_org("555260000004");
    auto published = repo.create(draft(org_id, "my-invoice", 1, "published"), std::nullopt);

    EXPECT_THROW(
        {
            Database::get().execute_write([&](auto& txn) {
                txn.exec_params("DELETE FROM document_templates WHERE id = $1", published.id);
                return true;
            });
        },
        std::exception);

    EXPECT_TRUE(repo.find_in_org(org_id, published.id).has_value());
}

TEST_F(DocumentTemplatesTest, PublishedVersionsMayOnlyMoveToArchived) {
    Docgen::DocumentTemplateRepository repo;
    auto org_id = make_org("555260000005");
    auto published = repo.create(draft(org_id, "my-invoice", 1, "published"), std::nullopt);

    // Обратно в черновик — нельзя: это открыло бы правку задним числом.
    EXPECT_THROW(repo.set_status(org_id, published.id, "draft"), Docgen::PublishedTemplateIsImmutable);

    auto archived = repo.set_status(org_id, published.id, "archived");
    ASSERT_TRUE(archived.has_value());
    EXPECT_EQ(archived->status, "archived");
    // Архивация ничего не переписала: текст остался прежним, и выпущенные
    // документы по-прежнему воспроизводимы.
    EXPECT_EQ(archived->source, published.source);
}

TEST_F(DocumentTemplatesTest, DraftsAreFreelyEditable) {
    Docgen::DocumentTemplateRepository repo;
    auto org_id = make_org("555260000006");
    auto created = repo.create(draft(org_id, "my-invoice", 1), std::nullopt);

    auto patch = created;
    patch.source = "#let d = json(\"input.json\")\n= Другой заголовок\n";
    auto updated = repo.update_draft(org_id, created.id, patch);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->source, patch.source);
}

TEST_F(DocumentTemplatesTest, TemplatesOfOneOrgAreInvisibleToAnother) {
    Docgen::DocumentTemplateRepository repo;
    auto mine = make_org("555260000007");
    auto theirs = make_org("555260000008");
    auto created = repo.create(draft(mine, "my-invoice", 1, "published"), std::nullopt);

    EXPECT_FALSE(repo.find_in_org(theirs, created.id).has_value());
    // И разрешение шаблона у чужой организации его не подхватывает.
    EXPECT_FALSE(repo.resolve(theirs, "my-invoice").has_value());
    EXPECT_TRUE(repo.resolve(mine, "my-invoice").has_value());
}

TEST_F(DocumentTemplatesTest, OwnTemplateWinsOverThePlatformOne) {
    Docgen::DocumentTemplateRepository repo;
    auto org_id = make_org("555260000009");
    repo.create(draft(std::nullopt, "invoice", 1, "published"), std::nullopt);
    auto own = repo.create(draft(org_id, "invoice", 1, "published"), std::nullopt);

    auto resolved = repo.resolve(org_id, "invoice");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->id, own.id);
}

TEST_F(DocumentTemplatesTest, ThePlatformTemplateResolvesWhenTheOrgHasNone) {
    Docgen::DocumentTemplateRepository repo;
    auto org_id = make_org("555260000010");
    auto platform = repo.create(draft(std::nullopt, "invoice", 1, "published"), std::nullopt);

    auto resolved = repo.resolve(org_id, "invoice");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->id, platform.id);
    EXPECT_TRUE(resolved->is_platform_template());
}

TEST_F(DocumentTemplatesTest, DraftsAreNeverResolvedForRendering) {
    // Незаконченный шаблон не должен попасть в документ. Разрешение смотрит
    // только на опубликованные версии.
    Docgen::DocumentTemplateRepository repo;
    auto org_id = make_org("555260000011");
    repo.create(draft(org_id, "invoice", 1), std::nullopt);

    EXPECT_FALSE(repo.resolve(org_id, "invoice").has_value());
}

TEST_F(DocumentTemplatesTest, TheNewestPublishedVersionWins) {
    Docgen::DocumentTemplateRepository repo;
    auto org_id = make_org("555260000012");
    repo.create(draft(org_id, "invoice", 1, "published"), std::nullopt);
    auto v2 = repo.create(draft(org_id, "invoice", 2, "published"), std::nullopt);

    auto resolved = repo.resolve(org_id, "invoice");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->id, v2.id);
    EXPECT_EQ(resolved->version, 2);
}

TEST_F(DocumentTemplatesTest, NextVersionCountsPerOwnerNotGlobally) {
    Docgen::DocumentTemplateRepository repo;
    auto org_id = make_org("555260000013");
    repo.create(draft(std::nullopt, "invoice", 1, "published"), std::nullopt);
    repo.create(draft(std::nullopt, "invoice", 2, "published"), std::nullopt);

    // У организации своих версий нет — счёт начинается с единицы, невзирая на
    // вторую версию шаблона площадки.
    EXPECT_EQ(repo.next_version(org_id, "invoice"), 1);
    EXPECT_EQ(repo.next_version(std::nullopt, "invoice"), 3);
}

TEST_F(DocumentTemplatesTest, ArchivedTemplatesStopResolving) {
    Docgen::DocumentTemplateRepository repo;
    auto org_id = make_org("555260000014");
    auto published = repo.create(draft(org_id, "invoice", 1, "published"), std::nullopt);
    ASSERT_TRUE(repo.resolve(org_id, "invoice").has_value());

    repo.set_status(org_id, published.id, "archived");
    EXPECT_FALSE(repo.resolve(org_id, "invoice").has_value());
}

}  // namespace
