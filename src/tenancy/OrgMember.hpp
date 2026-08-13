/**
 * @file OrgMember.hpp
 * @brief Membership row: a user's role within an organization. Mirrors the
 *        `org_members` table (migrations/006_organizations.sql). role is
 *        CHECK-constrained to 'owner' | 'accountant' | 'viewer' at the DB
 *        layer; Tenancy::is_valid_role() (Organization.hpp) mirrors that
 *        constraint app-side.
 */

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace Tenancy {

struct OrgMember {
    std::string id;
    std::string org_id;
    std::string user_id;
    std::string role;  // 'owner' | 'accountant' | 'viewer'
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static OrgMember from_row(const Row& row) {
        OrgMember m;
        m.id = row["id"].template as<std::string>();
        m.org_id = row["org_id"].template as<std::string>();
        m.user_id = row["user_id"].template as<std::string>();
        m.role = row["role"].template as<std::string>();
        m.created_at = row["created_at"].template as<std::string>();
        m.updated_at = row["updated_at"].template as<std::string>();
        return m;
    }
};

inline void to_json(nlohmann::json& j, const OrgMember& m) {
    j = nlohmann::json{
        {"id", m.id},
        {"org_id", m.org_id},
        {"user_id", m.user_id},
        {"role", m.role},
        {"created_at", m.created_at},
        {"updated_at", m.updated_at},
    };
}

}  // namespace Tenancy
