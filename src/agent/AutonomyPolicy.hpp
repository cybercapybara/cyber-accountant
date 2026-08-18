/**
 * @file AutonomyPolicy.hpp
 * @brief Движок политик автономии: можно ли агенту выполнить действие САМОМУ,
 *        нужно ли одобрение человека, или действие запрещено (спека P4 §4).
 *
 * ЗАПРЕТ ПО УМОЛЧАНИЮ, И УМОЛЧАНИЕ ЖИВЁТ ЗДЕСЬ. Действие, для которого политики
 * не завели, требует человека (`approve`). Умолчание нельзя класть в таблицу:
 * тогда пустая таблица означала бы «правил нет», а это ровно то состояние, в
 * котором система живёт до первой настройки — то есть самый опасный момент
 * оказался бы самым разрешительным.
 *
 * ПРОВЕРЯЕТ КОД, А НЕ МОДЕЛЬ. Границы (предельная сумма, белый список
 * контрагентов) вычисляются здесь, над структурными полями действия. Модель
 * можно уговорить текстом письма; проверку суммы уговорить нельзя. Это прямое
 * следствие решения владельца дать агенту полномочия шире человеческих: чем
 * шире полномочия, тем важнее, чтобы границы стояли вне модели.
 *
 * ЗАГОЛОВОК НЕ ХОДИТ В БАЗУ. Он решает по уже загруженной политике, поэтому
 * проверяем без Postgres — а значит проверяем много и дёшево.
 */

#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace Agent {

/// Режим автономии. Порядок значений — от разрешительного к запретительному,
/// и на него опирается `strictest_of`.
enum class Mode { kAuto, kApprove, kForbid };

inline const char* mode_name(Mode m) {
    switch (m) {
        case Mode::kAuto:
            return "auto";
        case Mode::kApprove:
            return "approve";
        case Mode::kForbid:
            return "forbid";
    }
    return "approve";  // недостижимо; умолчание всё равно ограничительное
}

inline std::optional<Mode> parse_mode(const std::string& s) {
    if (s == "auto")
        return Mode::kAuto;
    if (s == "approve")
        return Mode::kApprove;
    if (s == "forbid")
        return Mode::kForbid;
    return std::nullopt;
}

/// Строка `autonomy_policies`, уже загруженная вызывающим.
struct Policy {
    std::string action;
    Mode mode = Mode::kApprove;
    /// Пусто = граница не задана. Ноль — это НЕ «без предела», а «только
    /// действия без суммы»; путать нельзя.
    std::optional<long long> max_amount_tiyn;
    std::vector<std::string> counterparty_allowlist;
    std::optional<std::string> doc_type;
    bool require_prior_approval = false;
};

/// Действие, которое агент собирается выполнить.
struct Action {
    std::string name;
    /// Сумма В ТИЫНАХ. Пусто — у действия суммы нет (например, черновик
    /// письма). Денежных СТРОК здесь нет и быть не может: агент не пишет
    /// печатаемых сумм нигде (P3 §3).
    std::optional<long long> amount_tiyn;
    std::optional<std::string> counterparty_id;
    std::optional<std::string> doc_type;
    /// Такое же действие уже одобряли руками. Вычисляет вызывающий по истории
    /// одобрений, а не модель.
    bool has_prior_approval = false;
};

struct Decision {
    Mode mode = Mode::kApprove;
    /// Человекочитаемая причина. Пишется в `agent_steps.policy_reason` ДАЖЕ
    /// когда действие разрешено: иначе вопрос «почему это прошло само»
    /// останется без ответа ровно тогда, когда ответ понадобится.
    std::string reason;
};

namespace detail {

/// Из двух режимов — более строгий. Запрет побеждает всё, одобрение побеждает
/// автономию.
inline Mode strictest_of(Mode a, Mode b) {
    return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
}

}  // namespace detail

/**
 * @brief Решить, что делать с действием.
 * @param policy политика для этого действия, если она есть
 * @param action что агент собирается сделать
 *
 * @details Порядок проверок выбран так, чтобы ограничение НИКОГДА не могло
 *          быть ослаблено последующей проверкой:
 *          1. Политики нет -> `approve`. Человек.
 *          2. Режим `forbid` -> запрет, и дальше ничего не рассматривается.
 *          3. Границы проверяются, только если режим `auto`. Для `approve`
 *             они бессмысленны: человек всё равно смотрит.
 *          4. Любая нарушенная граница ПОНИЖАЕТ до `approve`. Не до запрета:
 *             превышение предела — это повод спросить, а не отказать, иначе
 *             агент молча перестал бы делать законную работу.
 */
inline Decision evaluate(const std::optional<Policy>& policy, const Action& action) {
    if (!policy)
        return {Mode::kApprove,
                "для действия '" + action.name + "' политика не задана — по умолчанию требуется одобрение"};

    if (policy->mode == Mode::kForbid)
        return {Mode::kForbid, "действие '" + action.name + "' запрещено политикой организации"};

    if (policy->mode == Mode::kApprove)
        return {Mode::kApprove, "политика для '" + action.name + "' требует одобрения"};

    // Дальше только режим `auto` — и каждая граница может его понизить.
    Mode mode = Mode::kAuto;
    std::string reason = "политика разрешает '" + action.name + "' автоматически";

    if (policy->max_amount_tiyn.has_value()) {
        // Действие БЕЗ суммы при заданном пределе — это не «ноль, значит
        // проходит». Предел объявлен для действий с деньгами, и действие, чью
        // сумму мы не знаем, автоматически пропускать нельзя.
        if (!action.amount_tiyn.has_value()) {
            mode = detail::strictest_of(mode, Mode::kApprove);
            reason = "у действия нет суммы, а политика задаёт предел — проверить человеку";
        } else if (*action.amount_tiyn > *policy->max_amount_tiyn) {
            mode = detail::strictest_of(mode, Mode::kApprove);
            reason = "сумма превышает предел политики";
        }
    }

    if (!policy->counterparty_allowlist.empty()) {
        const bool listed = action.counterparty_id.has_value() &&
                            std::find(policy->counterparty_allowlist.begin(),
                                      policy->counterparty_allowlist.end(),
                                      *action.counterparty_id) != policy->counterparty_allowlist.end();
        if (!listed) {
            mode = detail::strictest_of(mode, Mode::kApprove);
            reason = action.counterparty_id.has_value() ? "контрагент не в белом списке политики"
                                                        : "у действия нет контрагента, а политика задаёт белый список";
        }
    }

    if (policy->doc_type.has_value() && action.doc_type.has_value() && *policy->doc_type != *action.doc_type) {
        mode = detail::strictest_of(mode, Mode::kApprove);
        reason = "тип документа не тот, для которого задана политика";
    }

    if (policy->require_prior_approval && !action.has_prior_approval) {
        mode = detail::strictest_of(mode, Mode::kApprove);
        reason = "политика разрешает автоматически только повторение уже одобренного действия";
    }

    return {mode, reason};
}

}  // namespace Agent
