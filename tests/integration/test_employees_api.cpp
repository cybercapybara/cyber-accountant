/**
 * @file test_employees_api.cpp
 * @brief Integration tests for EmployeesController — Task 11.
 *
 * Follows the direct-controller-invocation idiom of test_counterparties_api.cpp
 * (stamp a hand-built AuthPrincipal + org claim onto a bare request, call the
 * handler, assert on the response). Covers, per route: happy path, the
 * viewer-mutation 403, cross-org 404, and the validation 422s the brief calls
 * out explicitly (IIN check digit, calendar-invalid date, unparseable salary).
 *
 * Valid IIN test fixtures below all pass Ledger::is_valid_bin_iin (the check-
 * digit algorithm, not just the 12-digit shape a plain regex would accept) —
 * generated offline against that exact algorithm.
 */

#include <optional>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/EmployeesController.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "hr/EmployeeRepository.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-employees-api-padding";

class EmployeesApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::EmployeesController ctrl;

    std::string config_file_name() const override { return "employees_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
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

    static Security::Auth::AuthPrincipal with_org(Security::Auth::AuthPrincipal p, const std::string& org_id) {
        p.org = org_id;
        return p;
    }

    /// Seed a user, add them to @p org_id with @p role, and return an
    /// org-scoped principal ready for authed()/authed_json().
    Security::Auth::AuthPrincipal member(const std::string& email, const std::string& org_id, const std::string& role) {
        auto user = seed_user(email);
        Security::Auth::AuthPrincipal p;
        p.subject = user.id;
        p.raw_claims = json{{"sub", user.id}};
        Tenancy::OrgMemberRepository members;
        members.add(org_id, user.id, role);
        return with_org(p, org_id);
    }

    static HttpRequestPtr authed(const Security::Auth::AuthPrincipal& p, HttpMethod method = Get) {
        return TestHelpers::authed(p, method);
    }

    static HttpRequestPtr authed_json(const Security::Auth::AuthPrincipal& p,
                                      const json& body,
                                      HttpMethod method = Post) {
        return TestHelpers::authed_json(p, body, method);
    }

    /// A full, valid create body — individual tests override one field.
    static json valid_create_body(const std::string& iin) {
        return json{
            {"iin", iin},
            {"last_name", "Серикбаева"},
            {"first_name", "Айгерим"},
            {"middle_name", "Кайратовна"},
            {"position", "Бухгалтер"},
            {"salary", "300000.00"},
            {"hired_on", "2026-01-15"},
        };
    }
};

// ── POST /api/v1/employees ──────────────────────────────────────────────────

TEST_F(EmployeesApiTest, CreateEmployeeSucceeds) {
    auto org = seed_org("333150000001", "Create Org LLP");
    auto accountant = member("accountant1@example.com", org.id, "accountant");

    auto req = authed_json(accountant, valid_create_body("156312191013"));
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k201Created);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["iin"].get<std::string>(), "156312191013");
    EXPECT_EQ(body["data"]["last_name"].get<std::string>(), "Серикбаева");
    EXPECT_EQ(body["data"]["salary_tiyn"].get<long long>(), 30000000);
    EXPECT_EQ(body["data"]["hired_on"].get<std::string>(), "2026-01-15");
    EXPECT_EQ(body["data"]["status"].get<std::string>(), "active");
    EXPECT_TRUE(body["data"]["dismissed_on"].is_null());
    EXPECT_FALSE(body["data"]["ipn_deduction_claimed"].get<bool>());
    EXPECT_FALSE(body["data"]["opvr_exempt"].get<bool>());

    Hr::EmployeeRepository repo;
    auto found = repo.find_in_org(body["data"]["id"].get<std::string>(), org.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->iin, "156312191013");
}

TEST_F(EmployeesApiTest, CreateViewerForbidden) {
    auto org = seed_org("333150000002", "Viewer Org LLP");
    auto viewer = member("viewer1@example.com", org.id, "viewer");

    auto req = authed_json(viewer, valid_create_body("988916681773"));
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(EmployeesApiTest, CreateInvalidIinRejected) {
    auto org = seed_org("333150000003", "Bad Iin Org LLP");
    auto accountant = member("accountant2@example.com", org.id, "accountant");

    auto body = valid_create_body("123");
    auto req = authed_json(accountant, body);
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    // "123" is not 12 digits — the DB CHAR(12) shape check would fail too,
    // but is_valid_bin_iin() rejects it first at the semantic-value layer.
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["errors"][0]["field"].get<std::string>(), "iin");
    EXPECT_EQ(resp_body["errors"][0]["code"].get<std::string>(), "invalid_iin");
}

