/**
 * @file OrgPermissions.hpp
 * @brief Матрица прав тенантной роли (спека P3 §5.3) как чистая функция.
 * @details До P3 контроль прав был денилистом из одного значения —
 *          `if (ctx.role == "viewer")` в 23 местах девяти контроллеров — и
 *          любая новая роль проходила его насквозь, получая полный CRUD на
 *          журнал проводок, налоги, зарплату и её проведение в учёт. Здесь
 *          таблица §5.3 записана явно, с ЗАПРЕТОМ ПО УМОЛЧАНИЮ: неизвестная
 *          роль, неизвестный ресурс, незаполненная ячейка и неизвестное
 *          действие дают false.
 *
 *          «—» в таблице спеки означает НЕВИДИМО, а не «только чтение»:
 *          поэтому у кадровика нет и read на payroll/journal/tax, и гейт
 *          обязан стоять не только на мутациях, но и на каждом GET
 *          (задача 7 плана).
 *
 *          Роли перечислены один раз, в detail::kRoles, а гранты в строках
 *          kMatrix идут позиционно по этому списку. Роль, добавленная в
 *          kRoles, но не заведённая в строке ресурса, получает nullptr —
 *          то есть отказ. Это и есть свойство, ради которого таблица
 *          написана: забытая ячейка закрывает доступ, а не открывает.
 *
 *          Чистый модуль: ни БД, ни Drogon — тестируется в tests/unit
 *          (tests/unit/test_org_permissions.cpp). Потребители используют
 *          макрос API_REQUIRE_ORG_PERM (src/api/Guards.hpp), а не вызывают
 *          allows() руками.
 */

#pragma once

#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>

