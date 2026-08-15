/**
 * @file test_hr_api.cpp
 * @brief Integration tests for HrController (hr-orders / labor-contracts /
 *        vacations, plus the two generate-document endpoints) — Task 11.
 *
 * Follows the direct-controller-invocation idiom of test_counterparties_api.cpp
 * / test_docgen_api.cpp. The generate-document tests assert a docgen.render
 * job actually landed on the queue (same idiom as test_docgen_api.cpp) —
 * never that a PDF was produced.
 */

#include <filesystem>
#include <optional>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/HrController.hpp"
#include "cache/Cache.hpp"
#include "database/Database.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "hr/Employee.hpp"
#include "hr/EmployeeRepository.hpp"
#include "hr/HrRepository.hpp"
#include "jobs/Jobs.hpp"
#include "ledger/DocumentRepository.hpp"
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

constexpr const char* kSecret = "test-jwt-secret-for-hr-api-padding";
constexpr const char* kRenderJobType = "docgen.render";

class HrApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::HrController ctrl;

    std::string config_file_name() const override { return "hr_api_test_config.json"; }

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
        if (!fs::exists("templates/latex/hr_order/v1/schema.json"))
            GTEST_SKIP() << "repo templates not reachable from this working directory";

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

    /// employee_id query param -> a bare GET request (authed(), Get by default).
    static HttpRequestPtr authed_with_query(const Security::Auth::AuthPrincipal& p,
                                            const std::string& query,
                                            HttpMethod method = Get) {
        auto req = TestHelpers::authed(p, method);
        req->setParameter("employee_id", query);
        return req;
    }

    Hr::Employee seed_employee(const std::string& org_id, const std::string& iin) {
        Hr::EmployeeRepository repo;
        Hr::Employee draft;
        draft.iin = iin;
        draft.last_name = "Серикбаева";
        draft.first_name = "Айгерим";
        draft.middle_name = "Кайратовна";
        draft.position = "Бухгалтер";
        draft.salary_tiyn = 30000000;
        draft.hired_on = "2026-01-15";
        draft.payout_iik = "";
        return repo.create(org_id, draft);
    }

    static long queue_depth() {
        return static_cast<long>(Cache::get().get_client().llen(Jobs::queue_key(kRenderJobType)));
    }
};

// ── POST /api/v1/hr-orders ───────────────────────────────────────────────────

TEST_F(HrApiTest, CreateOrderSucceeds) {
    auto org = seed_org("777150000001", "Order Org LLP");
    auto accountant = member("accountant1@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "156312191013");

    json body = {{"employee_id", employee.id},
                 {"kind", "hire"},
                 {"number", "42"},
                 {"issued_on", "2026-01-10"},
                 {"effective_from", "2026-01-15"}};
    auto req = authed_json(accountant, body);
    HttpResponsePtr resp;
    ctrl.createOrder(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k201Created);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["data"]["kind"].get<std::string>(), "hire");
    EXPECT_EQ(resp_body["data"]["employee_id"].get<std::string>(), employee.id);
}

