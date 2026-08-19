/**
 * @file AgentRepository.hpp
 * @brief Весь SQL агента: прогоны, шаги, политики, одобрения
 *        (migrations/029_agent_foundation.sql).
 *
 * ЗАЧЕМ ОТДЕЛЬНЫЙ ЗАГОЛОВОК, А НЕ ЧЕТЫРЕ. Эти четыре таблицы читаются и
 * пишутся вместе на каждом шаге прогона: шаг записывается вместе с вердиктом
 * политики, а вердикт `approve` тут же порождает запрос одобрения. Разнести их
 * значило бы получить четыре объекта, которые всегда создают друг друга.
 *
 * ГРАНИЦА NULL И НУЛЯ переживает базу. `max_amount_tiyn IS NULL` — «предел не
 * задан», а `0` — «только действия без денег». Движок политик их различает
 * (см. тесты), и отображение сюда обязано различие сохранить: `value_or(0)`
 * здесь превратил бы строгую политику в неограниченную.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/AutonomyPolicy.hpp"
#include "database/Database.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"

namespace Agent {

using json = nlohmann::json;

struct Run {
    std::string id;
    std::string org_id;
    std::string trigger;
    std::optional<std::string> started_by;
    std::string status;
    std::string summary;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cost_tiyn = 0;
    std::optional<std::string> error;
    std::string created_at;
};

struct Step {
    std::string id;
    std::string run_id;
    int seq = 0;
    std::string kind;
    std::optional<std::string> tool;
    json payload = json::object();
    std::optional<std::string> policy_decision;
    std::optional<std::string> policy_reason;
    std::string created_at;
};

struct ApprovalRequest {
    std::string id;
    std::string org_id;
    std::optional<std::string> run_id;
    std::string action;
    json payload = json::object();
    std::string human_diff;
    std::string status;
    std::optional<std::string> decided_by;
    std::string comment;
    std::string created_at;
};

class AgentRepository {
public:
    // ---------------------------------------------------------------- прогоны
    Run start_run(const std::string& org_id, const std::string& trigger, const std::optional<std::string>& started_by) {
        return Database::get().execute_write([&](auto& txn) {
            auto r =
                txn.exec_params("INSERT INTO agent_runs (org_id, trigger, started_by) VALUES ($1, $2, $3) RETURNING " +
                                    std::string(kRunColumns),
                                org_id,
                                trigger,
                                started_by);
            return run_from(r[0]);
        });
    }

    /// @return false, если прогона нет в этой организации — ожидаемая ветка,
    ///         а не исключение: прогон мог быть удалён вместе с организацией.
    bool finish_run(const std::string& org_id,
                    const std::string& run_id,
                    const std::string& status,
                    const std::string& summary,
                    const std::optional<std::string>& error = std::nullopt) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE agent_runs SET status = $3, summary = $4, error = $5 "
                " WHERE id = $2 AND org_id = $1 RETURNING id",
                org_id,
                run_id,
                status,
                summary,
                error);
            return !r.empty();
        });
    }

    /// Прибавить расход. ИМЕННО ПРИБАВИТЬ, а не записать: прогон делает много
    /// вызовов модели, и присваивание потеряло бы всё, кроме последнего, —
    /// то есть бюджет организации считался бы неверно в меньшую сторону.
    bool add_usage(const std::string& org_id,
                   const std::string& run_id,
                   long long input_tokens,
                   long long output_tokens,
                   long long cost_tiyn) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE agent_runs SET input_tokens = input_tokens + $3, "
                "  output_tokens = output_tokens + $4, cost_tiyn = cost_tiyn + $5 "
                " WHERE id = $2 AND org_id = $1 RETURNING id",
                org_id,
                run_id,
                input_tokens,
                output_tokens,
                cost_tiyn);
            return !r.empty();
        });
    }

    std::optional<Run> find_run(const std::string& org_id, const std::string& run_id) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Run> {
            auto r =
                txn.exec_params("SELECT " + std::string(kRunColumns) + " FROM agent_runs WHERE id = $2 AND org_id = $1",
                                org_id,
                                run_id);
            if (r.empty())
                return std::nullopt;
            return run_from(r[0]);
        });
    }

    /// Расход организации за период — для бюджета (§9 спеки).
    long long cost_since(const std::string& org_id, const std::string& since_iso) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT COALESCE(SUM(cost_tiyn), 0) AS total FROM agent_runs "
                " WHERE org_id = $1 AND created_at >= $2::timestamptz",
                org_id,
                since_iso);
            return r[0]["total"].template as<long long>();
        });
    }

    // ------------------------------------------------------------------ шаги
    /// Номер шага НАЗНАЧАЕТ БАЗА, а не вызывающий: два параллельных шага иначе
    /// получили бы один номер и один из них упал бы на UNIQUE уже после того,
    /// как работа выполнена.
    Step append_step(const std::string& run_id,
                     const std::string& kind,
                     const json& payload,
                     const std::optional<std::string>& tool = std::nullopt,
                     const std::optional<std::string>& policy_decision = std::nullopt,
                     const std::optional<std::string>& policy_reason = std::nullopt) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "INSERT INTO agent_steps (run_id, seq, kind, tool, payload, policy_decision, policy_reason) "
                "VALUES ($1, (SELECT COALESCE(MAX(seq), 0) + 1 FROM agent_steps WHERE run_id = $1), "
                "        $2, $3, $4::jsonb, $5, $6) RETURNING " +
                    std::string(kStepColumns),
                run_id,
                kind,
                tool,
                payload.dump(),
                policy_decision,
                policy_reason);
            return step_from(r[0]);
        });
    }

    std::vector<Step> list_steps(const std::string& run_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT " + std::string(kStepColumns) + " FROM agent_steps WHERE run_id = $1 ORDER BY seq", run_id);
            std::vector<Step> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(step_from(row));
            return out;
        });
    }

    // -------------------------------------------------------------- политики
    /**
     * @brief Политика организации для действия, если она заведена.
     * @details Пустой ответ — НЕ ошибка и не повод разрешить: движок трактует
     *          его как «требуется одобрение» (§4 спеки).
     */
    std::optional<Policy> find_policy(const std::string& org_id, const std::string& action) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Policy> {
            auto r = txn.exec_params(
                "SELECT action, mode, max_amount_tiyn, counterparty_allowlist, doc_type, "
                "       require_prior_approval "
                "  FROM autonomy_policies WHERE org_id = $1 AND action = $2 LIMIT 1",
                org_id,
                action);
            if (r.empty())
                return std::nullopt;
            const auto& row = r[0];

            Policy p;
            p.action = row["action"].template as<std::string>();
            // Режим, который мы не понимаем, — НЕ повод угадать. Строка в БД
            // могла быть записана будущей версией; трактовать неизвестное как
            // `auto` значило бы выдать автономию по недоразумению.
            p.mode = parse_mode(row["mode"].template as<std::string>()).value_or(Mode::kApprove);
            if (!row["max_amount_tiyn"].is_null())
                p.max_amount_tiyn = row["max_amount_tiyn"].template as<long long>();
            if (!row["doc_type"].is_null())
                p.doc_type = row["doc_type"].template as<std::string>();
            p.require_prior_approval = row["require_prior_approval"].template as<bool>();
            if (!row["counterparty_allowlist"].is_null())
                p.counterparty_allowlist = parse_uuid_array(row["counterparty_allowlist"].template as<std::string>());
            return p;
        });
    }

    // -------------------------------------------------------------- одобрения
    ApprovalRequest request_approval(const std::string& org_id,
                                     const std::optional<std::string>& run_id,
                                     const std::string& action,
                                     const json& payload,
                                     const std::string& human_diff) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "INSERT INTO approval_requests (org_id, run_id, action, payload, human_diff) "
                "VALUES ($1, $2, $3, $4::jsonb, $5) RETURNING " +
                    std::string(kApprovalColumns),
                org_id,
                run_id,
                action,
                payload.dump(),
                human_diff);
            return approval_from(r[0]);
        });
    }

    /// Решение по запросу. Решивший и время проставляются ВМЕСТЕ со статусом —
    /// иначе в базе появилось бы одобрение без ответственного, что CHECK
    /// `ck_approval_decided_together` и запрещает.
    std::optional<ApprovalRequest> decide(const std::string& org_id,
                                          const std::string& id,
                                          const std::string& status,
                                          const std::string& decided_by,
                                          const std::string& comment) {
        return Database::get().execute_write([&](auto& txn) -> std::optional<ApprovalRequest> {
            auto r = txn.exec_params(
                "UPDATE approval_requests SET status = $3, decided_by = $4, decided_at = now(), "
                "  comment = $5 "
                " WHERE id = $2 AND org_id = $1 AND status = 'pending' RETURNING " +
                    std::string(kApprovalColumns),
                org_id,
                id,
                status,
                decided_by,
                comment);
            if (r.empty())
                return std::nullopt;
            return approval_from(r[0]);
        });
    }

    std::vector<ApprovalRequest> list_pending(const std::string& org_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT " + std::string(kApprovalColumns) +
                                         " FROM approval_requests WHERE org_id = $1 AND status = 'pending' "
                                         " ORDER BY created_at DESC",
                                     org_id);
            std::vector<ApprovalRequest> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(approval_from(row));
            return out;
        });
    }

    /// Было ли такое действие уже одобрено руками — вход для политики
    /// `require_prior_approval`. Считает БАЗА, а не модель: «мы это уже
    /// одобряли» не должно быть утверждением, которое агент делает о себе сам.
    bool has_prior_approval(const std::string& org_id, const std::string& action) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT 1 FROM approval_requests "
                " WHERE org_id = $1 AND action = $2 AND status = 'approved' LIMIT 1",
                org_id,
                action);
            return !r.empty();
        });
    }

