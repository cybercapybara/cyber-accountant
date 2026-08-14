/**
 * @file test_payroll_service.cpp
 * @brief Integration tests for Payroll::PayrollService against a real
 *        Postgres (migration 013) — calculate_run's per-active-employee
 *        payslip generation and draft-recalculation-replaces contract,
 *        approve()'s draft->approved transition and its 409 on a repeat
 *        calculate_run, the rates_snapshot reproducibility record,
 *        post_to_journal's balanced journal entry, list_active's dismissed
 *        exclusion flowing through to payroll, and cross-org isolation.
 */

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "hr/Employee.hpp"
#include "hr/EmployeeRepository.hpp"
#include "ledger/JournalRepository.hpp"
#include "ledger/JournalService.hpp"
#include "payroll/PayrollRepository.hpp"
#include "payroll/PayrollService.hpp"
#include "payroll/Payslip.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

class PayrollServiceTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // Centralized org-data wipe (TestHelpers::wipe_org_data(),
        // test_helpers.hpp): payroll_runs/payslips (migration 013) carry no
        // row-level blocking trigger of their own, so the plain `DELETE FROM
        // organizations` this helper already runs is enough — payroll_runs
        // cascades off organizations, and payslips cascades further off both
        // payroll_runs and employees via their composite FKs. Same posture
        // as test_hr.cpp's fixture note.
        TestHelpers::wipe_org_data();
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
    }

    /// Create a tenant and return its id. @p tax_regime defaults to
    /// 'snr_simplified' (Hr/Journal test fixtures' own default); pass
    /// 'standard' for tests that need Payroll::Rates::social_tax_applies to
    /// come back true (see PayrollService::build_rates).
    std::string make_org(const std::string& bin, const std::string& tax_regime = "snr_simplified") {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "Payroll Test Org " + bin, tax_regime, false).id;
    }

    /// Confirmed "User"-role user — payroll journal entries' created_by_user_id
    /// is a (nullable) FK to users(id), same rationale as
    /// test_journal_service.cpp's own seed_user().
    std::string seed_user(const std::string& email) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name("User");
        if (!role) {
            ADD_FAILURE() << "role 'User' missing — seed migration?";
            throw std::runtime_error("seed role missing: User");
        }
        auto created = users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
        return created.id;
    }

    static Hr::Employee make_draft_employee(const std::string& iin,
                                            long long salary_tiyn,
                                            const std::string& last_name = "Ivanov") {
        Hr::Employee draft;
        draft.iin = iin;
        draft.last_name = last_name;
        draft.first_name = "Aidos";
        draft.position = "Accountant";
        draft.salary_tiyn = salary_tiyn;
        draft.hired_on = "2026-01-10";
        draft.payout_iik = "KZ000000000000000000";
        return draft;
    }
};

