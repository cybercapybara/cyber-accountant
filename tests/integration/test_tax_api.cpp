/**
 * @file test_tax_api.cpp
 * @brief Integration tests for Api::TaxController (Task 12).
 *
 * Two fixtures on purpose:
 *   - `TaxApiTest` needs only Postgres/Redis and covers the reference,
 *     calculation, alert and deadline routes;
 *   - `TaxFilingApiTest` additionally needs a real S3-compatible endpoint
 *     (MinIO in the test compose profile, same as test_documents_api.cpp /
 *     test_s3_storage.cpp) because the filing pipeline actually PUTs the ФНО
 *     XML and presigns download URLs. Splitting them keeps the whole tax
 *     surface covered in environments without MinIO instead of skipping it
 *     wholesale.
 *
 * Direct controller invocation throughout (test_hr_api.cpp / test_documents_api.cpp
 * idiom); the docgen side is asserted as "a docgen.render job landed on the
 * queue", never as "a PDF exists".
 *
 * Dates are always passed explicitly (?on=2026-06-30 rather than the
 * today-by-default path) so the assertions do not depend on the wall clock of
 * whatever machine runs them — migration 011's rates are effective from
 * 2026-01-01.
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/TaxController.hpp"
#include "cache/Cache.hpp"
#include "database/Database.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "jobs/Jobs.hpp"
#include "ledger/DocumentRepository.hpp"
#include "ledger/JournalEntry.hpp"
#include "ledger/JournalService.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "storage/Storage.hpp"
#include "tax/FnoXml.hpp"
#include "tax/TaxCalculation.hpp"
#include "tax/TaxFilingRepository.hpp"
#include "tax/TaxService.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;
namespace fs = std::filesystem;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-tax-api-padding!!";
constexpr const char* kRenderJobType = "docgen.render";
/// Inside migration 011's 2026-01-01 seed window, so every rate/constant the
/// tax service needs is in force regardless of the host clock.
constexpr const char* kOnDate = "2026-06-30";

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

// Same defaults as test_s3_storage.cpp / test_documents_api.cpp.
std::string s3_endpoint() {
    return env_or("S3_TEST_ENDPOINT", "http://localhost:9000");
}
std::string s3_region() {
    return env_or("S3_TEST_REGION", "us-east-1");
}
std::string s3_bucket() {
    return env_or("S3_TEST_BUCKET", "test-bucket");
}
std::string s3_access_key() {
    return env_or("S3_TEST_ACCESS_KEY", "test");
}
std::string s3_secret_key() {
    return env_or("S3_TEST_SECRET_KEY", "test-secret-key");
}

std::string run_capture(const std::string& cmd) {
    std::string out;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr)
        throw std::runtime_error("popen failed for: " + cmd);
    char buf[4096];
    std::size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0)
        out.append(buf, n);
    pclose(pipe);
    return out;
}

bool is_minio_available() {
    const std::string cmd =
        "curl -s -o /dev/null -w '%{http_code}' --max-time 2 '" + s3_endpoint() + "/minio/health/live' 2>/dev/null";
    try {
        return run_capture(cmd) == "200";
    } catch (...) {
        return false;
    }
}

/// Everything both fixtures share: org/user/member seeding, request builders
/// and posted-income journal fixtures.
class TaxApiBase : public TestHelpers::CoreBackedTest {
protected:
    Api::TaxController ctrl;

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["jobs"]["enabled"] = true;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void reset_tenant_data() {
        TestHelpers::wipe_org_data();
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
    }

    Tenancy::Organization seed_org(const std::string& bin, const std::string& name) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, name, "snr_simplified", false);
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

    static json body_of(const HttpResponsePtr& resp) { return json::parse(std::string(resp->body())); }

    static Ledger::JournalLine line(const std::string& account_code,
                                    const std::string& side,
                                    const std::string& amount,
                                    std::optional<std::string> vat_amount = std::nullopt) {
        Ledger::JournalLine l;
        l.account_code = account_code;
        l.side = side;
        l.amount = amount;
        l.vat_amount = std::move(vat_amount);
        return l;
    }

    /// Balanced, POSTED cash-in/income entry — the only shape TaxService's
    /// aggregates look at (same fixture shape as test_tax_service.cpp).
    void post_income(const std::string& org_id,
                     const std::string& user_id,
                     const std::string& entry_date,
                     const std::string& amount,
                     std::optional<std::string> vat_amount = std::nullopt) {
        Ledger::JournalService svc;
        auto entry = svc.create_draft(org_id,
                                      user_id,
                                      entry_date,
                                      "Income " + amount,
                                      {line("1030", "debit", amount), line("6010", "credit", amount, vat_amount)});
        svc.post(org_id, entry.id);
    }
};

// ============================================================================
// Reference data, calculations, alerts, deadlines — no object storage needed.
// ============================================================================

class TaxApiTest : public TaxApiBase {
protected:
    std::string config_file_name() const override { return "tax_api_test_config.json"; }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        reset_tenant_data();
    }
};

// ── GET /api/v1/tax/rates ────────────────────────────────────────────────────

TEST_F(TaxApiTest, RatesReturnsSeededRatesAndConstants) {
    auto org = seed_org("777170000001", "Tax Rates Org LLP");
    auto viewer = member("tax-viewer1@example.com", org.id, "viewer");

    auto req = authed(viewer);
    req->setParameter("on", kOnDate);
    HttpResponsePtr resp;
    ctrl.listRates(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto data = body_of(resp)["data"];
    EXPECT_EQ(data["on"].get<std::string>(), kOnDate);
    ASSERT_TRUE(data["rates"].is_array());
    ASSERT_TRUE(data["constants"].is_array());
    EXPECT_FALSE(data["rates"].empty());
    EXPECT_FALSE(data["constants"].empty());
    // Rates travel as integer basis points, never a float percentage.
    EXPECT_TRUE(data["rates"][0]["rate_bp"].is_number_integer());
}

TEST_F(TaxApiTest, RatesRejectsMalformedOnDate) {
    auto org = seed_org("777170000002", "Tax Rates Bad Date Org LLP");
    auto viewer = member("tax-viewer2@example.com", org.id, "viewer");

    auto req = authed(viewer);
    req->setParameter("on", "2026-02-30");  // shape-valid, calendar-invalid
    HttpResponsePtr resp;
    ctrl.listRates(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    EXPECT_EQ(body_of(resp)["errors"][0]["field"].get<std::string>(), "on");
}

// ── POST /api/v1/tax/calculations ────────────────────────────────────────────

TEST_F(TaxApiTest, CreateSnrCalculationStoresSnapshots) {
    auto org = seed_org("777170000003", "Tax Calc Org LLP");
    auto accountant = member("tax-acc3@example.com", org.id, "accountant");
    auto user = seed_user("tax-journal3@example.com");
    post_income(org.id, user.id, "2026-02-01", "500000.00");

    auto req = authed_json(
        accountant, json{{"kind", "snr_simplified"}, {"period_from", "2026-01-01"}, {"period_to", "2026-06-30"}});
    HttpResponsePtr resp;
    ctrl.createCalculation(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK) << resp->body();
    auto data = body_of(resp)["data"];
    EXPECT_EQ(data["kind"].get<std::string>(), "snr_simplified");
    EXPECT_EQ(data["result_snapshot"]["income_tiyn"].get<long long>(), 50000000LL);
    // 4% of 500,000 ₸ = 20,000 ₸ = 2,000,000 tiyn.
    EXPECT_EQ(data["total_tiyn"].get<long long>(), 2000000LL);
    EXPECT_TRUE(data["input_snapshot"].contains("rate_bp"));
}

TEST_F(TaxApiTest, RecalculationReplacesRatherThanDuplicates) {
    auto org = seed_org("777170000004", "Tax Recalc Org LLP");
    auto accountant = member("tax-acc4@example.com", org.id, "accountant");

    const json body{{"kind", "vat"}, {"period_from", "2026-01-01"}, {"period_to", "2026-03-31"}};
    HttpResponsePtr first;
    ctrl.createCalculation(authed_json(accountant, body), [&](const HttpResponsePtr& r) { first = r; });
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(first->statusCode(), k200OK) << first->body();
    HttpResponsePtr second;
    ctrl.createCalculation(authed_json(accountant, body), [&](const HttpResponsePtr& r) { second = r; });
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(second->statusCode(), k200OK);
    EXPECT_EQ(body_of(first)["data"]["id"].get<std::string>(), body_of(second)["data"]["id"].get<std::string>());

    HttpResponsePtr listed;
    ctrl.listCalculations(authed(accountant), [&](const HttpResponsePtr& r) { listed = r; });
    ASSERT_NE(listed, nullptr);
    ASSERT_EQ(listed->statusCode(), k200OK);
    EXPECT_EQ(body_of(listed)["total"].get<long>(), 1);
}

TEST_F(TaxApiTest, CreateCalculationViewerForbidden) {
    auto org = seed_org("777170000005", "Tax Calc Viewer Org LLP");
    auto viewer = member("tax-viewer5@example.com", org.id, "viewer");

    auto req = authed_json(viewer, json{{"kind", "vat"}, {"period_from", "2026-01-01"}, {"period_to", "2026-03-31"}});
    HttpResponsePtr resp;
    ctrl.createCalculation(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
    EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "viewer_read_only");
}

TEST_F(TaxApiTest, CreateCalculationMissingFieldsBadRequest) {
    auto org = seed_org("777170000006", "Tax Calc Shape Org LLP");
    auto accountant = member("tax-acc6@example.com", org.id, "accountant");

    HttpResponsePtr resp;
    ctrl.createCalculation(authed_json(accountant, json{{"kind", "vat"}}), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(TaxApiTest, CreateCalculationUnknownKindUnprocessable) {
    auto org = seed_org("777170000007", "Tax Calc Kind Org LLP");
    auto accountant = member("tax-acc7@example.com", org.id, "accountant");

    auto req =
        authed_json(accountant, json{{"kind", "excise"}, {"period_from", "2026-01-01"}, {"period_to", "2026-03-31"}});
    HttpResponsePtr resp;
    ctrl.createCalculation(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    EXPECT_EQ(body_of(resp)["errors"][0]["field"].get<std::string>(), "kind");
}

TEST_F(TaxApiTest, CreateCalculationReversedPeriodUnprocessable) {
    auto org = seed_org("777170000008", "Tax Calc Period Org LLP");
    auto accountant = member("tax-acc8@example.com", org.id, "accountant");

    auto req =
        authed_json(accountant, json{{"kind", "vat"}, {"period_from", "2026-03-31"}, {"period_to", "2026-01-01"}});
    HttpResponsePtr resp;
    ctrl.createCalculation(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    EXPECT_EQ(body_of(resp)["errors"][0]["code"].get<std::string>(), "before_period_from");
}

TEST_F(TaxApiTest, CreateCalculationBeforeAnySeededRateUnprocessable) {
    // 2019 predates migration 011's 2026-01-01 seed: Tax::MissingTaxReference
    // is a bad VALUE for a caller-chosen period, so it must be a 422 — never
    // an undifferentiated 500.
    auto org = seed_org("777170000009", "Tax Calc Old Period Org LLP");
    auto accountant = member("tax-acc9@example.com", org.id, "accountant");

    auto req = authed_json(
        accountant, json{{"kind", "snr_simplified"}, {"period_from", "2019-01-01"}, {"period_to", "2019-06-30"}});
    HttpResponsePtr resp;
    ctrl.createCalculation(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    EXPECT_EQ(body_of(resp)["errors"][0]["code"].get<std::string>(), "missing_tax_reference");
}

// ── GET /api/v1/tax/calculations ─────────────────────────────────────────────

TEST_F(TaxApiTest, ListCalculationsFiltersAndIsOrgScoped) {
    auto org_a = seed_org("777170000010", "Tax List A LLP");
    auto org_b = seed_org("777170000011", "Tax List B LLP");
    auto member_a = member("tax-list-a@example.com", org_a.id, "accountant");
    auto member_b = member("tax-list-b@example.com", org_b.id, "accountant");

    Tax::TaxService svc;
    svc.calculate_snr(org_a.id, "2026-01-01", "2026-06-30");
    svc.calculate_vat(org_a.id, "2027-01-01", "2027-03-31");

    HttpResponsePtr all;
    ctrl.listCalculations(authed(member_a), [&](const HttpResponsePtr& r) { all = r; });
    ASSERT_NE(all, nullptr);
    ASSERT_EQ(all->statusCode(), k200OK);
    EXPECT_EQ(body_of(all)["total"].get<long>(), 2);

    auto by_kind = authed(member_a);
    by_kind->setParameter("kind", "vat");
    HttpResponsePtr filtered;
    ctrl.listCalculations(by_kind, [&](const HttpResponsePtr& r) { filtered = r; });
    ASSERT_NE(filtered, nullptr);
    ASSERT_EQ(filtered->statusCode(), k200OK);
    EXPECT_EQ(body_of(filtered)["total"].get<long>(), 1);

    auto by_year = authed(member_a);
    by_year->setParameter("year", "2026");
    HttpResponsePtr yearly;
    ctrl.listCalculations(by_year, [&](const HttpResponsePtr& r) { yearly = r; });
    ASSERT_NE(yearly, nullptr);
    ASSERT_EQ(yearly->statusCode(), k200OK);
    ASSERT_EQ(body_of(yearly)["data"].size(), 1u);
    EXPECT_EQ(body_of(yearly)["data"][0]["kind"].get<std::string>(), "snr_simplified");

    // Another tenant sees none of it.
    HttpResponsePtr other;
    ctrl.listCalculations(authed(member_b), [&](const HttpResponsePtr& r) { other = r; });
    ASSERT_NE(other, nullptr);
    ASSERT_EQ(other->statusCode(), k200OK);
    EXPECT_EQ(body_of(other)["total"].get<long>(), 0);
}

TEST_F(TaxApiTest, ListCalculationsRejectsUnknownKindFilter) {
    auto org = seed_org("777170000012", "Tax List Filter Org LLP");
    auto viewer = member("tax-viewer12@example.com", org.id, "viewer");

    auto req = authed(viewer);
    req->setParameter("kind", "excise");
    HttpResponsePtr resp;
    ctrl.listCalculations(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
}

// ── GET /api/v1/tax/alerts ───────────────────────────────────────────────────

TEST_F(TaxApiTest, AlertsFireNearTheVatThreshold) {
    auto org = seed_org("777170000013", "Tax Alerts Org LLP");
    auto viewer = member("tax-viewer13@example.com", org.id, "viewer");
    auto user = seed_user("tax-journal13@example.com");
    // Well past both 2026 thresholds (VAT registration = 10 000 МРП), so both
    // alert kinds must be present regardless of the exact seeded МРП.
    post_income(org.id, user.id, "2026-03-01", "500000000.00");

    auto req = authed(viewer);
    req->setParameter("on", kOnDate);
    HttpResponsePtr resp;
    ctrl.listAlerts(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK) << resp->body();
    auto data = body_of(resp)["data"];
    ASSERT_TRUE(data.is_array());
    ASSERT_FALSE(data.empty());
    std::vector<std::string> kinds;
    for (const auto& a : data)
        kinds.push_back(a["kind"].get<std::string>());
    EXPECT_NE(std::find(kinds.begin(), kinds.end(), std::string("vat_registration")), kinds.end());
}

TEST_F(TaxApiTest, AlertsAreEmptyWithoutIncome) {
    auto org = seed_org("777170000014", "Tax Quiet Org LLP");
    auto viewer = member("tax-viewer14@example.com", org.id, "viewer");

    auto req = authed(viewer);
    req->setParameter("on", kOnDate);
    HttpResponsePtr resp;
    ctrl.listAlerts(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    EXPECT_TRUE(body_of(resp)["data"].empty());
}

// ── GET /api/v1/tax/deadlines ────────────────────────────────────────────────

TEST_F(TaxApiTest, DeadlinesReturnUpcomingDueDates) {
    auto org = seed_org("777170000015", "Tax Deadlines Org LLP");
    auto viewer = member("tax-viewer15@example.com", org.id, "viewer");

    auto req = authed(viewer);
    req->setParameter("on", "2026-01-01");
    req->setParameter("horizon_days", "400");
    HttpResponsePtr resp;
    ctrl.listDeadlines(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto data = body_of(resp)["data"];
    ASSERT_TRUE(data.is_array());
    ASSERT_FALSE(data.empty());
    EXPECT_TRUE(data[0].contains("form"));
    EXPECT_TRUE(data[0].contains("due_date"));
    EXPECT_GE(data[0]["days_left"].get<int>(), 0);
}

TEST_F(TaxApiTest, DeadlinesClampAnAbsurdHorizon) {
    auto org = seed_org("777170000016", "Tax Horizon Org LLP");
    auto viewer = member("tax-viewer16@example.com", org.id, "viewer");

    auto req = authed(viewer);
    req->setParameter("on", "2026-01-01");
    req->setParameter("horizon_days", "999999");  // clamped, not rejected
    HttpResponsePtr resp;
    ctrl.listDeadlines(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k200OK);
}

// ============================================================================
// Filings — need MinIO (real PUT + presign).
// ============================================================================

class TaxFilingApiTest : public TaxApiBase {
protected:
    std::string config_file_name() const override { return "tax_filing_api_test_config.json"; }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        if (!fs::exists("templates/latex/fno_910/v1/schema.json"))
            GTEST_SKIP() << "repo templates not reachable from this working directory";
        reset_tenant_data();
        TestHelpers::drain_jobs({kRenderJobType});

        if (!is_minio_available()) {
            const std::string require_infra = env_or("CI_REQUIRE_INFRA", "");
            const bool must_have_infra = !require_infra.empty() && require_infra != "0" && require_infra != "false";
            if (must_have_infra) {
                FAIL() << "CI_REQUIRE_INFRA is set but MinIO is unavailable at " << s3_endpoint()
                       << " — this suite would have SKIPPED instead of running.";
            }
            GTEST_SKIP() << "MinIO not available at " << s3_endpoint();
        }

        Storage::S3Storage::Config cfg;
        cfg.endpoint = s3_endpoint();
        cfg.region = s3_region();
        cfg.bucket = s3_bucket();
        cfg.access_key = s3_access_key();
        cfg.secret_key = s3_secret_key();
        cfg.timeout_sec = 5;
        cfg.connect_timeout_sec = 2;
        Storage::install_for_testing(std::make_unique<Storage::S3Storage>(cfg));
    }

    void TearDown() override {
        Storage::reset_for_testing();
        if (!::testing::Test::IsSkipped() && Cache::is_initialized())
            TestHelpers::drain_jobs({kRenderJobType});
        TestHelpers::CoreBackedTest::TearDown();
    }

    static long queue_depth() {
        return static_cast<long>(Cache::get().get_client().llen(Jobs::queue_key(kRenderJobType)));
    }

    /// The free-text fields templates/latex/fno_910/v1/schema.json requires
    /// and the database cannot hold (see TaxController.hpp's header).
    static json fno910_extra() {
        return json{{"tax_words", "Двадцать тысяч тенге 00 тиын"},
                    {"director", "Ахметов Ерлан Серикович"},
                    {"accountant", "Серикбаева Айгерим Кайратовна"}};
    }

    /// Same for fno_300. `sales_tenge` is deliberately NOT here: the
    /// revenue turnover is derived from calculate_vat's own
    /// `result_snapshot.income_tiyn`, and supplying it is a 422 (see
    /// FilingRejectsASuppliedSalesTurnoverAndDerivesItFromTheLedger).
    static json fno300_extra() {
        return json{{"balance_words", "Ноль тенге 00 тиын"},
                    {"director", "Ахметов Ерлан Серикович"},
                    {"accountant", "Серикбаева Айгерим Кайратовна"}};
    }

    /// POST /tax/filings through the controller, returning the new filing id
    /// (or nullopt with a recorded failure) — for tests whose SUBJECT is a
    /// different route and that only need a filing to exist.
    std::optional<std::string> create_filing(const Security::Auth::AuthPrincipal& principal,
                                             const std::string& kind,
                                             const std::string& calculation_id,
                                             const json& document_input) {
        auto req = authed_json(
            principal, json{{"kind", kind}, {"calculation_id", calculation_id}, {"document_input", document_input}});
        HttpResponsePtr resp;
        ctrl.createFiling(req, [&](const HttpResponsePtr& r) { resp = r; });
        if (resp == nullptr) {
            ADD_FAILURE() << "createFiling did not answer";
            return std::nullopt;
        }
        if (resp->statusCode() != k202Accepted) {
            ADD_FAILURE() << "createFiling failed: " << resp->body();
            return std::nullopt;
        }
        return body_of(resp)["filing_id"].get<std::string>();
    }
};

TEST_F(TaxFilingApiTest, FilingStoresXmlAndQueuesPdf) {
    auto org = seed_org("777180000001", "Filing Org LLP");
    auto accountant = member("filing-acc1@example.com", org.id, "accountant");
    auto user = seed_user("filing-journal1@example.com");
    post_income(org.id, user.id, "2026-02-01", "500000.00");

    Tax::TaxService svc;
    auto calc = svc.calculate_snr(org.id, "2026-01-01", "2026-06-30");

    const long before = queue_depth();
    auto req = authed_json(accountant,
                           json{{"kind", "910.00"}, {"calculation_id", calc.id}, {"document_input", fno910_extra()}});
    HttpResponsePtr resp;
    ctrl.createFiling(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted) << resp->body();
    auto payload = body_of(resp);
    ASSERT_FALSE(payload["filing_id"].get<std::string>().empty());
    EXPECT_TRUE(payload["xml_ready"].get<bool>());
    EXPECT_TRUE(payload["render_queued"].get<bool>());
    // A docgen.render job was actually enqueued for the printable form.
    EXPECT_EQ(queue_depth(), before + 1);

    Tax::TaxFilingRepository filings;
    auto filing = filings.find_in_org(payload["filing_id"].get<std::string>(), org.id, /*from_primary=*/true);
    ASSERT_TRUE(filing);
    EXPECT_EQ(filing->kind, "910.00");
    EXPECT_EQ(filing->status, "generated");
    EXPECT_EQ(filing->calculation_id, calc.id);
    EXPECT_FALSE(filing->schema_validated);  // no content XSD exists for any ФНО
    ASSERT_TRUE(filing->xml_s3_key);
    ASSERT_TRUE(filing->document_id);

    // The XML object really landed in storage and really is the 910.00 form.
    ASSERT_TRUE(Storage::get().exists(*filing->xml_s3_key));
    auto stored = Storage::get().get(*filing->xml_s3_key);
    ASSERT_TRUE(stored);
    EXPECT_NE(stored->find("<fno"), std::string::npos);
    EXPECT_NE(stored->find("910.00"), std::string::npos);

    // The printable form exists as a draft document carrying the template.
    Ledger::DocumentRepository documents;
    auto document = documents.find_in_org(*filing->document_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(document);
    EXPECT_EQ(document->doc_type, "fno");
    ASSERT_TRUE(document->template_slug);
    EXPECT_EQ(*document->template_slug, "fno_910");
}

