/**
 * @file test_payroll_api.cpp
 * @brief Integration tests for Api::PayrollController (Task 12) — the HTTP
 *        surface over Payroll::PayrollService.
 *
 * Follows the direct-controller-invocation idiom of test_hr_api.cpp /
 * test_employees_api.cpp: handlers are called on a real Postgres with a real
 * org context, never through a live HTTP server. The generate-document test
 * asserts a docgen.render job landed on the queue (same idiom as
 * test_docgen_api.cpp) — never that a PDF was produced.
 *
 * Coverage per the task brief: happy path, viewer-403, cross-org-404 and 422
 * per route, plus the named `PostToJournalReturnsBalancedEntry`, which
 * independently re-sums the produced journal entry's lines rather than
 * trusting the service's own balance check.
 */

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/PayrollController.hpp"
#include "cache/Cache.hpp"
#include "database/Database.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "hr/Employee.hpp"
#include "hr/EmployeeRepository.hpp"
#include "jobs/Jobs.hpp"
#include "ledger/DocumentRepository.hpp"
#include "ledger/JournalEntry.hpp"
#include "ledger/JournalRepository.hpp"
#include "ledger/JournalService.hpp"
#include "money/AmountInWords.hpp"
#include "money/MoneyFormat.hpp"
#include "payroll/PayrollRepository.hpp"
#include "payroll/PayrollService.hpp"
#include "repo_templates.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;
namespace fs = std::filesystem;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-payroll-api-pad";
constexpr const char* kRenderJobType = "docgen.render";

class PayrollApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::PayrollController ctrl;

    std::string config_file_name() const override { return "payroll_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["jobs"]["enabled"] = true;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        REQUIRE_REPO_TEMPLATE("payslip");

        TestHelpers::wipe_org_data();
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
        drain_queue();
    }

    void TearDown() override {
        if (!::testing::Test::IsSkipped() && Cache::is_initialized())
            drain_queue();
        TestHelpers::CoreBackedTest::TearDown();
    }

    static void drain_queue() { TestHelpers::drain_jobs({kRenderJobType}); }

    /// 'standard' (not 'snr_simplified') so Payroll::Rates::social_tax_applies
    /// comes back true and the 3150 credit line actually appears — the
    /// balance assertion is then exercising every line of the entry, not a
    /// six-line subset.
    Tenancy::Organization seed_org(const std::string& bin,
                                   const std::string& name,
                                   const std::string& tax_regime = "standard") {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, name, tax_regime, false);
    }

    Domain::User seed_user(const std::string& email) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name("User");
        if (!role) {
            ADD_FAILURE() << "role 'User' missing — seed migration?";
            throw std::runtime_error("seed role missing: User");
        }
        return users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
    }

    Security::Auth::AuthPrincipal member(const std::string& email, const std::string& org_id, const std::string& role) {
        auto user = seed_user(email);
        Security::Auth::AuthPrincipal p;
        p.subject = user.id;
        p.raw_claims = json{{"sub", user.id}};
        p.org = org_id;
        Tenancy::OrgMemberRepository members;
        members.add(org_id, user.id, role);
        return p;
    }

    static HttpRequestPtr authed(const Security::Auth::AuthPrincipal& p, HttpMethod method = Get) {
        return TestHelpers::authed(p, method);
    }

    static HttpRequestPtr authed_json(const Security::Auth::AuthPrincipal& p,
                                      const json& body,
                                      HttpMethod method = Post) {
        return TestHelpers::authed_json(p, body, method);
    }

    Hr::Employee seed_employee(const std::string& org_id, const std::string& iin, long long salary_tiyn = 50000000) {
        Hr::EmployeeRepository repo;
        Hr::Employee draft;
        draft.iin = iin;
        draft.last_name = "Серикбаева";
        draft.first_name = "Айгерим";
        draft.middle_name = "Кайратовна";
        draft.position = "Бухгалтер";
        draft.salary_tiyn = salary_tiyn;
        draft.hired_on = "2026-01-15";
        draft.payout_iik = "";
        return repo.create(org_id, draft);
    }

    static long queue_depth() {
        return static_cast<long>(Cache::get().get_client().llen(Jobs::queue_key(kRenderJobType)));
    }

    /// Calculate + approve a run through the SERVICE (not the controller), so
    /// a test about approve()/post-to-journal isn't also asserting
    /// calculate()'s behaviour.
    Payroll::PayrollRun seeded_run(const std::string& org_id, int year, int month, bool approve) {
        Payroll::PayrollService svc;
        auto run = svc.calculate_run(org_id, year, month);
        if (approve) {
            auto approved = svc.approve(org_id, run.id);
            if (approved)
                run = *approved;
        }
        return run;
    }

    static json body_of(const HttpResponsePtr& resp) { return json::parse(std::string(resp->body())); }
};

