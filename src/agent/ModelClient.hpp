/**
 * @file ModelClient.hpp
 * @brief Вызов модели через прокси-фильтр (спека P4 §2, §9).
 *
 * ТРИ ОТКАЗА ДО ЛЮБОГО ЗАПРОСА, и все три — с внятной причиной, а не с
 * падением:
 *   1. Ключ не настроен. Агент просто не работает, и это состояние системы
 *      прямо сейчас: ключа в кластере нет. Отказ обязан называть, ЧТО и КУДА
 *      положить, иначе первый же человек полезет искать причину в коде.
 *   2. Адрес ведёт наружу кластера (`Agent::Egress`). Прокси — граница
 *      маскирования персональных данных, и обойти её настройкой нельзя.
 *   3. Бюджет организации исчерпан. Прогон останавливается с причиной, а не
 *      обрывается молча.
 *
 * ПЕРЕНАПРАВЛЕНИЯ ОТКЛЮЧЕНЫ. Редирект с разрешённого адреса на запрещённый
 * обесценивает проверку, сделанную ДО запроса, — ровно та же оговорка стоит в
 * src/webhooks/Webhooks.hpp, где защита зеркальная.
 *
 * КЛЮЧ НЕ ПОПАДАЕТ НИ В ЛОГИ, НИ В ТРАССИРОВКУ. Он живёт в заголовке запроса и
 * нигде больше; сообщения об ошибках его не печатают даже при отладке.
 */

#pragma once

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include "agent/ModelEgress.hpp"
#include "email/Mailer.hpp"  // detail::ensure_curl_init — одноразовый curl_global_init
#include "utils/Config.hpp"

