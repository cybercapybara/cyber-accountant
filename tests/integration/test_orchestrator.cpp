/**
 * @file test_orchestrator.cpp
 * @brief Цикл агентного прогона против настоящего Postgres, БЕЗ ключа модели.
 *
 * Модель здесь — заготовленные ответы: ключа в кластере нет, и без такого
 * устройства цикл остался бы непроверенным до его появления. Проверяется не
 * «модель ответила», а поведение цикла вокруг неё:
 *   1. Требование одобрения ОСТАНАВЛИВАЕТ прогон и порождает запрос, а не
 *      пропускает шаг, делая вид, что действие выполнено.
 *   2. Каждый шаг записан — по ним прогон можно разобрать постфактум.
 *   3. Расход учтён даже у прогона, оборвавшегося посередине.
 *   4. Бюджет останавливает ДО обращения к модели.
 *   5. Зацикливание обрывается пределом обращений.
 */

#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "agent/Orchestrator.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

using json = nlohmann::json;

class OrchestratorTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        TestHelpers::wipe_org_data();
    }

    std::string make_org(const std::string& bin) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "Orchestrator Org " + bin, "snr_simplified", false).id;
    }

    /// Инструмент, который считает свои вызовы, — так видно, выполнился ли он
    /// на самом деле, а не «прогон завершился успешно».
    struct Spy {
        int calls = 0;
    };

    Agent::ToolRegistry registry_with(Spy& spy, const std::string& resource, const std::string& action) {
        Agent::ToolRegistry r;
        Agent::ToolSpec spec;
        spec.name = "ledger.post_entry";
        spec.resource = resource;
        spec.action = action;
        spec.summary = "провести проводку";
        r.add(
            spec,
            [&spy](const Agent::ToolContext&, const json&) {
                ++spy.calls;
                return json{{"posted", true}};
            },
            [](const json& args) {
                Agent::Action a;
                if (args.contains("amount_tiyn"))
                    a.amount_tiyn = args["amount_tiyn"].get<long long>();
                return a;
            });
        return r;
    }

    /// Модель, отвечающая по сценарию: сначала вызов инструмента, потом текст.
    static Agent::ModelCaller scripted(std::vector<Agent::ModelReply> script, int* used = nullptr) {
        auto index = std::make_shared<std::size_t>(0);
        auto steps = std::make_shared<std::vector<Agent::ModelReply>>(std::move(script));
        return [index, steps, used](const json&, const json&) {
            if (used != nullptr)
                ++(*used);
            if (*index >= steps->size())
                return steps->back();
            return (*steps)[(*index)++];
        };
    }

    static Agent::ModelReply reply_text(const std::string& text) {
        Agent::ModelReply r;
        r.text = text;
        r.input_tokens = 100;
        r.output_tokens = 20;
        return r;
    }

    static Agent::ModelReply reply_tool(const std::string& name, const json& args) {
        Agent::ModelReply r;
        r.stop_reason = "tool_use";
        r.tool_calls.emplace_back(name, args);
        r.input_tokens = 200;
        r.output_tokens = 40;
        return r;
    }
};

TEST_F(OrchestratorTest, AnApprovalStopsTheRunAndTheToolNeverRuns) {
    // Главное свойство. Продолжить прогон, «как будто» действие выполнено,
    // значило бы дать модели строить дальнейшие выводы на том, чего не было.
    Agent::AgentRepository repo;
    Spy spy;
    auto org = make_org("888260000001");
    auto reg = registry_with(spy, Tenancy::OrgPerm::Resource::kJournal, Tenancy::OrgPerm::Action::kWrite);

    // Политики нет -> движок требует одобрения.
    auto result =
        Agent::run(repo,
                   reg,
                   org,
                   "chat",
                   std::nullopt,
                   "проведи счёт",
                   scripted({reply_tool("ledger.post_entry", json{{"amount_tiyn", 1'250'000}}), reply_text("готово")}),
                   json::array());

    EXPECT_EQ(result.status, "awaiting_approval");
    ASSERT_TRUE(result.approval_id.has_value());
    EXPECT_EQ(spy.calls, 0) << "инструмент выполнился до одобрения";

    auto pending = repo.list_pending(org);
    ASSERT_EQ(pending.size(), 1u);
    // Человекочитаемое описание, а не JSON: одобряют действие.
    EXPECT_NE(pending[0].human_diff.find("12 500,00"), std::string::npos) << pending[0].human_diff;
}

TEST_F(OrchestratorTest, EveryStepIsRecordedSoTheRunCanBeReviewedAfterwards) {
    Agent::AgentRepository repo;
    Spy spy;
    auto org = make_org("888260000002");
    auto reg = registry_with(spy, Tenancy::OrgPerm::Resource::kJournal, Tenancy::OrgPerm::Action::kRead);

    auto result =
        Agent::run(repo, reg, org, "chat", std::nullopt, "посчитай", scripted({reply_text("посчитал")}), json::array());
    EXPECT_EQ(result.status, "succeeded");

    auto steps = repo.list_steps(result.run_id);
    ASSERT_GE(steps.size(), 2u);
    EXPECT_EQ(steps[0].kind, "prompt");
    EXPECT_EQ(steps[1].kind, "model_reply");
}