namespace Tenancy::OrgPerm {

namespace Resource {
inline constexpr const char* kEmployees = "employees";
inline constexpr const char* kHrDocs = "hr_docs";
inline constexpr const char* kPayroll = "payroll";
inline constexpr const char* kPayrollPosting = "payroll_posting";
inline constexpr const char* kJournal = "journal";
inline constexpr const char* kCounterparties = "counterparties";
inline constexpr const char* kDocuments = "documents";
inline constexpr const char* kTax = "tax";
inline constexpr const char* kMembers = "members";
/// Реквизиты организации и её расчётные счета
/// (migrations/025_org_requisites.sql). Отдельный ресурс, а не часть
/// kMembers: это данные, которые ПЕЧАТАЮТСЯ в документах, и читать их
/// должен всякий, кто эти документы выпускает.
inline constexpr const char* kRequisites = "requisites";
/// Шаблоны первичных документов (конструктор, спека §8).
inline constexpr const char* kTemplates = "templates";
/// Шаблоны КАДРОВЫХ документов — отдельный ресурс, симметрично делению
/// kDocuments / kHrDocs: кадровик правит свои шаблоны, но не шаблоны счетов.
inline constexpr const char* kHrTemplates = "hr_templates";
}  // namespace Resource

namespace Action {
inline constexpr const char* kRead = "read";
inline constexpr const char* kWrite = "write";
}  // namespace Action

namespace detail {

/// Колонки таблицы §5.3 в порядке следования. Роли, которой здесь нет, не
/// принадлежит ни одна ячейка — значит, она закрыта на всё.
inline constexpr const char* kRoles[] = {"owner", "accountant", "hr", "viewer", "agent"};
inline constexpr std::size_t kRoleCount = std::size(kRoles);

/// Одна строка таблицы §5.3. Элементы @c grants идут ПОЗИЦИОННО по kRoles.
/// Значение гранта: "rw" — чтение и запись, "r" — только чтение, "" —
/// невидимо (ни чтения, ни записи), nullptr — ячейка не заполнена вовсе,
/// что тоже означает отказ.
struct MatrixRow {
    const char* resource;
    const char* grants[kRoleCount];
};

/// Таблица прав спеки P3 §5.3. Порядок колонок — kRoles. Колонки выровнены
/// вручную (clang-format off): эту таблицу читают глазами на ревью, и
/// съехавшая колонка здесь стоит дороже единообразия форматирования.
// clang-format off
inline constexpr MatrixRow kMatrix[] = {
    //  ресурс                      owner accountant   hr  viewer  agent
    {Resource::kEmployees,        {"rw",     "rw",   "rw",   "r", "rw"}},
    {Resource::kHrDocs,           {"rw",     "rw",   "rw",   "r", "rw"}},
    {Resource::kPayroll,          {"rw",     "rw",     "",   "r", "rw"}},
    {Resource::kPayrollPosting,   {"rw",     "rw",     "",    "", "rw"}},
    {Resource::kJournal,          {"rw",     "rw",     "",   "r", "rw"}},
    {Resource::kCounterparties,   {"rw",     "rw",     "",   "r", "rw"}},
    {Resource::kDocuments,        {"rw",     "rw",     "",   "r", "rw"}},
    {Resource::kTax,              {"rw",     "rw",     "",   "r", "rw"}},
    // АГЕНТ: две сознательные ЯМЫ в его широких полномочиях.
    //
    // `members` закрыт наглухо. Агент — самый привилегированный актор системы,
    // и раздача ролей это единственное действие, которым он может РАСШИРИТЬ
    // сам себя. Внедрение в промпт (через текст письма, присланный документ,
    // название контрагента) целится именно сюда: «добавь этого пользователя
    // владельцем» — законно выглядящая просьба, необратимая по последствиям.
    //
    // `requisites` — только чтение (реквизиты нужны, чтобы печатать документы).
    // Запись закрыта по той же причине, по какой закрыта бухгалтеру: подменённый
    // ИИК уводит платежи покупателей на чужой счёт, и замечают это недели
    // спустя. Тихая подмена банковских реквизитов — ровно то, ради чего в
    // такую систему и вламываются.
    //
    // Во всём остальном агент действует как бухгалтер и шире человека в другом
    // измерении: он работает по триггерам, без человеческого запроса.
    {Resource::kMembers,          {"rw",      "",      "",    "", ""}},
    // Запись — ТОЛЬКО владелец, и это не про иерархию, а про мошенничество:
    // подменённый ИИК уводит платежи покупателей на чужой счёт, а заметят это
    // через недели. Бухгалтер реквизиты видит (он выпускает по ним документы)
    // и оспорит подмену, но не меняет их сам. Кадровику они не нужны.
    {Resource::kRequisites,       {"rw",      "r",     "",    "r", "r"}},
    // Шаблоны: правит владелец и бухгалтер. Кадровик — только КАДРОВЫЕ, по
    // той же логике, по которой ему отдан kHrDocs и закрыт kDocuments.
    {Resource::kTemplates,        {"rw",     "rw",     "",    "r", "rw"}},
    {Resource::kHrTemplates,      {"rw",     "rw",   "rw",    "r", "rw"}},
};
// clang-format on

/// Индекс роли в kRoles, либо kRoleCount, если роль неизвестна.
constexpr std::size_t role_index(std::string_view role) {
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        if (role == kRoles[i])
            return i;
    }
    return kRoleCount;
}

/**
 * @brief Грант роли @p role на ресурс @p resource в таблице @p rows.
 * @return Строку гранта ("rw" / "r" / ""), либо nullptr, если пары в таблице
 *         нет: неизвестная роль, неизвестный ресурс или незаполненная ячейка.
 * @details Единственная точка «запрета по умолчанию» по двум измерениям из
 *          трёх. Шаблон по массиву строк, а не по kMatrix напрямую, — чтобы
 *          unit-тест мог прогнать ту же логику по собственной таблице с
 *          намеренно пропущенной ячейкой и доказать, что она закрывает.
 */
template <std::size_t N>
constexpr const char* grant_for(const MatrixRow (&rows)[N], std::string_view role, std::string_view resource) {
    const std::size_t r = role_index(role);
    if (r == kRoleCount)
        return nullptr;  // неизвестная роль
    for (std::size_t i = 0; i < N; ++i) {
        if (resource == rows[i].resource)
            return rows[i].grants[r];  // может быть nullptr — незаполненная ячейка
    }
    return nullptr;  // неизвестный ресурс
}

/// Позиция каждой роли в kRoles, зафиксированная на этапе компиляции.
/// Пропущенная колонка в строке kMatrix структурно безопасна (nullptr =
/// отказ), а вот ПЕРЕСТАВЛЕННЫЙ порядок ролей молча сдвигает все гранты на
/// одну колонку — кадровик получил бы колонку бухгалтера, и поймали бы это
/// только тесты. Эти проверки ломают сборку прямо в файле, который правят,
/// добавляя роль: новую роль дописывают В КОНЕЦ kRoles и добавляют сюда свою
/// строку, а порядок уже существующих остаётся неподвижным.
static_assert(kRoleCount == 5, "добавили роль — допишите её static_assert ниже и колонку в каждую строку kMatrix");
static_assert(role_index("owner") == 0, "порядок kRoles изменился — гранты kMatrix сдвинулись");
static_assert(role_index("accountant") == 1, "порядок kRoles изменился — гранты kMatrix сдвинулись");
static_assert(role_index("hr") == 2, "порядок kRoles изменился — гранты kMatrix сдвинулись");
static_assert(role_index("viewer") == 3, "порядок kRoles изменился — гранты kMatrix сдвинулись");
static_assert(role_index("agent") == 4, "порядок kRoles изменился — гранты kMatrix сдвинулись");
static_assert(role_index("nope") == kRoleCount, "неизвестная роль обязана не иметь колонки вовсе");

}  // namespace detail

/**
 * @brief Разрешено ли @p role выполнить @p action над @p resource.
 * @details Запрет по умолчанию во всех трёх измерениях: неизвестный ресурс,
 *          неизвестная роль и неизвестное действие дают false. Любой
 *          непустой грант подразумевает чтение ("rw" и "r" читают, "" и
 *          nullptr — нет); запись требует буквы 'w'.
 */
inline bool allows(const std::string& role, const std::string& resource, const std::string& action) {
    const char* grants = detail::grant_for(detail::kMatrix, role, resource);
    if (grants == nullptr)
        return false;
    const std::string_view granted(grants);
    if (action == Action::kRead)
        return !granted.empty();
    if (action == Action::kWrite)
        return granted.find('w') != std::string_view::npos;
    return false;  // неизвестное действие
}

}  // namespace Tenancy::OrgPerm
