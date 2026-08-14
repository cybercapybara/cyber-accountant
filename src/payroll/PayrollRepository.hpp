/**
 * @file PayrollRepository.hpp
 * @brief All read access to `payroll_runs` / `payslips` lives here
 *        (migrations/013_payroll.sql — design spec §7.2).
 *
 * Org-scoped, so this extends Tenancy::OrgCrudBase for header reads —
 * find_in_org/list_in_org/count_in_org against `payroll_runs`, mirroring
 * Ledger::JournalRepository's overall shape.
 *
 * Deliberately NO insert/update/delete methods: calculating a run needs
 * MULTIPLE statements (an upsert-shaped header write, a wipe of the run's
 * prior payslips on recalculation, then one INSERT per active employee) to
 * land in exactly ONE database transaction — same rationale as
 * Ledger::JournalRepository's write-side split. All of that (plus the
 * approve()/post_to_journal() transitions) lives in
 * Payroll::PayrollService instead of here; this repository only ever reads.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "payroll/Payslip.hpp"
#include "tenancy/OrgScoped.hpp"

namespace Payroll {

class PayrollRepository : public Tenancy::OrgCrudBase<PayrollRepository, PayrollRun, std::string> {
public:
    // OrgCrudBase contract — supplies find_in_org(id,org_id)/
    // list_in_org(org_id,limit,offset)/count_in_org(org_id) against
    // payroll_runs headers. Payslips are never selected here (see
    // list_payslips) — a header row alone never tells you whether its
    // payslips are loaded, so from_row leaves PayrollRun::payslips empty by
    // design (mirrors JournalEntry::lines).
    static constexpr const char* kTable = "payroll_runs";
    static constexpr const char* kColumns =
        "id, org_id, period_year, period_month, status, calculated_at, rates_snapshot, journal_entry_id, posted_at, "
        "created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "period_year DESC, period_month DESC";
    static constexpr const char* kOrgColumn = "org_id";

    /// Shared with PayrollService's write-side RETURNING clauses so both
    /// files agree on the exact payslip shape read back from Postgres.
    static constexpr const char* kPayslipColumns =
        "id, org_id, run_id, employee_id, gross_tiyn, opv, vosms, ipn, net, opvr, so, osms, social_tax, created_at, "
        "updated_at";

    /**
     * @brief The run for exactly one (org_id, period_year, period_month), if
     *        one has ever been calculated — the UNIQUE key
     *        migrations/013_payroll.sql declares on payroll_runs.
     * @p from_primary mirrors OrgCrudBase::find_in_org's parameter of the
     *        same name: PayrollService::calculate_run passes true so its
     *        approved/draft status check (and the compare-and-swap UPDATE
     *        that follows it) never acts on a stale replica read.
     */
    std::optional<PayrollRun> find_by_period(const std::string& org_id,
                                             int period_year,
                                             int period_month,
                                             bool from_primary = false) {
        auto query = [&](auto& txn) -> std::optional<PayrollRun> {
            auto r = txn.exec_params("SELECT " + std::string(kColumns) +
                                         " FROM payroll_runs WHERE org_id = $1 AND period_year = $2 AND "
                                         "period_month = $3",
                                     org_id,
                                     period_year,
                                     period_month);
            if (r.empty())
                return std::nullopt;
            return PayrollRun::from_row(r[0]);
        };
        return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
    }

    /**
     * @brief Payslips of @p run, in insertion order (oldest first). A
     *        standalone read — for the payslips rows written moments earlier
     *        inside a still-open write transaction, PayrollService reads
     *        them off that same transaction directly instead (same posture
     *        as Ledger::JournalRepository::load_lines).
     * @p from_primary mirrors OrgCrudBase::find_in_org's parameter of the
     *        same name: pass true right after a write (e.g. PayrollService
     *        re-loading payslips just approved) so a lagging replica can't
     *        return an empty/stale array.
     */
    std::vector<Payslip> list_payslips(const PayrollRun& run, bool from_primary = false) {
        auto query = [&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT " + std::string(kPayslipColumns) + " FROM payslips WHERE run_id = $1 ORDER BY created_at, id",
                run.id);
            std::vector<Payslip> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Payslip::from_row(row));
            return out;
        };
        return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
    }
};

}  // namespace Payroll
