/**
 * @file test_agent_repository.cpp
 * @brief Хранилище агента против настоящего Postgres
 *        (migrations/029_agent_foundation.sql).
 *
 * Проверяется не CRUD, а свойства, при нарушении которых агент становится
 * неразбираемым или неограниченным:
 *   1. Граница NULL и НУЛЯ переживает базу: `max_amount_tiyn IS NULL` — «предел
 *      не задан», `0` — «только действия без денег». Спутать их значит выдать
 *      неограниченную автономию тому, кто просил самую строгую.
 *   2. Расход ПРИБАВЛЯЕТСЯ, а не присваивается: иначе бюджет считался бы по
 *      последнему вызову модели вместо всех.
 *   3. Одобрение без ответственного невозможно — это держит БАЗА.
 *   4. Прогоны и одобрения одной организации недостижимы из другой.
 */

#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "agent/AgentRepository.hpp"
#include "database/Database.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

using json = nlohmann::json;

class AgentRepoTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        TestHelpers::wipe_org_data();
    }

    std::string make_org(const std::string& bin) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "Agent Org " + bin, "snr_simplified", false).id;
    }

    std::string make_user(const std::string& email) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name("User");
        if (!role)
            throw std::runtime_error("seed role missing: User");
        return users.create(email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, true).id;
    }

    void set_policy(const std::string& org_id,
                    const std::string& action,
                    const std::string& mode,
                    const std::string& max_amount_sql,
                    const std::string& allowlist_sql = "NULL") {
        // Значения идут ПАРАМЕТРАМИ; текстом остаются только структурные куски
        // (NULL / 0 / ARRAY[...]) — именно их этот тест и различает. Склейка
        // значений в SQL здесь была бы и небезопасна, и лишней.
        Database::get().execute_write([&](auto& txn) {
            txn.exec_params(
                "INSERT INTO autonomy_policies (org_id, action, mode, max_amount_tiyn, counterparty_allowlist) "
                "VALUES ($1, $2, $3, " +
                    max_amount_sql + ", " + allowlist_sql + ")",
                org_id,
                action,
                mode);
            return true;
        });
    }
};

// --------------------------------------------------------------------------
// Прогоны и расход
// --------------------------------------------------------------------------

TEST_F(AgentRepoTest, ARunStartsRunningAndFinishesWithAStatus) {
    Agent::AgentRepository repo;
    auto org = make_org("777260000001");
    auto user = make_user("agent-run@example.com");

    auto run = repo.start_run(org, "chat", user);
    EXPECT_EQ(run.status, "running");
    EXPECT_EQ(run.trigger, "chat");
    ASSERT_TRUE(run.started_by.has_value());

    ASSERT_TRUE(repo.finish_run(org, run.id, "succeeded", "разобрал три документа"));
    auto reloaded = repo.find_run(org, run.id);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->status, "succeeded");
    EXPECT_EQ(reloaded->summary, "разобрал три документа");
}

TEST_F(AgentRepoTest, AScheduledRunHasNoHumanInitiatorAndThatIsNotAnError) {
    // Записывать сюда кого-то «за компанию» значило бы подделать след: у
    // прогона по расписанию инициатора-человека нет.
    Agent::AgentRepository repo;
    auto org = make_org("777260000002");
    auto run = repo.start_run(org, "schedule", std::nullopt);
    EXPECT_FALSE(run.started_by.has_value());
}

