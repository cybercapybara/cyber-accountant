/**
 * @file Employee.hpp
 * @brief Employee row — carточка сотрудника организации. Mirrors the
 *        `employees` table (migrations/012_hr.sql — design spec §7.2).
 *
 * Domain-only — no SQL here; persistence lives in
 * src/hr/EmployeeRepository.hpp. Follows the same from_row/to_json idioms as
 * src/ledger/Counterparty.hpp: from_row is a templated static factory (works
 * with any pqxx row-like type), to_json is a free function found via ADL,
 * and every nullable DB column becomes a std::optional<...> here — populated
 * only when the column isn't NULL, but always rendered as an explicit JSON
 * `null` (never an omitted key) so every response has a stable shape.
 *
 * `iin` is the employee's ИИН, stored as-is; validating its check digit
 * (Ledger::is_valid_bin_iin, src/ledger/KzIdentifiers.hpp) is the API
 * layer's job, not this repository's — same posture as
 * Ledger::Counterparty::identifier.
 */

#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace Hr {

struct Employee {
    std::string id;
    std::string org_id;
    std::string iin;  // ИИН, 12 digits, stored as-is
    std::string last_name;
    std::string first_name;
    std::optional<std::string> middle_name;
    std::string position;
    long long salary_tiyn = 0;
    std::string hired_on;
    std::optional<std::string> dismissed_on;
    bool ipn_deduction_claimed = false;
    bool opvr_exempt = false;
    std::string payout_iik;
    std::string status;  // 'active' | 'dismissed' — CHECK in migrations/012_hr.sql
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Employee from_row(const Row& row) {
        Employee e;
        e.id = row["id"].template as<std::string>();
        e.org_id = row["org_id"].template as<std::string>();
        e.iin = row["iin"].template as<std::string>();
        e.last_name = row["last_name"].template as<std::string>();
        e.first_name = row["first_name"].template as<std::string>();
        if (!row["middle_name"].is_null())
            e.middle_name = row["middle_name"].template as<std::string>();
        e.position = row["position"].template as<std::string>();
        e.salary_tiyn = row["salary_tiyn"].template as<long long>();
        e.hired_on = row["hired_on"].template as<std::string>();
        if (!row["dismissed_on"].is_null())
            e.dismissed_on = row["dismissed_on"].template as<std::string>();
        e.ipn_deduction_claimed = row["ipn_deduction_claimed"].template as<bool>();
        e.opvr_exempt = row["opvr_exempt"].template as<bool>();
        e.payout_iik = row["payout_iik"].template as<std::string>();
        e.status = row["status"].template as<std::string>();
        e.created_at = row["created_at"].template as<std::string>();
        e.updated_at = row["updated_at"].template as<std::string>();
        return e;
    }
};

inline void to_json(nlohmann::json& j, const Employee& e) {
    j = nlohmann::json{
        {"id", e.id},
        {"org_id", e.org_id},
        {"iin", e.iin},
        {"last_name", e.last_name},
        {"first_name", e.first_name},
        {"middle_name", e.middle_name ? nlohmann::json(*e.middle_name) : nlohmann::json(nullptr)},
        {"position", e.position},
        {"salary_tiyn", e.salary_tiyn},
        {"hired_on", e.hired_on},
        {"dismissed_on", e.dismissed_on ? nlohmann::json(*e.dismissed_on) : nlohmann::json(nullptr)},
        {"ipn_deduction_claimed", e.ipn_deduction_claimed},
        {"opvr_exempt", e.opvr_exempt},
        {"payout_iik", e.payout_iik},
        {"status", e.status},
        {"created_at", e.created_at},
        {"updated_at", e.updated_at},
    };
}

}  // namespace Hr
