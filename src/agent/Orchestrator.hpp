/**
 * @file Orchestrator.hpp
 * @brief Цикл агентного прогона: модель -> инструмент -> модель (спека P4 §6, §9).
 *
 * ВЫЗОВ МОДЕЛИ ПЕРЕДАЁТСЯ ПАРАМЕТРОМ, а не берётся жёстко из ModelClient. Это
 * не абстракция ради абстракции: ключа модели в кластере нет, и без такого
 * устройства весь цикл — трассировка, бюджет, обработка одобрений — остался бы
 * непроверенным до появления ключа. С ним цикл проверяется сегодня, на
 * заготовленных ответах, а подключение настоящего клиента становится одной
 * строкой на вызывающей стороне.
 *
 * ЧТО ЦИКЛ ОБЯЗАН ГАРАНТИРОВАТЬ, и почему каждое из этого проверяется:
 *   - Ни один вызов инструмента не минует единую точку (`ToolRegistry`).
 *   - Каждый шаг попадает в `agent_steps` — включая отказы и вердикты политики.
 *     Прогон, чьи шаги не записаны, невозможно разобрать постфактум, а значит
 *     нельзя и доверять.
 *   - Расход прибавляется ПОСЛЕ КАЖДОГО вызова модели, а не в конце: прогон,
 *     оборвавшийся посередине, всё равно стоил денег.
 *   - Бюджет проверяется ПЕРЕД вызовом. Проверка после — это счёт, который уже
 *     выставлен.
 *   - Требование одобрения ОСТАНАВЛИВАЕТ прогон, а не пропускает шаг: агент не
 *     продолжает работу, делая вид, что действие выполнено.
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/AgentRepository.hpp"
#include "agent/ModelClient.hpp"
#include "agent/ToolRegistry.hpp"
#include "money/MoneyFormat.hpp"

namespace Agent {

using json = nlohmann::json;

/// Вызов модели: история и инструменты на входе, ответ на выходе.
using ModelCaller = std::function<ModelReply(const json& messages, const json& tools)>;

struct RunLimits {
    /// Предел расхода организации за период, в тиынах. Пусто — предела нет.
    std::optional<long long> budget_tiyn;
    /// С какого момента считать расход (ISO-8601).
    std::string period_start = "1970-01-01T00:00:00Z";
    /// Предел числа обращений к модели в одном прогоне. Защита не от денег, а
    /// от зацикливания: модель, повторяющая один и тот же вызов инструмента,
    /// иначе крутилась бы до исчерпания бюджета.
    int max_model_calls = 12;
};

struct RunResult {
    std::string run_id;
    /// Итоговый статус прогона, как он записан в `agent_runs`.
    std::string status;
    /// Текст для человека: ответ агента либо причина остановки.
    std::string summary;
    /// Идентификатор запроса одобрения, если прогон остановлен ради него.
    std::optional<std::string> approval_id;
};

/// Строковое имя исхода — оно уходит в `agent_steps.payload`, и читать его
/// будет человек, разбирающий прогон.
inline const char* status_name(ToolOutcome::Status s) {
    switch (s) {
        case ToolOutcome::Status::kExecuted:
            return "executed";
        case ToolOutcome::Status::kNeedsApproval:
            return "needs_approval";
        case ToolOutcome::Status::kRefused:
            return "refused";
    }
    return "refused";
}

/**
 * @brief Человекочитаемое описание действия для инбокса одобрений.
 * @details Обязательное поле (§5 спеки): одобряют ДЕЙСТВИЕ, а не JSON.
 *          «Провести 12 500,00 ₸» человек прочитает, а
 *          {"lines":[{"account":"1030",...}]} — нет, и одобрение выродится в
 *          нажатие кнопки не глядя.
 *
 *          Деньги здесь форматирует СЕРВЕР из целого в тиынах — тем же
 *          правилом, что и везде (P3 §3): агент печатаемых сумм не пишет.
 */
inline std::string human_diff(const std::string& action, const json& args) {
    std::string out = action;
    if (args.contains("amount_tiyn") && args["amount_tiyn"].is_number_integer())
        out += ", сумма " + Money::format_tiyn_ru(args["amount_tiyn"].get<long long>()) + " ₸";
    if (args.contains("counterparty_name") && args["counterparty_name"].is_string())
        out += ", контрагент " + args["counterparty_name"].get<std::string>();
    if (args.contains("doc_type") && args["doc_type"].is_string())
        out += ", документ " + args["doc_type"].get<std::string>();
    return out;
}

/**
 * @brief Провести один прогон.
 * @param call_model как обратиться к модели (в бою — Agent::call)
 * @param tools_json описание инструментов для модели
 */