// ── POST /api/v1/payroll-runs ────────────────────────────────────────────────

TEST_F(PayrollApiTest, CalculateRunReturnsPayslips) {
    auto org = seed_org("777160000001", "Payroll Org LLP");
    auto accountant = member("payroll-acc1@example.com", org.id, "accountant");
    seed_employee(org.id, "156312191013");
    seed_employee(org.id, "988916681773");

    auto req = authed_json(accountant, json{{"year", 2026}, {"month", 3}});
    HttpResponsePtr resp;
    ctrl.calculate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto data = body_of(resp)["data"];
    EXPECT_EQ(data["period_year"].get<int>(), 2026);
    EXPECT_EQ(data["period_month"].get<int>(), 3);
    EXPECT_EQ(data["status"].get<std::string>(), "draft");
    ASSERT_TRUE(data["payslips"].is_array());
    EXPECT_EQ(data["payslips"].size(), 2u);
    // The rates snapshot is the reproducibility record — it must not be empty.
    EXPECT_TRUE(data["rates_snapshot"].contains("effective_date"));
}

TEST_F(PayrollApiTest, CalculateRunViewerForbidden) {
    auto org = seed_org("777160000002", "Payroll Viewer Org LLP");
    auto viewer = member("payroll-viewer1@example.com", org.id, "viewer");

    auto req = authed_json(viewer, json{{"year", 2026}, {"month", 3}});
    HttpResponsePtr resp;
    ctrl.calculate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
    EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "org_role_denied");
}

TEST_F(PayrollApiTest, CalculateRunMissingFieldsBadRequest) {
    auto org = seed_org("777160000003", "Payroll Shape Org LLP");
    auto accountant = member("payroll-acc3@example.com", org.id, "accountant");

    auto req = authed_json(accountant, json{{"year", 2026}});
    HttpResponsePtr resp;
    ctrl.calculate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "validation_failed");
}

TEST_F(PayrollApiTest, CalculateRunOutOfRangeMonthUnprocessable) {
    auto org = seed_org("777160000004", "Payroll Range Org LLP");
    auto accountant = member("payroll-acc4@example.com", org.id, "accountant");

    auto req = authed_json(accountant, json{{"year", 2026}, {"month", 13}});
    HttpResponsePtr resp;
    ctrl.calculate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto errors = body_of(resp)["errors"];
    ASSERT_TRUE(errors.is_array());
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0]["field"].get<std::string>(), "month");
    EXPECT_EQ(errors[0]["code"].get<std::string>(), "out_of_range");
}

TEST_F(PayrollApiTest, CalculateRunOutOfRangeYearUnprocessable) {
    auto org = seed_org("777160000030", "Payroll Year Range Org LLP");
    auto accountant = member("payroll-acc30@example.com", org.id, "accountant");

    auto req = authed_json(accountant, json{{"year", 20261}, {"month", 3}});
    HttpResponsePtr resp;
    ctrl.calculate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto errors = body_of(resp)["errors"];
    ASSERT_TRUE(errors.is_array());
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0]["field"].get<std::string>(), "year");
    EXPECT_EQ(errors[0]["code"].get<std::string>(), "out_of_range");
}

