/**
 * @file test_hr.cpp
 * @brief Integration tests for Hr::EmployeeRepository / Hr::HrRepository
 *        against a real Postgres (migration 012). Exercises employee
 *        create/find, the typed 409 on a duplicate (org_id, iin),
 *        dismiss()'s status+dismissed_on transition, list_active()
 *        excluding dismissed employees, the JSONB round-trip on hr_orders'
 *        payload, vacations.days persistence, and that employees/orders are
 *        isolated per organization the same way every other org-scoped
 *        table in this codebase is.
 */

#include <algorithm>
#include <optional>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "hr/Employee.hpp"
#include "hr/EmployeeRepository.hpp"
#include "hr/HrDocuments.hpp"
#include "hr/HrRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

class HrRepoTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // Centralized org-data wipe (TestHelpers::wipe_org_data(),
        // test_helpers.hpp). employees/labor_contracts/hr_orders/vacations
        // (migration 012) carry NO row-level blocking trigger of their own
        // — unlike journal_entries/journal_lines' immutability guards — so
        // the plain `DELETE FROM organizations` this helper already runs is
        // enough on its own: employees.org_id cascades on organization
        // delete, and labor_contracts/hr_orders/vacations each cascade
        // further off employees via their own composite
        // (employee_id, org_id) -> employees(id, org_id) ON DELETE CASCADE
        // FK. The one wrinkle — hr_orders.document_id is a plain (non-CASCADE)
        // FK onto documents(id) — is also already handled: this helper's
        // `TRUNCATE TABLE ..., documents CASCADE` step, which runs BEFORE
        // the organizations DELETE, additionally cascade-truncates hr_orders
        // (Postgres TRUNCATE ... CASCADE truncates every table with an FK
        // onto a truncated table, not just matching rows) — harmless here,
        // since that leaves hr_orders empty before the DELETE FROM
        // organizations step even runs. No changes to wipe_org_data() were
        // needed for this table set.
        TestHelpers::wipe_org_data();
    }

    std::string make_org(const std::string& bin) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "HR Test Org " + bin, "snr_simplified", false).id;
    }

    Hr::Employee make_draft_employee(const std::string& iin, const std::string& last_name = "Ivanov") {
        Hr::Employee draft;
        draft.iin = iin;
        draft.last_name = last_name;
        draft.first_name = "Aidos";
        draft.position = "Accountant";
        draft.salary_tiyn = 30000000;  // 300,000 tenge
        draft.hired_on = "2026-01-10";
        draft.payout_iik = "KZ000000000000000000";
        return draft;
    }
};

TEST_F(HrRepoTest, CreateFindEmployee) {
    Hr::EmployeeRepository repo;
    auto org_id = make_org("111270000101");

    auto draft = make_draft_employee("111122333344");
    draft.middle_name = "Bekovich";
    draft.ipn_deduction_claimed = true;

    auto emp = repo.create(org_id, draft);

    EXPECT_FALSE(emp.id.empty());
    EXPECT_EQ(emp.org_id, org_id);
    EXPECT_EQ(emp.iin, "111122333344");
    EXPECT_EQ(emp.last_name, "Ivanov");
    EXPECT_EQ(emp.first_name, "Aidos");
    ASSERT_TRUE(emp.middle_name);
    EXPECT_EQ(*emp.middle_name, "Bekovich");
    EXPECT_EQ(emp.position, "Accountant");
    EXPECT_EQ(emp.salary_tiyn, 30000000);
    EXPECT_EQ(emp.hired_on, "2026-01-10");
    EXPECT_FALSE(emp.dismissed_on);
    EXPECT_TRUE(emp.ipn_deduction_claimed);
    EXPECT_FALSE(emp.opvr_exempt);
    EXPECT_EQ(emp.status, "active");

    auto found = repo.find_in_org(emp.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->iin, "111122333344");
    EXPECT_EQ(found->status, "active");
}

