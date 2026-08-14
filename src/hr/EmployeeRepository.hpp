/**
 * @file EmployeeRepository.hpp
 * @brief All SQL touching `employees` lives here.
 *
 * Org-scoped (design spec §5: "методов 'выбрать без org' не существует"), so
 * this extends Tenancy::OrgCrudBase rather than Repositories::CrudBase —
 * find_in_org/list_in_org/count_in_org come from the base, and
 * create/update/dismiss/list_active are the bespoke queries this table
 * needs. Mirrors src/ledger/CounterpartyRepository.hpp: constraint
 * violations surface as a typed exception (DuplicateEmployeeIin) via
 * Repositories::detail::translate_sql, so the HTTP layer maps it to 409 via
 * Api::with_repo_errors() without sniffing SQLSTATEs itself.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "hr/Employee.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"
#include "tenancy/OrgScoped.hpp"

namespace Hr {

// Stable 409 code carried on the exception, so with_repo_errors() maps it
// without including this header.
struct DuplicateEmployeeIin : Repositories::ConflictError {
    DuplicateEmployeeIin()
        : Repositories::ConflictError("employee_iin_taken",
                                      "An employee with that IIN already exists in this organization") {}
};

class EmployeeRepository : public Tenancy::OrgCrudBase<EmployeeRepository, Employee, std::string> {
public:
    // OrgCrudBase contract — supplies find_in_org(id,org_id) /
    // list_in_org(org_id,limit,offset) / count_in_org(org_id).
    static constexpr const char* kTable = "employees";
    static constexpr const char* kColumns =
        "id, org_id, iin, last_name, first_name, middle_name, position, salary_tiyn, hired_on, dismissed_on, "
        "ipn_deduction_claimed, opvr_exempt, payout_iik, status, created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "created_at DESC";
    static constexpr const char* kOrgColumn = "org_id";

    /**
     * @brief Insert a new employee for @p org_id. Throws DuplicateEmployeeIin
     *        on UNIQUE(org_id, iin) violation (SQLSTATE 23505). Only the
     *        fields callers can populate on hire are read off @p draft —
     *        id/status/created_at/updated_at are DB-assigned/defaulted, and
     *        dismissed_on is left NULL (an employee is never created
     *        pre-dismissed; see dismiss() below for that transition).
     */
    Employee create(const std::string& org_id, const Employee& draft) {
        return Repositories::detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    auto r = txn.exec_params(
                        "INSERT INTO employees (org_id, iin, last_name, first_name, middle_name, position, "
                        "salary_tiyn, hired_on, ipn_deduction_claimed, opvr_exempt, payout_iik) "
                        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11) "
                        "RETURNING " +
                            std::string(kColumns),
                        org_id,
                        draft.iin,
                        draft.last_name,
                        draft.first_name,
                        draft.middle_name,
                        draft.position,
                        draft.salary_tiyn,
                        draft.hired_on,
                        draft.ipn_deduction_claimed,
                        draft.opvr_exempt,
                        draft.payout_iik);
                    return Employee::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateEmployeeIin{};
            });
    }

    /**
     * @brief Patch every editable field on the employee identified by
     *        (@p id, @p org_id) in one UPDATE + RETURNING. Throws
     *        DuplicateEmployeeIin if the patched iin collides with another
     *        row's UNIQUE(org_id, iin). hired_on/status/dismissed_on are
     *        deliberately NOT patchable here — hired_on is fixed at create()
     *        time, and status/dismissed_on move together only through
     *        dismiss() below, never independently.
     * @return std::nullopt if no row matches — id and org_id both scope the
     *         WHERE clause, so a wrong org is indistinguishable from a
     *         missing id (same rationale as OrgCrudBase::find_in_org).
     */
    std::optional<Employee> update(const std::string& org_id, const std::string& id, const Employee& patch) {
        return Repositories::detail::translate_sql(
            [&]() -> std::optional<Employee> {
                return Database::get().execute_write([&](auto& txn) -> std::optional<Employee> {
                    auto r = txn.exec_params(
                        "UPDATE employees SET iin = $3, last_name = $4, first_name = $5, middle_name = $6, "
                        "position = $7, salary_tiyn = $8, ipn_deduction_claimed = $9, opvr_exempt = $10, "
                        "payout_iik = $11 "
                        "WHERE id = $1 AND org_id = $2 "
                        "RETURNING " +
                            std::string(kColumns),
                        id,
                        org_id,
                        patch.iin,
                        patch.last_name,
                        patch.first_name,
                        patch.middle_name,
                        patch.position,
                        patch.salary_tiyn,
                        patch.ipn_deduction_claimed,
                        patch.opvr_exempt,
                        patch.payout_iik);
                    if (r.empty())
                        return std::nullopt;
                    return Employee::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateEmployeeIin{};
            });
    }

    /**
     * @brief Dismiss the employee identified by (@p id, @p org_id): sets
     *        status = 'dismissed' and dismissed_on = @p date in one
     *        UPDATE + RETURNING.
     * @return std::nullopt if no row matches (id, org_id, status='active')
     *         all three — a wrong org, a missing id, and an already-dismissed
     *         employee are all indistinguishable from each other here, same
     *         "can't tell why, only that nothing was written" contract as
     *         Ledger::DocumentRepository::set_status.
     */
    std::optional<Employee> dismiss(const std::string& org_id, const std::string& id, const std::string& date) {
        return Database::get().execute_write([&](auto& txn) -> std::optional<Employee> {
            auto r = txn.exec_params(
                "UPDATE employees SET status = 'dismissed', dismissed_on = $3 "
                "WHERE id = $1 AND org_id = $2 AND status = 'active' "
                "RETURNING " +
                    std::string(kColumns),
                id,
                org_id,
                date);
            if (r.empty())
                return std::nullopt;
            return Employee::from_row(r[0]);
        });
    }

    /// Employees with status = 'active' for @p org_id, newest-hired first
    /// (kOrderBy). Excludes dismissed employees — callers that need the full
    /// roster including dismissals use OrgCrudBase::list_in_org instead.
    std::vector<Employee> list_active(const std::string& org_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT " + std::string(kColumns) +
                                         " FROM employees WHERE org_id = $1 AND status = 'active' ORDER BY " +
                                         std::string(kOrderBy),
                                     org_id);
            std::vector<Employee> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Employee::from_row(row));
            return out;
        });
    }
};

}  // namespace Hr
