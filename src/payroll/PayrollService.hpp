/**
 * @file PayrollService.hpp
 * @brief Business logic for payroll runs: calculate/recalculate, approve, and
 *        post the period's withholdings to the journal (design spec §7.2,
 *        phase P2 task-4-brief.md).
 *
 * `calculate_run()` resolves the rates/constants a single time, on the LAST
 * calendar day of (period_year, period_month) — Tax::TaxReferenceRepository's
 * `rate_on`/`constant_on` both take an effective-date, and "the rate/constant
 * in force at period end" is the natural snapshot instant for a monthly
 * payroll run (mirrors how Tax::TaxService resolves rates for its own
 * period-end calculations, per that service's own file header). The exact
 * bp/tiyn/unit values used are written into `payroll_runs.rates_snapshot` so
 * the run stays reproducible even after a later migration adds a new
 * `effective_from` row for the same rate — see Payslip.hpp's doc comment.
 *
 * Every employee whose EMPLOYMENT OVERLAPS the run's period
 * (Hr::EmployeeRepository::list_employed_during — hired on or before the
 * period's last day and not dismissed before its first, evaluated in SQL, not
 * filtered again here) gets exactly one Payroll::calculate() call and one
 * payslips row. Note this is deliberately NOT the roster as it stands
 * today: a run is calculated FOR a period, frequently
 * after the fact, so today's roster would both pay somebody hired after the
 * period ended and skip somebody who worked it and has since left. See
 * calculate_run() for the (full month vs prorated) pay decision for an
 * employee who joined or left mid-period.
 *
 * A recalculation of a 'draft' run REPLACES its payslips (DELETE + re-INSERT
 * inside the same transaction as the header write) rather than appending —
 * same "recalculate overwrites, doesn't duplicate" contract as
 * Tax::TaxCalculationRepository::upsert. Recalculating an 'approved' run is
 * rejected with InvalidRunState (→ 409): once a run is approved, the payroll
 * office has signed off on those exact figures, so amending them silently out
 * from under an already-approved (and possibly already posted-to-journal)
 * run is not offered — the caller has to know what it's asking for.
 *
 * `post_to_journal()` requires the run to be 'approved' (→ InvalidRunState
 * otherwise — posting a draft's still-mutable figures would let a later
 * calculate_run() silently invalidate an already-posted entry) and refuses a
 * SECOND post of the same run: `payroll_runs.journal_entry_id` starts NULL
 * and is set, together with `posted_at`, in the very statement that records
 * which entry a run was posted to (Fix round 1, code review — the earlier
 * version had neither the 'approved' gate nor this guard, so calling it
 * twice created a second, fully balanced, duplicate journal entry). The
 * guard is checked twice: once up front (`journal_entry_id` already set →
 * InvalidRunState("already_posted") before wasting a journal entry on a
 * request that's going to be rejected anyway) and once as a
 * compare-and-swap on the final UPDATE (`... AND journal_entry_id IS NULL
 * RETURNING id`, zero rows → the same error) — the same
 * check-then-CAS shape as approve()'s draft→approved transition. The CAS is
 * the actual source of truth; the up-front check is an optimization. Because
 * Ledger::JournalService owns every write to `journal_entries` internally
 * (see that file's header — create_draft() and post() each commit their own
 * transaction, by design, so no caller can extend that transaction), this
 * CAS necessarily runs in a transaction of its own, AFTER the entry is
 * created and posted, not literally inside the same transaction as the
 * INSERT — under a true concurrent double-call (two requests racing between
 * the up-front check and the CAS) the loser's already-posted entry would
 * become a real, un-referenced duplicate in the ledger, same as the
 * winner's. That residual window is not closed here: closing it would mean
 * JournalService accepting a caller-supplied transaction, a change to P1
 * code out of scope for this fix. In practice, the request path serializes
 * on the same run_id well before this matters, so the everyday risk this fix
 * closes — a caller (or a retried request) calling post_to_journal twice in
 * sequence for a run that already has an entry — cannot ever create a second
 * entry: the up-front check alone stops it.
 *
 * `post_to_journal()` sums every payslip's fields across the whole run and
 * posts ONE journal entry via Ledger::JournalService::create_draft + post:
 *   - debit  7210 (административные расходы) for gross + opvr + so + osms +
 *     social_tax — the full employer cost, mirroring
 *     Payroll::Result::employer_cost_total's own formula;
 *   - credit 3350 (краткосрочная задолженность по оплате труда) for net;
 *   - credit 3120 (ИПН) for ipn;
 *   - credit 3220 (обязательства по пенсионным отчислениям) for opv + opvr —
 *     ОПВ (withheld from the employee) and ОПВР (employer-borne) share one
 *     liability account in the standard chart of accounts (migrations/
 *     008_accounts.sql), so they are summed onto the same credit line;
 *   - credit 3210 (обязательства по социальному страхованию) for so;
 *   - credit 3230 (прочие обязательства по другим обязательным платежам) for
 *     osms + vosms — same one-account-two-contributions shape as 3220;
 *   - credit 3150 (социальный налог) for social_tax.
 *
 * EVERY one of those lines — the 7210 debit included — is omitted when its
 * total comes out ZERO (Fix round 2, code review; only 3150 used to be
 * guarded). Two reasons, and both matter:
 *   - Ledger::parse_tiyn rejects an amount <= 0 BEFORE any SQL runs, so a
 *     zero line makes create_draft throw std::invalid_argument, which the API
 *     layer's generic handler reports as an opaque 500. That is not an exotic
 *     edge case: on the 2026 seed the ИПН base is `0.88·gross − 30 МРП`,
 *     which is zero for any gross at or below roughly 147 400 ₸, so an
 *     organization whose employees all claim the standard deduction and earn
 *     near the minimum wage had `ipn_total == 0` and could NEVER post payroll
 *     at all, with no message explaining why and no way around it in the UI.
 *     Other buckets reach zero the same ordinary way: `opv_total +
 *     opvr_total` on a roster of ОПВР-exempt pensioners whose ОПВ is nil, or
 *     any bucket at all once a rate is legitimately set to 0 bp.
 *   - A zero-amount line carries no accounting meaning anyway: it is a
 *     liability row asserting that nothing is owed.
 * Dropping such a line cannot unbalance the entry, because a zero addend
 * contributes nothing to either side of Σdebit = Σcredit.
 *
 * The fix is here and NOT in parse_tiyn: its refusal of non-positive amounts
 * is correct and protects the ledger from meaningless and sign-flipped rows.
 * A run in which literally every bucket is zero (only reachable with an
 * all-zero-salary roster) therefore produces NO lines at all, and
 * create_draft rejects the empty entry — a run with nothing to post is
 * genuinely not postable, and that is the honest answer.
 *
 * Σcredits is constructed to equal the debit by the arithmetic above (every
 * withheld/accrued tiyn is credited to exactly one of the liability
 * accounts, and the debit is their sum) — JournalService::create_draft's own
 * Σdebit=Σcredit check (backed by migrations/009_journal.sql's deferred
 * balance trigger) is the second line of defense, not the primary one; see
 * PostToJournalBalances in tests/integration/test_payroll_service.cpp for the
 * hard proof.
 */

