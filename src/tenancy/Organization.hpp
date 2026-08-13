/**
 * @file Organization.hpp
 * @brief Organization row (tenant). Mirrors the `organizations` table
 *        (migrations/006_organizations.sql — design spec §5).
 *
 * Domain-only — no SQL here; persistence lives in
 * src/tenancy/OrganizationRepository.hpp. Follows the same from_row/to_json
 * idioms as src/domain/User.hpp and src/domain/Role.hpp: from_row is a
 * templated static factory (works with any pqxx row-like type), and to_json
 * is a free function found via ADL so `nlohmann::json j = org;` "just works"
 * without a member method.
 */

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace Tenancy {

struct Organization {
    std::string id;
    std::string bin;  // 12-digit BIN/IIN, unique
    std::string name;
    std::string tax_regime;  // 'snr_simplified' | 'standard'
    bool vat_payer = false;
    std::string status;  // 'active' | 'suspended' | 'archived'
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Organization from_row(const Row& row) {
        Organization o;
        o.id = row["id"].template as<std::string>();
        o.bin = row["bin"].template as<std::string>();
        o.name = row["name"].template as<std::string>();
        o.tax_regime = row["tax_regime"].template as<std::string>();
        o.vat_payer = row["vat_payer"].template as<bool>();
        o.status = row["status"].template as<std::string>();
        o.created_at = row["created_at"].template as<std::string>();
        o.updated_at = row["updated_at"].template as<std::string>();
        return o;
    }
};

/// Public JSON shape — everything on the row is safe to expose (no secrets).
inline void to_json(nlohmann::json& j, const Organization& o) {
    j = nlohmann::json{
        {"id", o.id},
        {"bin", o.bin},
        {"name", o.name},
        {"tax_regime", o.tax_regime},
        {"vat_payer", o.vat_payer},
        {"status", o.status},
        {"created_at", o.created_at},
        {"updated_at", o.updated_at},
    };
}

/// The three tenancy roles org_members.role is CHECK-constrained to
/// (migrations/006_organizations.sql). Kept here so callers can validate a
/// role value before it ever reaches the database.
inline bool is_valid_role(const std::string& role) {
    return role == "owner" || role == "accountant" || role == "viewer";
}

}  // namespace Tenancy