TEST_F(AgentRepoTest, UsageAccumulatesAcrossModelCalls) {
    // Прогон делает много вызовов модели. Присваивание вместо прибавления
    // потеряло бы всё, кроме последнего, и бюджет организации считался бы
    // неверно в МЕНЬШУЮ сторону — то есть предел не срабатывал бы.
    Agent::AgentRepository repo;
    auto org = make_org("777260000003");
    auto run = repo.start_run(org, "chat", std::nullopt);

    ASSERT_TRUE(repo.add_usage(org, run.id, 1000, 200, 15'00));
    ASSERT_TRUE(repo.add_usage(org, run.id, 500, 100, 7'00));

    auto reloaded = repo.find_run(org, run.id);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->input_tokens, 1500);
    EXPECT_EQ(reloaded->output_tokens, 300);
    EXPECT_EQ(reloaded->cost_tiyn, 22'00);
}

TEST_F(AgentRepoTest, CostSinceSumsOnlyThisOrgsRuns) {
    Agent::AgentRepository repo;
    auto mine = make_org("777260000004");
    auto theirs = make_org("777260000005");

    auto a = repo.start_run(mine, "chat", std::nullopt);
    auto b = repo.start_run(theirs, "chat", std::nullopt);
    repo.add_usage(mine, a.id, 0, 0, 100'00);
    repo.add_usage(theirs, b.id, 0, 0, 999'00);

    EXPECT_EQ(repo.cost_since(mine, "2000-01-01T00:00:00Z"), 100'00);
}

// --------------------------------------------------------------------------
// Шаги
// --------------------------------------------------------------------------

TEST_F(AgentRepoTest, StepNumbersAreAssignedByTheDatabaseAndStayOrdered) {
    Agent::AgentRepository repo;
    auto org = make_org("777260000006");
    auto run = repo.start_run(org, "chat", std::nullopt);

    repo.append_step(run.id, "prompt", json{{"text", "посчитай налог"}});
    repo.append_step(
        run.id, "tool_call", json{{"args", json::object()}}, "tax.calculate", "auto", "политика разрешает");
    repo.append_step(run.id, "model_reply", json{{"text", "готово"}});

    auto steps = repo.list_steps(run.id);
    ASSERT_EQ(steps.size(), 3u);
    EXPECT_EQ(steps[0].seq, 1);
    EXPECT_EQ(steps[1].seq, 2);
    EXPECT_EQ(steps[2].seq, 3);
    EXPECT_EQ(steps[1].tool.value_or(""), "tax.calculate");
    // Вердикт политики хранится ДАЖЕ для разрешённого шага: иначе вопрос
    // «почему это прошло само» останется без ответа.
    EXPECT_EQ(steps[1].policy_decision.value_or(""), "auto");
    EXPECT_FALSE(steps[1].policy_reason.value_or("").empty());
}

// --------------------------------------------------------------------------
// Политики: NULL против нуля
// --------------------------------------------------------------------------

TEST_F(AgentRepoTest, AnUnsetLimitSurvivesTheDatabaseAsUnset) {
    Agent::AgentRepository repo;
    auto org = make_org("777260000007");
    set_policy(org, "ledger.post_entry", "auto", "NULL");

    auto p = repo.find_policy(org, "ledger.post_entry");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->mode, Agent::Mode::kAuto);
    EXPECT_FALSE(p->max_amount_tiyn.has_value()) << "предел «не задан» превратился в число";
}

TEST_F(AgentRepoTest, AZeroLimitSurvivesTheDatabaseAsZeroNotAsUnset) {
    // Ровно та подмена, из-за которой самая строгая политика стала бы
    // неограниченной: 0 обязан остаться нулём.
    Agent::AgentRepository repo;
    auto org = make_org("777260000008");
    set_policy(org, "ledger.post_entry", "auto", "0");

    auto p = repo.find_policy(org, "ledger.post_entry");
    ASSERT_TRUE(p.has_value());
    ASSERT_TRUE(p->max_amount_tiyn.has_value());
    EXPECT_EQ(*p->max_amount_tiyn, 0);

    Agent::Action a;
    a.name = "ledger.post_entry";
    a.amount_tiyn = 1;
    EXPECT_EQ(Agent::evaluate(p, a).mode, Agent::Mode::kApprove);
}

TEST_F(AgentRepoTest, TheAllowlistRoundTripsThroughThePostgresArray) {
    Agent::AgentRepository repo;
    auto org = make_org("777260000009");
    set_policy(org,
               "docgen.render",
               "auto",
               "NULL",
               "ARRAY['11111111-1111-1111-1111-111111111111','22222222-2222-2222-2222-222222222222']::uuid[]");

    auto p = repo.find_policy(org, "docgen.render");
    ASSERT_TRUE(p.has_value());
    ASSERT_EQ(p->counterparty_allowlist.size(), 2u);
    EXPECT_EQ(p->counterparty_allowlist[0], "11111111-1111-1111-1111-111111111111");
}

TEST_F(AgentRepoTest, AMissingPolicyIsEmptyWhichTheEngineReadsAsAskAHuman) {
    Agent::AgentRepository repo;
    auto org = make_org("777260000010");

    auto p = repo.find_policy(org, "ledger.post_entry");
    EXPECT_FALSE(p.has_value());

    Agent::Action a;
    a.name = "ledger.post_entry";
    EXPECT_EQ(Agent::evaluate(p, a).mode, Agent::Mode::kApprove);
}

TEST_F(AgentRepoTest, AnotherOrgsPolicyIsNeverPickedUp) {
    Agent::AgentRepository repo;
    auto mine = make_org("777260000011");
    auto theirs = make_org("777260000012");
    set_policy(theirs, "ledger.post_entry", "auto", "NULL");

    EXPECT_FALSE(repo.find_policy(mine, "ledger.post_entry").has_value());
}

// --------------------------------------------------------------------------
// Одобрения
// --------------------------------------------------------------------------

TEST_F(AgentRepoTest, AnApprovalCarriesAHumanReadableDiffAndAppearsPending) {
    Agent::AgentRepository repo;
    auto org = make_org("777260000013");
    auto run = repo.start_run(org, "chat", std::nullopt);

    auto req = repo.request_approval(org,
                                     run.id,
                                     "ledger.post_entry",
                                     json{{"amount_tiyn", 1'250'000}},
                                     "провести 12 500,00 ₸ по счёту от ТОО «Ромашка»");
    EXPECT_EQ(req.status, "pending");
    EXPECT_NE(req.human_diff.find("12 500,00"), std::string::npos);

    auto pending = repo.list_pending(org);
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].id, req.id);
}

TEST_F(AgentRepoTest, DecidingRecordsWhoDecidedAndRemovesItFromPending) {
    Agent::AgentRepository repo;
    auto org = make_org("777260000014");
    auto user = make_user("approver@example.com");
    auto req = repo.request_approval(org, std::nullopt, "mail.send", json::object(), "отправить письмо");

    auto decided = repo.decide(org, req.id, "approved", user, "ок");
    ASSERT_TRUE(decided.has_value());
    EXPECT_EQ(decided->status, "approved");
    ASSERT_TRUE(decided->decided_by.has_value());
    EXPECT_EQ(*decided->decided_by, user);
    EXPECT_TRUE(repo.list_pending(org).empty());
}

TEST_F(AgentRepoTest, AnApprovalWithoutSomeoneResponsibleIsImpossibleEvenBypassingTheRepository) {
    // Гарантию держит БАЗА. Если бы она жила только в C++, любой другой путь
    // записи создал бы одобрение без ответственного — то есть ровно то, чего
    // инбокс и должен не допускать.
    Agent::AgentRepository repo;
    auto org = make_org("777260000015");
    auto req = repo.request_approval(org, std::nullopt, "mail.send", json::object(), "отправить письмо");

    EXPECT_THROW(
        {
            Database::get().execute_write([&](auto& txn) {
                txn.exec_params("UPDATE approval_requests SET status = 'approved' WHERE id = $1", req.id);
                return true;
            });
        },
        std::exception);
}

TEST_F(AgentRepoTest, ADecidedRequestCannotBeDecidedTwice) {
    // Второе решение по тому же запросу — это переписывание уже принятого
    // человеком решения, и допускать его нельзя.
    Agent::AgentRepository repo;
    auto org = make_org("777260000016");
    auto first = make_user("first@example.com");
    auto second = make_user("second@example.com");
    auto req = repo.request_approval(org, std::nullopt, "mail.send", json::object(), "отправить письмо");

    ASSERT_TRUE(repo.decide(org, req.id, "approved", first, "ок").has_value());
    EXPECT_FALSE(repo.decide(org, req.id, "rejected", second, "передумал").has_value());
}

TEST_F(AgentRepoTest, AnotherOrgCannotDecideOurApproval) {
    Agent::AgentRepository repo;
    auto mine = make_org("777260000017");
    auto theirs = make_org("777260000018");
    auto user = make_user("stranger@example.com");
    auto req = repo.request_approval(mine, std::nullopt, "mail.send", json::object(), "отправить письмо");

    EXPECT_FALSE(repo.decide(theirs, req.id, "approved", user, "ок").has_value());
    EXPECT_EQ(repo.list_pending(mine).size(), 1u);
}

TEST_F(AgentRepoTest, PriorApprovalIsAnswerredByTheDatabaseNotByTheAgent) {
    // Вход для политики require_prior_approval. «Мы это уже одобряли» не должно
    // быть утверждением, которое агент делает о себе сам.
    Agent::AgentRepository repo;
    auto org = make_org("777260000019");
    auto user = make_user("prior@example.com");

    EXPECT_FALSE(repo.has_prior_approval(org, "mail.send"));
    auto req = repo.request_approval(org, std::nullopt, "mail.send", json::object(), "отправить письмо");
    EXPECT_FALSE(repo.has_prior_approval(org, "mail.send")) << "ожидающий запрос засчитан как одобрение";

    repo.decide(org, req.id, "approved", user, "ок");
    EXPECT_TRUE(repo.has_prior_approval(org, "mail.send"));
}

}  // namespace