namespace Agent {

using json = nlohmann::json;

/// Отказ, который вызывающий обязан показать человеку, а не проглотить.
struct NotConfigured : std::runtime_error {
    explicit NotConfigured(const std::string& what) : std::runtime_error(what) {}
};

/// Ответ модели, приведённый к тому, что нужно циклу прогона.
struct ModelReply {
    /// Текст ответа (может быть пуст, если модель только вызвала инструмент).
    std::string text;
    /// Запрошенные вызовы инструментов: имя и аргументы.
    std::vector<std::pair<std::string, json>> tool_calls;
    long long input_tokens = 0;
    long long output_tokens = 0;
    /// Причина остановки, как её вернула модель.
    std::string stop_reason;
};

namespace detail {

inline std::string cfg(const char* path, const char* env, const char* fallback) {
    if (Config::is_initialized())
        return Config::get().get<std::string>(path, env, fallback);
    if (const char* v = std::getenv(env))
        return v;
    return fallback;
}

inline std::size_t collect(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace detail

/// Адрес прокси. Умолчание — сервис в кластере, а не адрес поставщика:
/// умолчание, ведущее наружу, однажды сработает при пустой настройке.
inline std::string base_url() {
    return detail::cfg("agent.base_url", "AGENT_BASE_URL", "http://guardrails-llm-filter.guardrails.svc:8080");
}

inline std::string model_name() {
    return detail::cfg("agent.model", "AGENT_MODEL", "claude-sonnet-5");
}

/// Ключ. ПУСТО — это законное состояние («агент не настроен»), а не ошибка
/// чтения: система работает без агента, и падать на старте она не должна.
inline std::string api_key() {
    return detail::cfg("agent.api_key", "AGENT_API_KEY", "");
}

/// Готов ли агент к работе. Вызывающий показывает это в интерфейсе, чтобы
/// «агент молчит» не выглядело поломкой.
inline bool is_configured() {
    return !api_key().empty() && Egress::refusal_reason(base_url()).empty();
}

/// Почему агент не готов. Пусто = готов.
inline std::string configuration_problem() {
    if (api_key().empty())
        return "ключ модели не настроен: положите его в секрет 'api' пространства cyber-accountant "
               "под ключом 'anthropic-api-key' и пропишите переменную AGENT_API_KEY";
    return Egress::refusal_reason(base_url());
}

/**
 * @brief Разбор ответа Messages API в то, что нужно циклу.
 * @details Вынесен отдельно и БЕЗ сети — значит проверяется тестами на
 *          заготовленных ответах, а не «когда появится ключ».
 */
inline ModelReply parse_reply(const std::string& raw) {
    ModelReply out;
    const json j = json::parse(raw, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded())
        throw std::runtime_error("agent: model returned a body that is not JSON");

    out.stop_reason = j.value("stop_reason", "");
    if (j.contains("usage")) {
        out.input_tokens = j["usage"].value("input_tokens", 0LL);
        out.output_tokens = j["usage"].value("output_tokens", 0LL);
    }
    if (!j.contains("content") || !j["content"].is_array())
        return out;

    for (const auto& block : j["content"]) {
        const std::string type = block.value("type", "");
        if (type == "text") {
            out.text += block.value("text", "");
        } else if (type == "tool_use") {
            out.tool_calls.emplace_back(block.value("name", ""), block.value("input", json::object()));
        }
        // Блок неизвестного типа ПРОПУСКАЕТСЯ молча намеренно: Messages API
        // добавляет типы со временем, и падать на новом виде блока значило бы
        // ломаться от чужого обновления. Инструменты и текст мы разбираем
        // явно, а всё прочее для цикла несущественно.
    }
    return out;
}

/**
 * @brief Один вызов модели.
 * @param messages история в формате Messages API
 * @param tools описание инструментов (имя, описание, схема аргументов)
 * @throws NotConfigured если ключа нет или адрес ведёт наружу — ДО сети
 * @throws std::runtime_error при транспортной ошибке или не-2xx
 */
inline ModelReply call(const json& messages, const json& tools, int max_tokens = 4096) {
    if (const std::string problem = configuration_problem(); !problem.empty())
        throw NotConfigured(problem);

    const std::string url = base_url() + "/v1/messages";
    const json body = {
        {"model", model_name()},
        {"max_tokens", max_tokens},
        {"messages", messages},
        {"tools", tools},
    };
    const std::string payload = body.dump();

    Email::detail::ensure_curl_init();
    std::unique_ptr<CURL, void (*)(CURL*)> h(curl_easy_init(), curl_easy_cleanup);
    if (!h)
        throw std::runtime_error("agent: curl_easy_init failed");

    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "content-type: application/json");
    hdrs = curl_slist_append(hdrs, "anthropic-version: 2023-06-01");
    const std::string key_hdr = "x-api-key: " + api_key();
    hdrs = curl_slist_append(hdrs, key_hdr.c_str());
    std::unique_ptr<curl_slist, void (*)(curl_slist*)> hdr_guard(hdrs, curl_slist_free_all);

    std::string response;
    curl_easy_setopt(h.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(h.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(h.get(), CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(h.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(h.get(), CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h.get(), CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(h.get(), CURLOPT_NOSIGNAL, 1L);
    // Редирект увёл бы запрос с проверенного адреса на непроверенный, то есть
    // мимо маскирования. Проверка ДО запроса без этого бессмысленна.
    curl_easy_setopt(h.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h.get(), CURLOPT_WRITEFUNCTION, &detail::collect);
    curl_easy_setopt(h.get(), CURLOPT_WRITEDATA, &response);

    const CURLcode rc = curl_easy_perform(h.get());
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("agent: transport error: ") + curl_easy_strerror(rc));
    long status = 0;
    curl_easy_getinfo(h.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        // Тело ответа НЕ печатаем целиком: в нём может оказаться эхо запроса, а
        // в запросе — данные организации. Достаточно кода и адреса прокси.
        throw std::runtime_error("agent: model returned HTTP " + std::to_string(status) + " via " +
                                 Egress::host_of(url));
    }

    return parse_reply(response);
}

}  // namespace Agent