TEST_F(HrRepoTest, DuplicateIinRejected) {
    Hr::EmployeeRepository repo;
    auto org_id = make_org("111270000102");

    repo.create(org_id, make_draft_employee("222233344455", "Petrov"));

    EXPECT_THROW({ repo.create(org_id, make_draft_employee("222233344455", "Sidorov")); }, Hr::DuplicateEmployeeIin);
}

TEST_F(HrRepoTest, DismissSetsStatusAndDate) {
    Hr::EmployeeRepository repo;
    auto org_id = make_org("111270000103");
    auto other_org_id = make_org("111270000104");

    auto emp = repo.create(org_id, make_draft_employee("333344455566"));
    ASSERT_EQ(emp.status, "active");

    // Cross-org dismiss is rejected the same way OrgCrudBase writes are: no
    // matching (id, org_id, status='active') row, so nullopt rather than a
    // 403/exception.
    EXPECT_FALSE(repo.dismiss(other_org_id, emp.id, "2026-06-30"));

    auto dismissed = repo.dismiss(org_id, emp.id, "2026-06-30");
    ASSERT_TRUE(dismissed);
    EXPECT_EQ(dismissed->status, "dismissed");
    ASSERT_TRUE(dismissed->dismissed_on);
    EXPECT_EQ(*dismissed->dismissed_on, "2026-06-30");

    // Re-dismissing an already-dismissed employee is a no-op (status filter
    // in the WHERE clause), not an error.
    EXPECT_FALSE(repo.dismiss(org_id, emp.id, "2026-07-01"));

    auto found = repo.find_in_org(emp.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->status, "dismissed");
    ASSERT_TRUE(found->dismissed_on);
    EXPECT_EQ(*found->dismissed_on, "2026-06-30");
}

TEST_F(HrRepoTest, ListActiveExcludesDismissed) {
    Hr::EmployeeRepository repo;
    auto org_id = make_org("111270000105");

    auto active_one = repo.create(org_id, make_draft_employee("444455566677", "Active1"));
    auto active_two = repo.create(org_id, make_draft_employee("444455566688", "Active2"));
    auto to_dismiss = repo.create(org_id, make_draft_employee("444455566699", "Dismissed"));

    ASSERT_TRUE(repo.dismiss(org_id, to_dismiss.id, "2026-03-01"));

    auto active = repo.list_active(org_id);
    ASSERT_EQ(active.size(), 2u);
    std::vector<std::string> ids;
    for (const auto& e : active)
        ids.push_back(e.id);
    EXPECT_NE(std::find(ids.begin(), ids.end(), active_one.id), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), active_two.id), ids.end());
    EXPECT_EQ(std::find(ids.begin(), ids.end(), to_dismiss.id), ids.end());

    // count_in_org (OrgCrudBase) still reports all three, dismissed included
    // — list_active() is the narrowed view, not a replacement for the base.
    EXPECT_EQ(repo.count_in_org(org_id), 3);
}

TEST_F(HrRepoTest, OrderWithPayloadRoundTrips) {
    Hr::EmployeeRepository employees;
    Hr::HrRepository hr;
    auto org_id = make_org("111270000106");

    auto emp = employees.create(org_id, make_draft_employee("555566677788"));

    nlohmann::json payload = {{"new_salary_tiyn", 35000000}, {"reason", "annual review"}};

    auto order = hr.create_order(org_id,
                                 emp.id,
                                 "salary_change",
                                 "ORD-0001",
                                 "2026-02-01",
                                 "2026-02-01",
                                 /*effective_to=*/std::nullopt,
                                 payload);

    EXPECT_FALSE(order.id.empty());
    EXPECT_EQ(order.org_id, org_id);
    EXPECT_EQ(order.employee_id, emp.id);
    EXPECT_EQ(order.kind, "salary_change");
    EXPECT_EQ(order.number, "ORD-0001");
    EXPECT_FALSE(order.effective_to);
    EXPECT_FALSE(order.document_id);
    ASSERT_TRUE(order.payload);
    EXPECT_EQ(*order.payload, payload);

    // Persisted — a fresh primary read confirms the same shape, including
    // the JSONB round-trip.
    auto found = hr.find_in_org(order.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(found);
    ASSERT_TRUE(found->payload);
    EXPECT_EQ(*found->payload, payload);

    auto listed = hr.list_orders(org_id, emp.id);
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0].id, order.id);

    // Orders scoped to a different (nonexistent-for-this-org) employee id
    // come back empty rather than matching everything.
    auto other_org_id = make_org("111270000107");
    auto other_emp = employees.create(other_org_id, make_draft_employee("555566677799"));
    EXPECT_TRUE(hr.list_orders(org_id, other_emp.id).empty());
}

