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
 *          - любые публичные домены, включая двухсегментные (`evil.ru`),
 *            потому что разрешение построено СПИСКОМ ДОПУСТИМЫХ ФОРМ, а не
 *            перечнем запрещённых зон;
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

    // ДАЛЬШЕ — ЗАКРЫТЫЙ СПИСОК ФОРМ, А НЕ СПИСОК ИСКЛЮЧЕНИЙ.
    //
    // Первая редакция разрешала любые два сегмента (`service.namespace`), а
    // публичные домены пыталась отсечь перечнем зон (.com/.org/.net/...).
    // Это неверно ПО УСТРОЙСТВУ: зон тысячи, и `evil.ru`, `evil.kz`,
    // `attacker.xyz` проходили насквозь. Перечень исключений открыт, значит
    // защита открыта.
    //
    // Поэтому форма `service.namespace` не разрешается вовсе: синтаксически
    // она НЕОТЛИЧИМА от публичного домена, и никакая проверка строки их не
    // разведёт. Внутрикластерный адрес всегда можно записать однозначно —
    // односоставным именем либо с `.svc`, — и сообщение об отказе прямо это
    // предлагает.
    if (host.find('.') == std::string::npos)
        return true;  // односоставное имя сервиса в том же пространстве
    if (host.size() >= 4 && host.compare(host.size() - 4, 4, ".svc") == 0)
        return true;                                 // <service>.<namespace>.svc
    return host.find(".svc.") != std::string::npos;  // ...svc.cluster.local
}

/// Причина отказа для сообщения об ошибке. Пусто = адрес допустим.
inline std::string refusal_reason(const std::string& url) {
    const std::string host = host_of(url);
    if (host.empty())
        return "адрес модели должен быть http(s): '" + url + "'";
    if (!is_allowed_host(host))
        return "агент обязан ходить к модели ТОЛЬКО через прокси внутри кластера, а адрес ведёт на '" + host +
               "'. Прокси — граница маскирования персональных данных; прямой адрес отключает её. "
               "Допустимы односоставное имя сервиса либо форма с '.svc' "
               "(например, guardrails-llm-filter.guardrails.svc.cluster.local): форма 'сервис.namespace' "
               "синтаксически неотличима от публичного домена и поэтому не принимается";
    return {};
}

}  // namespace Agent::Egress
