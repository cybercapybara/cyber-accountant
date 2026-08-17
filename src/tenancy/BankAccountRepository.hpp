/**
 * @file BankAccountRepository.hpp
 * @brief Весь SQL, трогающий `bank_accounts`, живёт здесь.
 *
 * Org-scoped (спека §5: «методов "выбрать без org" не существует»), поэтому
 * наследует Tenancy::OrgCrudBase, а не Repositories::CrudBase:
 * find_in_org/list_in_org/count_in_org приходят из базы, а create/update/
 * remove/set_primary — запросы, нужные именно этой таблице. Нарушения
 * ограничений всплывают типизированными исключениями через
 * Repositories::detail::translate_sql, и HTTP-слой отображает их в 409 через
 * Api::with_repo_errors(), не разбирая SQLSTATE сам.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"
#include "tenancy/BankAccount.hpp"
#include "tenancy/OrgScoped.hpp"

namespace Tenancy {

/// Стабильный код 409 на исключении, чтобы with_repo_errors() отобразил его,
/// не подключая этот заголовок.
struct DuplicateBankAccount : Repositories::ConflictError {
    DuplicateBankAccount()
        : Repositories::ConflictError("bank_account_iik_taken",
                                      "A bank account with that IIK already exists in this organization") {}
};

class BankAccountRepository : public Tenancy::OrgCrudBase<BankAccountRepository, BankAccount, std::string> {
public:
    // Контракт OrgCrudBase — даёт find_in_org(id,org_id) /
    // list_in_org(org_id,limit,offset) / count_in_org(org_id).
    static constexpr const char* kTable = "bank_accounts";
    static constexpr const char* kColumns =
        "id, org_id, iik, bank_name, bik, kbe, currency, is_primary, created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    /// Основной счёт идёт первым: он же подставляется в документ по умолчанию,
    /// и список, где он не наверху, вводит в заблуждение.
    static constexpr const char* kOrderBy = "is_primary DESC, created_at DESC";
    static constexpr const char* kOrgColumn = "org_id";

    /**
     * @brief Добавить счёт организации @p org_id.
     * @details Бросает DuplicateBankAccount при нарушении UNIQUE(org_id, iik)
     *          (SQLSTATE 23505). Если @p draft помечен основным, прежний
     *          основной снимается В ТОЙ ЖЕ ТРАНЗАКЦИИ — иначе частичный
     *          уникальный индекс uq_bank_accounts_one_primary отверг бы
     *          вставку, и пользователь получил бы конфликт вместо ожидаемого
     *          «теперь основной этот».
     */
    BankAccount create(const std::string& org_id, const BankAccount& draft) {
        return Repositories::detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    if (draft.is_primary)
                        clear_primary(txn, org_id);
                    auto r = txn.exec_params(
                        "INSERT INTO bank_accounts (org_id, iik, bank_name, bik, kbe, currency, is_primary) "
                        "VALUES ($1, $2, $3, $4, $5, $6, $7) RETURNING " +
                            std::string(kColumns),
                        org_id,
                        draft.iik,
                        draft.bank_name,
                        draft.bik,
                        draft.kbe,
                        draft.currency.empty() ? std::string("KZT") : draft.currency,
                        draft.is_primary);
                    return BankAccount::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateBankAccount{};
            });
    }

    /**
     * @brief Обновить счёт, найденный по паре (@p id, @p org_id).
     * @details Пара в WHERE — это и есть изоляция арендаторов: чужой id не
     *          совпадёт с org_id и вернёт пустой результат, а не чужую строку.
     * @return std::nullopt, если строка не найдена — ожидаемая ветка для 404.
     */
    std::optional<BankAccount> update(const std::string& org_id, const std::string& id, const BankAccount& patch) {
        return Repositories::detail::translate_sql(
            [&]() -> std::optional<BankAccount> {
                return Database::get().execute_write([&](auto& txn) -> std::optional<BankAccount> {
                    if (patch.is_primary)
                        clear_primary(txn, org_id, id);
                    auto r = txn.exec_params(
                        "UPDATE bank_accounts SET iik = $3, bank_name = $4, bik = $5, kbe = $6, currency = $7, "
                        "is_primary = $8 WHERE id = $1 AND org_id = $2 RETURNING " +
                            std::string(kColumns),
                        id,
                        org_id,
                        patch.iik,
                        patch.bank_name,
                        patch.bik,
                        patch.kbe,
                        patch.currency.empty() ? std::string("KZT") : patch.currency,
                        patch.is_primary);
                    if (r.empty())
                        return std::nullopt;
                    return BankAccount::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateBankAccount{};
            });
    }

    /**
     * @brief Удалить счёт организации.
     * @return false, если строки нет — ожидаемая ветка для 404.
     */
    bool remove(const std::string& org_id, const std::string& id) {
        return Database::get().execute_write([&](auto& txn) {
            auto r =
                txn.exec_params("DELETE FROM bank_accounts WHERE id = $1 AND org_id = $2 RETURNING id", id, org_id);
            return !r.empty();
        });
    }

    /**
     * @brief Основной счёт организации, если он назначен.
     * @details Это то, что подставляется в документ. Индекс
     *          uq_bank_accounts_one_primary гарантирует, что строка здесь не
     *          более одной, поэтому LIMIT 1 — не способ скрыть неоднозначность,
     *          а следствие ограничения БД.
     */
    std::optional<BankAccount> find_primary(const std::string& org_id) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<BankAccount> {
            auto r = txn.exec_params(
                "SELECT " + std::string(kColumns) + " FROM bank_accounts WHERE org_id = $1 AND is_primary LIMIT 1",
                org_id);
            if (r.empty())
                return std::nullopt;
            return BankAccount::from_row(r[0]);
        });
    }

private:
    /// Снять признак «основной» со всех счетов организации, кроме @p keep_id.
    /// Вызывается ТОЛЬКО изнутри уже открытой транзакции вызывающего метода:
    /// снятие и установка обязаны быть атомарны, иначе между ними существует
    /// момент без основного счёта.
    template <typename Txn>
    static void clear_primary(Txn& txn, const std::string& org_id, const std::string& keep_id = "") {
        if (keep_id.empty()) {
            txn.exec_params("UPDATE bank_accounts SET is_primary = FALSE WHERE org_id = $1 AND is_primary", org_id);
        } else {
            txn.exec_params("UPDATE bank_accounts SET is_primary = FALSE WHERE org_id = $1 AND is_primary AND id <> $2",
                            org_id,
                            keep_id);
        }
    }
};

}  // namespace Tenancy