TEST_F(HrRepoTest, VacationDaysStored) {
    Hr::EmployeeRepository employees;
    Hr::HrRepository hr;
    auto org_id = make_org("111270000108");

    auto emp = employees.create(org_id, make_draft_employee("666677788899"));

    auto vac = hr.create_vacation(org_id, emp.id, "2026-07-01", "2026-07-15", 15, "annual");

    EXPECT_FALSE(vac.id.empty());
    EXPECT_EQ(vac.employee_id, emp.id);
    EXPECT_EQ(vac.starts_on, "2026-07-01");
    EXPECT_EQ(vac.ends_on, "2026-07-15");
    EXPECT_EQ(vac.days, 15);
    EXPECT_EQ(vac.kind, "annual");

    auto listed = hr.list_vacations(org_id, emp.id);
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0].days, 15);

    auto listed_all = hr.list_vacations(org_id);
    ASSERT_EQ(listed_all.size(), 1u);
    EXPECT_EQ(listed_all[0].id, vac.id);
}

TEST_F(HrRepoTest, CrossOrgIsolated) {
    Hr::EmployeeRepository employees;
    Hr::HrRepository hr;
    auto org_a = make_org("111270000109");
    auto org_b = make_org("111270000110");

    auto emp_a = employees.create(org_a, make_draft_employee("777788899900"));
    hr.create_order(org_a, emp_a.id, "hire", "ORD-A-0001", "2026-01-10", "2026-01-10");
    hr.create_contract(org_a, emp_a.id, "CNT-A-0001", "2026-01-09", "2026-01-10");
    hr.create_vacation(org_a, emp_a.id, "2026-05-01", "2026-05-05", 5, "annual");

    // Reads scoped to org_b never surface org_a's employee or its documents.
    EXPECT_FALSE(employees.find_in_org(emp_a.id, org_b, /*from_primary=*/true));
    EXPECT_TRUE(employees.list_active(org_b).empty());
    EXPECT_EQ(employees.count_in_org(org_b), 0);

    EXPECT_TRUE(hr.list_orders(org_b, emp_a.id).empty());
    EXPECT_TRUE(hr.list_orders(org_b).empty());
    EXPECT_TRUE(hr.list_contracts(org_b, emp_a.id).empty());
    EXPECT_TRUE(hr.list_vacations(org_b, emp_a.id).empty());
    EXPECT_TRUE(hr.list_vacations(org_b).empty());

    // A cross-org employee_id/org_id pair trips the composite FK
    // (employee_id, org_id) -> employees(id, org_id) at the SQL level —
    // migrations/012_hr.sql's isolation guarantee, not application code
    // (foreign_key_violation, SQLSTATE 23503 — same shape as
    // test_journal_schema.cpp's CrossOrgLineRejectedByCompositeFk).
    EXPECT_THROW({ hr.create_order(org_b, emp_a.id, "hire", "ORD-B-0001", "2026-01-10", "2026-01-10"); },
                 pqxx::sql_error);

    // org_a's own view is unaffected by any of the above.
    EXPECT_EQ(employees.count_in_org(org_a), 1);
    EXPECT_EQ(hr.list_orders(org_a).size(), 1u);
    EXPECT_EQ(hr.list_contracts(org_a, emp_a.id).size(), 1u);
    EXPECT_EQ(hr.list_vacations(org_a).size(), 1u);
}

}  // namespace
