/**
 * @file ToolRegistry.hpp
 * @brief Единственная точка исполнения инструментов агента (спека P4 §3, §4).
 *
 * ПОЧЕМУ ТОЧКА ОДНА. Проверка прав и политики стоит здесь, и обойти её,
 * добавив новый инструмент, нельзя: обработчик недостижим иначе как через
 * `execute`, потому что регистр владеет им и наружу не отдаёт. Это не
 * договорённость «все зовут execute», а устройство.
 *
 * Урок оплачен в этой же сессии: политика подстановки продавца была применена
 * к созданию документа и НЕ применена к его правке — и документ, отказавшийся
 * принять чужого продавца при создании, принимал его правкой. Точек входа было
 * две, а защищена одна. Здесь точка одна физически.
 *
 * ПОРЯДОК ПРОВЕРОК тоже не произволен:
 *   1. Известен ли инструмент. Неизвестное имя — отказ, а не «ничего не
 *      делаем»: модель, попросившая несуществующий инструмент, должна
 *      получить ошибку, а не тишину, которую примет за успех.
 *   2. Права роли `agent` по матрице (`OrgPermissions`). Полномочия агента
 *      шире человеческих, но объявлены поимённо, и проверяются они ДО
 *      политики: политика решает «можно ли САМОМУ», а матрица — «можно ли
 *      вообще». Разрешительная политика не выдаёт того, чего нет в матрице.
 *   3. Политика автономии. `forbid` — отказ, `approve` — запрос одобрения,
 *      `auto` — исполнение.
 *
 * ЧЕГО ЗДЕСЬ НЕТ. Ни SQL, ни S3, ни секретов: инструмент — это обёртка над
 * доменным сервисом, и агенту не достаётся ничего, кроме объявленных обёрток.
 */

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/AutonomyPolicy.hpp"
#include "tenancy/OrgPermissions.hpp"

namespace Agent {

using json = nlohmann::json;

/// Что инструмент делает с данными организации. Определяет ячейку матрицы.
struct ToolSpec {
    std::string name;
    /// Ресурс матрицы прав (Tenancy::OrgPerm::Resource::*).
    std::string resource;
    /// Действие матрицы: чтение или запись.
    std::string action;
    /// Человекочитаемое описание для инбокса одобрений и для промпта.
    std::string summary;
    /// Есть ли у действия денежная сумма. Если да, обработчик обязан вернуть
    /// её в тиынах через `amount_of` — иначе предел политики нечем проверять,
    /// а «нет суммы» при заданном пределе честно понижает до одобрения.
    bool carries_amount = false;
};

/// Контекст прогона: организация, от чьего имени идёт работа, и сам прогон.
struct ToolContext {
    std::string org_id;
    std::string run_id;
    /// Роль ВСЕГДА "agent": агент действует от собственного имени и не
    /// подменяет пользователя (спека §3.1). Поле существует, чтобы это было
    /// видно в коде, а не подразумевалось.
    std::string role = "agent";
};

/// Итог попытки исполнить инструмент.
struct ToolOutcome {
    enum class Status { kExecuted, kNeedsApproval, kRefused };
    Status status = Status::kRefused;
    /// Ответ инструмента — только при kExecuted.
    json result = json::object();
    /// Почему отказано или почему нужно одобрение. Пустым не бывает.
    std::string reason;
    /// Вердикт политики, как он пишется в agent_steps.policy_decision.
    /// Отсутствует, если до политики дело не дошло (неизвестный инструмент
    /// или запрет матрицы).
    std::optional<Mode> policy_mode;
};

/// Обработчик: получает контекст и аргументы, возвращает результат.
using ToolHandler = std::function<json(const ToolContext&, const json&)>;
/// Извлечение полей действия из аргументов — для политики. Отдельная функция,
/// потому что сумма и контрагент лежат у каждого инструмента по-своему, а
/// движок политик обязан получать их одинаково.
using ActionExtractor = std::function<Action(const json&)>;
/// Загрузка политики. Отдана вызывающему, чтобы регистр не ходил в БД: так он
/// проверяется без Postgres.
using PolicyLoader = std::function<std::optional<Policy>(const std::string& org_id, const std::string& action)>;

class ToolRegistry {
public:
    /// Зарегистрировать инструмент. Обработчик после этого недостижим извне —
    /// регистр владеет им, и другого пути к нему нет.
    void add(ToolSpec spec, ToolHandler handler, ActionExtractor extractor = {}) {
        const std::string name = spec.name;
        tools_.emplace(name, Entry{std::move(spec), std::move(handler), std::move(extractor)});
    }

    bool knows(const std::string& name) const { return tools_.find(name) != tools_.end(); }

    /// Список инструментов для промпта: имя и описание, без внутренностей.
    std::vector<std::pair<std::string, std::string>> catalogue() const {
        std::vector<std::pair<std::string, std::string>> out;
        out.reserve(tools_.size());
        for (const auto& [name, e] : tools_)
            out.emplace_back(name, e.spec.summary);
        return out;
    }

    /**
     * @brief ЕДИНСТВЕННЫЙ способ выполнить инструмент.
     * @param load_policy как достать политику организации для этого действия
     */
    ToolOutcome execute(const ToolContext& ctx,
                        const std::string& name,
                        const json& args,
                        const PolicyLoader& load_policy) const {
        const auto it = tools_.find(name);
        if (it == tools_.end()) {
            // Отказ, а не тишина: модель, попросившая несуществующий
            // инструмент, иначе примет пустой ответ за успешное выполнение.
            return {
                ToolOutcome::Status::kRefused, json::object(), "инструмент '" + name + "' не существует", std::nullopt};
        }
        const Entry& entry = it->second;

        // Матрица прав — ДО политики. Политика решает «можно ли самому», а
        // матрица «можно ли вообще»; разрешительная политика не может выдать
        // того, чего в матрице нет.
        if (!Tenancy::OrgPerm::allows(ctx.role, entry.spec.resource, entry.spec.action)) {
            return {ToolOutcome::Status::kRefused,
                    json::object(),
                    "роли '" + ctx.role + "' не разрешён доступ '" + entry.spec.action + "' к '" + entry.spec.resource +
                        "'",
                    std::nullopt};
        }

        Action action = entry.extractor ? entry.extractor(args) : Action{};
        action.name = name;

        const Decision decision = evaluate(load_policy(ctx.org_id, name), action);
        if (decision.mode == Mode::kForbid)
            return {ToolOutcome::Status::kRefused, json::object(), decision.reason, decision.mode};
        if (decision.mode == Mode::kApprove)
            return {ToolOutcome::Status::kNeedsApproval, json::object(), decision.reason, decision.mode};

        return {ToolOutcome::Status::kExecuted, entry.handler(ctx, args), decision.reason, decision.mode};
    }

private:
    struct Entry {
        ToolSpec spec;
        ToolHandler handler;
        ActionExtractor extractor;
    };
    std::map<std::string, Entry> tools_;
};

}  // namespace Agent
