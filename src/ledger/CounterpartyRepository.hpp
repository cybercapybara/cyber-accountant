/**
 * @file CounterpartyRepository.hpp
 * @brief All SQL touching `counterparties` lives here.
 *
 * Org-scoped (design spec §5: "методов 'выбрать без org' не существует"), so
 * this extends Tenancy::OrgCrudBase rather than Repositories::CrudBase —
 * find_in_org/list_in_org/count_in_org come from the base, and
 * create/update/find_by_identifier are the bespoke queries this table needs.
 * Mirrors Tenancy::OrganizationRepository: constraint violations surface as
 * typed exceptions (DuplicateCounterparty) via
 * Repositories::detail::translate_sql, so the HTTP layer maps them to 409
 * via Api::with_repo_errors() without sniffing SQLSTATEs itself.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "ledger/Counterparty.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"
#include "tenancy/OrgScoped.hpp"

namespace Ledger {

// Stable 409 code carried on the exception, so with_repo_errors() maps it
// without including this header.
struct DuplicateCounterparty : Repositories::ConflictError {
    DuplicateCounterparty()
        : Repositories::ConflictError("counterparty_identifier_taken",
                                      "A counterparty with that identifier already exists in this organization") {}
};

class CounterpartyRepository : public Tenancy::OrgCrudBase<CounterpartyRepository, Counterparty, std::string> {
public:
    // OrgCrudBase contract — supplies find_in_org(id,org_id) /
    // list_in_org(org_id,limit,offset) / count_in_org(org_id).
    static constexpr const char* kTable = "counterparties";
    static constexpr const char* kColumns =
        "id, org_id, identifier, name, address, iik, bik, kbe, is_resident, vat_payer, contact_email, created_at, "
        "updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "created_at DESC";
    static constexpr const char* kOrgColumn = "org_id";

    /**
     * @brief Insert a new counterparty for @p org_id. Throws
     *        DuplicateCounterparty on UNIQUE(org_id, identifier) violation
     *        (SQLSTATE 23505). Only the fields callers can populate are read
     *        off @p draft — id/created_at/updated_at are DB-assigned.
     */
    Counterparty create(const std::string& org_id, const Counterparty& draft) {
        return Repositories::detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    auto r = txn.exec_params(
                        "INSERT INTO counterparties (org_id, identifier, name, address, iik, bik, kbe, is_resident, "
                        "vat_payer, contact_email) "
                        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10) "
                        "RETURNING " +
                            std::string(kColumns),
                        org_id,
                        draft.identifier,
                        draft.name,
                        draft.address,
                        draft.iik,
                        draft.bik,
                        draft.kbe,
                        draft.is_resident,
                        draft.vat_payer,
                        draft.contact_email);
                    return Counterparty::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateCounterparty{};
            });
    }

    /**
     * @brief Patch every editable field on the counterparty identified by
     *        (@p id, @p org_id) in one UPDATE + RETURNING. Throws
     *        DuplicateCounterparty if the patched identifier collides with
     *        another row's UNIQUE(org_id, identifier).
     * @return std::nullopt if no row matches — id and org_id both scope the
     *         WHERE clause, so a wrong org is indistinguishable from a
     *         missing id (same rationale as OrgCrudBase::find_in_org).
     */
    std::optional<Counterparty> update(const std::string& org_id, const std::string& id, const Counterparty& patch) {
        return Repositories::detail::translate_sql(
            [&]() -> std::optional<Counterparty> {
                return Database::get().execute_write([&](auto& txn) -> std::optional<Counterparty> {
                    auto r = txn.exec_params(
                        "UPDATE counterparties SET identifier = $3, name = $4, address = $5, iik = $6, bik = $7, "
                        "kbe = $8, is_resident = $9, vat_payer = $10, contact_email = $11 "
                        "WHERE id = $1 AND org_id = $2 "
                        "RETURNING " +
                            std::string(kColumns),
                        id,
                        org_id,
                        patch.identifier,
                        patch.name,
                        patch.address,
                        patch.iik,
                        patch.bik,
                        patch.kbe,
                        patch.is_resident,
                        patch.vat_payer,
                        patch.contact_email);
                    if (r.empty())
                        return std::nullopt;
                    return Counterparty::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateCounterparty{};
            });
    }

    /// Lookup by the org's own BIN/IIN, scoped to @p org_id — used to check
    /// for an existing counterparty before create (or by API layer lookups).
    std::optional<Counterparty> find_by_identifier(const std::string& org_id, const std::string& identifier) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Counterparty> {
            auto r = txn.exec_params(
                "SELECT " + std::string(kColumns) + " FROM counterparties WHERE org_id = $1 AND identifier = $2",
                org_id,
                identifier);
            if (r.empty())
                return std::nullopt;
            return Counterparty::from_row(r[0]);
        });
    }
};

}  // namespace Ledger
