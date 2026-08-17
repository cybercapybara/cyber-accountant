/**
 * @file BankAccount.hpp
 * @brief Расчётный счёт организации. Отражает таблицу `bank_accounts`
 *        (migrations/025_org_requisites.sql — спека конструктора шаблонов §7.2).
 *
 * Только домен, без SQL; персистентность — в
 * src/tenancy/BankAccountRepository.hpp. Идиомы from_row/to_json те же, что у
 * src/tenancy/Organization.hpp и src/ledger/Counterparty.hpp: from_row —
 * шаблонная статическая фабрика, to_json — свободная функция, находимая через
 * ADL.
 *
 * ЗАЧЕМ ОТДЕЛЬНАЯ ТАБЛИЦА, А НЕ КОЛОНКИ В organizations: у ТОО счетов обычно
 * несколько (тенге и валюта), у каждого свой банк, БИК и КБе. Ровно поэтому
 * `is_primary` существует и ограничен в БД частичным уникальным индексом —
 * подстановка счёта в документ обязана быть детерминированной.
 */

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace Tenancy {

struct BankAccount {
    std::string id;
    std::string org_id;
    std::string iik;  // IBAN KZ..; формат на уровне БД не проверяется
    std::string bank_name;
    std::string bik;
    std::string kbe;
    std::string currency;  // 'KZT' по умолчанию
    bool is_primary = false;
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static BankAccount from_row(const Row& row) {
        BankAccount a;
        a.id = row["id"].template as<std::string>();
        a.org_id = row["org_id"].template as<std::string>();
        a.iik = row["iik"].template as<std::string>();
        a.bank_name = row["bank_name"].template as<std::string>();
        a.bik = row["bik"].template as<std::string>();
        a.kbe = row["kbe"].template as<std::string>();
        a.currency = row["currency"].template as<std::string>();
        a.is_primary = row["is_primary"].template as<bool>();
        a.created_at = row["created_at"].template as<std::string>();
        a.updated_at = row["updated_at"].template as<std::string>();
        return a;
    }
};

/// Публичная форма JSON — на строке нет секретов, отдаётся целиком.
inline void to_json(nlohmann::json& j, const BankAccount& a) {
    j = nlohmann::json{
        {"id", a.id},
        {"org_id", a.org_id},
        {"iik", a.iik},
        {"bank_name", a.bank_name},
        {"bik", a.bik},
        {"kbe", a.kbe},
        {"currency", a.currency},
        {"is_primary", a.is_primary},
        {"created_at", a.created_at},
        {"updated_at", a.updated_at},
    };
}

}  // namespace Tenancy
