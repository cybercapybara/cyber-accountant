/**
 * @file Account.hpp
 * @brief Chart-of-accounts row. Mirrors the `accounts` table
 *        (migrations/008_accounts.sql — design spec §6.1), seeded with the
 *        Kazakhstan standard chart of accounts (приказ МФ РК №185).
 *
 * Domain-only — no SQL here; persistence lives in
 * src/ledger/AccountRepository.hpp. Follows the same from_row/to_json idioms
 * as src/tenancy/Organization.hpp and src/ledger/Counterparty.hpp: from_row
 * is a templated static factory (works with any pqxx row-like type), and
 * to_json is a free function found via ADL so `nlohmann::json j = account;`
 * "just works" without a member method.
 *
 * `org_id` is the ONE documented exception to "org_id NOT NULL" (see
 * AccountRepository.hpp and Global Constraints in the P1 plan): NULL means a
 * system row from the standard chart, visible to every organization;
 * non-NULL means a tenant-created subaccount, visible only to that org.
 * `parent_code` is set on subaccounts and points at the code of the (system
 * or own) account they extend; it is NULL on top-level system rows.
 */

#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace Ledger {

struct Account {
    std::string id;
    std::optional<std::string> org_id;  // NULL = system row (standard chart)
    std::string code;
    std::string name_ru;
    std::string name_kk;  // filled in P2 (bilingual ФНО forms); empty in P1
    std::string type;     // 'asset' | 'liability' | 'equity' | 'income' | 'expense'
    std::optional<std::string> parent_code;
    bool currency_tracked = false;
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Account from_row(const Row& row) {
        Account a;
        a.id = row["id"].template as<std::string>();
        if (!row["org_id"].is_null())
            a.org_id = row["org_id"].template as<std::string>();
        a.code = row["code"].template as<std::string>();
        a.name_ru = row["name_ru"].template as<std::string>();
        a.name_kk = row["name_kk"].template as<std::string>();
        a.type = row["type"].template as<std::string>();
        if (!row["parent_code"].is_null())
            a.parent_code = row["parent_code"].template as<std::string>();
        a.currency_tracked = row["currency_tracked"].template as<bool>();
        a.created_at = row["created_at"].template as<std::string>();
        a.updated_at = row["updated_at"].template as<std::string>();
        return a;
    }
};

/// Public JSON shape — everything on the row is safe to expose (no secrets).
inline void to_json(nlohmann::json& j, const Account& a) {
    j = nlohmann::json{
        {"id", a.id},
        {"org_id", a.org_id ? nlohmann::json(*a.org_id) : nlohmann::json(nullptr)},
        {"code", a.code},
        {"name_ru", a.name_ru},
        {"name_kk", a.name_kk},
        {"type", a.type},
        {"parent_code", a.parent_code ? nlohmann::json(*a.parent_code) : nlohmann::json(nullptr)},
        {"currency_tracked", a.currency_tracked},
        {"created_at", a.created_at},
        {"updated_at", a.updated_at},
    };
}

}  // namespace Ledger