TEST_F(HrApiTest, CreateOrderViewerForbidden) {
    auto org = seed_org("777150000002", "Order Viewer Org LLP");
    auto viewer = member("viewer1@example.com", org.id, "viewer");
    auto employee = seed_employee(org.id, "988916681773");

    json body = {{"employee_id", employee.id},
                 {"kind", "hire"},
                 {"number", "1"},
                 {"issued_on", "2026-01-10"},
                 {"effective_from", "2026-01-15"}};
    auto req = authed_json(viewer, body);
    HttpResponsePtr resp;
    ctrl.createOrder(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(HrApiTest, CreateOrderInvalidKindRejected) {
    auto org = seed_org("777150000003", "Order Bad Kind Org LLP");
    auto accountant = member("accountant2@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "312460130157");

    json body = {{"employee_id", employee.id},
                 {"kind", "not_a_kind"},
                 {"number", "1"},
                 {"issued_on", "2026-01-10"},
                 {"effective_from", "2026-01-15"}};
    auto req = authed_json(accountant, body);
    HttpResponsePtr resp;
    ctrl.createOrder(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["errors"][0]["field"].get<std::string>(), "kind");
}

TEST_F(HrApiTest, CreateOrderInvalidDateRejected) {
    auto org = seed_org("777150000004", "Order Bad Date Org LLP");
    auto accountant = member("accountant3@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "181385520307");

    json body = {{"employee_id", employee.id},
                 {"kind", "hire"},
                 {"number", "1"},
                 {"issued_on", "2026-02-30"},
                 {"effective_from", "2026-01-15"}};
    auto req = authed_json(accountant, body);
    HttpResponsePtr resp;
    ctrl.createOrder(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["errors"][0]["field"].get<std::string>(), "issued_on");
    EXPECT_EQ(resp_body["errors"][0]["code"].get<std::string>(), "invalid_date");
}

TEST_F(HrApiTest, CreateOrderForeignEmployeeRejected) {
    auto org_a = seed_org("777150000005", "Order Org A LLP");
    auto org_b = seed_org("777150000006", "Order Org B LLP");
    auto accountant_b = member("accountant4@example.com", org_b.id, "accountant");
    auto employee_a = seed_employee(org_a.id, "160480216736");

    json body = {{"employee_id", employee_a.id},
                 {"kind", "hire"},
                 {"number", "1"},
                 {"issued_on", "2026-01-10"},
                 {"effective_from", "2026-01-15"}};
    auto req = authed_json(accountant_b, body);
    HttpResponsePtr resp;
    ctrl.createOrder(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["errors"][0]["field"].get<std::string>(), "employee_id");
    EXPECT_EQ(resp_body["errors"][0]["code"].get<std::string>(), "foreign_employee");
}

// ── GET /api/v1/hr-orders ────────────────────────────────────────────────────

TEST_F(HrApiTest, ListOrdersFilteredByEmployee) {
    auto org = seed_org("777150000007", "Order List Org LLP");
    auto accountant = member("accountant5@example.com", org.id, "accountant");
    auto employee1 = seed_employee(org.id, "768463260966");
    auto employee2 = seed_employee(org.id, "776828107547");

    Hr::HrRepository repo;
    repo.create_order(org.id, employee1.id, "hire", "1", "2026-01-10", "2026-01-15");
    repo.create_order(org.id, employee2.id, "hire", "2", "2026-01-11", "2026-01-16");

    HttpResponsePtr resp;
    ctrl.listOrders(authed_with_query(accountant, employee1.id), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    ASSERT_TRUE(body["data"].is_array());
    ASSERT_EQ(body["data"].size(), 1U);
    EXPECT_EQ(body["data"][0]["employee_id"].get<std::string>(), employee1.id);
}

// Final fix round: GET /hr-orders is paginated — `hr_orders` grows for the
// life of an organization, so the route used to be an unbounded org-wide
// SELECT. The envelope is now {data, total, limit, offset}, and ?limit
// really bounds the page while `total` still reports the full count.
TEST_F(HrApiTest, ListOrdersIsPaginated) {
    auto org = seed_org("777150000040", "Order Page Org LLP");
    auto accountant = member("accountant40@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "156312191013");

    Hr::HrRepository repo;
    repo.create_order(org.id, employee.id, "hire", "1", "2026-01-10", "2026-01-15");
    repo.create_order(org.id, employee.id, "vacation", "2", "2026-01-11", "2026-01-16");
    repo.create_order(org.id, employee.id, "vacation", "3", "2026-01-12", "2026-01-17");

    auto first_req = TestHelpers::authed(accountant, Get);
    first_req->setParameter("limit", "2");
    HttpResponsePtr first;
    ctrl.listOrders(first_req, [&](const HttpResponsePtr& r) { first = r; });
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(first->statusCode(), k200OK);
    auto page1 = json::parse(std::string(first->body()));
    EXPECT_EQ(page1["data"].size(), 2U);
    EXPECT_EQ(page1["total"].get<long>(), 3);
    EXPECT_EQ(page1["limit"].get<int>(), 2);
    EXPECT_EQ(page1["offset"].get<int>(), 0);

    auto second_req = TestHelpers::authed(accountant, Get);
    second_req->setParameter("limit", "2");
    second_req->setParameter("offset", "2");
    HttpResponsePtr second;
    ctrl.listOrders(second_req, [&](const HttpResponsePtr& r) { second = r; });
    ASSERT_NE(second, nullptr);
    auto page2 = json::parse(std::string(second->body()));
    EXPECT_EQ(page2["data"].size(), 1U);
    EXPECT_EQ(page2["total"].get<long>(), 3);
    EXPECT_EQ(page2["offset"].get<int>(), 2);
}

// ── POST /api/v1/hr-orders/{id}/generate-document ───────────────────────────

TEST_F(HrApiTest, GenerateOrderDocumentAcceptedAndEnqueues) {
    auto org = seed_org("777150000008", "Order Gen Org LLP");
    auto accountant = member("accountant6@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "061071077377");

    Hr::HrRepository repo;
    auto order = repo.create_order(org.id, employee.id, "hire", "42", "2026-01-10", "2026-01-15");

    json extra = {{"director", "Ахметов Ерлан Серикович"}};
    auto req = authed_json(accountant, extra);
    HttpResponsePtr resp;
    ctrl.generateOrderDocument(
        req, [&](const HttpResponsePtr& r) { resp = r; }, order.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted);
    auto body = json::parse(std::string(resp->body()));
    const std::string document_id = body["document_id"].get<std::string>();
    ASSERT_FALSE(document_id.empty());
    EXPECT_TRUE(body["render_queued"].get<bool>());

    Ledger::DocumentRepository documents;
    auto doc = documents.find_in_org(document_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->doc_type, "hr");
    EXPECT_EQ(doc->source, "generated");
    EXPECT_EQ(doc->template_slug.value_or(""), "hr_order");
    ASSERT_TRUE(doc->input_snapshot.has_value());
    EXPECT_EQ((*doc->input_snapshot)["director"].get<std::string>(), "Ахметов Ерлан Серикович");
    EXPECT_EQ((*doc->input_snapshot)["issued_on"].get<std::string>(), "10.01.2026");
    EXPECT_EQ((*doc->input_snapshot)["employee"]["iin"].get<std::string>(), "061071077377");

    auto refreshed_order = repo.find_in_org(order.id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(refreshed_order.has_value());
    EXPECT_EQ(refreshed_order->document_id.value_or(""), document_id);

    ASSERT_EQ(queue_depth(), 1);
}

TEST_F(HrApiTest, GenerateOrderDocumentViewerForbidden) {
    auto org = seed_org("777150000009", "Order Gen Viewer Org LLP");
    auto viewer = member("viewer2@example.com", org.id, "viewer");
    auto employee = seed_employee(org.id, "087179126861");

    Hr::HrRepository repo;
    auto order = repo.create_order(org.id, employee.id, "hire", "1", "2026-01-10", "2026-01-15");

    auto req = authed(viewer, Post);
    HttpResponsePtr resp;
    ctrl.generateOrderDocument(
        req, [&](const HttpResponsePtr& r) { resp = r; }, order.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
    EXPECT_EQ(queue_depth(), 0);
}

TEST_F(HrApiTest, GenerateOrderDocumentCrossOrgNotFound) {
    auto org_a = seed_org("777150000010", "Order Gen A LLP");
    auto org_b = seed_org("777150000011", "Order Gen B LLP");
    auto accountant_b = member("accountant8@example.com", org_b.id, "accountant");
    auto employee_a = seed_employee(org_a.id, "310038140276");

    Hr::HrRepository repo;
    auto order = repo.create_order(org_a.id, employee_a.id, "hire", "1", "2026-01-10", "2026-01-15");

    auto req = authed(accountant_b, Post);
    HttpResponsePtr resp;
    ctrl.generateOrderDocument(
        req, [&](const HttpResponsePtr& r) { resp = r; }, order.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
    EXPECT_EQ(queue_depth(), 0);
}

TEST_F(HrApiTest, GenerateOrderDocumentMissingRequiredFieldRejected) {
    auto org = seed_org("777150000012", "Order Gen Missing Org LLP");
    auto accountant = member("accountant9@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "837748630575");

    Hr::HrRepository repo;
    auto order = repo.create_order(org.id, employee.id, "hire", "1", "2026-01-10", "2026-01-15");

    // No body -> "director" (required by hr_order's schema, not derivable
    // from any stored column) is missing.
    auto req = authed(accountant, Post);
    HttpResponsePtr resp;
    ctrl.generateOrderDocument(
        req, [&](const HttpResponsePtr& r) { resp = r; }, order.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "input");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "schema_validation_failed");
    EXPECT_EQ(queue_depth(), 0);
}

// ── POST /api/v1/labor-contracts ─────────────────────────────────────────────

TEST_F(HrApiTest, CreateContractSucceeds) {
    auto org = seed_org("777150000013", "Contract Org LLP");
    auto accountant = member("accountant10@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "731284151557");

    json body = {
        {"employee_id", employee.id}, {"number", "17"}, {"signed_on", "2026-01-10"}, {"starts_on", "2026-01-15"}};
    auto req = authed_json(accountant, body);
    HttpResponsePtr resp;
    ctrl.createContract(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k201Created);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["data"]["number"].get<std::string>(), "17");
    EXPECT_EQ(resp_body["data"]["employee_id"].get<std::string>(), employee.id);
}

TEST_F(HrApiTest, CreateContractViewerForbidden) {
    auto org = seed_org("777150000014", "Contract Viewer Org LLP");
    auto viewer = member("viewer3@example.com", org.id, "viewer");
    auto employee = seed_employee(org.id, "953432817050");

    json body = {
        {"employee_id", employee.id}, {"number", "1"}, {"signed_on", "2026-01-10"}, {"starts_on", "2026-01-15"}};
    auto req = authed_json(viewer, body);
    HttpResponsePtr resp;
    ctrl.createContract(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(HrApiTest, CreateContractInvalidDateRejected) {
    auto org = seed_org("777150000015", "Contract Bad Date Org LLP");
    auto accountant = member("accountant11@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "972805129058");

    json body = {
        {"employee_id", employee.id}, {"number", "1"}, {"signed_on", "2026-02-30"}, {"starts_on", "2026-01-15"}};
    auto req = authed_json(accountant, body);
    HttpResponsePtr resp;
    ctrl.createContract(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["errors"][0]["field"].get<std::string>(), "signed_on");
    EXPECT_EQ(resp_body["errors"][0]["code"].get<std::string>(), "invalid_date");
}

TEST_F(HrApiTest, CreateContractForeignEmployeeRejected) {
    auto org_a = seed_org("777150000016", "Contract Org A LLP");
    auto org_b = seed_org("777150000017", "Contract Org B LLP");
    auto accountant_b = member("accountant12@example.com", org_b.id, "accountant");
    auto employee_a = seed_employee(org_a.id, "581750806942");

    json body = {
        {"employee_id", employee_a.id}, {"number", "1"}, {"signed_on", "2026-01-10"}, {"starts_on", "2026-01-15"}};
    auto req = authed_json(accountant_b, body);
    HttpResponsePtr resp;
    ctrl.createContract(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["errors"][0]["field"].get<std::string>(), "employee_id");
    EXPECT_EQ(resp_body["errors"][0]["code"].get<std::string>(), "foreign_employee");
}

// ── GET /api/v1/labor-contracts ──────────────────────────────────────────────

TEST_F(HrApiTest, ListContractsRequiresEmployeeId) {
    auto org = seed_org("777150000018", "Contract List Org LLP");
    auto accountant = member("accountant13@example.com", org.id, "accountant");

    HttpResponsePtr resp;
    ctrl.listContracts(authed(accountant), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(HrApiTest, ListContractsSucceeds) {
    auto org = seed_org("777150000019", "Contract List Org 2 LLP");
    auto accountant = member("accountant14@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "610763273927");

    Hr::HrRepository repo;
    repo.create_contract(org.id, employee.id, "17", "2026-01-10", "2026-01-15");

    HttpResponsePtr resp;
    ctrl.listContracts(authed_with_query(accountant, employee.id), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    ASSERT_TRUE(body["data"].is_array());
    EXPECT_EQ(body["data"].size(), 1U);
}

// ── POST /api/v1/labor-contracts/{id}/generate-document ─────────────────────

TEST_F(HrApiTest, GenerateContractDocumentAcceptedAndEnqueues) {
    auto org = seed_org("777150000020", "Contract Gen Org LLP");
    auto accountant = member("accountant15@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "368906152128");

    Hr::HrRepository repo;
    auto contract = repo.create_contract(org.id, employee.id, "17", "2026-01-10", "2026-01-15");

    // `salary_words`/`salary_words_kk` are NOT here: after P3 both are
    // derived from employees.salary_tiyn, and sending either is a 422.
    json extra = {{"employer", {{"director", "Ахметов Ерлан Серикович"}}},
                  {"work_schedule", "Пятидневная рабочая неделя"}};
    auto req = authed_json(accountant, extra);
    HttpResponsePtr resp;
    ctrl.generateContractDocument(
        req, [&](const HttpResponsePtr& r) { resp = r; }, contract.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted);
    auto body = json::parse(std::string(resp->body()));
    const std::string document_id = body["document_id"].get<std::string>();
    ASSERT_FALSE(document_id.empty());
    EXPECT_TRUE(body["render_queued"].get<bool>());

    Ledger::DocumentRepository documents;
    auto doc = documents.find_in_org(document_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->doc_type, "hr");
    EXPECT_EQ(doc->template_slug.value_or(""), "labor_contract");
    ASSERT_TRUE(doc->input_snapshot.has_value());
    EXPECT_EQ((*doc->input_snapshot)["salary_tenge"].get<std::string>(), "300000.00");
    EXPECT_EQ((*doc->input_snapshot)["employer"]["director"].get<std::string>(), "Ахметов Ерлан Серикович");
    EXPECT_EQ((*doc->input_snapshot)["employer"]["name"].get<std::string>(), org.name);
    // Both spellings come from the ONE integer salary_tenge was formatted
    // from, so the digits and the two texts cannot disagree; the Kazakh one
    // is a distinct string, not a copy of the Russian.
    EXPECT_EQ((*doc->input_snapshot)["salary_words"].get<std::string>(), "Триста тысяч тенге 00 тиын");
    EXPECT_FALSE((*doc->input_snapshot)["salary_words_kk"].get<std::string>().empty());
    EXPECT_NE((*doc->input_snapshot)["salary_words_kk"].get<std::string>(),
              (*doc->input_snapshot)["salary_words"].get<std::string>());

    ASSERT_EQ(queue_depth(), 1);
}

TEST_F(HrApiTest, GenerateContractDocumentViewerForbidden) {
    auto org = seed_org("777150000021", "Contract Gen Viewer Org LLP");
    auto viewer = member("viewer4@example.com", org.id, "viewer");
    auto employee = seed_employee(org.id, "588330607309");

    Hr::HrRepository repo;
    auto contract = repo.create_contract(org.id, employee.id, "1", "2026-01-10", "2026-01-15");

    auto req = authed(viewer, Post);
    HttpResponsePtr resp;
    ctrl.generateContractDocument(
        req, [&](const HttpResponsePtr& r) { resp = r; }, contract.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
    EXPECT_EQ(queue_depth(), 0);
}

TEST_F(HrApiTest, GenerateContractDocumentCrossOrgNotFound) {
    auto org_a = seed_org("777150000022", "Contract Gen A LLP");
    auto org_b = seed_org("777150000023", "Contract Gen B LLP");
    auto accountant_b = member("accountant16@example.com", org_b.id, "accountant");
    auto employee_a = seed_employee(org_a.id, "358211032228");

    Hr::HrRepository repo;
    auto contract = repo.create_contract(org_a.id, employee_a.id, "1", "2026-01-10", "2026-01-15");

    auto req = authed(accountant_b, Post);
    HttpResponsePtr resp;
    ctrl.generateContractDocument(
        req, [&](const HttpResponsePtr& r) { resp = r; }, contract.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
    EXPECT_EQ(queue_depth(), 0);
}

TEST_F(HrApiTest, GenerateContractDocumentMissingRequiredFieldRejected) {
    auto org = seed_org("777150000024", "Contract Gen Missing Org LLP");
    auto accountant = member("accountant17@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "156312191013");

    Hr::HrRepository repo;
    auto contract = repo.create_contract(org.id, employee.id, "1", "2026-01-10", "2026-01-15");

    // No body -> work_schedule/employer.director missing (the two *_words
    // fields the server now derives are already present).
    auto req = authed(accountant, Post);
    HttpResponsePtr resp;
    ctrl.generateContractDocument(
        req, [&](const HttpResponsePtr& r) { resp = r; }, contract.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "input");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "schema_validation_failed");
    EXPECT_EQ(queue_depth(), 0);
}

TEST_F(HrApiTest, GenerateContractDocumentMalformedIdRejected) {
    auto org = seed_org("777150000037", "Contract Malformed Id Org LLP");
    auto accountant = member("accountant28@example.com", org.id, "accountant");

    auto req = authed(accountant, Post);
    HttpResponsePtr resp;
    ctrl.generateContractDocument(
        req, [&](const HttpResponsePtr& r) { resp = r; }, "not-a-uuid");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

// Fix round 1 (security): same allowlist guarantee as
// GenerateOrderDocumentOverrideIinRejectedAndValueUnchanged, for the
// labor_contract variant — an attempt to override the authoritative
// salary_tenge (derived from the employee's stored salary_tiyn) is
// rejected outright, and a subsequent legitimate request still produces a
// document whose stored input carries the REAL salary, never the attempt.
TEST_F(HrApiTest, GenerateContractDocumentOverrideSalaryRejectedAndValueUnchanged) {
    auto org = seed_org("777150000038", "Contract Override Org LLP");
    auto accountant = member("accountant29@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "312460130157");

    Hr::HrRepository repo;
    auto contract = repo.create_contract(org.id, employee.id, "1", "2026-01-10", "2026-01-15");

    json malicious = {{"salary_tenge", "999999.00"},
                      {"work_schedule", "Пятидневная рабочая неделя"},
                      {"employer", {{"director", "Ахметов Ерлан Серикович"}}}};
    auto bad_req = authed_json(accountant, malicious);
    HttpResponsePtr bad_resp;
    ctrl.generateContractDocument(
        bad_req, [&](const HttpResponsePtr& r) { bad_resp = r; }, contract.id);
    ASSERT_NE(bad_resp, nullptr);
    EXPECT_EQ(bad_resp->statusCode(), k422UnprocessableEntity);
    auto bad_body = json::parse(std::string(bad_resp->body()));
    EXPECT_EQ(bad_body["errors"][0]["field"].get<std::string>(), "salary_tenge");
    EXPECT_EQ(bad_body["errors"][0]["code"].get<std::string>(), "not_allowed_override");
    EXPECT_EQ(queue_depth(), 0);

    json legit = {{"work_schedule", "Пятидневная рабочая неделя"},
                  {"employer", {{"director", "Ахметов Ерлан Серикович"}}}};
    auto good_req = authed_json(accountant, legit);
    HttpResponsePtr good_resp;
    ctrl.generateContractDocument(
        good_req, [&](const HttpResponsePtr& r) { good_resp = r; }, contract.id);
    ASSERT_NE(good_resp, nullptr);
    ASSERT_EQ(good_resp->statusCode(), k202Accepted);
    auto good_body = json::parse(std::string(good_resp->body()));
    const std::string document_id = good_body["document_id"].get<std::string>();

    Ledger::DocumentRepository documents;
    auto doc = documents.find_in_org(document_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(doc.has_value());
    ASSERT_TRUE(doc->input_snapshot.has_value());
    EXPECT_EQ((*doc->input_snapshot)["salary_tenge"].get<std::string>(), "300000.00");
}

// P3: the amount spelled out in words used to be caller-supplied, so a
// contract could print "300000.00" next to "Один тенге 00 тиын". Both
// spellings are server-derived now, which makes either of them in the body a
// 422 on the existing allowlist — no new mechanism, one fewer allowlisted
// field.
TEST_F(HrApiTest, GenerateContractDocumentRejectsClientSuppliedSalaryWords) {
    auto org = seed_org("777150000039", "Contract Words Org LLP");
    auto accountant = member("accountant30@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "156312191013");

    Hr::HrRepository repo;
    auto contract = repo.create_contract(org.id, employee.id, "2", "2026-01-10", "2026-01-15");

    json body = {{"salary_words", "Один тенге 00 тиын"},
                 {"work_schedule", "Пятидневная рабочая неделя"},
                 {"employer", {{"director", "Ахметов Ерлан Серикович"}}}};
    auto req = authed_json(accountant, body);
    HttpResponsePtr resp;
    ctrl.generateContractDocument(
        req, [&](const HttpResponsePtr& r) { resp = r; }, contract.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto payload = json::parse(std::string(resp->body()));
    EXPECT_EQ(payload["errors"][0]["field"].get<std::string>(), "salary_words");
    EXPECT_EQ(payload["errors"][0]["code"].get<std::string>(), "not_allowed_override");
    EXPECT_EQ(queue_depth(), 0);
}

TEST_F(HrApiTest, GenerateOrderDocumentMalformedIdRejected) {
    auto org = seed_org("777150000033", "Order Malformed Id Org LLP");
    auto accountant = member("accountant24@example.com", org.id, "accountant");

    auto req = authed(accountant, Post);
    HttpResponsePtr resp;
    ctrl.generateOrderDocument(
        req, [&](const HttpResponsePtr& r) { resp = r; }, "not-a-uuid");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

// Fix round 1 (security): the extra body merged onto a generate-document's
// auto-derived input is now allowlisted (HrController.hpp's
// hr_order_allowed_extra_fields()) — an attempt to override an
// authoritative, database-derived field (here: employee.iin) must be
// rejected outright, and a subsequent LEGITIMATE request (allowed field
// only) must still produce a document whose stored input carries the REAL
// employee iin, never the attempted override.
TEST_F(HrApiTest, GenerateOrderDocumentOverrideIinRejectedAndValueUnchanged) {
    auto org = seed_org("777150000034", "Order Override Org LLP");
    auto accountant = member("accountant25@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "988916681773");

    Hr::HrRepository repo;
    auto order = repo.create_order(org.id, employee.id, "hire", "1", "2026-01-10", "2026-01-15");

    json malicious = {{"director", "Ахметов Ерлан Серикович"}, {"employee", {{"iin", "999999999999"}}}};
    auto bad_req = authed_json(accountant, malicious);
    HttpResponsePtr bad_resp;
    ctrl.generateOrderDocument(
        bad_req, [&](const HttpResponsePtr& r) { bad_resp = r; }, order.id);
    ASSERT_NE(bad_resp, nullptr);
    EXPECT_EQ(bad_resp->statusCode(), k422UnprocessableEntity);
    auto bad_body = json::parse(std::string(bad_resp->body()));
    EXPECT_EQ(bad_body["errors"][0]["field"].get<std::string>(), "employee.iin");
    EXPECT_EQ(bad_body["errors"][0]["code"].get<std::string>(), "not_allowed_override");
    EXPECT_EQ(queue_depth(), 0);

    json legit = {{"director", "Ахметов Ерлан Серикович"}};
    auto good_req = authed_json(accountant, legit);
    HttpResponsePtr good_resp;
    ctrl.generateOrderDocument(
        good_req, [&](const HttpResponsePtr& r) { good_resp = r; }, order.id);
    ASSERT_NE(good_resp, nullptr);
    ASSERT_EQ(good_resp->statusCode(), k202Accepted);
    auto good_body = json::parse(std::string(good_resp->body()));
    const std::string document_id = good_body["document_id"].get<std::string>();

    Ledger::DocumentRepository documents;
    auto doc = documents.find_in_org(document_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(doc.has_value());
    ASSERT_TRUE(doc->input_snapshot.has_value());
    EXPECT_EQ((*doc->input_snapshot)["employee"]["iin"].get<std::string>(), "988916681773");
}

// Same pagination guarantee as ListOrdersIsPaginated, for GET /vacations —
// and the `total` reported alongside a page reflects the ?employee_id filter
// in force, not the whole organization.
TEST_F(HrApiTest, ListVacationsIsPaginatedAndCountsTheFilteredSet) {
    auto org = seed_org("777150000041", "Vacation Page Org LLP");
    auto accountant = member("accountant41@example.com", org.id, "accountant");
    auto employee1 = seed_employee(org.id, "156312191013");
    auto employee2 = seed_employee(org.id, "988916681773");

    Hr::HrRepository repo;
    repo.create_vacation(org.id, employee1.id, "2026-07-01", "2026-07-14", 14, "annual");
    repo.create_vacation(org.id, employee1.id, "2026-08-01", "2026-08-10", 10, "unpaid");
    repo.create_vacation(org.id, employee2.id, "2026-09-01", "2026-09-05", 5, "sick");

    auto req = TestHelpers::authed(accountant, Get);
    req->setParameter("limit", "1");
    HttpResponsePtr resp;
    ctrl.listVacations(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto page = json::parse(std::string(resp->body()));
    EXPECT_EQ(page["data"].size(), 1U);
    EXPECT_EQ(page["total"].get<long>(), 3);
    EXPECT_EQ(page["limit"].get<int>(), 1);

    auto filtered_req = authed_with_query(accountant, employee1.id);
    HttpResponsePtr filtered;
    ctrl.listVacations(filtered_req, [&](const HttpResponsePtr& r) { filtered = r; });
    ASSERT_NE(filtered, nullptr);
    auto filtered_page = json::parse(std::string(filtered->body()));
    EXPECT_EQ(filtered_page["data"].size(), 2U);
    EXPECT_EQ(filtered_page["total"].get<long>(), 2);
}

// ── POST /api/v1/vacations ───────────────────────────────────────────────────

TEST_F(HrApiTest, CreateVacationSucceeds) {
    auto org = seed_org("777150000025", "Vacation Org LLP");
    auto accountant = member("accountant18@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "988916681773");

    json body = {{"employee_id", employee.id},
                 {"starts_on", "2026-07-01"},
                 {"ends_on", "2026-07-14"},
                 {"days", 14},
                 {"kind", "annual"}};
    auto req = authed_json(accountant, body);
    HttpResponsePtr resp;
    ctrl.createVacation(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k201Created);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["data"]["days"].get<int>(), 14);
    EXPECT_EQ(resp_body["data"]["kind"].get<std::string>(), "annual");
}

TEST_F(HrApiTest, CreateVacationViewerForbidden) {
    auto org = seed_org("777150000026", "Vacation Viewer Org LLP");
    auto viewer = member("viewer5@example.com", org.id, "viewer");
    auto employee = seed_employee(org.id, "312460130157");

    json body = {{"employee_id", employee.id},
                 {"starts_on", "2026-07-01"},
                 {"ends_on", "2026-07-14"},
                 {"days", 14},
                 {"kind", "annual"}};
    auto req = authed_json(viewer, body);
    HttpResponsePtr resp;
    ctrl.createVacation(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(HrApiTest, CreateVacationInvalidDateRejected) {
    auto org = seed_org("777150000027", "Vacation Bad Date Org LLP");
    auto accountant = member("accountant19@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "181385520307");

    json body = {{"employee_id", employee.id},
                 {"starts_on", "2026-02-30"},
                 {"ends_on", "2026-07-14"},
                 {"days", 14},
                 {"kind", "annual"}};
    auto req = authed_json(accountant, body);
    HttpResponsePtr resp;
    ctrl.createVacation(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["errors"][0]["field"].get<std::string>(), "starts_on");
    EXPECT_EQ(resp_body["errors"][0]["code"].get<std::string>(), "invalid_date");
}

TEST_F(HrApiTest, CreateVacationEndsBeforeStartsRejected) {
    auto org = seed_org("777150000028", "Vacation Order Org LLP");
    auto accountant = member("accountant20@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "160480216736");

    json body = {{"employee_id", employee.id},
                 {"starts_on", "2026-07-14"},
                 {"ends_on", "2026-07-01"},
                 {"days", 14},
                 {"kind", "annual"}};
    auto req = authed_json(accountant, body);
    HttpResponsePtr resp;
    ctrl.createVacation(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["errors"][0]["field"].get<std::string>(), "ends_on");
    EXPECT_EQ(resp_body["errors"][0]["code"].get<std::string>(), "before_starts_on");
}

TEST_F(HrApiTest, CreateVacationInvalidKindRejected) {
    auto org = seed_org("777150000029", "Vacation Bad Kind Org LLP");
    auto accountant = member("accountant21@example.com", org.id, "accountant");
    auto employee = seed_employee(org.id, "768463260966");

    json body = {{"employee_id", employee.id},
                 {"starts_on", "2026-07-01"},
                 {"ends_on", "2026-07-14"},
                 {"days", 14},
                 {"kind", "not_a_kind"}};
    auto req = authed_json(accountant, body);
    HttpResponsePtr resp;
    ctrl.createVacation(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["errors"][0]["field"].get<std::string>(), "kind");
}

TEST_F(HrApiTest, CreateVacationForeignEmployeeRejected) {
    auto org_a = seed_org("777150000030", "Vacation Org A LLP");
    auto org_b = seed_org("777150000031", "Vacation Org B LLP");
    auto accountant_b = member("accountant22@example.com", org_b.id, "accountant");
    auto employee_a = seed_employee(org_a.id, "776828107547");

    json body = {{"employee_id", employee_a.id},
                 {"starts_on", "2026-07-01"},
                 {"ends_on", "2026-07-14"},
                 {"days", 14},
                 {"kind", "annual"}};
    auto req = authed_json(accountant_b, body);
    HttpResponsePtr resp;
    ctrl.createVacation(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto resp_body = json::parse(std::string(resp->body()));
    EXPECT_EQ(resp_body["errors"][0]["field"].get<std::string>(), "employee_id");
    EXPECT_EQ(resp_body["errors"][0]["code"].get<std::string>(), "foreign_employee");
}

// ── GET /api/v1/vacations ────────────────────────────────────────────────────

TEST_F(HrApiTest, ListVacationsFilteredByEmployee) {
    auto org = seed_org("777150000032", "Vacation List Org LLP");
    auto accountant = member("accountant23@example.com", org.id, "accountant");
    auto employee1 = seed_employee(org.id, "061071077377");
    auto employee2 = seed_employee(org.id, "087179126861");

    Hr::HrRepository repo;
    repo.create_vacation(org.id, employee1.id, "2026-07-01", "2026-07-14", 14, "annual");
    repo.create_vacation(org.id, employee2.id, "2026-08-01", "2026-08-05", 5, "sick");

    HttpResponsePtr resp;
    ctrl.listVacations(authed_with_query(accountant, employee1.id), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    ASSERT_TRUE(body["data"].is_array());
    ASSERT_EQ(body["data"].size(), 1U);
    EXPECT_EQ(body["data"][0]["employee_id"].get<std::string>(), employee1.id);
}

}  // namespace