TEST_F(TaxFilingApiTest, VatFilingUsesTheQuarterlyForm) {
    auto org = seed_org("777180000002", "Filing Vat Org LLP");
    auto accountant = member("filing-acc2@example.com", org.id, "accountant");
    auto user = seed_user("filing-journal2@example.com");
    post_income(org.id, user.id, "2026-02-01", "1000000.00", std::string("160000.00"));

    Tax::TaxService svc;
    auto calc = svc.calculate_vat(org.id, "2026-01-01", "2026-03-31");

    auto req = authed_json(accountant,
                           json{{"kind", "300.00"}, {"calculation_id", calc.id}, {"document_input", fno300_extra()}});
    HttpResponsePtr resp;
    ctrl.createFiling(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted) << resp->body();

    Tax::TaxFilingRepository filings;
    auto filing = filings.find_in_org(body_of(resp)["filing_id"].get<std::string>(), org.id, /*from_primary=*/true);
    ASSERT_TRUE(filing);
    EXPECT_EQ(filing->kind, "300.00");
    ASSERT_TRUE(filing->xml_s3_key);
    auto stored = Storage::get().get(*filing->xml_s3_key);
    ASSERT_TRUE(stored);
    EXPECT_NE(stored->find("300.00"), std::string::npos);
}