TEST_F(EmployeesApiTest, CreateInvalidHiredOnRejected) {
    auto org = seed_org("333150000004", "Bad Date Org LLP");
    auto accountant = member("accountant3@example.com", org.id, "accountant");

    auto body = valid_create_body("312460130157");
    body["hired_on"] = "2026-02-30";
    auto req = authed_json(accountant, body);
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["errors"][0]["field"].get<std::string>(), "hired_on");
    EXPECT_EQ(resp_body["errors"][0]["code"].get<std::string>(), "invalid_date");
}

TEST_F(EmployeesApiTest, CreateInvalidSalaryRejected) {
    auto org = seed_org("333150000005", "Bad Salary Org LLP");
    auto accountant = member("accountant4@example.com", org.id, "accountant");

    auto body = valid_create_body("181385520307");
    body["salary"] = "abc";
    auto req = authed_json(accountant, body);
    HttpResponsePtr resp;
    ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["errors"][0]["field"].get<std::string>(), "salary");
    EXPECT_EQ(resp_body["errors"][0]["code"].get<std::string>(), "invalid_salary");
}

TEST_F(EmployeesApiTest, CreateDuplicateIinConflict) {
    auto org = seed_org("333150000006", "Dup Org LLP");
    auto accountant = member("accountant5@example.com", org.id, "accountant");

    auto req1 = authed_json(accountant, valid_create_body("160480216736"));
    HttpResponsePtr resp1;
    ctrl.create(req1, [&](const HttpResponsePtr& r) { resp1 = r; });
    ASSERT_EQ(resp1->statusCode(), k201Created);

    auto req2 = authed_json(accountant, valid_create_body("160480216736"));
    HttpResponsePtr resp2;
    ctrl.create(req2, [&](const HttpResponsePtr& r) { resp2 = r; });
    ASSERT_NE(resp2, nullptr);
    EXPECT_EQ(resp2->statusCode(), k409Conflict);
    auto body = json::parse(std::string(resp2->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "employee_iin_taken");
}

// ── GET /api/v1/employees ────────────────────────────────────────────────────

TEST_F(EmployeesApiTest, ListEmployeesPaginated) {
    auto org = seed_org("333150000007", "List Org LLP");
    auto accountant = member("accountant6@example.com", org.id, "accountant");

    for (const std::string& iin : {std::string{"768463260966"}, std::string{"776828107547"}}) {
        auto req = authed_json(accountant, valid_create_body(iin));
        HttpResponsePtr resp;
        ctrl.create(req, [&](const HttpResponsePtr& r) { resp = r; });
        ASSERT_EQ(resp->statusCode(), k201Created);
    }

    auto viewer = member("viewer2@example.com", org.id, "viewer");
    HttpResponsePtr list_resp;
    ctrl.list(authed(viewer), [&](const HttpResponsePtr& r) { list_resp = r; });
    ASSERT_NE(list_resp, nullptr);
    ASSERT_EQ(list_resp->statusCode(), k200OK);
    auto body = json::parse(std::string(list_resp->body()));
    EXPECT_EQ(body["total"].get<long>(), 2);
    ASSERT_TRUE(body["data"].is_array());
    EXPECT_EQ(body["data"].size(), 2U);
}

// ── GET /api/v1/employees/{id} ───────────────────────────────────────────────

TEST_F(EmployeesApiTest, GetEmployeeSucceeds) {
    auto org = seed_org("333150000008", "Get Org LLP");
    auto accountant = member("accountant7@example.com", org.id, "accountant");
    auto create_req = authed_json(accountant, valid_create_body("061071077377"));
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    HttpResponsePtr resp;
    ctrl.get(authed(accountant), [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["id"].get<std::string>(), created_id);
}

TEST_F(EmployeesApiTest, GetEmployeeCrossOrgNotFound) {
    auto org_a = seed_org("333150000009", "Org A LLP");
    auto org_b = seed_org("333150000010", "Org B LLP");
    auto accountant_a = member("accountant8@example.com", org_a.id, "accountant");
    auto accountant_b = member("accountant9@example.com", org_b.id, "accountant");

    auto create_req = authed_json(accountant_a, valid_create_body("087179126861"));
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    HttpResponsePtr resp;
    ctrl.get(authed(accountant_b), [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

TEST_F(EmployeesApiTest, GetEmployeeMalformedIdRejected) {
    auto org = seed_org("333150000024", "Malformed Id Org LLP");
    auto accountant = member("accountant23@example.com", org.id, "accountant");

    HttpResponsePtr resp;
    ctrl.get(authed(accountant), [&](const HttpResponsePtr& r) { resp = r; }, "not-a-uuid");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

// ── PATCH /api/v1/employees/{id} ─────────────────────────────────────────────

TEST_F(EmployeesApiTest, PatchEmployeeSucceeds) {
    auto org = seed_org("333150000011", "Patch Org LLP");
    auto accountant = member("accountant10@example.com", org.id, "accountant");
    auto create_req = authed_json(accountant, valid_create_body("310038140276"));
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    json patch_body = {
        {"iin", "310038140276"},
        {"last_name", "Серикбаева"},
        {"first_name", "Айгерим"},
        {"position", "Главный бухгалтер"},
        {"salary", "450000.00"},
    };
    auto patch_req = authed_json(accountant, patch_body, Patch);
    HttpResponsePtr resp;
    ctrl.patch(patch_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["position"].get<std::string>(), "Главный бухгалтер");
    EXPECT_EQ(body["data"]["salary_tiyn"].get<long long>(), 45000000);
    // hired_on is untouched by patch (EmployeeRepository::update() never sets it).
    EXPECT_EQ(body["data"]["hired_on"].get<std::string>(), "2026-01-15");
}

TEST_F(EmployeesApiTest, PatchViewerForbidden) {
    auto org = seed_org("333150000012", "Patch Viewer Org LLP");
    auto accountant = member("accountant11@example.com", org.id, "accountant");
    auto viewer = member("viewer3@example.com", org.id, "viewer");
    auto create_req = authed_json(accountant, valid_create_body("837748630575"));
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    json patch_body = {
        {"iin", "837748630575"}, {"last_name", "X"}, {"first_name", "Y"}, {"position", "Z"}, {"salary", "1.00"}};
    auto patch_req = authed_json(viewer, patch_body, Patch);
    HttpResponsePtr resp;
    ctrl.patch(patch_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(EmployeesApiTest, PatchCrossOrgNotFound) {
    auto org_a = seed_org("333150000013", "Patch A LLP");
    auto org_b = seed_org("333150000014", "Patch B LLP");
    auto accountant_a = member("accountant12@example.com", org_a.id, "accountant");
    auto accountant_b = member("accountant13@example.com", org_b.id, "accountant");

    auto create_req = authed_json(accountant_a, valid_create_body("731284151557"));
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    json patch_body = {
        {"iin", "731284151557"}, {"last_name", "X"}, {"first_name", "Y"}, {"position", "Z"}, {"salary", "1.00"}};
    auto patch_req = authed_json(accountant_b, patch_body, Patch);
    HttpResponsePtr resp;
    ctrl.patch(patch_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

TEST_F(EmployeesApiTest, PatchInvalidIinRejected) {
    auto org = seed_org("333150000015", "Patch Invalid Org LLP");
    auto accountant = member("accountant14@example.com", org.id, "accountant");
    auto create_req = authed_json(accountant, valid_create_body("953432817050"));
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    json patch_body = {{"iin", "123"}, {"last_name", "X"}, {"first_name", "Y"}, {"position", "Z"}, {"salary", "1.00"}};
    auto patch_req = authed_json(accountant, patch_body, Patch);
    HttpResponsePtr resp;
    ctrl.patch(patch_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "iin");
}

TEST_F(EmployeesApiTest, PatchInvalidSalaryRejected) {
    auto org = seed_org("333150000016", "Patch Salary Org LLP");
    auto accountant = member("accountant15@example.com", org.id, "accountant");
    auto create_req = authed_json(accountant, valid_create_body("972805129058"));
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    json patch_body = {
        {"iin", "972805129058"}, {"last_name", "X"}, {"first_name", "Y"}, {"position", "Z"}, {"salary", "abc"}};
    auto patch_req = authed_json(accountant, patch_body, Patch);
    HttpResponsePtr resp;
    ctrl.patch(patch_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "salary");
}

// PATCH semantics decision (EmployeesController.hpp file header): lifecycle
// fields are REJECTED with 422, not silently dropped.
TEST_F(EmployeesApiTest, PatchHiredOnRejected) {
    auto org = seed_org("333150000017", "Patch Hired On Org LLP");
    auto accountant = member("accountant16@example.com", org.id, "accountant");
    auto create_req = authed_json(accountant, valid_create_body("581750806942"));
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    json patch_body = {{"iin", "581750806942"},
                       {"last_name", "X"},
                       {"first_name", "Y"},
                       {"position", "Z"},
                       {"salary", "1.00"},
                       {"hired_on", "2026-02-01"}};
    auto patch_req = authed_json(accountant, patch_body, Patch);
    HttpResponsePtr resp;
    ctrl.patch(patch_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "hired_on");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "immutable_field");
}

TEST_F(EmployeesApiTest, PatchDismissedOnRejected) {
    auto org = seed_org("333150000018", "Patch Dismissed On Org LLP");
    auto accountant = member("accountant17@example.com", org.id, "accountant");
    auto create_req = authed_json(accountant, valid_create_body("610763273927"));
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    json patch_body = {{"iin", "610763273927"},
                       {"last_name", "X"},
                       {"first_name", "Y"},
                       {"position", "Z"},
                       {"salary", "1.00"},
                       {"dismissed_on", "2026-02-01"}};
    auto patch_req = authed_json(accountant, patch_body, Patch);
    HttpResponsePtr resp;
    ctrl.patch(patch_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "dismissed_on");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "use_dismiss_endpoint");
}

// ── POST /api/v1/employees/{id}/dismiss ─────────────────────────────────────

TEST_F(EmployeesApiTest, DismissEmployeeSucceeds) {
    auto org = seed_org("333150000019", "Dismiss Org LLP");
    auto accountant = member("accountant18@example.com", org.id, "accountant");
    auto create_req = authed_json(accountant, valid_create_body("368906152128"));
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    auto dismiss_req = authed_json(accountant, json{{"dismissed_on", "2026-06-30"}});
    HttpResponsePtr resp;
    ctrl.dismiss(dismiss_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["status"].get<std::string>(), "dismissed");
    EXPECT_EQ(body["data"]["dismissed_on"].get<std::string>(), "2026-06-30");
}

TEST_F(EmployeesApiTest, DismissViewerForbidden) {
    auto org = seed_org("333150000020", "Dismiss Viewer Org LLP");
    auto accountant = member("accountant19@example.com", org.id, "accountant");
    auto viewer = member("viewer4@example.com", org.id, "viewer");
    auto create_req = authed_json(accountant, valid_create_body("588330607309"));
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    auto dismiss_req = authed_json(viewer, json{{"dismissed_on", "2026-06-30"}});
    HttpResponsePtr resp;
    ctrl.dismiss(dismiss_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(EmployeesApiTest, DismissCrossOrgNotFound) {
    auto org_a = seed_org("333150000021", "Dismiss A LLP");
    auto org_b = seed_org("333150000022", "Dismiss B LLP");
    auto accountant_a = member("accountant20@example.com", org_a.id, "accountant");
    auto accountant_b = member("accountant21@example.com", org_b.id, "accountant");

    auto create_req = authed_json(accountant_a, valid_create_body("358211032228"));
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    auto dismiss_req = authed_json(accountant_b, json{{"dismissed_on", "2026-06-30"}});
    HttpResponsePtr resp;
    ctrl.dismiss(dismiss_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

TEST_F(EmployeesApiTest, DismissInvalidDateRejected) {
    auto org = seed_org("333150000023", "Dismiss Bad Date Org LLP");
    auto accountant = member("accountant22@example.com", org.id, "accountant");
    auto create_req = authed_json(accountant, valid_create_body("249358038376"));
    HttpResponsePtr create_resp;
    ctrl.create(create_req, [&](const HttpResponsePtr& r) { create_resp = r; });
    ASSERT_EQ(create_resp->statusCode(), k201Created);
    auto created_id = json::parse(std::string(create_resp->body()))["data"]["id"].get<std::string>();

    auto dismiss_req = authed_json(accountant, json{{"dismissed_on", "2026-02-30"}});
    HttpResponsePtr resp;
    ctrl.dismiss(dismiss_req, [&](const HttpResponsePtr& r) { resp = r; }, created_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "dismissed_on");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "invalid_date");
}

}  // namespace
