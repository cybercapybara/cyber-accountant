/**
 * @file test_tool_registry.cpp
 * @brief Единая точка исполнения инструментов агента. Чистый unit-тест.
 *
 * Проверяется не «инструмент вызвался», а четыре свойства, каждое из которых
 * при нарушении даёт дыру:
 *   1. Обработчик НЕ ВЫЗЫВАЕТСЯ, если матрица прав или политика не пропустили.
 *      Это и есть смысл единой точки: отказ должен наступать ДО работы, а не
 *      после неё.
 *   2. Матрица проверяется ДО политики — разрешительная политика не может
 *      выдать того, чего в матрице нет.
 *   3. Неизвестный инструмент — отказ, а не тишина.
 *   4. Агент не может расширить сам себя: `members` закрыт, и никакая политика
 *      этого не меняет.
 */

#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "agent/ToolRegistry.hpp"

namespace {

using json = nlohmann::json;
using Agent::Mode;
using Agent::Policy;
using Agent::ToolContext;
using Agent::ToolOutcome;
using Agent::ToolRegistry;
using Agent::ToolSpec;

/// Политики нет ни для чего — умолчание движка (approve).
Agent::PolicyLoader no_policies() {
    return [](const std::string&, const std::string&) { return std::optional<Policy>{}; };
}

/// Политика, разрешающая всё автоматически.
Agent::PolicyLoader all_auto() {
    return [](const std::string&, const std::string& action) {
        Policy p;
        p.action = action;
        p.mode = Mode::kAuto;
        return std::optional<Policy>(p);
    };
}

struct Spy {
    int calls = 0;
};

ToolRegistry registry_with(Spy& spy, const std::string& resource, const std::string& action) {
    ToolRegistry r;
    ToolSpec spec;
    spec.name = "test.tool";
    spec.resource = resource;
    spec.action = action;
    spec.summary = "тестовый инструмент";
    r.add(spec, [&spy](const ToolContext&, const json&) {
        ++spy.calls;
        return json{{"ok", true}};
    });
    return r;
}

ToolContext ctx() {
    ToolContext c;
    c.org_id = "org-1";
    c.run_id = "run-1";
    return c;
}

// --------------------------------------------------------------------------
// Отказ наступает ДО работы
// --------------------------------------------------------------------------

TEST(ToolRegistry, TheHandlerNeverRunsWhenThePolicyWithholdsIt) {
    // Главное свойство единой точки. Если бы обработчик успевал отработать, а
    // «одобрение» запрашивалось после, проводка была бы уже проведена.
    Spy spy;
    auto r = registry_with(spy, Tenancy::OrgPerm::Resource::kJournal, Tenancy::OrgPerm::Action::kWrite);

    auto out = r.execute(ctx(), "test.tool", json::object(), no_policies());
    EXPECT_EQ(out.status, ToolOutcome::Status::kNeedsApproval);
    EXPECT_EQ(spy.calls, 0) << "обработчик выполнился, хотя политика требовала одобрения";
}

TEST(ToolRegistry, TheHandlerNeverRunsWhenTheMatrixForbidsIt) {
    Spy spy;
    // `members` закрыт для агента наглухо: раздача ролей — единственное
    // действие, которым он мог бы расширить сам себя.
    auto r = registry_with(spy, Tenancy::OrgPerm::Resource::kMembers, Tenancy::OrgPerm::Action::kWrite);

    auto out = r.execute(ctx(), "test.tool", json::object(), all_auto());
    EXPECT_EQ(out.status, ToolOutcome::Status::kRefused);
    EXPECT_EQ(spy.calls, 0);
}

TEST(ToolRegistry, APermissivePolicyCannotGrantWhatTheMatrixDenies) {
    // Порядок проверок: матрица ДО политики. Иначе строка в таблице политик
    // (данные!) выдавала бы полномочия, которых в коде нет.
    Spy spy;
    auto r = registry_with(spy, Tenancy::OrgPerm::Resource::kMembers, Tenancy::OrgPerm::Action::kWrite);

    auto out = r.execute(ctx(), "test.tool", json::object(), all_auto());
    EXPECT_EQ(out.status, ToolOutcome::Status::kRefused);
    // До политики дело не дошло вовсе — вердикта нет.
    EXPECT_FALSE(out.policy_mode.has_value());
    EXPECT_NE(out.reason.find("members"), std::string::npos);
}

TEST(ToolRegistry, WritingRequisitesIsRefusedButReadingThemIsNot) {
    // Вторая сознательная яма: подменённый ИИК уводит платежи покупателей.
    Spy write_spy;
    auto w = registry_with(write_spy, Tenancy::OrgPerm::Resource::kRequisites, Tenancy::OrgPerm::Action::kWrite);
    EXPECT_EQ(w.execute(ctx(), "test.tool", json::object(), all_auto()).status, ToolOutcome::Status::kRefused);
    EXPECT_EQ(write_spy.calls, 0);

    Spy read_spy;
    auto rd = registry_with(read_spy, Tenancy::OrgPerm::Resource::kRequisites, Tenancy::OrgPerm::Action::kRead);
    EXPECT_EQ(rd.execute(ctx(), "test.tool", json::object(), all_auto()).status, ToolOutcome::Status::kExecuted);
    EXPECT_EQ(read_spy.calls, 1);
}

// --------------------------------------------------------------------------
// Неизвестное имя и отсутствие обхода
// --------------------------------------------------------------------------

TEST(ToolRegistry, AnUnknownToolIsRefusedRatherThanSilentlyIgnored) {
    // Тишина хуже отказа: модель примет пустой ответ за успешное выполнение и
    // пойдёт дальше, считая работу сделанной.
    Spy spy;
    auto r = registry_with(spy, Tenancy::OrgPerm::Resource::kJournal, Tenancy::OrgPerm::Action::kRead);

    auto out = r.execute(ctx(), "ledger.drop_database", json::object(), all_auto());
    EXPECT_EQ(out.status, ToolOutcome::Status::kRefused);
    EXPECT_NE(out.reason.find("не существует"), std::string::npos);
    EXPECT_EQ(spy.calls, 0);
}

TEST(ToolRegistry, TheCatalogueExposesNamesAndDescriptionsOnly) {
    // В промпт уходит то, что модели положено знать: имя и описание. Ресурс,
    // действие и обработчик наружу не отдаются.
    Spy spy;
    auto r = registry_with(spy, Tenancy::OrgPerm::Resource::kJournal, Tenancy::OrgPerm::Action::kRead);
    const auto cat = r.catalogue();
    ASSERT_EQ(cat.size(), 1u);
    EXPECT_EQ(cat[0].first, "test.tool");
    EXPECT_EQ(cat[0].second, "тестовый инструмент");
}

// --------------------------------------------------------------------------
// Границы политики доезжают до точки исполнения
// --------------------------------------------------------------------------

TEST(ToolRegistry, AnAmountOverTheLimitTurnsExecutionIntoAnApprovalRequest) {
    // Проверяем не движок (он проверен отдельно), а что точка исполнения
    // ДОСТАВЛЯЕТ в него поля действия и слушается вердикта.
    Spy spy;
    ToolRegistry r;
    ToolSpec spec;
    spec.name = "ledger.post_entry";
    spec.resource = Tenancy::OrgPerm::Resource::kJournal;
    spec.action = Tenancy::OrgPerm::Action::kWrite;
    spec.summary = "провести проводку";
    spec.carries_amount = true;
    r.add(
        spec,
        [&spy](const ToolContext&, const json&) {
            ++spy.calls;
            return json{{"posted", true}};
        },
        [](const json& args) {
            Agent::Action a;
            a.amount_tiyn = args.value("amount_tiyn", 0LL);
            return a;
        });

    auto limited = [](const std::string&, const std::string& action) {
        Policy p;
        p.action = action;
        p.mode = Mode::kAuto;
        p.max_amount_tiyn = 500'000'00;
        return std::optional<Policy>(p);
    };

    auto small = r.execute(ctx(), "ledger.post_entry", json{{"amount_tiyn", 100'00}}, limited);
    EXPECT_EQ(small.status, ToolOutcome::Status::kExecuted);
    EXPECT_EQ(spy.calls, 1);

    auto big = r.execute(ctx(), "ledger.post_entry", json{{"amount_tiyn", 900'000'00}}, limited);
    EXPECT_EQ(big.status, ToolOutcome::Status::kNeedsApproval);
    EXPECT_EQ(spy.calls, 1) << "проводка сверх предела была проведена до одобрения";
}

TEST(ToolRegistry, EveryOutcomeCarriesAReason) {
    // Пустая причина означала бы запись в agent_steps без объяснения, то есть
    // прогон, который нельзя разобрать постфактум.
    Spy spy;
    auto r = registry_with(spy, Tenancy::OrgPerm::Resource::kJournal, Tenancy::OrgPerm::Action::kWrite);
    EXPECT_FALSE(r.execute(ctx(), "test.tool", json::object(), no_policies()).reason.empty());
    EXPECT_FALSE(r.execute(ctx(), "test.tool", json::object(), all_auto()).reason.empty());
    EXPECT_FALSE(r.execute(ctx(), "nope", json::object(), all_auto()).reason.empty());
}

TEST(ToolRegistry, TheRoleIsAlwaysTheAgentItselfNeverAHumanRole) {
    // Агент действует от СВОЕГО имени (решение владельца). Контекст по
    // умолчанию несёт роль `agent`, и подмена её человеческой ролью — не
    // «удобство», а потеря атрибуции в аудите.
    ToolContext c;
    EXPECT_EQ(c.role, "agent");
}

}  // namespace