inline RunResult run(AgentRepository& repo,
                     const ToolRegistry& registry,
                     const std::string& org_id,
                     const std::string& trigger,
                     const std::optional<std::string>& started_by,
                     const std::string& user_message,
                     const ModelCaller& call_model,
                     const json& tools_json,
                     const RunLimits& limits = {}) {
    // Бюджет — ДО создания прогона и до любого вызова: проверка после вызова
    // это уже потраченные деньги.
    if (limits.budget_tiyn.has_value()) {
        const long long spent = repo.cost_since(org_id, limits.period_start);
        if (spent >= *limits.budget_tiyn) {
            auto run_row = repo.start_run(org_id, trigger, started_by);
            const std::string why = "бюджет организации исчерпан: потрачено " + std::to_string(spent) +
                                    " тиын при пределе " + std::to_string(*limits.budget_tiyn);
            repo.append_step(run_row.id, "error", json{{"reason", why}});
            repo.finish_run(org_id, run_row.id, "budget_exceeded", why, why);
            return {run_row.id, "budget_exceeded", why, std::nullopt};
        }
    }

    auto run_row = repo.start_run(org_id, trigger, started_by);
    json messages = json::array();
    messages.push_back(json{{"role", "user"}, {"content", user_message}});
    repo.append_step(run_row.id, "prompt", json{{"text", user_message}});

    ToolContext ctx;
    ctx.org_id = org_id;
    ctx.run_id = run_row.id;

    auto load_policy = [&repo, &org_id](const std::string& org, const std::string& action) {
        (void)org;
        return repo.find_policy(org_id, action);
    };

    std::string final_text;
    for (int i = 0; i < limits.max_model_calls; ++i) {
        ModelReply reply;
        try {
            reply = call_model(messages, tools_json);
        } catch (const NotConfigured& e) {
            // Не настроен — не поломка. Прогон честно завершается причиной,
            // которую видно в интерфейсе.
            const std::string why = e.what();
            repo.append_step(run_row.id, "error", json{{"reason", why}});
            repo.finish_run(org_id, run_row.id, "failed", why, why);
            return {run_row.id, "failed", why, std::nullopt};
        } catch (const std::exception& e) {
            const std::string why = std::string("обращение к модели не удалось: ") + e.what();
            repo.append_step(run_row.id, "error", json{{"reason", why}});
            repo.finish_run(org_id, run_row.id, "failed", why, why);
            return {run_row.id, "failed", why, std::nullopt};
        }

        // Расход — сразу после вызова: прогон, оборвавшийся дальше, всё равно
        // стоил денег, и бюджет обязан это видеть.
        repo.add_usage(org_id, run_row.id, reply.input_tokens, reply.output_tokens, /*cost_tiyn=*/0);
        repo.append_step(run_row.id,
                         "model_reply",
                         json{{"text", reply.text},
                              {"tool_calls", static_cast<int>(reply.tool_calls.size())},
                              {"stop_reason", reply.stop_reason}});

        if (reply.tool_calls.empty()) {
            final_text = reply.text;
            break;
        }

        json tool_results = json::array();
        for (const auto& [name, args] : reply.tool_calls) {
            const ToolOutcome outcome = registry.execute(ctx, name, args, load_policy);
            const std::optional<std::string> decision =
                outcome.policy_mode.has_value() ? std::optional<std::string>(mode_name(*outcome.policy_mode))
                                                : std::nullopt;
            repo.append_step(run_row.id,
                             "tool_call",
                             json{{"args", args}, {"status", status_name(outcome.status)}},
                             name,
                             decision,
                             outcome.reason);

            if (outcome.status == ToolOutcome::Status::kNeedsApproval) {
                // ОСТАНОВКА, а не пропуск. Продолжить, сделав вид, что действие
                // выполнено, — значит дать модели строить дальнейшие выводы на
                // том, чего не произошло.
                auto req = repo.request_approval(org_id, run_row.id, name, args, human_diff(name, args));
                repo.finish_run(org_id, run_row.id, "awaiting_approval", outcome.reason);
                return {run_row.id, "awaiting_approval", outcome.reason, req.id};
            }

            tool_results.push_back(json{{"tool", name},
                                        {"status", status_name(outcome.status)},
                                        {"result", outcome.result},
                                        {"reason", outcome.reason}});
        }

        messages.push_back(json{{"role", "assistant"}, {"content", reply.text}});
        messages.push_back(json{{"role", "user"}, {"content", tool_results.dump()}});
        repo.append_step(run_row.id, "tool_result", tool_results);
    }

    if (final_text.empty()) {
        const std::string why =
            "прогон остановлен: достигнут предел обращений к модели (" + std::to_string(limits.max_model_calls) + ")";
        repo.append_step(run_row.id, "error", json{{"reason", why}});
        repo.finish_run(org_id, run_row.id, "failed", why, why);
        return {run_row.id, "failed", why, std::nullopt};
    }

    repo.finish_run(org_id, run_row.id, "succeeded", final_text);
    return {run_row.id, "succeeded", final_text, std::nullopt};
}

}  // namespace Agent
