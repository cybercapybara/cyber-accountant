/**
 * @file test_model_client.cpp
 * @brief Клиент модели без сети: настройка, отказы и разбор ответа.
 *
 * Ключа модели в кластере нет, и это НОРМАЛЬНОЕ состояние системы, а не
 * временная беда. Значит проверять надо ровно два свойства: что без ключа
 * агент внятно отказывается (а не падает и не молчит), и что разбор ответа
 * работает на заготовленных телах — иначе он остался бы непроверенным до
 * появления ключа.
 */

#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "agent/ModelClient.hpp"

namespace {

using json = nlohmann::json;

class ModelClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        unsetenv("AGENT_API_KEY");
        unsetenv("AGENT_BASE_URL");
    }
    void TearDown() override {
        unsetenv("AGENT_API_KEY");
        unsetenv("AGENT_BASE_URL");
    }
};

TEST_F(ModelClientTest, WithoutAKeyTheAgentIsNotConfiguredAndSaysWhereToPutIt) {
    EXPECT_FALSE(Agent::is_configured());
    const auto problem = Agent::configuration_problem();
    EXPECT_FALSE(problem.empty());
    // Сообщение обязано называть КУДА положить ключ: иначе первый же человек
    // пойдёт искать причину в коде.
    EXPECT_NE(problem.find("anthropic-api-key"), std::string::npos);
    EXPECT_NE(problem.find("AGENT_API_KEY"), std::string::npos);
}

TEST_F(ModelClientTest, CallingWithoutAKeyRefusesBeforeTouchingTheNetwork) {
    // Отказ ДО сети: иначе в лучшем случае получим таймаут, в худшем — запрос
    // без ключа уйдёт наружу и попадёт в чужие логи.
    EXPECT_THROW(Agent::call(json::array(), json::array()), Agent::NotConfigured);
}

TEST_F(ModelClientTest, AKeyAloneIsNotEnoughIfTheAddressLeavesTheCluster) {
    setenv("AGENT_API_KEY", "sk-ant-test", 1);
    setenv("AGENT_BASE_URL", "https://api.anthropic.com", 1);
    EXPECT_FALSE(Agent::is_configured());
    EXPECT_THROW(Agent::call(json::array(), json::array()), Agent::NotConfigured);
}

TEST_F(ModelClientTest, TheDefaultAddressPointsInsideTheClusterNotAtTheVendor) {
    // Умолчание, ведущее наружу, однажды сработает при пустой настройке — и
    // персональные данные уйдут мимо маскирования.
    EXPECT_TRUE(Agent::Egress::refusal_reason(Agent::base_url()).empty()) << Agent::base_url();
}

TEST_F(ModelClientTest, ParsingPullsOutTextToolCallsAndUsage) {
    const std::string raw = R"({
      "stop_reason": "tool_use",
      "usage": {"input_tokens": 1200, "output_tokens": 340},
      "content": [
        {"type": "text", "text": "Считаю налог."},
        {"type": "tool_use", "name": "tax.calculate", "input": {"period": "2026-07"}}
      ]})";
    const auto reply = Agent::parse_reply(raw);
    EXPECT_EQ(reply.text, "Считаю налог.");
    EXPECT_EQ(reply.stop_reason, "tool_use");
    EXPECT_EQ(reply.input_tokens, 1200);
    EXPECT_EQ(reply.output_tokens, 340);
    ASSERT_EQ(reply.tool_calls.size(), 1u);
    EXPECT_EQ(reply.tool_calls[0].first, "tax.calculate");
    EXPECT_EQ(reply.tool_calls[0].second["period"], "2026-07");
}

TEST_F(ModelClientTest, AnUnknownContentBlockIsSkippedRatherThanFatal) {
    // Messages API добавляет типы блоков со временем. Падать на новом виде
    // блока значило бы ломаться от чужого обновления.
    const std::string raw = R"({
      "content": [
        {"type": "thinking", "thinking": "..."},
        {"type": "text", "text": "готово"}
      ]})";
    const auto reply = Agent::parse_reply(raw);
    EXPECT_EQ(reply.text, "готово");
    EXPECT_TRUE(reply.tool_calls.empty());
}

TEST_F(ModelClientTest, AMissingUsageIsZeroNotAnError) {
    // Расход неизвестен — это ноль к бюджету, а не отказ разобрать ответ.
    const auto reply = Agent::parse_reply(R"({"content":[{"type":"text","text":"ок"}]})");
    EXPECT_EQ(reply.input_tokens, 0);
    EXPECT_EQ(reply.output_tokens, 0);
}

TEST_F(ModelClientTest, ABodyThatIsNotJsonIsAClearErrorNotACrash) {
    EXPECT_THROW(Agent::parse_reply("<html>502 Bad Gateway</html>"), std::runtime_error);
}

TEST_F(ModelClientTest, SeveralTextBlocksAreConcatenatedInOrder) {
    const auto reply =
        Agent::parse_reply(R"({"content":[{"type":"text","text":"часть 1 "},{"type":"text","text":"часть 2"}]})");
    EXPECT_EQ(reply.text, "часть 1 часть 2");
}

}  // namespace
