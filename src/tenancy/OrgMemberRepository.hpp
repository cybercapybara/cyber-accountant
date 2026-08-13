/**
 * @file OrgMemberRepository.hpp
 * @brief All SQL touching `org_members` lives here.
 *
 * Every query is scoped by the (org_id, user_id) composite the table's
 * UNIQUE constraint enforces (migrations/006_organizations.sql) rather than
 * the row's own `id`, so this repo does NOT extend CrudBase (same rationale
 * as UserRepository not extending it: the bespoke access pattern doesn't fit
 * the single-PK contract). role stays app-side validated via
 * Tenancy::is_valid_role() where useful; the DB CHECK constraint is the
 * final backstop and raises a plain pqxx::sql_error (23514) if bypassed.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"
#include "tenancy/OrgMember.hpp"

namespace Tenancy {

// Stable 409 code carried on the exception, mirrors UserRepository::DuplicateEmail.
struct DuplicateMembership : Repositories::ConflictError {
    DuplicateMembership()
        : Repositories::ConflictError("membership_exists", "This user is already a member of the organization") {}
};

class OrgMemberRepository {
public:
    static constexpr const char* kColumns = "id, org_id, user_id, role, created_at, updated_at";

    /// The membership row for one (org, user) pair, if any.
    std::optional<OrgMember> find_membership(const std::string& org_id, const std::string& user_id) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<OrgMember> {
            auto r = txn.exec_params(
                "SELECT " + std::string(kColumns) + " FROM org_members WHERE org_id = $1 AND user_id = $2",
                org_id,
                user_id);
            if (r.empty())
                return std::nullopt;
            return OrgMember::from_row(r[0]);
        });
    }

    /// Every organization a user belongs to.
    std::vector<OrgMember> list_for_user(const std::string& user_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT " + std::string(kColumns) + " FROM org_members WHERE user_id = $1 ORDER BY created_at",
                user_id);
            std::vector<OrgMember> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(OrgMember::from_row(row));
            return out;
        });
    }

    /// Every member of an organization.
    std::vector<OrgMember> list_members(const std::string& org_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT " + std::string(kColumns) + " FROM org_members WHERE org_id = $1 ORDER BY created_at", org_id);
            std::vector<OrgMember> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(OrgMember::from_row(row));
            return out;
        });
    }

    /**
     * @brief Add a user to an organization with a role. Throws
     *        DuplicateMembership on UNIQUE(org_id, user_id) violation
     *        (SQLSTATE 23505) — a user can only hold one role per org.
     */
    OrgMember add(const std::string& org_id, const std::string& user_id, const std::string& role) {
        return Repositories::detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    auto r = txn.exec_params(
                        "INSERT INTO org_members (org_id, user_id, role) VALUES ($1, $2, $3) "
                        "RETURNING " +
                            std::string(kColumns),
                        org_id,
                        user_id,
                        role);
                    return OrgMember::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateMembership{};
            });
    }

    /**
     * @return false if no such membership — an expected branch the caller
     *         maps to 404, not an exceptional repo error (mirrors
     *         UserRepository::mark_confirmed).
     */
    bool set_role(const std::string& org_id, const std::string& user_id, const std::string& role) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("UPDATE org_members SET role = $3 WHERE org_id = $1 AND user_id = $2 RETURNING id",
                                     org_id,
                                     user_id,
                                     role);
            return !r.empty();
        });
    }

    /// @return false if no such membership existed.
    bool remove(const std::string& org_id, const std::string& user_id) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "DELETE FROM org_members WHERE org_id = $1 AND user_id = $2 RETURNING id", org_id, user_id);
            return !r.empty();
        });
    }
};

}  // namespace Tenancy