#pragma once

#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "database/Database.hpp"
#include "hr/EmployeeRepository.hpp"
#include "ledger/JournalService.hpp"
#include "payroll/PayrollCalculator.hpp"
#include "payroll/PayrollRepository.hpp"
#include "payroll/Payslip.hpp"
#include "repositories/RepoErrors.hpp"
#include "tax/TaxReferenceRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"

namespace Payroll {

/// → 409. Thrown when calculate_run()/approve() is asked to act on a run
/// whose current status forbids the requested transition (recalculating an
/// 'approved' run, approving anything but a 'draft' run). Same
/// not-found-vs-wrong-state distinction as Ledger::InvalidEntryState:
/// std::nullopt from a lookup means "no such run visible to this org", this
/// exception means "the run exists but is in the wrong state".
struct InvalidRunState : Repositories::ConflictError {
    explicit InvalidRunState(std::string message)
        : Repositories::ConflictError("invalid_run_state", std::move(message)) {}
};

/// → 422. Thrown when a tax rate or constant `calculate_run()` needs is not in
/// force on the period's effective date (the last calendar day of the period —
/// see the file header). The direct counterpart of Tax::MissingTaxReference,
/// which Api::TaxController already maps to 422 for exactly the same
/// condition on the tax side.
///
/// Why a type of its own rather than the bare std::runtime_error this used to
/// throw (code review, fix round 1): the API layer's generic handler
/// (Api::with_repo_errors) maps any unrecognized std::exception to a 500, so
/// "payroll was run for a period whose rates have not been seeded yet" — a
/// foreseeable operator/reference-data gap, correctable by adding a row to
/// `tax_rates`/`tax_constants` — was being reported to the client as a server
/// bug. Deriving from std::runtime_error (not Repositories::ConflictError)
/// keeps it OUT of the 409 bucket: nothing about the RUN's state is wrong, the
/// reference data behind it is simply absent.
///
/// `kind`/`reference`/`effective_on` are carried as fields, not just baked
/// into what(), so the HTTP layer can name the exact missing row in its 422
/// body — an operator reading the response learns WHICH rate or constant to
/// seed and for WHICH date, without going to the server log.
struct MissingPayrollReference : std::runtime_error {
    MissingPayrollReference(std::string what_kind, std::string name, std::string effective_date)
        // NOTE: the base is initialized BEFORE any member, so the three
        // parameters are still intact here — they are only moved from in the
        // member initializers that follow.
        : std::runtime_error("payroll: no tax " + what_kind + " '" + name + "' effective on " + effective_date)
        , kind(std::move(what_kind))
        , reference(std::move(name))
        , effective_on(std::move(effective_date)) {}

