/**
 * @file HrRepository.hpp
 * @brief All SQL touching `hr_orders` / `labor_contracts` / `vacations`
 *        lives here.
 *
 * Org-scoped (design spec §5: "методов 'выбрать без org' не существует"), so
 * this extends Tenancy::OrgCrudBase — parameterized on HrOrder, the entity
 * OrgCrudBase's find_in_org/list_in_org/count_in_org read against
 * `hr_orders` — while create_contract/list_contracts/create_vacation/
 * list_vacations are bespoke queries against the other two tables that
 * OrgCrudBase's single-Entity contract can't express. Mirrors
 * src/ledger/DocumentRepository.hpp's overall shape: none of these writes
 * throw a domain 409 — cross-org (employee_id, org_id) mismatches are
 * rejected at the composite-FK level (migrations/012_hr.sql) and surface as
 * a raw pqxx::sql_error (SQLSTATE 23503), the same "trusts its caller on
 * this axis" posture DocumentRepository takes toward document_entries.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "hr/HrDocuments.hpp"
#include "tenancy/OrgScoped.hpp"

namespace Hr {

class HrRepository : public Tenancy::OrgCrudBase<HrRepository, HrOrder, std::string> {
public:
    // OrgCrudBase contract — supplies find_in_org(id,org_id) /
    // list_in_org(org_id,limit,offset) / count_in_org(org_id) against
    // `hr_orders`. labor_contracts/vacations rows are never selected through
    // this base — list_contracts/list_vacations below are the bespoke
    // queries for those two tables.
    static constexpr const char* kTable = "hr_orders";
    static constexpr const char* kColumns =
        "id, org_id, employee_id, kind, number, issued_on, effective_from, effective_to, payload, document_id, "
        "created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "issued_on DESC, created_at DESC";
    static constexpr const char* kOrgColumn = "org_id";

    static constexpr const char* kContractColumns =
        "id, org_id, employee_id, number, signed_on, starts_on, ends_on, terms_json, created_at, updated_at";
    static constexpr const char* kVacationColumns =
        "id, org_id, employee_id, starts_on, ends_on, days, kind, created_at, updated_at";

    /**
     * @brief Insert a new HR order for @p org_id / @p employee_id. No
     *        domain-level conflict here (no UNIQUE constraint on hr_orders
     *        beyond its own id) — a cross-org @p employee_id (one that
     *        doesn't belong to @p org_id) trips the composite FK
     *        (employee_id, org_id) -> employees(id, org_id) and bubbles up
     *        as a raw pqxx::sql_error (SQLSTATE 23503), same posture as this
     *        file's header comment.
     */
    HrOrder create_order(const std::string& org_id,
                         const std::string& employee_id,
                         const std::string& kind,
                         const std::string& number,
                         const std::string& issued_on,
                         const std::string& effective_from,
                         std::optional<std::string> effective_to = std::nullopt,
                         std::optional<nlohmann::json> payload = std::nullopt,
                         std::optional<std::string> document_id = std::nullopt) {
        std::optional<std::string> payload_text;
        if (payload)
            payload_text = payload->dump();

        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "INSERT INTO hr_orders (org_id, employee_id, kind, number, issued_on, effective_from, "
                "effective_to, payload, document_id) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8::jsonb, $9) "
                "RETURNING " +
                    std::string(kColumns),
                org_id,
                employee_id,
                kind,
                number,
                issued_on,
                effective_from,
                effective_to,
                payload_text,
                document_id);
            return HrOrder::from_row(r[0]);
        });
    }

    /// HR orders for @p org_id, optionally narrowed to one
    /// @p employee_id_opt — nullopt means "every employee", same
    /// allowlisted-filter idiom as
    /// Ledger::DocumentRepository::list_filtered's doc_type/status.
    std::vector<HrOrder> list_orders(const std::string& org_id,
                                     const std::optional<std::string>& employee_id_opt = std::nullopt) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT " + std::string(kColumns) +
                                         " FROM hr_orders WHERE org_id = $1 "
                                         "AND ($2::uuid IS NULL OR employee_id = $2::uuid) "
                                         "ORDER BY " +
                                         std::string(kOrderBy),
                                     org_id,
                                     employee_id_opt);
            std::vector<HrOrder> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(HrOrder::from_row(row));
            return out;
        });
    }

    /**
     * @brief Attach a document (Ledger::Document) to an existing HR order.
     * @return false if no row matches (id, org_id) both — the standard
     *         OrgCrudBase-style "wrong org is indistinguishable from
     *         missing" contract, not a separate 403 branch.
     */
    bool attach_document(const std::string& org_id, const std::string& order_id, const std::string& document_id) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("UPDATE hr_orders SET document_id = $3 WHERE id = $1 AND org_id = $2 RETURNING id",
                                     order_id,
                                     org_id,
                                     document_id);
            return !r.empty();
        });
    }

    /// Insert a new labor contract. Same cross-org FK posture as
    /// create_order() above — a mismatched @p employee_id/@p org_id pair
    /// trips the composite FK and bubbles up as a raw pqxx::sql_error.
    LaborContract create_contract(const std::string& org_id,
                                  const std::string& employee_id,
                                  const std::string& number,
                                  const std::string& signed_on,
                                  const std::string& starts_on,
                                  std::optional<std::string> ends_on = std::nullopt,
                                  std::optional<nlohmann::json> terms_json = std::nullopt) {
        std::optional<std::string> terms_text;
        if (terms_json)
            terms_text = terms_json->dump();

        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "INSERT INTO labor_contracts (org_id, employee_id, number, signed_on, starts_on, ends_on, "
                "terms_json) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7::jsonb) "
                "RETURNING " +
                    std::string(kContractColumns),
                org_id,
                employee_id,
                number,
                signed_on,
                starts_on,
                ends_on,
                terms_text);
            return LaborContract::from_row(r[0]);
        });
    }

    /// Labor contracts for @p org_id / @p employee_id, newest-signed first.
    std::vector<LaborContract> list_contracts(const std::string& org_id, const std::string& employee_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT " + std::string(kContractColumns) +
                                         " FROM labor_contracts WHERE org_id = $1 AND employee_id = $2 "
                                         "ORDER BY signed_on DESC, created_at DESC",
                                     org_id,
                                     employee_id);
            std::vector<LaborContract> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(LaborContract::from_row(row));
            return out;
        });
    }

    /// Insert a new vacation record. Same cross-org FK posture as
    /// create_order() above.
    Vacation create_vacation(const std::string& org_id,
                             const std::string& employee_id,
                             const std::string& starts_on,
                             const std::string& ends_on,
                             int days,
                             const std::string& kind) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "INSERT INTO vacations (org_id, employee_id, starts_on, ends_on, days, kind) "
                "VALUES ($1, $2, $3, $4, $5, $6) "
                "RETURNING " +
                    std::string(kVacationColumns),
                org_id,
                employee_id,
                starts_on,
                ends_on,
                days,
                kind);
            return Vacation::from_row(r[0]);
        });
    }

    /// Vacations for @p org_id, optionally narrowed to one
    /// @p employee_id_opt — nullopt means "every employee", same idiom as
    /// list_orders() above.
    std::vector<Vacation> list_vacations(const std::string& org_id,
                                         const std::optional<std::string>& employee_id_opt = std::nullopt) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT " + std::string(kVacationColumns) +
                                         " FROM vacations WHERE org_id = $1 "
                                         "AND ($2::uuid IS NULL OR employee_id = $2::uuid) "
                                         "ORDER BY starts_on DESC, created_at DESC",
                                     org_id,
                                     employee_id_opt);
            std::vector<Vacation> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Vacation::from_row(row));
            return out;
        });
    }
};

}  // namespace Hr