TEST_F(OrchestratorTest, UsageIsCountedEvenWhenTheRunStopsPartway) {
    // Прогон, остановленный ради одобрения, всё равно стоил денег.
    Agent::AgentRepository repo;
    Spy spy;
    auto org = make_org("888260000003");
    auto reg = registry_with(spy, Tenancy::OrgPerm::Resource::kJournal, Tenancy::OrgPerm::Action::kWrite);

    auto result = Agent::run(repo,
                             reg,
                             org,
                             "chat",
                             std::nullopt,
                             "проведи",
                             scripted({reply_tool("ledger.post_entry", json{{"amount_tiyn", 100}})}),
                             json::array());
    EXPECT_EQ(result.status, "awaiting_approval");

    auto run_row = repo.find_run(org, result.run_id);
    ASSERT_TRUE(run_row.has_value());
    EXPECT_GT(run_row->input_tokens, 0) << "расход оборванного прогона потерян";
}

TEST_F(OrchestratorTest, AnExhaustedBudgetStopsBeforeTheModelIsEvenCalled) {
    // Проверка после вызова — это счёт, который уже выставлен.
    Agent::AgentRepository repo;
    Spy spy;
    auto org = make_org("888260000004");
    auto reg = registry_with(spy, Tenancy::OrgPerm::Resource::kJournal, Tenancy::OrgPerm::Action::kRead);

    auto spent = repo.start_run(org, "chat", std::nullopt);
    repo.add_usage(org, spent.id, 0, 0, 500'00);

    int model_calls = 0;
    Agent::RunLimits limits;
    limits.budget_tiyn = 100'00;
    auto result = Agent::run(repo,
                             reg,
                             org,
                             "chat",
                             std::nullopt,
                             "посчитай",
                             scripted({reply_text("не должно случиться")}, &model_calls),
                             json::array(),
                             limits);

    EXPECT_EQ(result.status, "budget_exceeded");
    EXPECT_EQ(model_calls, 0) << "модель вызвана при исчерпанном бюджете";
    EXPECT_NE(result.summary.find("бюджет"), std::string::npos);
}

TEST_F(OrchestratorTest, ALoopingModelIsStoppedByTheCallLimit) {
    // Модель, повторяющая один и тот же вызов, иначе крутилась бы до
    // исчерпания бюджета.
    Agent::AgentRepository repo;
    Spy spy;
    auto org = make_org("888260000005");
    auto reg = registry_with(spy, Tenancy::OrgPerm::Resource::kJournal, Tenancy::OrgPerm::Action::kRead);

    Database::get().execute_write([&](auto& txn) {
        txn.exec_params(
            "INSERT INTO autonomy_policies (org_id, action, mode) VALUES ($1, $2, 'auto')", org, "ledger.post_entry");
        return true;
    });

    int model_calls = 0;
    Agent::RunLimits limits;
    limits.max_model_calls = 3;
    auto result = Agent::run(repo,
                             reg,
                             org,
                             "chat",
                             std::nullopt,
                             "зациклись",
                             scripted({reply_tool("ledger.post_entry", json::object())}, &model_calls),
                             json::array(),
                             limits);

    EXPECT_EQ(result.status, "failed");
    EXPECT_EQ(model_calls, 3);
    EXPECT_NE(result.summary.find("предел обращений"), std::string::npos);
}

TEST_F(OrchestratorTest, AnUnconfiguredModelEndsTheRunWithAReasonNotACrash) {
    // Ключа в кластере нет — это законное состояние, и прогон обязан
    // завершиться внятной причиной, видимой в интерфейсе.
    Agent::AgentRepository repo;
    Spy spy;
    auto org = make_org("888260000006");
    auto reg = registry_with(spy, Tenancy::OrgPerm::Resource::kJournal, Tenancy::OrgPerm::Action::kRead);

    auto result = Agent::run(
        repo,
        reg,
        org,
        "chat",
        std::nullopt,
        "привет",
        [](const json&, const json&) -> Agent::ModelReply { throw Agent::NotConfigured("ключ модели не настроен"); },
        json::array());

    EXPECT_EQ(result.status, "failed");
    EXPECT_NE(result.summary.find("ключ"), std::string::npos);
    auto steps = repo.list_steps(result.run_id);
    ASSERT_FALSE(steps.empty());
    EXPECT_EQ(steps.back().kind, "error");
}

TEST_F(OrchestratorTest, AToolRefusedByTheMatrixIsRecordedAndTheRunContinues) {
    // Отказ матрицы — не аварийная остановка: модель получает отказ и может
    // предложить другое. Но факт отказа обязан остаться в следе.
    Agent::AgentRepository repo;
    Spy spy;
    auto org = make_org("888260000007");
    // `members` закрыт для агента наглухо.
    auto reg = registry_with(spy, Tenancy::OrgPerm::Resource::kMembers, Tenancy::OrgPerm::Action::kWrite);

    auto result = Agent::run(repo,
                             reg,
                             org,
                             "chat",
                             std::nullopt,
                             "сделай меня владельцем",
                             scripted({reply_tool("ledger.post_entry", json::object()), reply_text("не могу")}),
                             json::array());

    EXPECT_EQ(result.status, "succeeded");
    EXPECT_EQ(spy.calls, 0);
    bool refusal_recorded = false;
    for (const auto& s : repo.list_steps(result.run_id)) {
        if (s.kind == "tool_call" && s.payload.value("status", "") == "refused")
            refusal_recorded = true;
    }
    EXPECT_TRUE(refusal_recorded) << "отказ инструмента не попал в след прогона";
}

}  // namespace