TEST_F(TaxFilingApiTest, FilingKindMustMatchTheCalculation) {
    auto org = seed_org("777180000003", "Filing Mismatch Org LLP");
    auto accountant = member("filing-acc3@example.com", org.id, "accountant");

    Tax::TaxService svc;
    auto calc = svc.calculate_snr(org.id, "2026-01-01", "2026-06-30");

    auto req = authed_json(accountant,
                           json{{"kind", "300.00"}, {"calculation_id", calc.id}, {"document_input", fno300_extra()}});
    HttpResponsePtr resp;
    ctrl.createFiling(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    EXPECT_EQ(body_of(resp)["errors"][0]["code"].get<std::string>(), "kind_mismatch");
}

TEST_F(TaxFilingApiTest, FilingWithoutFreeTextFieldsUnprocessable) {
    auto org = seed_org("777180000004", "Filing Schema Org LLP");
    auto accountant = member("filing-acc4@example.com", org.id, "accountant");

    Tax::TaxService svc;
    auto calc = svc.calculate_snr(org.id, "2026-01-01", "2026-06-30");

    const long before = queue_depth();
    auto req = authed_json(accountant, json{{"kind", "910.00"}, {"calculation_id", calc.id}});
    HttpResponsePtr resp;
    ctrl.createFiling(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    EXPECT_EQ(body_of(resp)["errors"][0]["code"].get<std::string>(), "schema_validation_failed");
    // Rejected BEFORE any side effect: nothing was enqueued, nothing stored.
    EXPECT_EQ(queue_depth(), before);
    Tax::TaxFilingRepository filings;
    EXPECT_EQ(filings.count_in_org(org.id), 0);
}

// Final fix round (security): `document_input` is allowlisted
// (TaxController::fno_300_allowed_extra_fields()). Without it,
// `{"balance_tenge": "1.00"}` produced a ФНО 300.00 PDF stating a 1 ₸
// balance while the XML stored for the SAME filing row stated the truth —
// both downloadable from the same endpoint. The override must be rejected
// outright, and a legitimate request must still store the REAL balance in
// both artifacts.
//
// This test bites: with the allowlist removed the first request answers 202
// and the stored document input carries "1.00".
TEST_F(TaxFilingApiTest, FilingRejectsAuthoritativeOverrideAndKeepsTheTrueFigures) {
    auto org = seed_org("777180000011", "Filing Override Org LLP");
    auto accountant = member("filing-acc11@example.com", org.id, "accountant");
    auto user = seed_user("filing-journal11@example.com");
    post_income(org.id, user.id, "2026-02-01", "1000000.00", std::string("160000.00"));

    Tax::TaxService svc;
    auto calc = svc.calculate_vat(org.id, "2026-01-01", "2026-03-31");
    const std::string true_balance = Ledger::format_tiyn(calc.total_tiyn < 0 ? -calc.total_tiyn : calc.total_tiyn);
    ASSERT_NE(true_balance, "1.00");

    const long before = queue_depth();
    json malicious = fno300_extra();
    malicious["balance_tenge"] = "1.00";
    auto bad_req =
        authed_json(accountant, json{{"kind", "300.00"}, {"calculation_id", calc.id}, {"document_input", malicious}});
    HttpResponsePtr bad_resp;
    ctrl.createFiling(bad_req, [&](const HttpResponsePtr& r) { bad_resp = r; });
    ASSERT_NE(bad_resp, nullptr);
    ASSERT_EQ(bad_resp->statusCode(), k422UnprocessableEntity) << bad_resp->body();
    auto bad_body = body_of(bad_resp);
    EXPECT_EQ(bad_body["errors"][0]["field"].get<std::string>(), "balance_tenge");
    EXPECT_EQ(bad_body["errors"][0]["code"].get<std::string>(), "not_allowed_override");
    // Rejected BEFORE any side effect: no filing row, nothing enqueued.
    Tax::TaxFilingRepository filings;
    EXPECT_EQ(filings.count_in_org(org.id), 0);
    EXPECT_EQ(queue_depth(), before);

    // A nested authoritative field is caught at its own leaf too.
    json malicious_bin = fno300_extra();
    malicious_bin["org"] = json{{"bin", "999999999999"}};
    auto bin_req = authed_json(
        accountant, json{{"kind", "300.00"}, {"calculation_id", calc.id}, {"document_input", malicious_bin}});
    HttpResponsePtr bin_resp;
    ctrl.createFiling(bin_req, [&](const HttpResponsePtr& r) { bin_resp = r; });
    ASSERT_NE(bin_resp, nullptr);
    EXPECT_EQ(bin_resp->statusCode(), k422UnprocessableEntity);
    EXPECT_EQ(body_of(bin_resp)["errors"][0]["field"].get<std::string>(), "org.bin");
    EXPECT_EQ(queue_depth(), before);

    // The allowlisted free-text fields alone still work, and both artifacts
    // carry the balance the calculation actually produced.
    auto filing_id = create_filing(accountant, "300.00", calc.id, fno300_extra());
    ASSERT_TRUE(filing_id.has_value());
    auto filing = filings.find_in_org(*filing_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(filing);
    ASSERT_TRUE(filing->document_id);
    Ledger::DocumentRepository documents;
    auto document = documents.find_in_org(*filing->document_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(document);
    ASSERT_TRUE(document->input_snapshot);
    const json& stored = *document->input_snapshot;
    EXPECT_EQ(stored["balance_tenge"].get<std::string>(), true_balance);
    EXPECT_EQ(stored["org"]["bin"].get<std::string>(), "777180000011");

    // The XML for the same filing agrees with the PDF input — the whole
    // point of the allowlist is that these two can never diverge.
    ASSERT_TRUE(filing->xml_s3_key);
    auto xml = Storage::get().get(*filing->xml_s3_key);
    ASSERT_TRUE(xml);
    const std::string xml_balance = std::to_string(Tax::FnoXml::round_half_up_to_tenge(calc.total_tiyn));
    EXPECT_NE(xml->find("<balanceTenge>" + xml_balance + "</balanceTenge>"), std::string::npos);
    EXPECT_NE(xml->find("777180000011"), std::string::npos);
}

// Final fix round: a result_snapshot missing a figure the form needs is a
// bug or a schema drift, never a legitimate 0 ₸ line in a filing — it fails
// loudly, naming the key, instead of silently filing a zero.
TEST_F(TaxFilingApiTest, FilingRefusesACalculationMissingASnapshotFigure) {
    auto org = seed_org("777180000012", "Filing Snapshot Org LLP");
    auto accountant = member("filing-acc12@example.com", org.id, "accountant");
    auto user = seed_user("filing-journal12@example.com");
    post_income(org.id, user.id, "2026-02-01", "500000.00");

    Tax::TaxService svc;
    auto calc = svc.calculate_snr(org.id, "2026-01-01", "2026-06-30");

    // Simulate the drift: drop `income_tiyn` from the stored snapshot.
    Database::get().execute_write([&](auto& txn) {
        txn.exec_params("UPDATE tax_calculations SET result_snapshot = result_snapshot - 'income_tiyn' WHERE id = $1",
                        calc.id);
        return 0;
    });

    const long before = queue_depth();
    auto req = authed_json(accountant,
                           json{{"kind", "910.00"}, {"calculation_id", calc.id}, {"document_input", fno910_extra()}});
    HttpResponsePtr resp;
    ctrl.createFiling(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k422UnprocessableEntity) << resp->body();
    auto payload = body_of(resp);
    EXPECT_EQ(payload["errors"][0]["field"].get<std::string>(), "calculation_id");
    EXPECT_EQ(payload["errors"][0]["code"].get<std::string>(), "incomplete_calculation");
    EXPECT_NE(payload["errors"][0]["message"].get<std::string>().find("income_tiyn"), std::string::npos);
    EXPECT_EQ(queue_depth(), before);
    Tax::TaxFilingRepository filings;
    EXPECT_EQ(filings.count_in_org(org.id), 0);
}

// Re-review finding: `sales_tenge` (the revenue TURNOVER on the ФНО 300.00)
// was wrongly allowlisted on the reasoning that nothing on the server could
// produce it. calculate_vat does not NEED that figure for its own
// arithmetic, but the query behind it is the one calculate_snr already runs,
// so it is now snapshotted (`result_snapshot.income_tiyn`) and derived here.
// An accountant may not declare their own revenue on a legal tax filing.
//
// This test bites twice: it fails if `sales_tenge` were allowlisted again
// (the first request would be accepted), and it fails if the derived figure
// were wrong, because the expected value is the turnover of two separate
// ledger postings — not a number that happens to match one of them.
TEST_F(TaxFilingApiTest, FilingRejectsASuppliedSalesTurnoverAndDerivesItFromTheLedger) {
    auto org = seed_org("777180000013", "Filing Turnover Org LLP");
    auto accountant = member("filing-acc13@example.com", org.id, "accountant");
    auto user = seed_user("filing-journal13@example.com");
    // Two sales in the SAME quarter: turnover 1 000 000 + 500 000 = 1 500 000 ₸,
    // of which only the vat_amounts (160 000 + 80 000) drive the balance.
    post_income(org.id, user.id, "2026-02-01", "1000000.00", std::string("160000.00"));
    post_income(org.id, user.id, "2026-03-15", "500000.00", std::string("80000.00"));

    Tax::TaxService svc;
    auto calc = svc.calculate_vat(org.id, "2026-01-01", "2026-03-31");
    ASSERT_TRUE(calc.result_snapshot.contains("income_tiyn"));
    const std::string derived_turnover = Ledger::format_tiyn(calc.result_snapshot.at("income_tiyn").get<long long>());
    EXPECT_EQ(derived_turnover, "1500000.00");
    // The turnover is NOT the balance — a filing that confused the two would
    // still pass a weaker assertion.
    EXPECT_NE(derived_turnover, Ledger::format_tiyn(calc.total_tiyn));

    const long before = queue_depth();
    json malicious = fno300_extra();
    malicious["sales_tenge"] = "1.00";
    auto bad_req =
        authed_json(accountant, json{{"kind", "300.00"}, {"calculation_id", calc.id}, {"document_input", malicious}});
    HttpResponsePtr bad_resp;
    ctrl.createFiling(bad_req, [&](const HttpResponsePtr& r) { bad_resp = r; });
    ASSERT_NE(bad_resp, nullptr);
    ASSERT_EQ(bad_resp->statusCode(), k422UnprocessableEntity) << bad_resp->body();
    auto bad_body = body_of(bad_resp);
    EXPECT_EQ(bad_body["errors"][0]["field"].get<std::string>(), "sales_tenge");
    EXPECT_EQ(bad_body["errors"][0]["code"].get<std::string>(), "not_allowed_override");
    Tax::TaxFilingRepository filings;
    EXPECT_EQ(filings.count_in_org(org.id), 0);
    EXPECT_EQ(queue_depth(), before);

    // The filing succeeds WITHOUT sales_tenge in the body — the server put
    // it there — and it carries the ledger's turnover, not "1.00".
    auto filing_id = create_filing(accountant, "300.00", calc.id, fno300_extra());
    ASSERT_TRUE(filing_id.has_value());
    auto filing = filings.find_in_org(*filing_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(filing);
    ASSERT_TRUE(filing->document_id);
    Ledger::DocumentRepository documents;
    auto document = documents.find_in_org(*filing->document_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(document);
    ASSERT_TRUE(document->input_snapshot);
    EXPECT_EQ((*document->input_snapshot)["sales_tenge"].get<std::string>(), derived_turnover);
}

// A VAT calculation whose snapshot predates the income_tiyn key cannot
// supply the turnover line, and must say so rather than file a 0 ₸ turnover
// — the fno_300 counterpart of
// FilingRefusesACalculationMissingASnapshotFigure.
TEST_F(TaxFilingApiTest, FilingRefusesAVatCalculationWithoutTheTurnoverFigure) {
    auto org = seed_org("777180000014", "Filing Turnover Drift Org LLP");
    auto accountant = member("filing-acc14@example.com", org.id, "accountant");
    auto user = seed_user("filing-journal14@example.com");
    post_income(org.id, user.id, "2026-02-01", "1000000.00", std::string("160000.00"));

    Tax::TaxService svc;
    auto calc = svc.calculate_vat(org.id, "2026-01-01", "2026-03-31");
    Database::get().execute_write([&](auto& txn) {
        txn.exec_params("UPDATE tax_calculations SET result_snapshot = result_snapshot - 'income_tiyn' WHERE id = $1",
                        calc.id);
        return 0;
    });

    auto req = authed_json(accountant,
                           json{{"kind", "300.00"}, {"calculation_id", calc.id}, {"document_input", fno300_extra()}});
    HttpResponsePtr resp;
    ctrl.createFiling(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k422UnprocessableEntity) << resp->body();
    auto payload = body_of(resp);
    EXPECT_EQ(payload["errors"][0]["code"].get<std::string>(), "incomplete_calculation");
    EXPECT_NE(payload["errors"][0]["message"].get<std::string>().find("income_tiyn"), std::string::npos);
    Tax::TaxFilingRepository filings;
    EXPECT_EQ(filings.count_in_org(org.id), 0);
}

TEST_F(TaxFilingApiTest, FilingViewerForbidden) {
    auto org = seed_org("777180000005", "Filing Viewer Org LLP");
    auto viewer = member("filing-viewer5@example.com", org.id, "viewer");

    Tax::TaxService svc;
    auto calc = svc.calculate_snr(org.id, "2026-01-01", "2026-06-30");

    auto req =
        authed_json(viewer, json{{"kind", "910.00"}, {"calculation_id", calc.id}, {"document_input", fno910_extra()}});
    HttpResponsePtr resp;
    ctrl.createFiling(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(TaxFilingApiTest, FilingRejectsAnotherOrgsCalculation) {
    auto org_a = seed_org("777180000006", "Filing Cross A LLP");
    auto org_b = seed_org("777180000007", "Filing Cross B LLP");
    auto member_b = member("filing-cross-b@example.com", org_b.id, "accountant");

    Tax::TaxService svc;
    auto calc_a = svc.calculate_snr(org_a.id, "2026-01-01", "2026-06-30");

    auto req = authed_json(member_b,
                           json{{"kind", "910.00"}, {"calculation_id", calc_a.id}, {"document_input", fno910_extra()}});
    HttpResponsePtr resp;
    ctrl.createFiling(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    EXPECT_EQ(body_of(resp)["errors"][0]["code"].get<std::string>(), "foreign_calculation");
}

// ── GET /api/v1/tax/filings ──────────────────────────────────────────────────

TEST_F(TaxFilingApiTest, ListFilingsReturnsThisOrgsFilingsAndFiltersByKind) {
    auto org = seed_org("777180000012", "Filing List Org LLP");
    auto accountant = member("filing-acc12@example.com", org.id, "accountant");
    auto user = seed_user("filing-journal12@example.com");
    post_income(org.id, user.id, "2026-02-01", "1000000.00", std::string("160000.00"));

    Tax::TaxService svc;
    auto snr = svc.calculate_snr(org.id, "2026-01-01", "2026-06-30");
    auto vat = svc.calculate_vat(org.id, "2026-01-01", "2026-03-31");
    ASSERT_TRUE(create_filing(accountant, "910.00", snr.id, fno910_extra()));
    ASSERT_TRUE(create_filing(accountant, "300.00", vat.id, fno300_extra()));

    HttpResponsePtr all;
    ctrl.listFilings(authed(accountant), [&](const HttpResponsePtr& r) { all = r; });
    ASSERT_NE(all, nullptr);
    ASSERT_EQ(all->statusCode(), k200OK) << all->body();
    auto all_body = body_of(all);
    EXPECT_EQ(all_body["total"].get<long>(), 2);
    ASSERT_EQ(all_body["data"].size(), 2u);
    for (const auto& f : all_body["data"])
        EXPECT_EQ(f["org_id"].get<std::string>(), org.id);

    auto by_kind = authed(accountant);
    by_kind->setParameter("kind", "300.00");
    HttpResponsePtr filtered;
    ctrl.listFilings(by_kind, [&](const HttpResponsePtr& r) { filtered = r; });
    ASSERT_NE(filtered, nullptr);
    ASSERT_EQ(filtered->statusCode(), k200OK);
    auto filtered_body = body_of(filtered);
    EXPECT_EQ(filtered_body["total"].get<long>(), 1);
    ASSERT_EQ(filtered_body["data"].size(), 1u);
    EXPECT_EQ(filtered_body["data"][0]["kind"].get<std::string>(), "300.00");
    EXPECT_EQ(filtered_body["data"][0]["calculation_id"].get<std::string>(), vat.id);
}

TEST_F(TaxFilingApiTest, ListFilingsRejectsUnknownKindFilter) {
    auto org = seed_org("777180000013", "Filing List Filter Org LLP");
    auto viewer = member("filing-viewer13@example.com", org.id, "viewer");

    auto req = authed(viewer);
    req->setParameter("kind", "910");  // close, but not a registered form code
    HttpResponsePtr resp;
    ctrl.listFilings(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto errors = body_of(resp)["errors"];
    ASSERT_TRUE(errors.is_array());
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0]["field"].get<std::string>(), "kind");
    EXPECT_EQ(errors[0]["code"].get<std::string>(), "not_allowed");
}

TEST_F(TaxFilingApiTest, ListFilingsNeverLeaksAnotherTenantsFilings) {
    // Filings carry a whole company's tax position — the list must be scoped
    // to the caller's org claim and nothing else.
    auto org_a = seed_org("777180000014", "Filing Tenant A LLP");
    auto org_b = seed_org("777180000015", "Filing Tenant B LLP");
    auto member_a = member("filing-tenant-a@example.com", org_a.id, "accountant");
    auto member_b = member("filing-tenant-b@example.com", org_b.id, "accountant");

    Tax::TaxService svc;
    auto calc_a = svc.calculate_snr(org_a.id, "2026-01-01", "2026-06-30");
    auto filing_a = create_filing(member_a, "910.00", calc_a.id, fno910_extra());
    ASSERT_TRUE(filing_a);

    // B has its own filing, so "empty" cannot be mistaken for "list is broken".
    auto calc_b = svc.calculate_snr(org_b.id, "2026-07-01", "2026-12-31");
    auto filing_b = create_filing(member_b, "910.00", calc_b.id, fno910_extra());
    ASSERT_TRUE(filing_b);

    HttpResponsePtr resp;
    ctrl.listFilings(authed(member_b), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto listed = body_of(resp);
    EXPECT_EQ(listed["total"].get<long>(), 1);
    ASSERT_EQ(listed["data"].size(), 1u);
    EXPECT_EQ(listed["data"][0]["id"].get<std::string>(), *filing_b);
    EXPECT_EQ(listed["data"][0]["org_id"].get<std::string>(), org_b.id);
    // A's filing id must appear nowhere in B's page.
    for (const auto& f : listed["data"])
        EXPECT_NE(f["id"].get<std::string>(), *filing_a);
}

// ── GET /api/v1/tax/filings/{id} ─────────────────────────────────────────────

TEST_F(TaxFilingApiTest, GetFilingMalformedIdBadRequest) {
    auto org = seed_org("777180000016", "Filing Get Bad Id Org LLP");
    auto viewer = member("filing-viewer16@example.com", org.id, "viewer");

    HttpResponsePtr resp;
    ctrl.getFiling(
        authed(viewer), [&](const HttpResponsePtr& r) { resp = r; }, "not-a-uuid");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "invalid_id");
}

TEST_F(TaxFilingApiTest, GetFilingCrossOrgNotFound) {
    auto org_a = seed_org("777180000008", "Filing Get A LLP");
    auto org_b = seed_org("777180000009", "Filing Get B LLP");
    auto member_a = member("filing-get-a@example.com", org_a.id, "accountant");
    auto member_b = member("filing-get-b@example.com", org_b.id, "accountant");

    Tax::TaxService svc;
    auto calc = svc.calculate_snr(org_a.id, "2026-01-01", "2026-06-30");
    auto req = authed_json(member_a,
                           json{{"kind", "910.00"}, {"calculation_id", calc.id}, {"document_input", fno910_extra()}});
    HttpResponsePtr created;
    ctrl.createFiling(req, [&](const HttpResponsePtr& r) { created = r; });
    ASSERT_NE(created, nullptr);
    ASSERT_EQ(created->statusCode(), k202Accepted) << created->body();
    const std::string filing_id = body_of(created)["filing_id"].get<std::string>();

    HttpResponsePtr mine;
    ctrl.getFiling(
        authed(member_a), [&](const HttpResponsePtr& r) { mine = r; }, filing_id);
    ASSERT_NE(mine, nullptr);
    EXPECT_EQ(mine->statusCode(), k200OK);

    HttpResponsePtr theirs;
    ctrl.getFiling(
        authed(member_b), [&](const HttpResponsePtr& r) { theirs = r; }, filing_id);
    ASSERT_NE(theirs, nullptr);
    EXPECT_EQ(theirs->statusCode(), k404NotFound);
}

TEST_F(TaxFilingApiTest, DownloadUrlArtifactSwitch) {
    auto org = seed_org("777180000010", "Filing Download Org LLP");
    auto accountant = member("filing-acc10@example.com", org.id, "accountant");

    Tax::TaxService svc;
    auto calc = svc.calculate_snr(org.id, "2026-01-01", "2026-06-30");
    auto create_req = authed_json(
        accountant, json{{"kind", "910.00"}, {"calculation_id", calc.id}, {"document_input", fno910_extra()}});
    HttpResponsePtr created;
    ctrl.createFiling(create_req, [&](const HttpResponsePtr& r) { created = r; });
    ASSERT_NE(created, nullptr);
    ASSERT_EQ(created->statusCode(), k202Accepted) << created->body();
    const std::string filing_id = body_of(created)["filing_id"].get<std::string>();

    Tax::TaxFilingRepository filings;
    auto filing = filings.find_in_org(filing_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(filing);
    ASSERT_TRUE(filing->document_id);

    // The PDF does not exist until the render worker writes it — until then
    // ?artifact=pdf is a 409, not a 404 and not a URL to nothing.
    auto pending = authed(accountant, Post);
    pending->setParameter("artifact", "pdf");
    HttpResponsePtr not_rendered;
    ctrl.filingDownloadUrl(
        pending, [&](const HttpResponsePtr& r) { not_rendered = r; }, filing_id);
    ASSERT_NE(not_rendered, nullptr);
    EXPECT_EQ(not_rendered->statusCode(), k409Conflict);
    EXPECT_EQ(body_of(not_rendered)["error"].get<std::string>(), "not_rendered");

    // Stand in for the render worker: give the document a stored file.
    Ledger::DocumentRepository documents;
    const std::string pdf_key = "org/" + org.id + "/generated/rendered-fno-910.pdf";
    ASSERT_TRUE(documents.set_file(org.id,
                                   *filing->document_id,
                                   pdf_key,
                                   std::string(64, 'a'),
                                   "application/pdf",
                                   /*size_bytes=*/1024));

    auto xml_req = authed(accountant, Post);
    xml_req->setParameter("artifact", "xml");
    HttpResponsePtr xml_resp;
    ctrl.filingDownloadUrl(
        xml_req, [&](const HttpResponsePtr& r) { xml_resp = r; }, filing_id);
    ASSERT_NE(xml_resp, nullptr);
    ASSERT_EQ(xml_resp->statusCode(), k200OK) << xml_resp->body();
    auto xml_body = body_of(xml_resp);
    EXPECT_EQ(xml_body["artifact"].get<std::string>(), "xml");
    EXPECT_EQ(xml_body["key"].get<std::string>(), *filing->xml_s3_key);
    EXPECT_FALSE(xml_body["url"].get<std::string>().empty());

    auto pdf_req = authed(accountant, Post);
    pdf_req->setParameter("artifact", "pdf");
    HttpResponsePtr pdf_resp;
    ctrl.filingDownloadUrl(
        pdf_req, [&](const HttpResponsePtr& r) { pdf_resp = r; }, filing_id);
    ASSERT_NE(pdf_resp, nullptr);
    ASSERT_EQ(pdf_resp->statusCode(), k200OK) << pdf_resp->body();
    auto pdf_body = body_of(pdf_resp);
    EXPECT_EQ(pdf_body["artifact"].get<std::string>(), "pdf");
    EXPECT_EQ(pdf_body["key"].get<std::string>(), pdf_key);

    // The whole point of the switch: two artifacts, two different objects.
    EXPECT_NE(xml_body["key"].get<std::string>(), pdf_body["key"].get<std::string>());
    EXPECT_NE(xml_body["url"].get<std::string>(), pdf_body["url"].get<std::string>());

    // An unknown artifact is a semantically invalid VALUE -> 422.
    auto bogus = authed(accountant, Post);
    bogus->setParameter("artifact", "docx");
    HttpResponsePtr bogus_resp;
    ctrl.filingDownloadUrl(
        bogus, [&](const HttpResponsePtr& r) { bogus_resp = r; }, filing_id);
    ASSERT_NE(bogus_resp, nullptr);
    EXPECT_EQ(bogus_resp->statusCode(), k422UnprocessableEntity);
    EXPECT_EQ(body_of(bogus_resp)["errors"][0]["field"].get<std::string>(), "artifact");
}

TEST_F(TaxFilingApiTest, DownloadUrlMalformedIdBadRequest) {
    auto org = seed_org("777180000011", "Filing Bad Id Org LLP");
    auto viewer = member("filing-viewer11@example.com", org.id, "viewer");

    HttpResponsePtr resp;
    ctrl.filingDownloadUrl(
        authed(viewer, Post), [&](const HttpResponsePtr& r) { resp = r; }, "not-a-uuid");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

}  // namespace
