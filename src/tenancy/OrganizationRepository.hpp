/**
 * @file OrganizationRepository.hpp
 * @brief All SQL touching `organizations` lives here.
 *
 * find/list/count come from CrudBase (RoleRepository/AuditRepository style);
 * create/update_status are the bespoke queries this table needs. Constraint
 * violations surface as typed exceptions (DuplicateBin), same pattern as
 * UserRepository's DuplicateEmail / RoleRepository's DuplicateRole, so the
 * HTTP layer maps them to 409 via Api::with_repo_errors() without sniffing
 * SQLSTATEs itself.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "repositories/CrudBase.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"
#include "tenancy/Organization.hpp"

namespace Tenancy {

// Stable 409 code carried on the exception, so with_repo_errors() maps it
// without including this header.
struct DuplicateBin : Repositories::ConflictError {
    DuplicateBin() : Repositories::ConflictError("bin_taken", "An organization with that BIN already exists") {}
};

struct OrganizationNotFound : Repositories::NotFoundError {
    OrganizationNotFound() : Repositories::NotFoundError("organization") {}
};

class OrganizationRepository : public Repositories::CrudBase<OrganizationRepository, Organization, std::string> {
public:
    // CrudBase contract — supplies find(id) / list(limit,offset) / count().
    static constexpr const char* kTable = "organizations";
    static constexpr const char* kColumns = "id, bin, name, tax_regime, vat_payer, status, created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "created_at DESC";

    /**
     * @brief Insert a new tenant. Throws DuplicateBin on UNIQUE(bin)
     *        violation (SQLSTATE 23505).
     */
    Organization create(const std::string& bin,
                        const std::string& name,
                        const std::string& tax_regime,
                        bool vat_payer) {
        return Repositories::detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    auto r = txn.exec_params(
                        "INSERT INTO organizations (bin, name, tax_regime, vat_payer) "
                        "VALUES ($1, $2, $3, $4) "
                        "RETURNING id, bin, name, tax_regime, vat_payer, status, created_at, updated_at",
                        bin,
                        name,
                        tax_regime,
                        vat_payer);
                    return Organization::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateBin{};
            });
    }

    /**
     * @brief Transition status ('active' | 'suspended' | 'archived', see the
     *        CHECK constraint in migrations/006_organizations.sql).
     * @return false if no such organization — an expected branch the caller
     *         maps to 404, not an exceptional repo error (mirrors
     *         UserRepository::mark_confirmed).
     */
    bool update_status(const std::string& id, const std::string& status) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("UPDATE organizations SET status = $2 WHERE id = $1 RETURNING id", id, status);
            return !r.empty();
        });
    }
};

}  // namespace Tenancy