TEST_F(PayrollServiceTest, CalculateRunProducesPayslipPerActiveEmployee) {
    Payroll::PayrollService svc;
    Hr::EmployeeRepository employees;
    auto org_id = make_org("111280000101");

    auto emp1 = employees.create(org_id, make_draft_employee("111122334401", 30000000, "First"));
    auto emp2 = employees.create(org_id, make_draft_employee("111122334402", 45000000, "Second"));

    auto run = svc.calculate_run(org_id, 2026, 3);

    EXPECT_FALSE(run.id.empty());
    EXPECT_EQ(run.org_id, org_id);
    EXPECT_EQ(run.period_year, 2026);
    EXPECT_EQ(run.period_month, 3);
    EXPECT_EQ(run.status, "draft");
    ASSERT_EQ(run.payslips.size(), 2u);

    std::vector<std::string> employee_ids;
    for (const auto& p : run.payslips) {
        EXPECT_EQ(p.org_id, org_id);
        EXPECT_EQ(p.run_id, run.id);
        employee_ids.push_back(p.employee_id);
        // Every field is non-negative and net = gross - opv - vosms - ipn.
        EXPECT_GE(p.gross_tiyn, 0);
        EXPECT_EQ(p.net, p.gross_tiyn - p.opv - p.vosms - p.ipn);
    }
    EXPECT_NE(std::find(employee_ids.begin(), employee_ids.end(), emp1.id), employee_ids.end());
    EXPECT_NE(std::find(employee_ids.begin(), employee_ids.end(), emp2.id), employee_ids.end());

    // Persisted — a fresh repository read confirms the same shape.
    Payroll::PayrollRepository repo;
    auto found = repo.find_in_org(run.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(found);
    EXPECT_EQ(repo.list_payslips(*found, /*from_primary=*/true).size(), 2u);
}

TEST_F(PayrollServiceTest, DismissedEmployeeExcluded) {
    Payroll::PayrollService svc;
    Hr::EmployeeRepository employees;
    auto org_id = make_org("111280000102");

    auto active = employees.create(org_id, make_draft_employee("111122334403", 30000000, "Active"));
    auto dismissed = employees.create(org_id, make_draft_employee("111122334404", 30000000, "Dismissed"));
    ASSERT_TRUE(employees.dismiss(org_id, dismissed.id, "2026-02-01"));

    auto run = svc.calculate_run(org_id, 2026, 3);

    ASSERT_EQ(run.payslips.size(), 1u);
    EXPECT_EQ(run.payslips[0].employee_id, active.id);
}

TEST_F(PayrollServiceTest, RecalculateDraftReplacesPayslips) {
    Payroll::PayrollService svc;
    Hr::EmployeeRepository employees;
    Payroll::PayrollRepository repo;
    auto org_id = make_org("111280000103");

    auto emp1 = employees.create(org_id, make_draft_employee("111122334405", 30000000, "First"));

    auto run1 = svc.calculate_run(org_id, 2026, 4);
    ASSERT_EQ(run1.payslips.size(), 1u);
    const std::string original_payslip_id = run1.payslips[0].id;

    // A second employee joins before the run is approved — recalculating
    // must pick it up AND replace (not duplicate) the first employee's
    // payslip.
    auto emp2 = employees.create(org_id, make_draft_employee("111122334406", 40000000, "Second"));

    auto run2 = svc.calculate_run(org_id, 2026, 4);

    EXPECT_EQ(run2.id, run1.id);  // same row, not a second run for the period
    ASSERT_EQ(run2.payslips.size(), 2u);

    // The DB itself holds exactly 2 payslips for this run — no leftover row
    // from the first calculation.
    auto found = repo.find_in_org(run1.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(found);
    auto persisted = repo.list_payslips(*found, /*from_primary=*/true);
    ASSERT_EQ(persisted.size(), 2u);
    for (const auto& p : persisted)
        EXPECT_NE(p.id, original_payslip_id);  // replaced, not the same row
}

TEST_F(PayrollServiceTest, ApproveBlocksRecalculation) {
    Payroll::PayrollService svc;
    Hr::EmployeeRepository employees;
    auto org_id = make_org("111280000104");
    employees.create(org_id, make_draft_employee("111122334407", 30000000));

    auto run = svc.calculate_run(org_id, 2026, 5);
    ASSERT_EQ(run.status, "draft");

    auto approved = svc.approve(org_id, run.id);
    ASSERT_TRUE(approved);
    EXPECT_EQ(approved->id, run.id);
    EXPECT_EQ(approved->status, "approved");
    EXPECT_EQ(approved->payslips.size(), 1u);

    // Re-approving an already-approved run is rejected the same way.
    EXPECT_THROW(svc.approve(org_id, run.id), Payroll::InvalidRunState);

    // Recalculating an approved run is rejected — 409-shaped.
    EXPECT_THROW(svc.calculate_run(org_id, 2026, 5), Payroll::InvalidRunState);

    Payroll::PayrollRepository repo;
    auto reloaded = repo.find_in_org(run.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(reloaded->status, "approved");
}

TEST_F(PayrollServiceTest, RatesSnapshotStored) {
    Payroll::PayrollService svc;
    Hr::EmployeeRepository employees;
    auto org_id = make_org("111280000105", "standard");
    employees.create(org_id, make_draft_employee("111122334408", 30000000));

    auto run = svc.calculate_run(org_id, 2026, 6);

    for (const char* key : {"ipn_bp",
                            "opv_bp",
                            "opvr_bp",
                            "so_bp",
                            "osms_bp",
                            "vosms_bp",
                            "social_tax_bp",
                            "mrp_tiyn",
                            "mzp_tiyn",
                            "ipn_deduction_mrp",
                            "opv_base_max_mzp",
                            "so_base_min_mzp",
                            "so_base_max_mzp",
                            "vosms_base_max_mzp",
                            "osms_base_max_mzp",
                            "social_tax_applies",
                            "effective_date"})
        EXPECT_TRUE(run.rates_snapshot.contains(key)) << key;

    EXPECT_EQ(run.rates_snapshot["ipn_bp"], 1000);
    EXPECT_EQ(run.rates_snapshot["opv_bp"], 1000);
    EXPECT_EQ(run.rates_snapshot["social_tax_applies"], true);      // 'standard' regime
    EXPECT_EQ(run.rates_snapshot["effective_date"], "2026-06-30");  // last day of period

    // Persisted — a fresh header read carries the same snapshot.
    Payroll::PayrollRepository repo;
    auto found = repo.find_in_org(run.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->rates_snapshot, run.rates_snapshot);
}

TEST_F(PayrollServiceTest, PostToJournalBalances) {
    Payroll::PayrollService svc;
    Hr::EmployeeRepository employees;
    auto org_id = make_org("111280000106", "standard");  // social_tax_applies=true
    auto user_id = seed_user("payroll-post@example.com");

    // Golden salary: 500,000 ₸ = 50,000,000 tiyn, no deduction claimed, not
    // ОПВР-exempt — chosen so every withholding bucket below is non-zero
    // (JournalService::parse_tiyn rejects a zero-amount line), and hand
    // verified against the 2026 seed (migrations/011_tax_reference.sql):
    //   opv    = 5,000,000 tiyn (10% of 50,000,000)
    //   vosms  = 1,000,000 tiyn (2% of 50,000,000)
    //   ipn    = 4,400,000 tiyn (10% of 50,000,000 - opv - vosms = 44,000,000)
    //   net    = 39,600,000 tiyn (50,000,000 - opv - vosms - ipn)
    //   so     = 2,250,000 tiyn (5% of gross - opv = 45,000,000, within the
    //            [1 МЗП, 7 МЗП] corridor so no clamping applies)
    //   osms   = 1,500,000 tiyn (3% of 50,000,000)
    //   opvr   = 1,750,000 tiyn (3.5% of 50,000,000)
    //   social_tax = 2,640,000 tiyn (6% of 44,000,000, 'standard' regime)
    employees.create(org_id, make_draft_employee("111122334409", 50000000));

    auto run = svc.calculate_run(org_id, 2026, 7);
    ASSERT_EQ(run.payslips.size(), 1u);
    const auto& p = run.payslips[0];
    ASSERT_EQ(p.opv, 5000000);
    ASSERT_EQ(p.vosms, 1000000);
    ASSERT_EQ(p.ipn, 4400000);
    ASSERT_EQ(p.net, 39600000);
    ASSERT_EQ(p.so, 2250000);
    ASSERT_EQ(p.osms, 1500000);
    ASSERT_EQ(p.opvr, 1750000);
    ASSERT_EQ(p.social_tax, 2640000);

    // post_to_journal now requires 'approved' (Fix round 1) — a still-draft
    // run's figures could be silently invalidated by a later recalculation.
    ASSERT_TRUE(svc.approve(org_id, run.id));

    auto entry_id = svc.post_to_journal(org_id, run.id, user_id);
    EXPECT_FALSE(entry_id.empty());

    Ledger::JournalRepository journal;
    auto entry = journal.find_in_org(entry_id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->status, "posted");
    EXPECT_EQ(entry->entry_date, "2026-07-31");  // last day of the period

    auto lines = journal.load_lines(*entry, /*from_primary=*/true);
    // debit 7210 + 6 non-zero credits: 3350 net, 3120 ИПН, 3220 ОПВ+ОПВР,
    // 3210 СО, 3230 ОСМС+ВОСМС, and 3150 социальный налог (standard regime,
    // so it is non-zero and posted).
    ASSERT_EQ(lines.size(), 7u);

    long long debit_total = 0;
    long long credit_total = 0;
    std::string debit_7210, credit_3350, credit_3120, credit_3220, credit_3210, credit_3230, credit_3150;
    for (const auto& l : lines) {
        const long long amount_tiyn = Ledger::parse_tiyn(l.amount);
        if (l.side == "debit") {
            debit_total += amount_tiyn;
            EXPECT_EQ(l.account_code, "7210");
            debit_7210 = l.amount;
        } else {
            credit_total += amount_tiyn;
            if (l.account_code == "3350")
                credit_3350 = l.amount;
            else if (l.account_code == "3120")
                credit_3120 = l.amount;
            else if (l.account_code == "3220")
                credit_3220 = l.amount;
            else if (l.account_code == "3210")
                credit_3210 = l.amount;
            else if (l.account_code == "3230")
                credit_3230 = l.amount;
            else if (l.account_code == "3150")
                credit_3150 = l.amount;
            else
                ADD_FAILURE() << "unexpected credit account " << l.account_code;
        }
    }

    // The invariant the balance trigger (migrations/009_journal.sql) enforces
    // — proven here at the value level, not just "it didn't throw".
    EXPECT_EQ(debit_total, credit_total);
    EXPECT_EQ(debit_total, 58140000);  // gross + opvr + so + osms + social_tax

    EXPECT_EQ(debit_7210, "581400.00");
    EXPECT_EQ(credit_3350, "396000.00");  // net
    EXPECT_EQ(credit_3120, "44000.00");   // ipn
    EXPECT_EQ(credit_3220, "67500.00");   // opv + opvr = 5,000,000 + 1,750,000
    EXPECT_EQ(credit_3210, "22500.00");   // so
    EXPECT_EQ(credit_3230, "25000.00");   // osms + vosms = 1,500,000 + 1,000,000
    EXPECT_EQ(credit_3150, "26400.00");   // social_tax

    // The run itself now records what it was posted to.
    Payroll::PayrollRepository repo;
    auto reloaded = repo.find_in_org(run.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(reloaded);
    ASSERT_TRUE(reloaded->journal_entry_id);
    EXPECT_EQ(*reloaded->journal_entry_id, entry_id);
    EXPECT_TRUE(reloaded->posted_at);
}

TEST_F(PayrollServiceTest, PostToJournalRequiresApproved) {
    Payroll::PayrollService svc;
    Hr::EmployeeRepository employees;
    auto org_id = make_org("111280000110", "standard");
    auto user_id = seed_user("payroll-post-draft@example.com");
    employees.create(org_id, make_draft_employee("111122334412", 50000000));

    auto run = svc.calculate_run(org_id, 2026, 7);
    ASSERT_EQ(run.status, "draft");

    EXPECT_THROW(svc.post_to_journal(org_id, run.id, user_id), Payroll::InvalidRunState);

    // Nothing was posted — no entry, no journal_entry_id.
    Ledger::JournalRepository journal;
    EXPECT_EQ(journal.count_in_org(org_id), 0);
    Payroll::PayrollRepository repo;
    auto reloaded = repo.find_in_org(run.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(reloaded);
    EXPECT_FALSE(reloaded->journal_entry_id);
}

TEST_F(PayrollServiceTest, PostToJournalTwiceRejected) {
    Payroll::PayrollService svc;
    Hr::EmployeeRepository employees;
    auto org_id = make_org("111280000111", "standard");
    auto user_id = seed_user("payroll-post-twice@example.com");
    employees.create(org_id, make_draft_employee("111122334413", 50000000));

    auto run = svc.calculate_run(org_id, 2026, 7);
    ASSERT_TRUE(svc.approve(org_id, run.id));

    auto first_entry_id = svc.post_to_journal(org_id, run.id, user_id);
    EXPECT_FALSE(first_entry_id.empty());

    // A second call — whether a caller retry or a genuine bug — must NOT
    // create a second, duplicate journal entry (the exact financial error
    // Fix round 1 closes).
    EXPECT_THROW(svc.post_to_journal(org_id, run.id, user_id), Payroll::InvalidRunState);

    Ledger::JournalRepository journal;
    EXPECT_EQ(journal.count_in_org(org_id), 1);  // exactly one entry, not two

    Payroll::PayrollRepository repo;
    auto reloaded = repo.find_in_org(run.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(reloaded);
    ASSERT_TRUE(reloaded->journal_entry_id);
    EXPECT_EQ(*reloaded->journal_entry_id, first_entry_id);  // still points at the first (only) entry
}

TEST_F(PayrollServiceTest, PostToJournalOmitsZeroSocialTaxLineOnSnrSimplified) {
    Payroll::PayrollService svc;
    Hr::EmployeeRepository employees;
    auto org_id = make_org("111280000107", "snr_simplified");  // social_tax_applies=false
    auto user_id = seed_user("payroll-post-snr@example.com");
    employees.create(org_id, make_draft_employee("111122334410", 50000000));

    auto run = svc.calculate_run(org_id, 2026, 7);
    ASSERT_EQ(run.payslips.size(), 1u);
    EXPECT_EQ(run.payslips[0].social_tax, 0);
    ASSERT_TRUE(svc.approve(org_id, run.id));

    auto entry_id = svc.post_to_journal(org_id, run.id, user_id);

    Ledger::JournalRepository journal;
    auto entry = journal.find_in_org(entry_id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(entry);
    auto lines = journal.load_lines(*entry, /*from_primary=*/true);
    // debit 7210 + 5 credits; no 3150 line, social_tax is zero under СНР
    ASSERT_EQ(lines.size(), 6u);
    for (const auto& l : lines)
        EXPECT_NE(l.account_code, "3150");
}

TEST_F(PayrollServiceTest, CrossOrgIsolated) {
    Payroll::PayrollService svc;
    Hr::EmployeeRepository employees;
    auto org_a = make_org("111280000108");
    auto org_b = make_org("111280000109");
    auto user_id = seed_user("payroll-cross@example.com");

    employees.create(org_a, make_draft_employee("111122334411", 30000000));
    auto run = svc.calculate_run(org_a, 2026, 8);

    // org_b never sees org_a's run, its payslips, nor can it approve or post it.
    EXPECT_FALSE(svc.find_run(org_b, 2026, 8));
    EXPECT_TRUE(svc.payslips_of(org_b, run.id).empty());
    EXPECT_FALSE(svc.approve(org_b, run.id));
    EXPECT_THROW(svc.post_to_journal(org_b, run.id, user_id), Repositories::NotFoundError);

    // org_a's own view is unaffected.
    EXPECT_TRUE(svc.find_run(org_a, 2026, 8).has_value());
    EXPECT_EQ(svc.payslips_of(org_a, run.id).size(), 1u);
}

}  // namespace