TEST_F(PayrollApiTest, CalculateRunWithoutSeededRatesUnprocessable) {
    // 2025 predates migration 011's 2026-01-01 seed, so the very first rate
    // lookup comes back empty. That is a reference-data gap an operator can
    // fix by seeding a row — it must be a 422 naming the missing rate, never
    // the 500 a bare std::runtime_error would produce (fix round 1).
    auto org = seed_org("777160000031", "Payroll No Rates Org LLP");
    auto accountant = member("payroll-acc31@example.com", org.id, "accountant");
    seed_employee(org.id, "156312191013");

    auto req = authed_json(accountant, json{{"year", 2025}, {"month", 6}});
    HttpResponsePtr resp;
    ctrl.calculate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k422UnprocessableEntity) << resp->body();
    auto payload = body_of(resp);
    EXPECT_EQ(payload["error"].get<std::string>(), "validation_failed");
    auto errors = payload["errors"];
    ASSERT_TRUE(errors.is_array());
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0]["field"].get<std::string>(), "year");
    EXPECT_EQ(errors[0]["code"].get<std::string>(), "missing_tax_reference");
    // The message has to be actionable on its own: which reference, what date.
    const std::string message = errors[0]["message"].get<std::string>();
    EXPECT_NE(message.find("2025-06-30"), std::string::npos) << message;
    EXPECT_NE(message.find("rate"), std::string::npos) << message;
    // Nothing was persisted for a period that could not be calculated.
    Payroll::PayrollRepository runs;
    EXPECT_FALSE(runs.find_by_period(org.id, 2025, 6, /*from_primary=*/true).has_value());
}

TEST_F(PayrollApiTest, RecalculateApprovedRunConflicts) {
    auto org = seed_org("777160000005", "Payroll Conflict Org LLP");
    auto accountant = member("payroll-acc5@example.com", org.id, "accountant");
    seed_employee(org.id, "156312191013");
    seeded_run(org.id, 2026, 4, /*approve=*/true);

    auto req = authed_json(accountant, json{{"year", 2026}, {"month", 4}});
    HttpResponsePtr resp;
    ctrl.calculate(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "invalid_run_state");
}

// ── GET /api/v1/payroll-runs ─────────────────────────────────────────────────