    std::string kind;          ///< "rate" (tax_rates) or "constant" (tax_constants)
    std::string reference;     ///< the missing `tax_rates.kind` / `tax_constants.key`
    std::string effective_on;  ///< the date the lookup was made for, "YYYY-MM-DD"
};

/// → 409. Thrown when an employee's stored `salary_tiyn` exceeds
/// Payroll::kMaxGrossTiyn, the bound above which the withholding arithmetic
/// would overflow `long long` (see that constant). `employees.salary_tiyn` is
/// a plain BIGINT and the API layer accepts up to 16 digits of tenge, so such
/// a row CAN exist — a typo with a few extra zeros, or a hostile request.
///
/// Reported as a CONFLICT (409) rather than letting Payroll::calculate's
/// std::out_of_range escape as a 500: this is a foreseeable data problem, and
/// a 500 tells the payroll clerk nothing. 409 rather than 422 because the
/// offending value is not a field of THIS request at all — the caller asked
/// to calculate a period, and the run cannot proceed while a stored employee
/// record is in a state payroll cannot process. The message names the
/// employee so the clerk knows exactly which card to go fix.
struct GrossSalaryOutOfRange : Repositories::ConflictError {
    explicit GrossSalaryOutOfRange(const std::string& employee_id, long long salary_tiyn)
        : Repositories::ConflictError("gross_salary_out_of_range",
                                      "employee " + employee_id + " has salary_tiyn " + std::to_string(salary_tiyn) +
                                          ", above the maximum " + std::to_string(kMaxGrossTiyn) +
                                          " payroll can calculate — correct the employee card first") {}
};

/// The date (YYYY-MM-DD) of the last calendar day of @p year / @p month —
/// the instant PayrollService::calculate_run resolves rates/constants at
/// (see file header) and the entry_date PayrollService::post_to_journal uses
/// for the resulting journal entry. Plain proleptic-Gregorian leap-year rule
/// (divisible by 4, not by 100 unless also by 400) — correct for every year
/// this system will ever run payroll against.
inline std::string last_day_of_month(int year, int month) {
    static constexpr int kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int day = kDaysInMonth[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        day = 29;
    // Sized for the conversions' theoretical maximum, not the 10 characters
    // a valid date needs — see Tax::TaxCalendar::format_date.
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
    return std::string(buf);
}

/// The date (YYYY-MM-DD) of the FIRST calendar day of @p year / @p month —
/// always day 01, so no leap-year reasoning is involved. Together with
/// last_day_of_month() this bounds the closed interval a payroll run covers,
/// which PayrollService::calculate_run hands to
/// Hr::EmployeeRepository::list_employed_during.
inline std::string first_day_of_month(int year, int month) {
    // Same generous sizing rationale as last_day_of_month above.
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-01", year, month);
    return std::string(buf);
}

class PayrollService {
public:
    /**
     * @brief Calculate (or recalculate) @p org_id's payroll run for
     *        (@p period_year, @p period_month): resolves rates as of the
     *        period's last day, runs Payroll::calculate over every employee
     *        EMPLOYED DURING that period, and persists the run header + one
     *        payslip per employee in a single transaction. See file header
     *        for the exact replace-on-recalculate / reject-on-approved
     *        contract.
     *
     * PRORATION — deliberate decision, Fix round 2 (code review). An employee
     * hired mid-period or dismissed mid-period is INCLUDED and receives a
     * FULL month's pay; the amount is not scaled by the fraction of the month
     * actually worked. Including them is not in question — they really did
     * work part of the period, and excluding them would be a straight
     * underpayment. What is deferred is the scaling.
     *
     * Proration is the accounting-correct answer, and it is deliberately NOT
     * implemented here because it is a materially larger change than it
     * looks, not a division: РК practice prorates a salaried oklad by WORKING
     * days (рабочие дни по производственному календарю), not calendar days,
     * so it needs a production calendar — the republic's holiday list per
     * year, plus the weekend-shift rules — which this system does not have
     * anywhere yet, and which is exactly the sort of reference data that must
     * live in a seeded table rather than in C++. It also interacts with
     * vacations/sick leave (migrations/012_hr.sql already carries a
     * `vacations` table that nothing in payroll reads yet) and with the ОПВ/
     * СО/ОСМС floors, which are monthly-МЗП-based and do not simply scale.
     * Doing a calendar-day approximation now would produce numbers that are
     * confidently wrong rather than obviously incomplete.
     *
     * TODO(payroll-proration): prorate a mid-period hire/dismissal by working
     * days against a seeded РК production calendar (new tax_* reference
     * table + Tax::TaxCalendar lookup), and fold in vacations/sick leave.
     * Until then a mid-period hire or dismissal is OVERPAID relative to days
     * worked and the payroll clerk must adjust that payslip by hand.
     *
     * @throws GrossSalaryOutOfRange if any included employee's stored salary
     *         exceeds Payroll::kMaxGrossTiyn — checked here, before the
     *         transaction opens, so the arithmetic below can never overflow
     *         and the caller gets a 409 naming the employee instead of a 500.
     * @throws MissingPayrollReference if any rate/constant the calculation
     *         needs is not in force on the period's last day (e.g. a period
     *         before migration 011's 2026-01-01 seed) — a reference-data gap
     *         the API layer reports as 422, not 500.
     * @throws InvalidRunState if a run already exists for this period and is
     *         'approved' — checked up front AND as a compare-and-swap on the
     *         UPDATE itself (Fix round 1, code review: the earlier version
     *         only checked up front, before the transaction opened, so a
     *         concurrent approve() landing in between could recalculate an
     *         already-approved run — the same TOCTOU approve() itself avoids
     *         via its own `AND status = 'draft'` CAS).
     */
    PayrollRun calculate_run(const std::string& org_id, int period_year, int period_month) {
        const std::string period_start = first_day_of_month(period_year, period_month);
        const std::string effective_date = last_day_of_month(period_year, period_month);
        const Rates rates = build_rates(org_id, effective_date);
        // The roster AS OF THE PERIOD, not as of today — see the file header
        // and the proration note above.
        const auto employees = employees_.list_employed_during(org_id, period_start, effective_date);
        // Screen every salary before opening the transaction: Payroll::
        // calculate would otherwise throw std::out_of_range mid-loop (a 500),
        // and doing it up front means no partial run is ever rolled back for
        // a reason the caller could have been told about immediately.
        for (const auto& employee : employees)
            if (employee.salary_tiyn < 0 || employee.salary_tiyn > kMaxGrossTiyn)
                throw GrossSalaryOutOfRange(employee.id, employee.salary_tiyn);
        // from_primary=true: this read feeds the CAS below, so it must never
        // observe a lagging replica's stale 'draft' when the primary already
        // committed 'approved' (e.g. this same run, just approved).
        const auto existing = payroll_.find_by_period(org_id, period_year, period_month, /*from_primary=*/true);
        if (existing && existing->status == "approved")
            throw InvalidRunState("cannot recalculate an approved payroll run");

        const nlohmann::json snapshot = rates_snapshot_json(rates, effective_date);

        return Database::get().execute_write([&](auto& txn) -> PayrollRun {
            PayrollRun run;
            if (existing) {
                // CAS: `AND status = 'draft'` closes the TOCTOU window
                // between the pre-transaction check above and this UPDATE —
                // a concurrent approve() that committed in between makes
                // this affect zero rows instead of silently overwriting an
                // approved run's figures (same shape as approve()'s own
                // `AND status = 'draft'` guard below).
                auto rr = txn.exec_params(
                    "UPDATE payroll_runs SET rates_snapshot = $3::jsonb, calculated_at = now() "
                    "WHERE id = $1 AND org_id = $2 AND status = 'draft' RETURNING " +
                        std::string(PayrollRepository::kColumns),
                    existing->id,
                    org_id,
                    snapshot.dump());
                if (rr.empty())
                    throw InvalidRunState("cannot recalculate an approved payroll run");
                run = PayrollRun::from_row(rr[0]);
                // Recalculation replaces every payslip of this run — see
                // file header. Scoped by BOTH run_id and org_id even though
                // run_id alone is already unique, matching every other
                // write in this service's belt-and-braces org scoping.
                txn.exec_params("DELETE FROM payslips WHERE run_id = $1 AND org_id = $2", run.id, org_id);
            } else {
                auto rr = txn.exec_params(
                    "INSERT INTO payroll_runs (org_id, period_year, period_month, rates_snapshot) "
                    "VALUES ($1, $2, $3, $4::jsonb) RETURNING " +
                        std::string(PayrollRepository::kColumns),
                    org_id,
                    period_year,
                    period_month,
                    snapshot.dump());
                run = PayrollRun::from_row(rr[0]);
            }

            run.payslips.reserve(employees.size());
            for (const auto& employee : employees) {
                Input input;
                input.gross_tiyn = employee.salary_tiyn;
                input.ipn_deduction_claimed = employee.ipn_deduction_claimed;
                input.opvr_exempt = employee.opvr_exempt;
                const Result result = calculate(input, rates);

                auto pr = txn.exec_params(
                    "INSERT INTO payslips (org_id, run_id, employee_id, gross_tiyn, opv, vosms, ipn, net, opvr, so, "
                    "osms, social_tax) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12) RETURNING " +
                        std::string(PayrollRepository::kPayslipColumns),
                    org_id,
                    run.id,
                    employee.id,
                    input.gross_tiyn,
                    result.opv,
                    result.vosms,
                    result.ipn,
                    result.net,
                    result.opvr,
                    result.so,
                    result.osms,
                    result.social_tax);
                run.payslips.push_back(Payslip::from_row(pr[0]));
            }
            return run;
        });
    }

    /**
     * @return std::nullopt if no such run is visible to @p org_id.
     * @throws InvalidRunState if the run exists but is not 'draft' (covers
     *         approving an already-approved run).
     */
    std::optional<PayrollRun> approve(const std::string& org_id, const std::string& run_id) {
        auto existing = payroll_.find_in_org(run_id, org_id, /*from_primary=*/true);
        if (!existing)
            return std::nullopt;
        if (existing->status != "draft")
            throw InvalidRunState("cannot approve a payroll run in status '" + existing->status + "'");

        auto updated = Database::get().execute_write([&](auto& txn) -> std::optional<PayrollRun> {
            auto r = txn.exec_params(
                "UPDATE payroll_runs SET status = 'approved' WHERE id = $1 AND org_id = $2 AND status = 'draft' "
                "RETURNING " +
                    std::string(PayrollRepository::kColumns),
                run_id,
                org_id);
            if (r.empty())
                return std::nullopt;
            return PayrollRun::from_row(r[0]);
        });
        if (!updated)
            throw InvalidRunState("run status changed concurrently — no longer 'draft'");

        updated->payslips = payroll_.list_payslips(*updated, /*from_primary=*/true);
        return updated;
    }

    /// The run header for exactly one (org_id, period_year, period_month),
    /// if one has ever been calculated. Header-only — payslips are NOT
    /// populated (mirrors PayrollRepository::find_in_org); use payslips_of()
    /// for those.
    std::optional<PayrollRun> find_run(const std::string& org_id, int period_year, int period_month) {
        return payroll_.find_by_period(org_id, period_year, period_month);
    }

    /// Payslips of the run identified by (@p run_id, @p org_id), or an empty
    /// vector if no such run is visible to @p org_id — same
    /// not-found-is-indistinguishable-from-wrong-org posture as
    /// OrgCrudBase::find_in_org, just expressed as "nothing to show" rather
    /// than std::nullopt since a list has an unambiguous empty state.
    std::vector<Payslip> payslips_of(const std::string& org_id, const std::string& run_id) {
        auto run = payroll_.find_in_org(run_id, org_id);
        if (!run)
            return {};
        return payroll_.list_payslips(*run);
    }

    /**
     * @brief Post @p run_id's total withholdings/accruals to the journal as
     *        one balanced entry (see file header for the exact debit/credit
     *        layout) and post it immediately.
     * @return the new entry's id.
     * @throws Repositories::NotFoundError("payroll_run") if no such run is
     *         visible to @p org_id.
     * @throws InvalidRunState if the run is not 'approved' yet, or has
     *         already been posted (`journal_entry_id` already set) — see
     *         file header for the exact check-then-CAS shape and its
     *         documented residual limitation under true concurrency.
     * @throws std::runtime_error if the run has no payslips yet (nothing to
     *         post — calculate_run() must run first).
     *
     * A bucket whose run-wide total is ZERO produces NO journal line (file
     * header, "EVERY one of those lines ... is omitted"), so the entry may
     * legitimately carry fewer than seven lines — an org on СНР has no 3150
     * line, an org paying near the minimum wage with the standard deduction
     * claimed has no 3120 line, and so on. This is the ordinary case, not a
     * degenerate one.
     */
    std::string post_to_journal(const std::string& org_id, const std::string& run_id, const std::string& user_id) {
        auto run = payroll_.find_in_org(run_id, org_id, /*from_primary=*/true);
        if (!run)
            throw Repositories::NotFoundError("payroll_run");
        if (run->status != "approved")
            throw InvalidRunState("cannot post a payroll run in status '" + run->status + "' — approve it first");
        if (run->journal_entry_id)
            throw InvalidRunState("already_posted");

        const auto payslips = payroll_.list_payslips(*run, /*from_primary=*/true);
        if (payslips.empty())
            throw std::runtime_error("cannot post an empty payroll run to the journal — calculate it first");

        long long gross_total = 0;
        long long opv_total = 0;
        long long vosms_total = 0;
        long long ipn_total = 0;
        long long net_total = 0;
        long long opvr_total = 0;
        long long so_total = 0;
        long long osms_total = 0;
        long long social_tax_total = 0;
        for (const auto& payslip : payslips) {
            gross_total += payslip.gross_tiyn;
            opv_total += payslip.opv;
            vosms_total += payslip.vosms;
            ipn_total += payslip.ipn;
            net_total += payslip.net;
            opvr_total += payslip.opvr;
            so_total += payslip.so;
            osms_total += payslip.osms;
            social_tax_total += payslip.social_tax;
        }

        const long long debit_total = gross_total + opvr_total + so_total + osms_total + social_tax_total;

        // Every line is emitted ONLY when its total is non-zero — see the
        // file header for why (Ledger::parse_tiyn rejects a zero amount, and
        // a zero line is meaningless accounting anyway) and for why dropping
        // one cannot unbalance the entry. Totals are sums of per-payslip
        // fields, each of which the `payslips` CHECK constraints keep >= 0,
        // so "not zero" here is the same test as "positive".
        std::vector<Ledger::JournalLine> lines;
        add_line(lines, "7210", "debit", debit_total);
        add_line(lines, "3350", "credit", net_total);
        add_line(lines, "3120", "credit", ipn_total);
        add_line(lines, "3220", "credit", opv_total + opvr_total);
        add_line(lines, "3210", "credit", so_total);
        add_line(lines, "3230", "credit", osms_total + vosms_total);
        add_line(lines, "3150", "credit", social_tax_total);

        const std::string description = "Начисление заработной платы за " + std::to_string(run->period_month) + "." +
                                        std::to_string(run->period_year);
        const std::string entry_date = last_day_of_month(run->period_year, run->period_month);

        auto entry = journal_.create_draft(org_id, user_id, entry_date, description, std::move(lines));
        journal_.post(org_id, entry.id);

        // CAS: claim this run as posted-to-`entry`, but only if nothing else
        // claimed it first. Zero rows means a concurrent call already set
        // journal_entry_id between the up-front check above and here — see
        // file header for why this is a second (not the only) line of
        // defense, and its documented residual gap under true concurrency.
        auto claimed = Database::get().execute_write([&](auto& txn) -> bool {
            auto r = txn.exec_params(
                "UPDATE payroll_runs SET journal_entry_id = $3, posted_at = now() "
                "WHERE id = $1 AND org_id = $2 AND journal_entry_id IS NULL RETURNING id",
                run_id,
                org_id,
                entry.id);
            return !r.empty();
        });
        if (!claimed)
            throw InvalidRunState("already_posted");

        return entry.id;
    }

private:
    /// Append one journal line for @p amount_tiyn on @p account_code /
    /// @p side — UNLESS the amount is zero, in which case nothing is
    /// appended at all. The single choke point through which every payroll
    /// journal line is built, so the "no zero-amount lines" rule cannot be
    /// forgotten for one bucket the way it was for every line but 3150 before
    /// Fix round 2 (see file header for the rule and its accounting
    /// rationale).
    ///
    /// @p amount_tiyn is never negative in this service — every total is a
    /// sum of `payslips` columns the schema constrains to >= 0 — and
    /// Ledger::format_tiyn throws if one ever were, so no sign handling is
    /// needed here beyond the zero test.
    static void add_line(std::vector<Ledger::JournalLine>& lines,
                         const std::string& account_code,
                         const std::string& side,
                         long long amount_tiyn) {
        if (amount_tiyn == 0)
            return;
        Ledger::JournalLine l;
        l.account_code = account_code;
        l.side = side;
        l.amount = Ledger::format_tiyn(amount_tiyn);
        lines.push_back(std::move(l));
    }

    long long rate_bp(const std::string& kind, const std::string& date) {
        auto rate = tax_.rate_on(kind, date, "");
        if (!rate)
            throw MissingPayrollReference("rate", kind, date);
        return rate->rate_bp;
    }

    long long constant_tiyn(const std::string& key, const std::string& date) {
        auto constant = tax_.constant_on(key, date);
        if (!constant)
            throw MissingPayrollReference("constant", key, date);
        return constant->value_tiyn;
    }

    long long constant_units(const std::string& key, const std::string& date) {
        auto constant = tax_.constant_on(key, date);
        if (!constant)
            throw MissingPayrollReference("constant", key, date);
        return constant->value_units.value_or(0);
    }

    /// Resolves every Rates field off Tax::TaxReferenceRepository as of
    /// @p effective_date, plus `social_tax_applies` off the organization's
    /// own tax_regime (migrations/006_organizations.sql):
    /// 'snr_simplified' orgs never owe social tax (see
    /// PayrollCalculator.hpp step 8) — an org that can't be found falls back
    /// to `true` (Rates' own default), the same as any other regime.
    Rates build_rates(const std::string& org_id, const std::string& effective_date) {
        Rates r;
        r.ipn_bp = rate_bp(Tax::RateKind::kIpn, effective_date);
        r.opv_bp = rate_bp(Tax::RateKind::kOpv, effective_date);
        r.opvr_bp = rate_bp(Tax::RateKind::kOpvr, effective_date);
        r.so_bp = rate_bp(Tax::RateKind::kSo, effective_date);
        r.osms_bp = rate_bp(Tax::RateKind::kOsms, effective_date);
        r.vosms_bp = rate_bp(Tax::RateKind::kVosms, effective_date);
        r.social_tax_bp = rate_bp(Tax::RateKind::kSocialTax, effective_date);

        r.mrp_tiyn = constant_tiyn("mrp", effective_date);
        r.mzp_tiyn = constant_tiyn("mzp", effective_date);
        r.ipn_deduction_mrp = constant_units("ipn_deduction_mrp", effective_date);
        r.opv_base_max_mzp = constant_units("opv_base_max_mzp", effective_date);
        r.so_base_min_mzp = constant_units("so_base_min_mzp", effective_date);
        r.so_base_max_mzp = constant_units("so_base_max_mzp", effective_date);
        r.vosms_base_max_mzp = constant_units("vosms_base_max_mzp", effective_date);
        r.osms_base_max_mzp = constant_units("osms_base_max_mzp", effective_date);

        auto org = orgs_.find(org_id);
        r.social_tax_applies = !(org && org->tax_regime == "snr_simplified");
        return r;
    }

    /// Every resolved rate/constant, keyed exactly like Rates' own field
    /// names, plus `effective_date` — the reproducibility record
    /// PayrollRun::rates_snapshot exists for (see file header).
    static nlohmann::json rates_snapshot_json(const Rates& r, const std::string& effective_date) {
        return nlohmann::json{
            {"effective_date", effective_date},
            {"ipn_bp", r.ipn_bp},
            {"opv_bp", r.opv_bp},
            {"opvr_bp", r.opvr_bp},
            {"so_bp", r.so_bp},
            {"osms_bp", r.osms_bp},
            {"vosms_bp", r.vosms_bp},
            {"social_tax_bp", r.social_tax_bp},
            {"mrp_tiyn", r.mrp_tiyn},
            {"mzp_tiyn", r.mzp_tiyn},
            {"ipn_deduction_mrp", r.ipn_deduction_mrp},
            {"opv_base_max_mzp", r.opv_base_max_mzp},
            {"so_base_min_mzp", r.so_base_min_mzp},
            {"so_base_max_mzp", r.so_base_max_mzp},
            {"vosms_base_max_mzp", r.vosms_base_max_mzp},
            {"osms_base_max_mzp", r.osms_base_max_mzp},
            {"social_tax_applies", r.social_tax_applies},
        };
    }

    PayrollRepository payroll_;
    Hr::EmployeeRepository employees_;
    Tax::TaxReferenceRepository tax_;
    Tenancy::OrganizationRepository orgs_;
    Ledger::JournalService journal_;
};

}  // namespace Payroll
