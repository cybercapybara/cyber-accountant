/**
 * @file ModelEgress.hpp
 * @brief Проверка адреса модели: агент ходит ТОЛЬКО в прокси-фильтр внутри
 *        кластера (спека P4 §2).
 *
 * ЗАЧЕМ ЭТО ОТДЕЛЬНЫЙ ЗАГОЛОВОК. Проверка чистая — строка на входе, вердикт на
 * выходе, ни сети, ни конфигурации. Значит её можно проверить дёшево и много,
 * а не «мы же настроили правильно».
 *
 * ЧТО ИМЕННО ЗАЩИЩАЕМ. `guardrails-llm-filter` — граница маскирования: через
 * агента идут ФИО сотрудников, БИН, номера счетов и суммы, и наружу они должны
 * уходить только через него. Прямой адрес `api.anthropic.com` в настройке —
 * это не «другой транспорт», а отключённое маскирование.
 *
 * ЗЕРКАЛО СУЩЕСТВУЮЩЕЙ ЗАЩИТЫ. `src/webhooks/Webhooks.hpp` запрещает
 * ВНУТРЕННИЕ адреса (чтобы вебхук не сходил в кластер), здесь запрещены
 * ВНЕШНИЕ. Обе защиты обязаны отключать перенаправления: редирект с
 * разрешённого адреса на запрещённый обходит любую проверку, сделанную до
 * запроса, — об этом прямо написано в вебхуках, и здесь это верно тем более.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace Agent::Egress {

/// Разбор `http://host:port/path` -> host. Пусто, если это не http(s).
inline std::string host_of(const std::string& url) {
    const std::string lower = [&] {
        std::string s = url;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }();
    std::size_t start = 0;
    if (lower.rfind("http://", 0) == 0)
        start = 7;
    else if (lower.rfind("https://", 0) == 0)
        start = 8;
    else
        return {};

    const std::size_t end = lower.find_first_of("/?#", start);
    std::string hostport = lower.substr(start, end == std::string::npos ? std::string::npos : end - start);
    // Учётные данные в адресе (user:pass@host) — обрезаем до хоста, иначе
    // `http://api.anthropic.com@guardrails/` прошёл бы проверку, обращаясь
    // наружу.
    const std::size_t at = hostport.rfind('@');
    if (at != std::string::npos)
        hostport = hostport.substr(at + 1);
    const std::size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(']') == std::string::npos)
        hostport = hostport.substr(0, colon);
    return hostport;
}

/**
 * @brief Разрешён ли этот адрес как выход к модели.
 * @details Разрешаем только кластерные имена: `<service>`,
 *          `<service>.<namespace>`, `<...>.svc`, `<...>.svc.cluster.local`.
 *          Всё остальное — отказ, включая:
 *          - публичные домены (`api.anthropic.com`);
 *          - `localhost` и петлевой адрес: соблазн «поставлю туннель рядом»
 *            обходит маскирование ровно так же, как прямой адрес;
 *          - IP-адреса: имя сервиса в кластере не бывает адресом, а адрес
 *            невозможно сверить с тем, чей он.
 */
inline bool is_allowed_host(const std::string& host) {
    if (host.empty())
        return false;
    if (host == "localhost" || host.rfind("127.", 0) == 0 || host == "::1" || host.front() == '[')
        return false;
    // Любой IPv4 — отказ: адрес не говорит, чей он.
    const bool looks_numeric =
        std::all_of(host.begin(), host.end(), [](unsigned char c) { return (std::isdigit(c) != 0) || c == '.'; });
    if (looks_numeric)
        return false;

    if (host.find('.') == std::string::npos)
        return true;  // односоставное имя сервиса в том же пространстве
    if (host.size() >= 4 && host.compare(host.size() - 4, 4, ".svc") == 0)
        return true;
    if (host.find(".svc.") != std::string::npos)
        return true;
    // `service.namespace` — два сегмента без публичного домена верхнего уровня.
    const std::size_t dots = static_cast<std::size_t>(std::count(host.begin(), host.end(), '.'));
    static constexpr const char* kPublicSuffixes[] = {".com", ".org", ".net", ".io", ".ai", ".dev", ".cloud"};
    const bool public_suffix = std::any_of(std::begin(kPublicSuffixes), std::end(kPublicSuffixes), [&](const char* s) {
        const std::string suffix(s);
        return host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
    });
    return dots == 1 && !public_suffix;
}

/// Причина отказа для сообщения об ошибке. Пусто = адрес допустим.
inline std::string refusal_reason(const std::string& url) {
    const std::string host = host_of(url);
    if (host.empty())
        return "адрес модели должен быть http(s): '" + url + "'";
    if (!is_allowed_host(host))
        return "агент обязан ходить к модели ТОЛЬКО через прокси внутри кластера, а адрес ведёт на '" + host +
               "'. Прокси — граница маскирования персональных данных; прямой адрес отключает её";
    return {};
}

}  // namespace Agent::Egress
