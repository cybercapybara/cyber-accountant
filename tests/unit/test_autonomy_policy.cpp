/**
 * @file test_autonomy_policy.cpp
 * @brief Движок политик автономии. Чистый unit-тест: ни БД, ни Drogon.
 *
 * Главный тест здесь — НЕ «политика auto разрешает», а то, что ни одна
 * проверка не может ОСЛАБИТЬ ограничение, поставленное предыдущей. Агент
 * действует от своего имени и полномочиями шире человеческих (решение
 * владельца), поэтому границы — единственное, что стоит между внедрением в
 * промпт и деньгами организации.
 */

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "agent/AutonomyPolicy.hpp"

namespace {

using Agent::Action;
using Agent::Mode;
using Agent::Policy;

Policy auto_policy(const std::string& action = "ledger.post_entry") {
    Policy p;
    p.action = action;
    p.mode = Mode::kAuto;
    return p;
}

Action money_action(long long tiyn, const std::string& name = "ledger.post_entry") {
    Action a;
    a.name = name;
    a.amount_tiyn = tiyn;
    return a;
}

// --------------------------------------------------------------------------
// Запрет по умолчанию
// --------------------------------------------------------------------------

TEST(AutonomyPolicy, NoPolicyMeansAskAHuman) {
    // Самое важное свойство движка. Пустая таблица политик — это состояние, в
    // котором система живёт ДО первой настройки; если бы умолчание было
    // разрешительным, самый неподготовленный момент оказался бы самым
    // опасным.
    auto d = Agent::evaluate(std::nullopt, money_action(1'000'00));
    EXPECT_EQ(d.mode, Mode::kApprove);
    EXPECT_NE(d.reason.find("не задана"), std::string::npos);
}

TEST(AutonomyPolicy, ForbidWinsOverEverything) {
    Policy p = auto_policy();
    p.mode = Mode::kForbid;
    p.max_amount_tiyn = 1'000'000'00;  // щедрый предел не спасает
    auto d = Agent::evaluate(p, money_action(1));
    EXPECT_EQ(d.mode, Mode::kForbid);
}

TEST(AutonomyPolicy, ApproveModeIgnoresGenerousBounds) {
    // Границы для режима `approve` бессмысленны: человек всё равно смотрит.
    Policy p = auto_policy();
    p.mode = Mode::kApprove;
    p.max_amount_tiyn = 1'000'000'00;
    EXPECT_EQ(Agent::evaluate(p, money_action(1)).mode, Mode::kApprove);
}

// --------------------------------------------------------------------------
// Границы: каждая может ПОНИЗИТЬ автономию и ни одна не может её вернуть
// --------------------------------------------------------------------------

TEST(AutonomyPolicy, WithinTheLimitRunsAutomatically) {
    Policy p = auto_policy();
    p.max_amount_tiyn = 500'000'00;
    auto d = Agent::evaluate(p, money_action(499'999'00));
    EXPECT_EQ(d.mode, Mode::kAuto);
}

TEST(AutonomyPolicy, OverTheLimitAsksRatherThanRefuses) {
    // Превышение предела — повод спросить, а не отказать: иначе агент молча
    // перестал бы делать законную работу, и владелец узнал бы об этом по
    // отсутствию результата.
    Policy p = auto_policy();
    p.max_amount_tiyn = 500'000'00;
    auto d = Agent::evaluate(p, money_action(500'000'01));
    EXPECT_EQ(d.mode, Mode::kApprove);
    EXPECT_NE(d.reason.find("предел"), std::string::npos);
}

TEST(AutonomyPolicy, AnActionWithNoAmountDoesNotSlipPastAnAmountLimit) {
    // Ловушка «нет суммы — значит ноль — значит проходит». Предел объявлен для
    // действий с деньгами, и действие, чью сумму мы не знаем, пропускать
    // автоматически нельзя.
    Policy p = auto_policy();
    p.max_amount_tiyn = 500'000'00;
    Action a;
    a.name = "ledger.post_entry";  // сумма не заполнена
    EXPECT_EQ(Agent::evaluate(p, a).mode, Mode::kApprove);
}

TEST(AutonomyPolicy, ZeroLimitIsNotUnlimited) {
    // max_amount_tiyn = 0 означает «только действия без денег», а НЕ «без
    // предела». Спутать эти два смысла — значит выдать неограниченную
    // автономию тому, кто просил максимально строгую.
    Policy p = auto_policy();
    p.max_amount_tiyn = 0;
    EXPECT_EQ(Agent::evaluate(p, money_action(1)).mode, Mode::kApprove);
    EXPECT_EQ(Agent::evaluate(p, money_action(0)).mode, Mode::kAuto);
}

TEST(AutonomyPolicy, ACounterpartyOutsideTheAllowlistAsks) {
    Policy p = auto_policy();
    p.counterparty_allowlist = {"aaaaaaaa-0000-0000-0000-000000000001"};
    Action a = money_action(100);
    a.counterparty_id = "bbbbbbbb-0000-0000-0000-000000000002";
    EXPECT_EQ(Agent::evaluate(p, a).mode, Mode::kApprove);

    a.counterparty_id = "aaaaaaaa-0000-0000-0000-000000000001";
    EXPECT_EQ(Agent::evaluate(p, a).mode, Mode::kAuto);
}

TEST(AutonomyPolicy, AMissingCounterpartyDoesNotSlipPastAnAllowlist) {
    // Тот же класс, что и с суммой: отсутствие поля не должно означать
    // «ограничение неприменимо».
    Policy p = auto_policy();
    p.counterparty_allowlist = {"aaaaaaaa-0000-0000-0000-000000000001"};
    EXPECT_EQ(Agent::evaluate(p, money_action(100)).mode, Mode::kApprove);
}

TEST(AutonomyPolicy, PriorApprovalIsRequiredWhenThePolicySaysSo) {
    Policy p = auto_policy();
    p.require_prior_approval = true;
    Action a = money_action(100);
    EXPECT_EQ(Agent::evaluate(p, a).mode, Mode::kApprove);

    a.has_prior_approval = true;
    EXPECT_EQ(Agent::evaluate(p, a).mode, Mode::kAuto);
}

TEST(AutonomyPolicy, OneViolationIsEnoughEvenWhenEverythingElsePasses) {
    // Ключевое свойство: проверки только УЖЕСТОЧАЮТ. Сумма в пределах, тип
    // документа верный, предыдущее одобрение есть — но контрагент чужой, и
    // этого достаточно.
    Policy p = auto_policy();
    p.max_amount_tiyn = 1'000'00;
    p.counterparty_allowlist = {"aaaaaaaa-0000-0000-0000-000000000001"};
    p.doc_type = "invoice";
    Action a = money_action(999'00);
    a.doc_type = "invoice";
    a.counterparty_id = "cccccccc-0000-0000-0000-000000000003";
    EXPECT_EQ(Agent::evaluate(p, a).mode, Mode::kApprove);
}

TEST(AutonomyPolicy, EveryDecisionCarriesAReasonEvenWhenItAllows) {
    // Причина пишется в agent_steps.policy_reason ДАЖЕ для разрешённого
    // действия: иначе вопрос «почему это прошло само» останется без ответа
    // ровно тогда, когда ответ понадобится.
    Policy p = auto_policy();
    auto d = Agent::evaluate(p, money_action(1));
    EXPECT_EQ(d.mode, Mode::kAuto);
    EXPECT_FALSE(d.reason.empty());
}

TEST(AutonomyPolicy, ModeParsingRefusesUnknownValuesRatherThanGuessing) {
    EXPECT_EQ(Agent::parse_mode("auto").value(), Mode::kAuto);
    EXPECT_EQ(Agent::parse_mode("approve").value(), Mode::kApprove);
    EXPECT_EQ(Agent::parse_mode("forbid").value(), Mode::kForbid);
    // Неизвестное значение — не «наверное auto»: строка из БД, которую мы не
    // понимаем, обязана быть отказом разбора, а не догадкой.
    EXPECT_FALSE(Agent::parse_mode("AUTO").has_value());
    EXPECT_FALSE(Agent::parse_mode("").has_value());
    EXPECT_FALSE(Agent::parse_mode("yes").has_value());
}

TEST(AutonomyPolicy, ModeNamesRoundTrip) {
    for (const auto m : {Mode::kAuto, Mode::kApprove, Mode::kForbid})
        EXPECT_EQ(Agent::parse_mode(Agent::mode_name(m)).value(), m);
}

}  // namespace
