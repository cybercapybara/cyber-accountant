/**
 * @file test_model_egress.cpp
 * @brief Проверка адреса модели. Чистый unit-тест: ни сети, ни конфигурации.
 *
 * Смысл этих тестов — не «разбор URL работает», а то, что ни один способ
 * записать адрес не выводит агента наружу мимо прокси. Прокси — граница
 * маскирования: через агента идут ФИО, БИН и номера счетов, и обход границы
 * означает, что они уходят к постороннему сервису в открытом виде.
 */

#include <string>

#include <gtest/gtest.h>

#include "agent/ModelEgress.hpp"

namespace {

using Agent::Egress::host_of;
using Agent::Egress::is_allowed_host;
using Agent::Egress::refusal_reason;

TEST(ModelEgress, TheClusterProxyIsAllowed) {
    for (const char* url : {"http://guardrails-llm-filter.guardrails.svc.cluster.local:8080",
                            "http://guardrails-llm-filter.guardrails.svc:8080",
                            "http://guardrails-llm-filter:8080"}) {
        EXPECT_TRUE(refusal_reason(url).empty()) << url;
    }
}

TEST(ModelEgress, TheModelVendorDirectlyIsRefused) {
    // Прямой адрес — это не «другой транспорт», а отключённое маскирование.
    EXPECT_FALSE(refusal_reason("https://api.anthropic.com/v1/messages").empty());
    EXPECT_NE(refusal_reason("https://api.anthropic.com").find("маскирован"), std::string::npos);
}

TEST(ModelEgress, LocalhostIsRefusedBecauseATunnelBypassesMaskingToo) {
    // Соблазн «поставлю туннель рядом и буду ходить в него» обходит границу
    // ровно так же, как прямой адрес.
    for (const char* url : {"http://localhost:8080", "http://127.0.0.1:8080", "http://[::1]:8080"})
        EXPECT_FALSE(refusal_reason(url).empty()) << url;
}

TEST(ModelEgress, ARawIpIsRefusedBecauseAnAddressDoesNotSayWhoseItIs) {
    EXPECT_FALSE(refusal_reason("http://10.0.11.251:8080").empty());
    EXPECT_FALSE(refusal_reason("http://93.184.216.34:8080").empty());
}

TEST(ModelEgress, CredentialsInTheUrlCannotDisguiseTheRealHost) {
    // Ловушка разбора: `http://прокси@api.anthropic.com/` ведёт НА
    // api.anthropic.com, а наивный разбор видит первым «прокси».
    EXPECT_EQ(host_of("http://guardrails@api.anthropic.com/v1"), "api.anthropic.com");
    EXPECT_FALSE(refusal_reason("http://guardrails@api.anthropic.com/v1").empty());

    // И обратная запись тоже не должна обманывать: хост здесь кластерный.
    EXPECT_EQ(host_of("http://api.anthropic.com@guardrails.guardrails.svc:8080/"), "guardrails.guardrails.svc");
}

TEST(ModelEgress, NonHttpSchemesAreRefused) {
    for (const char* url : {"file:///etc/passwd", "ftp://example.com", "", "guardrails:8080"})
        EXPECT_FALSE(refusal_reason(url).empty()) << url;
}

TEST(ModelEgress, AnyTwoSegmentHostIsRefusedBecauseADenylistOfZonesIsUnbounded) {
    // НАЙДЕНО ПРОВЕРКОЙ БЕЗОПАСНОСТИ, и находка настоящая. Первая редакция
    // разрешала любые два сегмента и отсекала публичные домены ПЕРЕЧНЕМ зон
    // (.com/.org/.net/.io/.ai). Зон тысячи — и всё, чего в перечне нет,
    // проходило насквозь. Список исключений открыт, значит защита открыта.
    //
    // Теперь разрешение построено списком ДОПУСТИМЫХ ФОРМ, поэтому отвергается
    // любой двухсегментный хост, включая те, что в прежний перечень не входили.
    for (const char* h : {"evil.com",
                          "example.org",
                          "sneaky.net",
                          "some.io",
                          "model.ai",
                          // ровно те, что проезжали через перечень зон:
                          "evil.ru",
                          "evil.kz",
                          "attacker.xyz",
                          "evil.co",
                          "bad.dev",
                          "x.tk"})
        EXPECT_FALSE(is_allowed_host(h)) << h;
}

TEST(ModelEgress, TheUnambiguousClusterFormsStillWork) {
    // Плата за строгость: `сервис.namespace` больше не принимается, потому что
    // синтаксически неотличим от публичного домена. Однозначные формы —
    // принимаются, и отказ прямо предлагает ими воспользоваться.
    EXPECT_TRUE(is_allowed_host("guardrails-llm-filter"));
    EXPECT_TRUE(is_allowed_host("guardrails-llm-filter.guardrails.svc"));
    EXPECT_TRUE(is_allowed_host("guardrails-llm-filter.guardrails.svc.cluster.local"));
    EXPECT_FALSE(is_allowed_host("guardrails-llm-filter.guardrails"));

    const auto reason = refusal_reason("http://guardrails-llm-filter.guardrails:8080");
    EXPECT_NE(reason.find(".svc"), std::string::npos) << "отказ не подсказывает допустимую форму";
}

TEST(ModelEgress, TheRefusalExplainsWhyRatherThanJustSayingNo) {
    // Сообщение читает человек, который настраивает выкладку. «Запрещено» без
    // причины он обойдёт, потому что решит, что это перестраховка.
    const auto reason = refusal_reason("https://api.anthropic.com");
    EXPECT_NE(reason.find("прокси"), std::string::npos);
    EXPECT_NE(reason.find("персональн"), std::string::npos);
}

}  // namespace
