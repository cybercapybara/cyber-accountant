/**
 * @file test_org_permissions.cpp
 * @brief Таблица прав §5.3 как код. Чистый unit-тест: ни БД, ни Drogon.
 *        Главный тест здесь — не «owner может всё», а deny-by-default:
 *        неизвестная роль, неизвестный ресурс и неизвестное действие
 *        обязаны давать false, иначе следующая роль повторит ошибку,
 *        из-за которой эта таблица и появилась.
 */

#include <cstddef>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include "tenancy/OrgPermissions.hpp"

namespace {

using namespace Tenancy::OrgPerm;

/// Все ресурсы матрицы — чтобы свипы шли по таблице целиком, а не по
/// вручную переписанному подмножеству.
constexpr const char* kAllResources[] = {
    Resource::kEmployees,
    Resource::kHrDocs,
    Resource::kPayroll,
    Resource::kPayrollPosting,
    Resource::kJournal,
    Resource::kCounterparties,
    Resource::kDocuments,
    Resource::kTax,
    Resource::kMembers,
};

constexpr const char* kAllActions[] = {Action::kRead, Action::kWrite};

TEST(OrgPermissions, OwnerAndAccountantHaveFullLedgerAccess) {
    for (const auto* role : {"owner", "accountant"}) {
        for (const auto* res : {Resource::kEmployees,
                                Resource::kHrDocs,
                                Resource::kPayroll,
                                Resource::kPayrollPosting,
                                Resource::kJournal,
                                Resource::kCounterparties,
                                Resource::kDocuments,
                                Resource::kTax}) {
            EXPECT_TRUE(allows(role, res, Action::kWrite)) << role << " / " << res;
        }
    }
}

TEST(OrgPermissions, HrSeesOnlyPeopleResources) {
    EXPECT_TRUE(allows("hr", Resource::kEmployees, Action::kWrite));
    EXPECT_TRUE(allows("hr", Resource::kEmployees, Action::kRead));
    EXPECT_TRUE(allows("hr", Resource::kHrDocs, Action::kWrite));
    EXPECT_TRUE(allows("hr", Resource::kHrDocs, Action::kRead));
    // «—» = невидимо: ни записи, НИ ЧТЕНИЯ.
    for (const auto* res : {Resource::kPayroll,
                            Resource::kPayrollPosting,
                            Resource::kJournal,
                            Resource::kCounterparties,
                            Resource::kDocuments,
                            Resource::kTax,
                            Resource::kMembers}) {
        EXPECT_FALSE(allows("hr", res, Action::kRead)) << res;
        EXPECT_FALSE(allows("hr", res, Action::kWrite)) << res;
    }
}

TEST(OrgPermissions, ViewerReadsButNeverWrites) {
    for (const auto* res : {Resource::kEmployees,
                            Resource::kHrDocs,
                            Resource::kPayroll,
                            Resource::kJournal,
                            Resource::kCounterparties,
                            Resource::kDocuments,
                            Resource::kTax}) {
        EXPECT_TRUE(allows("viewer", res, Action::kRead)) << res;
        EXPECT_FALSE(allows("viewer", res, Action::kWrite)) << res;
    }
    // Проведение зарплаты в учёт viewer не видит вовсе.
    EXPECT_FALSE(allows("viewer", Resource::kPayrollPosting, Action::kRead));
    EXPECT_FALSE(allows("viewer", Resource::kPayrollPosting, Action::kWrite));
}

TEST(OrgPermissions, MembersAreOwnerOnly) {
    EXPECT_TRUE(allows("owner", Resource::kMembers, Action::kRead));
    EXPECT_TRUE(allows("owner", Resource::kMembers, Action::kWrite));
    for (const auto* role : {"accountant", "hr", "viewer"}) {
        EXPECT_FALSE(allows(role, Resource::kMembers, Action::kRead)) << role;
        EXPECT_FALSE(allows(role, Resource::kMembers, Action::kWrite)) << role;
    }
}

TEST(OrgPermissions, DenyByDefault) {
    EXPECT_FALSE(allows("superuser", Resource::kJournal, Action::kRead));
    EXPECT_FALSE(allows("", Resource::kJournal, Action::kRead));
    EXPECT_FALSE(allows("owner", "no_such_resource", Action::kRead));
    EXPECT_FALSE(allows("owner", Resource::kJournal, "delete"));
    EXPECT_FALSE(allows("owner", Resource::kJournal, ""));
}

/// Вся таблица §5.3 целиком, ячейка за ячейкой, включая «—». Отдельно от
/// тестов выше: те читаются как намерение, этот — как сверка с документом.
TEST(OrgPermissions, WholeMatrixMatchesSpec53) {
    struct Expectation {
        const char* role;
        const char* resource;
        bool read;
        bool write;
    };
    // clang-format off
    constexpr Expectation kExpected[] = {
        //  роль          ресурс                        read   write
        {"owner",      Resource::kEmployees,       true,  true},
        {"owner",      Resource::kHrDocs,          true,  true},
        {"owner",      Resource::kPayroll,         true,  true},
        {"owner",      Resource::kPayrollPosting,  true,  true},
        {"owner",      Resource::kJournal,         true,  true},
        {"owner",      Resource::kCounterparties,  true,  true},
        {"owner",      Resource::kDocuments,       true,  true},
        {"owner",      Resource::kTax,             true,  true},
        {"owner",      Resource::kMembers,         true,  true},

        {"accountant", Resource::kEmployees,       true,  true},
        {"accountant", Resource::kHrDocs,          true,  true},
        {"accountant", Resource::kPayroll,         true,  true},
        {"accountant", Resource::kPayrollPosting,  true,  true},
        {"accountant", Resource::kJournal,         true,  true},
        {"accountant", Resource::kCounterparties,  true,  true},
        {"accountant", Resource::kDocuments,       true,  true},
        {"accountant", Resource::kTax,             true,  true},
        {"accountant", Resource::kMembers,         false, false},

        {"hr",         Resource::kEmployees,       true,  true},
        {"hr",         Resource::kHrDocs,          true,  true},
        {"hr",         Resource::kPayroll,         false, false},
        {"hr",         Resource::kPayrollPosting,  false, false},
        {"hr",         Resource::kJournal,         false, false},
        {"hr",         Resource::kCounterparties,  false, false},
        {"hr",         Resource::kDocuments,       false, false},
        {"hr",         Resource::kTax,             false, false},
        {"hr",         Resource::kMembers,         false, false},

        {"viewer",     Resource::kEmployees,       true,  false},
        {"viewer",     Resource::kHrDocs,          true,  false},
        {"viewer",     Resource::kPayroll,         true,  false},
        {"viewer",     Resource::kPayrollPosting,  false, false},
        {"viewer",     Resource::kJournal,         true,  false},
        {"viewer",     Resource::kCounterparties,  true,  false},
        {"viewer",     Resource::kDocuments,       true,  false},
        {"viewer",     Resource::kTax,             true,  false},
        {"viewer",     Resource::kMembers,         false, false},
    };
    // clang-format on

    // Сверка полноты: перечислены все роли × все ресурсы, ни одна ячейка
    // спеки не забыта в самом тесте.
    ASSERT_EQ(std::size(kExpected), detail::kRoleCount * std::size(kAllResources));

    for (const auto& e : kExpected) {
        EXPECT_EQ(allows(e.role, e.resource, Action::kRead), e.read) << e.role << " / read / " << e.resource;
        EXPECT_EQ(allows(e.role, e.resource, Action::kWrite), e.write) << e.role << " / write / " << e.resource;
    }
}

/// Неизвестная роль закрыта НА ВСЮ таблицу, а не только на одну ячейку.
/// Это ровно тот сценарий, который денилист `role == "viewer"` открывал
/// настежь: роль, о которой код не знает, проходила все 23 проверки.
TEST(OrgPermissions, UnknownRoleIsDeniedEveryResourceAndAction) {
    for (const auto* role : {"auditor",
                             "superuser",
                             "admin",
                             "root",
                             "hr_assistant",
                             "Owner",    // регистр значим
                             "owner ",   // хвостовой пробел
                             " owner",   // ведущий пробел
                             "owner\t",  // табуляция
                             "own",      // префикс известной роли
                             "ownerx",   // суффикс к известной роли
                             "viewer2",
                             "accountan",
                             ""}) {
        for (const auto* res : kAllResources) {
            for (const auto* act : kAllActions) {
                EXPECT_FALSE(allows(role, res, act)) << role << " / " << act << " / " << res;
            }
        }
    }
}

/// Неизвестное действие закрыто для всех известных ролей и всех ресурсов —
/// третье измерение запрета по умолчанию.
TEST(OrgPermissions, UnknownActionIsDeniedForEveryRoleAndResource) {
    for (std::size_t i = 0; i < detail::kRoleCount; ++i) {
        for (const auto* res : kAllResources) {
            for (const auto* act : {"delete", "write ", "READ", "rw", "*", ""}) {
                EXPECT_FALSE(allows(detail::kRoles[i], res, act)) << detail::kRoles[i] << " / " << act << " / " << res;
            }
        }
    }
}

/// Неизвестный ресурс закрыт для всех известных ролей — второе измерение.
TEST(OrgPermissions, UnknownResourceIsDeniedForEveryRole) {
    for (std::size_t i = 0; i < detail::kRoleCount; ++i) {
        for (const auto* res : {"no_such_resource", "Journal", "journal ", "*", "orgs", ""}) {
            for (const auto* act : kAllActions) {
                EXPECT_FALSE(allows(detail::kRoles[i], res, act)) << detail::kRoles[i] << " / " << act << " / " << res;
            }
        }
    }
}

/// РЕГРЕССИЯ на ту самую ошибку, ради которой задача существует: роль
/// заведена в системе, но её ячейка в строке ресурса не заполнена. Здесь
/// это воспроизводится буквально — таблица, в которой заполнена только
/// колонка owner, — и прогоняется через ту же самую функцию поиска, что и
/// боевая kMatrix. Незаполненная ячейка обязана давать «нет гранта»
/// (nullptr), то есть отказ, а не сквозной проход.
TEST(OrgPermissions, RoleWithoutTableEntryIsDeniedNotAllowed) {
    static constexpr detail::MatrixRow kPartial[] = {
        {Resource::kJournal, {"rw"}},  // заполнена только колонка owner
    };
    // Заполненная ячейка — грант есть.
    EXPECT_NE(detail::grant_for(kPartial, "owner", Resource::kJournal), nullptr);
    // Незаполненные — nullptr, и это отказ, а не «разрешено по умолчанию».
    for (std::size_t i = 1; i < detail::kRoleCount; ++i) {
        EXPECT_EQ(detail::grant_for(kPartial, detail::kRoles[i], Resource::kJournal), nullptr) << detail::kRoles[i];
    }
    // То же свойство на этапе компиляции — чтобы регрессия не смогла
    // проскочить даже мимо запуска тестов.
    static_assert(detail::grant_for(kPartial, "owner", Resource::kJournal) != nullptr);
    static_assert(detail::grant_for(kPartial, "accountant", Resource::kJournal) == nullptr);
    static_assert(detail::grant_for(kPartial, "hr", Resource::kJournal) == nullptr);
    static_assert(detail::grant_for(kPartial, "viewer", Resource::kJournal) == nullptr);
    // И неизвестная роль в такой таблице тоже закрыта.
    static_assert(detail::grant_for(kPartial, "newcomer", Resource::kJournal) == nullptr);
}

/// Каждая ячейка боевой матрицы — либо валидный грант, либо явное «невидимо».
/// Ловит опечатку вида "w" (запись без чтения) или "rwx" при правке таблицы.
TEST(OrgPermissions, EveryMatrixCellIsAWellFormedGrant) {
    for (const auto& row : detail::kMatrix) {
        for (std::size_t i = 0; i < detail::kRoleCount; ++i) {
            const char* cell = row.grants[i];
            ASSERT_NE(cell, nullptr) << row.resource << " / " << detail::kRoles[i];
            const std::string grant(cell);
            EXPECT_TRUE(grant.empty() || grant == "r" || grant == "rw")
                << row.resource << " / " << detail::kRoles[i] << " = '" << grant << "'";
        }
    }
}

}  // namespace