TEST_F(PayrollApiTest, ListRunsFiltersByYear) {
    auto org = seed_org("777160000006", "Payroll List Org LLP");
    auto viewer = member("payroll-viewer6@example.com", org.id, "viewer");
    seed_employee(org.id, "156312191013");
    seeded_run(org.id, 2026, 1, /*approve=*/false);
    seeded_run(org.id, 2027, 1, /*approve=*/false);

    auto all = authed(viewer);
    HttpResponsePtr resp;
    ctrl.list(all, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    EXPECT_EQ(body_of(resp)["total"].get<long>(), 2);

    auto filtered = authed(viewer);
    filtered->setParameter("year", "2027");
    resp = nullptr;
    ctrl.list(filtered, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto filtered_body = body_of(resp);
    EXPECT_EQ(filtered_body["total"].get<long>(), 1);
    ASSERT_EQ(filtered_body["data"].size(), 1u);
    EXPECT_EQ(filtered_body["data"][0]["period_year"].get<int>(), 2027);
}

TEST_F(PayrollApiTest, ListRunsRejectsNonNumericYear) {
    auto org = seed_org("777160000007", "Payroll Year Org LLP");
    auto viewer = member("payroll-viewer7@example.com", org.id, "viewer");

    auto req = authed(viewer);
    req->setParameter("year", "twenty-six");
    HttpResponsePtr resp;
    ctrl.list(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
}

TEST_F(PayrollApiTest, ListRunsIsOrgScoped) {
    auto org_a = seed_org("777160000008", "Payroll Org A LLP");
    auto org_b = seed_org("777160000009", "Payroll Org B LLP");
    auto member_a = member("payroll-a@example.com", org_a.id, "accountant");
    seed_employee(org_a.id, "156312191013");
    seeded_run(org_a.id, 2026, 5, /*approve=*/false);
    auto member_b = member("payroll-b@example.com", org_b.id, "accountant");

    HttpResponsePtr resp;
    ctrl.list(authed(member_b), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    EXPECT_EQ(body_of(resp)["total"].get<long>(), 0);
}

// ── POST /api/v1/payroll-runs/{id}/approve ───────────────────────────────────

TEST_F(PayrollApiTest, ApproveMovesRunToApproved) {
    auto org = seed_org("777160000010", "Payroll Approve Org LLP");
    auto accountant = member("payroll-acc10@example.com", org.id, "accountant");
    seed_employee(org.id, "156312191013");
    auto run = seeded_run(org.id, 2026, 6, /*approve=*/false);

    HttpResponsePtr resp;
    ctrl.approve(
        authed(accountant, Post), [&](const HttpResponsePtr& r) { resp = r; }, run.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    EXPECT_EQ(body_of(resp)["data"]["status"].get<std::string>(), "approved");
}

TEST_F(PayrollApiTest, ApproveTwiceConflicts) {
    auto org = seed_org("777160000011", "Payroll Approve Twice Org LLP");
    auto accountant = member("payroll-acc11@example.com", org.id, "accountant");
    seed_employee(org.id, "156312191013");
    auto run = seeded_run(org.id, 2026, 7, /*approve=*/true);

    HttpResponsePtr resp;
    ctrl.approve(
        authed(accountant, Post), [&](const HttpResponsePtr& r) { resp = r; }, run.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "invalid_run_state");
}

TEST_F(PayrollApiTest, ApproveViewerForbidden) {
    auto org = seed_org("777160000012", "Payroll Approve Viewer Org LLP");
    auto viewer = member("payroll-viewer12@example.com", org.id, "viewer");
    seed_employee(org.id, "156312191013");
    auto run = seeded_run(org.id, 2026, 8, /*approve=*/false);

    HttpResponsePtr resp;
    ctrl.approve(
        authed(viewer, Post), [&](const HttpResponsePtr& r) { resp = r; }, run.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(PayrollApiTest, ApproveCrossOrgNotFound) {
    auto org_a = seed_org("777160000013", "Payroll Cross A LLP");
    auto org_b = seed_org("777160000014", "Payroll Cross B LLP");
    member("payroll-cross-a@example.com", org_a.id, "accountant");
    auto member_b = member("payroll-cross-b@example.com", org_b.id, "accountant");
    seed_employee(org_a.id, "156312191013");
    auto run_a = seeded_run(org_a.id, 2026, 9, /*approve=*/false);

    HttpResponsePtr resp;
    ctrl.approve(
        authed(member_b, Post), [&](const HttpResponsePtr& r) { resp = r; }, run_a.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

TEST_F(PayrollApiTest, ApproveMalformedIdBadRequest) {
    auto org = seed_org("777160000015", "Payroll Bad Id Org LLP");
    auto accountant = member("payroll-acc15@example.com", org.id, "accountant");

    HttpResponsePtr resp;
    ctrl.approve(
        authed(accountant, Post), [&](const HttpResponsePtr& r) { resp = r; }, "not-a-uuid");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "invalid_id");
}

// ── POST /api/v1/payroll-runs/{id}/post-to-journal ───────────────────────────

TEST_F(PayrollApiTest, PostToJournalReturnsBalancedEntry) {
    auto org = seed_org("777160000016", "Payroll Journal Org LLP");
    auto accountant = member("payroll-acc16@example.com", org.id, "accountant");
    seed_employee(org.id, "156312191013", 50000000);
    seed_employee(org.id, "988916681773", 30000000);
    auto run = seeded_run(org.id, 2026, 10, /*approve=*/true);

    HttpResponsePtr resp;
    ctrl.postToJournal(
        authed(accountant, Post), [&](const HttpResponsePtr& r) { resp = r; }, run.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK) << resp->body();
    const std::string entry_id = body_of(resp)["entry_id"].get<std::string>();
    ASSERT_FALSE(entry_id.empty());

    // Re-sum the entry's lines independently of the service's own check.
    Ledger::JournalRepository journal;
    auto entry = journal.find_in_org(entry_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->status, "posted");
    auto lines = journal.load_lines(*entry, /*from_primary=*/true);
    ASSERT_GE(lines.size(), 2u);
    long long debit_tiyn = 0;
    long long credit_tiyn = 0;
    for (const auto& l : lines) {
        const long long amount = Ledger::parse_tiyn(l.amount);
        if (l.side == "debit")
            debit_tiyn += amount;
        else
            credit_tiyn += amount;
    }
    EXPECT_GT(debit_tiyn, 0);
    EXPECT_EQ(debit_tiyn, credit_tiyn);
}

TEST_F(PayrollApiTest, PostToJournalUnapprovedRunConflicts) {
    auto org = seed_org("777160000017", "Payroll Unapproved Org LLP");
    auto accountant = member("payroll-acc17@example.com", org.id, "accountant");
    seed_employee(org.id, "156312191013");
    auto run = seeded_run(org.id, 2026, 11, /*approve=*/false);

    HttpResponsePtr resp;
    ctrl.postToJournal(
        authed(accountant, Post), [&](const HttpResponsePtr& r) { resp = r; }, run.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "invalid_run_state");
}

TEST_F(PayrollApiTest, PostToJournalTwiceConflicts) {
    auto org = seed_org("777160000018", "Payroll Double Post Org LLP");
    auto accountant = member("payroll-acc18@example.com", org.id, "accountant");
    seed_employee(org.id, "156312191013");
    auto run = seeded_run(org.id, 2026, 12, /*approve=*/true);

    HttpResponsePtr first;
    ctrl.postToJournal(
        authed(accountant, Post), [&](const HttpResponsePtr& r) { first = r; }, run.id);
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(first->statusCode(), k200OK) << first->body();

    HttpResponsePtr second;
    ctrl.postToJournal(
        authed(accountant, Post), [&](const HttpResponsePtr& r) { second = r; }, run.id);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->statusCode(), k409Conflict);
    EXPECT_EQ(body_of(second)["error"].get<std::string>(), "invalid_run_state");
}

TEST_F(PayrollApiTest, PostToJournalEmptyRunConflicts) {
    // An org with NO active employees produces an approved run with zero
    // payslips — the controller's own 409 `empty_run` pre-check (the case
    // PayrollService signals with a bare std::runtime_error).
    auto org = seed_org("777160000019", "Payroll Empty Org LLP");
    auto accountant = member("payroll-acc19@example.com", org.id, "accountant");
    auto run = seeded_run(org.id, 2026, 2, /*approve=*/true);

    HttpResponsePtr resp;
    ctrl.postToJournal(
        authed(accountant, Post), [&](const HttpResponsePtr& r) { resp = r; }, run.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "empty_run");
}

TEST_F(PayrollApiTest, PostToJournalViewerForbidden) {
    auto org = seed_org("777160000020", "Payroll Post Viewer Org LLP");
    auto viewer = member("payroll-viewer20@example.com", org.id, "viewer");
    seed_employee(org.id, "156312191013");
    auto run = seeded_run(org.id, 2026, 3, /*approve=*/true);

    HttpResponsePtr resp;
    ctrl.postToJournal(
        authed(viewer, Post), [&](const HttpResponsePtr& r) { resp = r; }, run.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(PayrollApiTest, PostToJournalCrossOrgNotFound) {
    auto org_a = seed_org("777160000021", "Payroll Post Cross A LLP");
    auto org_b = seed_org("777160000022", "Payroll Post Cross B LLP");
    auto member_b = member("payroll-post-cross-b@example.com", org_b.id, "accountant");
    seed_employee(org_a.id, "156312191013");
    auto run_a = seeded_run(org_a.id, 2026, 4, /*approve=*/true);

    HttpResponsePtr resp;
    ctrl.postToJournal(
        authed(member_b, Post), [&](const HttpResponsePtr& r) { resp = r; }, run_a.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

// ── GET /api/v1/payroll-runs/{id}/payslips ───────────────────────────────────

TEST_F(PayrollApiTest, ListPayslipsReturnsOnePerEmployee) {
    auto org = seed_org("777160000023", "Payroll Payslips Org LLP");
    auto viewer = member("payroll-viewer23@example.com", org.id, "viewer");
    seed_employee(org.id, "156312191013");
    seed_employee(org.id, "988916681773");
    auto run = seeded_run(org.id, 2026, 5, /*approve=*/false);

    HttpResponsePtr resp;
    ctrl.listPayslips(
        authed(viewer), [&](const HttpResponsePtr& r) { resp = r; }, run.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto data = body_of(resp)["data"];
    ASSERT_TRUE(data.is_array());
    EXPECT_EQ(data.size(), 2u);
    // Money crosses the wire as integer tiyn, never a float.
    EXPECT_TRUE(data[0]["gross_tiyn"].is_number_integer());
}

TEST_F(PayrollApiTest, ListPayslipsCrossOrgNotFound) {
    auto org_a = seed_org("777160000024", "Payroll Payslips Cross A LLP");
    auto org_b = seed_org("777160000025", "Payroll Payslips Cross B LLP");
    auto member_b = member("payroll-slips-cross-b@example.com", org_b.id, "accountant");
    seed_employee(org_a.id, "156312191013");
    auto run_a = seeded_run(org_a.id, 2026, 6, /*approve=*/false);

    HttpResponsePtr resp;
    ctrl.listPayslips(
        authed(member_b), [&](const HttpResponsePtr& r) { resp = r; }, run_a.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

// ── POST /payroll-runs/{id}/payslips/{employee_id}/generate-document ─────────

TEST_F(PayrollApiTest, GeneratePayslipDocumentQueuesRender) {
    auto org = seed_org("777160000026", "Payroll Docgen Org LLP");
    auto accountant = member("payroll-acc26@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "156312191013");
    auto run = seeded_run(org.id, 2026, 7, /*approve=*/false);

    const long before = queue_depth();
    // After P3 the caller supplies NOTHING: `net_words` is derived from the
    // payslip's own net, and sending it is a 422 (see
    // GeneratePayslipRejectsClientSuppliedNetWords).
    auto req = authed_json(accountant, json::object());
    HttpResponsePtr resp;
    ctrl.generatePayslip(
        req, [&](const HttpResponsePtr& r) { resp = r; }, run.id, employee.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted) << resp->body();
    auto payload = body_of(resp);
    EXPECT_FALSE(payload["document_id"].get<std::string>().empty());
    EXPECT_TRUE(payload["render_queued"].get<bool>());
    EXPECT_EQ(queue_depth(), before + 1);
}

// P3 replaces the old "an empty body is a 422 because net_words is missing"
// test: an empty body is now the ONLY valid body, and the words come from
// the same integer `net` is formatted from, so a payslip whose digits and
// words disagree is no longer expressible.
TEST_F(PayrollApiTest, GeneratePayslipDerivesNetWordsFromTheStoredNet) {
    auto org = seed_org("777160000027", "Payroll Docgen Words Org LLP");
    auto accountant = member("payroll-acc27@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "156312191013");
    auto run = seeded_run(org.id, 2026, 8, /*approve=*/false);

    Payroll::PayrollRepository payroll_repo;
    auto payslips = payroll_repo.list_payslips(run, /*from_primary=*/true);
    ASSERT_EQ(payslips.size(), 1U);
    const std::string expected_words = Money::to_words_ru(payslips[0].net);

    auto req = authed_json(accountant, json::object());
    HttpResponsePtr resp;
    ctrl.generatePayslip(
        req, [&](const HttpResponsePtr& r) { resp = r; }, run.id, employee.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted) << resp->body();

    Ledger::DocumentRepository documents;
    auto doc = documents.find_in_org(body_of(resp)["document_id"].get<std::string>(), org.id, /*from_primary=*/true);
    ASSERT_TRUE(doc.has_value());
    // doc_type is the §5.3 RESOURCE of this document, and a payslip is a
    // payroll calculation, not a кадровый документ. While this was "hr" the
    // hr role could read the whole calculation through GET /documents/{id}
    // (v0.4.0 acceptance defect); LedgerDocumentsController::resource_for
    // keys off exactly this column, so pin it here at the producer.
    EXPECT_EQ(doc->doc_type, "payroll");
    // input_snapshot lives on the document's VERSION now
    // (migrations/018_document_versions.sql); the render that would publish
    // version 1 was only enqueued, so it is read off the version itself.
    auto version = documents.latest_version(org.id, doc->id);
    ASSERT_TRUE(version.has_value());
    ASSERT_TRUE(version->input_snapshot.has_value());
    EXPECT_EQ((*version->input_snapshot)["net_words"].get<std::string>(), expected_words);
    // The PRINTED form. Asserted BOTH ways on purpose: the payslip is handed
    // to an employee, so it must carry Money::format_tiyn_ru's "250 575,00",
    // and it must NOT carry Ledger::format_tiyn's "250575.00" — which is what
    // v0.4.2 printed, and which still belongs on journal_lines.amount and in
    // the payroll API. A single positive assertion would go green again the
    // moment someone "unified" the two formatters the wrong way round.
    EXPECT_EQ((*version->input_snapshot)["net"].get<std::string>(), Money::format_tiyn_ru(payslips[0].net));
    EXPECT_NE((*version->input_snapshot)["net"].get<std::string>(), Ledger::format_tiyn(payslips[0].net));
}

TEST_F(PayrollApiTest, GeneratePayslipRejectsClientSuppliedNetWords) {
    auto org = seed_org("777160000031", "Payroll Docgen 422 Org LLP");
    auto accountant = member("payroll-acc31@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "156312191013");
    auto run = seeded_run(org.id, 2026, 12, /*approve=*/false);

    const long before = queue_depth();
    auto req = authed_json(accountant, json{{"net_words", "Один тенге 00 тиын"}});
    HttpResponsePtr resp;
    ctrl.generatePayslip(
        req, [&](const HttpResponsePtr& r) { resp = r; }, run.id, employee.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto errors = body_of(resp)["errors"];
    ASSERT_TRUE(errors.is_array());
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0]["field"].get<std::string>(), "net_words");
    EXPECT_EQ(errors[0]["code"].get<std::string>(), "not_allowed_override");
    EXPECT_EQ(queue_depth(), before);

    // An EMPTY object has no leaves, so a recursing allowlist used to walk
    // straight past it — while RFC 7396 merge_patch happily REPLACES the
    // derived string with {}. It is checked at its own path now, so the
    // "any key outside the allowlist is a 422" promise is literally true.
    auto empty_obj_req = authed_json(accountant, json{{"net_words", json::object()}});
    HttpResponsePtr empty_obj_resp;
    ctrl.generatePayslip(
        empty_obj_req, [&](const HttpResponsePtr& r) { empty_obj_resp = r; }, run.id, employee.id);
    ASSERT_NE(empty_obj_resp, nullptr);
    EXPECT_EQ(empty_obj_resp->statusCode(), k422UnprocessableEntity);
    EXPECT_EQ(body_of(empty_obj_resp)["errors"][0]["field"].get<std::string>(), "net_words");
    EXPECT_EQ(body_of(empty_obj_resp)["errors"][0]["code"].get<std::string>(), "not_allowed_override");
    EXPECT_EQ(queue_depth(), before);
}

// Final fix round (security): the body merged onto a payslip's auto-derived
// input is allowlisted (PayrollController::payslip_allowed_extra_fields()),
// and `net_words` is the entire allowlist. An attempt to rewrite the money
// or the employee's identity in a document that sits next to the payroll run
// and the journal entry for the same period must be rejected outright — and
// a subsequent LEGITIMATE request must still store the REAL figures.
//
// This test bites: with the allowlist removed, the first request answers 202
// and the stored input carries "1.00" as the net.
TEST_F(PayrollApiTest, GeneratePayslipRejectsAuthoritativeOverrideAndKeepsTheTrueFigures) {
    auto org = seed_org("777160000030", "Payroll Override Org LLP");
    auto accountant = member("payroll-acc30@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "156312191013");
    auto run = seeded_run(org.id, 2026, 11, /*approve=*/false);

    // The figures actually on file, for the comparison at the end.
    Payroll::PayrollRepository payroll_repo;
    auto payslips = payroll_repo.list_payslips(run, /*from_primary=*/true);
    ASSERT_EQ(payslips.size(), 1U);
    // The figures as the payslip PRINTS them — Money::format_tiyn_ru, not
    // Ledger::format_tiyn (see GeneratePayslipDocument above).
    const std::string true_net = Money::format_tiyn_ru(payslips[0].net);
    const std::string true_gross = Money::format_tiyn_ru(payslips[0].gross_tiyn);
    ASSERT_NE(true_net, "1.00");
    ASSERT_NE(true_gross, Ledger::format_tiyn(payslips[0].gross_tiyn));

    const long before = queue_depth();
    json malicious = {{"net", "1.00"}};
    auto bad_req = authed_json(accountant, malicious);
    HttpResponsePtr bad_resp;
    ctrl.generatePayslip(
        bad_req, [&](const HttpResponsePtr& r) { bad_resp = r; }, run.id, employee.id);
    ASSERT_NE(bad_resp, nullptr);
    ASSERT_EQ(bad_resp->statusCode(), k422UnprocessableEntity) << bad_resp->body();
    auto bad_body = body_of(bad_resp);
    EXPECT_EQ(bad_body["errors"][0]["field"].get<std::string>(), "net");
    EXPECT_EQ(bad_body["errors"][0]["code"].get<std::string>(), "not_allowed_override");
    // Rejected BEFORE any side effect.
    EXPECT_EQ(queue_depth(), before);

    // A nested authoritative field is caught at its own leaf, not waved
    // through because its parent object is not itself an allowlisted key.
    json malicious_iin = {{"employee", {{"iin", "999999999999"}}}};
    auto iin_req = authed_json(accountant, malicious_iin);
    HttpResponsePtr iin_resp;
    ctrl.generatePayslip(
        iin_req, [&](const HttpResponsePtr& r) { iin_resp = r; }, run.id, employee.id);
    ASSERT_NE(iin_resp, nullptr);
    EXPECT_EQ(iin_resp->statusCode(), k422UnprocessableEntity);
    EXPECT_EQ(body_of(iin_resp)["errors"][0]["field"].get<std::string>(), "employee.iin");
    EXPECT_EQ(queue_depth(), before);

    // An empty body — the only body the P3 allowlist accepts — still works,
    // and the document that lands carries the figures the payroll run
    // actually computed.
    auto good_req = authed_json(accountant, json::object());
    HttpResponsePtr good_resp;
    ctrl.generatePayslip(
        good_req, [&](const HttpResponsePtr& r) { good_resp = r; }, run.id, employee.id);
    ASSERT_NE(good_resp, nullptr);
    ASSERT_EQ(good_resp->statusCode(), k202Accepted) << good_resp->body();
    const std::string document_id = body_of(good_resp)["document_id"].get<std::string>();

    Ledger::DocumentRepository documents;
    auto doc = documents.find_in_org(document_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->doc_type, "payroll");  // see the doc_type note above
    // Snapshot on version 1 — nothing published it (migration 018).
    auto version = documents.latest_version(org.id, doc->id);
    ASSERT_TRUE(version.has_value());
    ASSERT_TRUE(version->input_snapshot.has_value());
    const json& stored = *version->input_snapshot;
    EXPECT_EQ(stored["net"].get<std::string>(), true_net);
    EXPECT_EQ(stored["gross_tenge"].get<std::string>(), true_gross);
    EXPECT_EQ(stored["employee"]["iin"].get<std::string>(), "156312191013");
    EXPECT_EQ(stored["net_words"].get<std::string>(), Money::to_words_ru(payslips[0].net));
}

TEST_F(PayrollApiTest, GeneratePayslipDocumentViewerForbidden) {
    auto org = seed_org("777160000028", "Payroll Docgen Viewer Org LLP");
    auto viewer = member("payroll-viewer28@example.com", org.id, "viewer");
    auto employee = seed_employee(org.id, "156312191013");
    auto run = seeded_run(org.id, 2026, 9, /*approve=*/false);

    auto req = authed_json(viewer, json::object());
    HttpResponsePtr resp;
    ctrl.generatePayslip(
        req, [&](const HttpResponsePtr& r) { resp = r; }, run.id, employee.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(PayrollApiTest, GeneratePayslipDocumentForeignEmployeeNotFound) {
    // The employee exists, but has no payslip in THIS run — a 404 on the
    // payslip, not a 500 and not a silently empty document.
    auto org = seed_org("777160000029", "Payroll Docgen 404 Org LLP");
    auto accountant = member("payroll-acc29@example.com", org.id, "accountant");
    seed_employee(org.id, "156312191013");
    auto run = seeded_run(org.id, 2026, 10, /*approve=*/false);
    auto latecomer = seed_employee(org.id, "988916681773");

    auto req = authed_json(accountant, json::object());
    HttpResponsePtr resp;
    ctrl.generatePayslip(
        req, [&](const HttpResponsePtr& r) { resp = r; }, run.id, latecomer.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

}  // namespace