private:
    static constexpr const char* kRunColumns =
        "id, org_id, trigger, started_by, status, summary, input_tokens, output_tokens, cost_tiyn, error, created_at";
    static constexpr const char* kStepColumns =
        "id, run_id, seq, kind, tool, payload::text AS payload, policy_decision, policy_reason, created_at";
    static constexpr const char* kApprovalColumns =
        "id, org_id, run_id, action, payload::text AS payload, human_diff, status, decided_by, comment, created_at";

    template <typename Row>
    static Run run_from(const Row& row) {
        Run r;
        r.id = row["id"].template as<std::string>();
        r.org_id = row["org_id"].template as<std::string>();
        r.trigger = row["trigger"].template as<std::string>();
        if (!row["started_by"].is_null())
            r.started_by = row["started_by"].template as<std::string>();
        r.status = row["status"].template as<std::string>();
        r.summary = row["summary"].template as<std::string>();
        r.input_tokens = row["input_tokens"].template as<long long>();
        r.output_tokens = row["output_tokens"].template as<long long>();
        r.cost_tiyn = row["cost_tiyn"].template as<long long>();
        if (!row["error"].is_null())
            r.error = row["error"].template as<std::string>();
        r.created_at = row["created_at"].template as<std::string>();
        return r;
    }

    template <typename Row>
    static Step step_from(const Row& row) {
        Step s;
        s.id = row["id"].template as<std::string>();
        s.run_id = row["run_id"].template as<std::string>();
        s.seq = row["seq"].template as<int>();
        s.kind = row["kind"].template as<std::string>();
        if (!row["tool"].is_null())
            s.tool = row["tool"].template as<std::string>();
        s.payload = json::parse(row["payload"].template as<std::string>());
        if (!row["policy_decision"].is_null())
            s.policy_decision = row["policy_decision"].template as<std::string>();
        if (!row["policy_reason"].is_null())
            s.policy_reason = row["policy_reason"].template as<std::string>();
        s.created_at = row["created_at"].template as<std::string>();
        return s;
    }

    template <typename Row>
    static ApprovalRequest approval_from(const Row& row) {
        ApprovalRequest a;
        a.id = row["id"].template as<std::string>();
        a.org_id = row["org_id"].template as<std::string>();
        if (!row["run_id"].is_null())
            a.run_id = row["run_id"].template as<std::string>();
        a.action = row["action"].template as<std::string>();
        a.payload = json::parse(row["payload"].template as<std::string>());
        a.human_diff = row["human_diff"].template as<std::string>();
        a.status = row["status"].template as<std::string>();
        if (!row["decided_by"].is_null())
            a.decided_by = row["decided_by"].template as<std::string>();
        a.comment = row["comment"].template as<std::string>();
        a.created_at = row["created_at"].template as<std::string>();
        return a;
    }

    /// Массив uuid[] приходит текстом вида {a,b,c}. Разбор здесь, а не в
    /// вызывающем: иначе каждый читатель политики повторял бы его по-своему.
    static std::vector<std::string> parse_uuid_array(const std::string& raw) {
        std::vector<std::string> out;
        std::string cur;
        for (const char c : raw) {
            if (c == '{' || c == '}' || c == '"')
                continue;
            if (c == ',') {
                if (!cur.empty())
                    out.push_back(cur);
                cur.clear();
                continue;
            }
            cur += c;
        }
        if (!cur.empty())
            out.push_back(cur);
        return out;
    }
};

}  // namespace Agent
