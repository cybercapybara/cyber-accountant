/**
 * @file test_org_read_gates.cpp
 * @brief «—» в матрице §5.3 означает НЕВИДИМО. Каждый закрытый для
 *        кадровика ресурс обязан отвечать 403 и на ЧТЕНИЕ, иначе тест на
 *        403 для POST /payroll-runs пройдёт, а GET /payroll-runs/{id}/
 *        payslips останется открытым — ровно тот класс ошибки, который дал
 *        утечку в P2.
 *
 * Фикстура и хелперы (seed_user/seed_org/member/authed) — канонический для
 * этого репозитория вид из tests/integration/test_documents_api.cpp, но БЕЗ
 * S3-части: ни один гейт здесь не доходит до объектного хранилища, включая
 * оба POST-маршрута выдачи пресайна (отказ случается раньше, чем
 * вызывается s3_backend()), поэтому MinIO этому набору не нужен.
 *
 * Про подставные uuid в табличном тесте: у пятнадцати перечисленных там
 * маршрутов гейт стоит ПЕРВОЙ строкой хендлера — до is_valid_uuid и до
 * любого обращения к базе. Ожидание «403 на несуществующий id» поэтому не
 * слабее, а строже: пропавший гейт даст 400/404, а гейт, съехавший ниже
 * поиска строки, — 404. Исключение — маршруты над ОДНИМ документом, где
 * решение зависит от doc_type прочитанной строки; они разобраны отдельным
 * тестом на реально засеянных документах.
 *
 * Отдельный блок — расчётный листок (doc_type='payroll',
 * migrations/023_payroll_doc_type.sql). Дефект приёмки v0.4.0: листок
 * создавался как doc_type='hr', попадал в ресурс hr_docs и потому читался
 * кадровиком, хотя ресурс payroll ему невидим. Тесты там проверяют ЗАПРЕТ
 * (чтение, download-url и запись — все три через ensure_document_access),
 * исключение листков из реестра И из его total, и два контрольных
 * разрешения: настоящий кадровый документ кадровику по-прежнему открыт, а
 * бухгалтеру открыто и то и другое.
 */

#include <cstddef>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/AccountsController.hpp"
#include "api/CounterpartiesController.hpp"
#include "api/DocgenController.hpp"
#include "api/EmployeesController.hpp"
#include "api/HrController.hpp"
#include "api/JournalController.hpp"
#include "api/LedgerDocumentsController.hpp"
#include "api/PayrollController.hpp"
#include "api/TaxController.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "ledger/DocumentRepository.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-org-read-gates-padding";

/// Синтаксически корректный uuid, которого заведомо нет ни в одной таблице —
/// см. шапку файла: пятнадцать табличных гейтов обязаны сработать до того,
/// как кто-нибудь пойдёт его искать.
constexpr const char* kAbsentId = "00000000-0000-4000-8000-00000000dead";

using Principal = Security::Auth::AuthPrincipal;

class OrgReadGatesTest : public TestHelpers::CoreBackedTest {
protected:
    Api::AccountsController accounts_;
    Api::CounterpartiesController counterparties_;
    Api::DocgenController docgen_;
    Api::EmployeesController employees_;
    Api::HrController hr_;
    Api::JournalController journal_;
    Api::LedgerDocumentsController documents_;
    Api::PayrollController payroll_;
    Api::TaxController tax_;

    Tenancy::Organization org_;

    std::string config_file_name() const override { return "org_read_gates_test_config.json"; }

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
        org_ = seed_org("555240000001", "Read Gates Org LLP");
    }

    struct Pair {
        Domain::User user;
        Principal principal;
    };

    Pair seed_user(const std::string& email, const std::string& role_name = "User") {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name(role_name);
        if (!role) {
            ADD_FAILURE() << "role " << role_name << " missing — seed migration?";
            throw std::runtime_error("seed role missing: " + role_name);
        }
        auto created = users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
        Pair p;
        p.user = created;
        p.principal.subject = created.id;
        p.principal.roles.push_back(role->name);
        p.principal.raw_claims = json{{"sub", created.id}, {"permissions", role->permissions}};
        return p;
    }

    static Principal with_org(Principal p, const std::string& org_id) {
        p.org = org_id;
        return p;
    }

    static Tenancy::Organization seed_org(const std::string& bin, const std::string& name) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, name, "snr_simplified", false);
    }

    /// Завести пользователя, вписать его в @p org_id с ролью @p role и
    /// вернуть org-scoped принципала, готовый для authed().
    Principal member(const std::string& email, const std::string& org_id, const std::string& role) {
        auto seeded = seed_user(email);
        Tenancy::OrgMemberRepository members;
        members.add(org_id, seeded.user.id, role);
        return with_org(seeded.principal, org_id);
    }

    std::string seed_document(const std::string& doc_type, const std::string& source) {
        Ledger::DocumentRepository repo;
        return repo.create(org_.id, doc_type, source, "draft").id;
    }

    static HttpRequestPtr authed(const Principal& p, HttpMethod method = Get) { return TestHelpers::authed(p, method); }

    static json body_of(const HttpResponsePtr& resp) {
        EXPECT_NE(resp, nullptr);
        return json::parse(std::string(resp->body()));
    }

    // ── обёртки над хендлерами ───────────────────────────────────────────
    // Каждый вызов получает СВОЙ callback: сигнатура хендлеров — приём
    // std::function по rvalue-ссылке, переиспользуемая lvalue-лямбда туда
    // не связывается.

    HttpResponsePtr documents_list(const Principal& p, const std::string& type) {
        auto req = authed(p);
        if (!type.empty())
            req->setParameter("type", type);
        HttpResponsePtr resp;
        documents_.list(req, [&](const HttpResponsePtr& r) { resp = r; });
        return resp;
    }

    HttpResponsePtr documents_get(const Principal& p, const std::string& id) {
        HttpResponsePtr resp;
        documents_.get(
            authed(p), [&](const HttpResponsePtr& r) { resp = r; }, id);
        return resp;
    }

    HttpResponsePtr documents_download_url(const Principal& p, const std::string& id) {
        HttpResponsePtr resp;
        documents_.downloadUrl(
            authed(p, Post), [&](const HttpResponsePtr& r) { resp = r; }, id);
        return resp;
    }

    /// POST /documents/{id}/void — маршрут ЗАПИСИ, ходит через тот же
    /// ensure_document_access(..., kWrite). Нужен, чтобы доказать: у
    /// расчётного листка закрыто не только чтение.
    HttpResponsePtr documents_void(const Principal& p, const std::string& id) {
        HttpResponsePtr resp;
        documents_.voidDocument(
            TestHelpers::authed_json(p, json{{"reason", "тест гейта"}}, Post),
            [&](const HttpResponsePtr& r) { resp = r; },
            id);
        return resp;
    }
};

// ── чтения, закрытые для кадровика ──────────────────────────────────────────

TEST_F(OrgReadGatesTest, HrIsDeniedReadOnPayrollJournalTaxAndDocuments) {
    auto hr = member("hr@example.com", org_.id, "hr");
    struct Case {
        const char* label;
        std::function<HttpResponsePtr(const Principal&)> invoke;
    };
    // ПОЛНЫЙ перечень гейтов, закрытых для кадровика, — не выборка.
    // Пятнадцать маршрутов, у каждого свой кейс; ещё три случая с
    // документами разобраны отдельными тестами ниже, потому что там
    // решение зависит от doc_type конкретной строки.
    const Case kCases[] = {
        // payroll (2)
        {"GET /payroll-runs",
         [&](const Principal& p) {
             HttpResponsePtr r;
             payroll_.list(authed(p), [&](const HttpResponsePtr& x) { r = x; });
             return r;
         }},
        {"GET /payroll-runs/{id}/payslips",
         [&](const Principal& p) {
             HttpResponsePtr r;
             payroll_.listPayslips(
                 authed(p), [&](const HttpResponsePtr& x) { r = x; }, kAbsentId);
             return r;
         }},
        // journal (3 — план счетов относится к тому же ресурсу)
        {"GET /journal-entries",
         [&](const Principal& p) {
             HttpResponsePtr r;
             journal_.list(authed(p), [&](const HttpResponsePtr& x) { r = x; });
             return r;
         }},
        {"GET /journal-entries/{id}",
         [&](const Principal& p) {
             HttpResponsePtr r;
             journal_.get(
                 authed(p), [&](const HttpResponsePtr& x) { r = x; }, kAbsentId);
             return r;
         }},
        {"GET /accounts",
         [&](const Principal& p) {
             HttpResponsePtr r;
             accounts_.list(authed(p), [&](const HttpResponsePtr& x) { r = x; });
             return r;
         }},
        // counterparties (2)
        {"GET /counterparties",
         [&](const Principal& p) {
             HttpResponsePtr r;
             counterparties_.list(authed(p), [&](const HttpResponsePtr& x) { r = x; });
             return r;
         }},
        {"GET /counterparties/{id}",
         [&](const Principal& p) {
             HttpResponsePtr r;
             counterparties_.get(
                 authed(p), [&](const HttpResponsePtr& x) { r = x; }, kAbsentId);
             return r;
         }},
        // tax (7 — включая getFiling и POST-выдачу пресайна)
        {"GET /tax/rates",
         [&](const Principal& p) {
             HttpResponsePtr r;
             tax_.listRates(authed(p), [&](const HttpResponsePtr& x) { r = x; });
             return r;
         }},
        {"GET /tax/deadlines",
         [&](const Principal& p) {
             HttpResponsePtr r;
             tax_.listDeadlines(authed(p), [&](const HttpResponsePtr& x) { r = x; });
             return r;
         }},
        {"GET /tax/alerts",
         [&](const Principal& p) {
             HttpResponsePtr r;
             tax_.listAlerts(authed(p), [&](const HttpResponsePtr& x) { r = x; });
             return r;
         }},
        {"GET /tax/calculations",
         [&](const Principal& p) {
             HttpResponsePtr r;
             tax_.listCalculations(authed(p), [&](const HttpResponsePtr& x) { r = x; });
             return r;
         }},
        {"GET /tax/filings",
         [&](const Principal& p) {
             HttpResponsePtr r;
             tax_.listFilings(authed(p), [&](const HttpResponsePtr& x) { r = x; });
             return r;
         }},
        {"GET /tax/filings/{id}",
         [&](const Principal& p) {
             HttpResponsePtr r;
             tax_.getFiling(
                 authed(p), [&](const HttpResponsePtr& x) { r = x; }, kAbsentId);
             return r;
         }},
        {"POST /tax/filings/{id}/download-url",
         [&](const Principal& p) {
             HttpResponsePtr r;
             tax_.filingDownloadUrl(
                 authed(p, Post), [&](const HttpResponsePtr& x) { r = x; }, kAbsentId);
             return r;
         }},
        // documents-как-реестр (1): шаблоны — тоже чтение ресурса documents
        {"GET /doc-templates",
         [&](const Principal& p) {
             HttpResponsePtr r;
             docgen_.listTemplates(authed(p), [&](const HttpResponsePtr& x) { r = x; });
             return r;
         }},
    };
    ASSERT_EQ(std::size(kCases), 15u) << "перечень обязан покрывать ВСЕ закрытые для hr гейты, а не выборку";
    for (const auto& c : kCases) {
        auto resp = c.invoke(hr);
        ASSERT_NE(resp, nullptr) << c.label;
        EXPECT_EQ(resp->statusCode(), k403Forbidden) << c.label;
        EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "org_role_denied") << c.label;
    }
}

TEST_F(OrgReadGatesTest, HrIsDeniedReadOnAnIndividualPrimaryDocument) {
    // Оставшиеся два гейта над ОДНИМ документом: решение зависит от
    // doc_type строки, поэтому они не влезают в табличный кейс выше.
    // Оба обязаны быть 403, а не 404: документ существует и принадлежит
    // этой организации — не та роль, а не «нет такого».
    auto hr = member("hr5@example.com", org_.id, "hr");
    const std::string invoice_id = seed_document("invoice", "generated");

    auto one = documents_get(hr, invoice_id);
    ASSERT_NE(one, nullptr);
    EXPECT_EQ(one->statusCode(), k403Forbidden) << "GET /documents/{id}";
    EXPECT_EQ(body_of(one)["error"].get<std::string>(), "org_role_denied") << "GET /documents/{id}";

    auto url = documents_download_url(hr, invoice_id);
    ASSERT_NE(url, nullptr);
    EXPECT_EQ(url->statusCode(), k403Forbidden) << "POST /documents/{id}/download-url";
    EXPECT_EQ(body_of(url)["error"].get<std::string>(), "org_role_denied") << "POST /documents/{id}/download-url";

    // А свой, кадровый — читается.
    const std::string hr_doc = seed_document("hr", "generated");
    EXPECT_EQ(documents_get(hr, hr_doc)->statusCode(), k200OK);

    // Чужая организация остаётся 404, а не превращается в 403: гейт стоит
    // ПОСЛЕ find_in_org именно для этого.
    auto other = seed_org("555240000009", "Other Org LLP");
    Ledger::DocumentRepository repo;
    const std::string foreign_hr_doc = repo.create(other.id, "hr", "generated", "draft").id;
    EXPECT_EQ(documents_get(hr, foreign_hr_doc)->statusCode(), k404NotFound);
}

// ── расчётный листок: payroll, а не hr_docs ─────────────────────────────────

TEST_F(OrgReadGatesTest, HrIsDeniedEveryAccessToAPayslipDocument) {
    // Дефект приёмки v0.4.0: расчётный листок создавался с doc_type='hr' и
    // потому попадал в ресурс hr_docs — кадровик читал его и видел ВЕСЬ
    // расчёт (gross, ОПВ, ВОСМС, ИПН, СО, ОСМС, ОПВР, соцналог, net), хотя
    // §5.3 говорит «Оклад видит, расчёты — нет». Теперь такой документ —
    // doc_type='payroll' (migrations/023), то есть ресурс payroll, который
    // кадровику невидим целиком: ни чтения, ни записи.
    auto hr = member("hr6@example.com", org_.id, "hr");
    const std::string payslip_id = seed_document("payroll", "generated");
    const std::string hr_order_id = seed_document("hr", "generated");

    // ЗАПРЕТ — чтение, выдача ссылки на файл и запись, все три через один
    // и тот же ensure_document_access().
    auto read = documents_get(hr, payslip_id);
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->statusCode(), k403Forbidden) << "GET /documents/{id} (payslip)";
    EXPECT_EQ(body_of(read)["error"].get<std::string>(), "org_role_denied") << "GET /documents/{id} (payslip)";

    auto url = documents_download_url(hr, payslip_id);
    ASSERT_NE(url, nullptr);
    EXPECT_EQ(url->statusCode(), k403Forbidden) << "POST /documents/{id}/download-url (payslip)";
    EXPECT_EQ(body_of(url)["error"].get<std::string>(), "org_role_denied")
        << "POST /documents/{id}/download-url (payslip)";

    auto voided = documents_void(hr, payslip_id);
    ASSERT_NE(voided, nullptr);
    EXPECT_EQ(voided->statusCode(), k403Forbidden) << "POST /documents/{id}/void (payslip)";
    EXPECT_EQ(body_of(voided)["error"].get<std::string>(), "org_role_denied") << "POST /documents/{id}/void (payslip)";

    // РАЗРЕШЕНИЕ — контрольный случай: настоящий кадровый документ той же
    // роли по-прежнему и читается, и правится. Иначе «починка» свелась бы
    // к тому, что кадровику закрыли всё подряд.
    EXPECT_EQ(documents_get(hr, hr_order_id)->statusCode(), k200OK) << "GET /documents/{id} (hr order)";
    EXPECT_EQ(documents_void(hr, hr_order_id)->statusCode(), k200OK) << "POST /documents/{id}/void (hr order)";
}

TEST_F(OrgReadGatesTest, AccountantStillReadsPayslipAndHrDocumentsAlike) {
    // Вторая половина того же утверждения: ресурс payroll закрыт КАДРОВИКУ,
    // а не всем — бухгалтер читает и расчётный листок, и кадровый приказ.
    auto acc = member("acc6@example.com", org_.id, "accountant");
    const std::string payslip_id = seed_document("payroll", "generated");
    const std::string hr_order_id = seed_document("hr", "generated");

    EXPECT_EQ(documents_get(acc, payslip_id)->statusCode(), k200OK) << "GET /documents/{id} (payslip)";
    EXPECT_EQ(documents_get(acc, hr_order_id)->statusCode(), k200OK) << "GET /documents/{id} (hr order)";
    // download-url доходит до хранилища и без MinIO отвечает 409 no_file /
    // 503 — важно лишь, что это НЕ 403: гейт роль пропустил.
    auto url = documents_download_url(acc, payslip_id);
    ASSERT_NE(url, nullptr);
    EXPECT_NE(url->statusCode(), k403Forbidden) << "POST /documents/{id}/download-url (payslip, accountant)";
}

TEST_F(OrgReadGatesTest, PayslipsAreExcludedFromTheHrRegistryAndItsTotal) {
    // Сужение GET /documents живёт на том же doc_type, что и resource_for,
    // поэтому расчётные листки обязаны выпасть И из строк, И из total —
    // именно total выдал бы кадровику количество расчётов, не показав их.
    seed_document("payroll", "generated");
    seed_document("payroll", "generated");
    seed_document("hr", "generated");
    seed_document("invoice", "generated");
    auto hr = member("hr7@example.com", org_.id, "hr");

    auto unfiltered = documents_list(hr, /*type=*/"");
    ASSERT_NE(unfiltered, nullptr);
    ASSERT_EQ(unfiltered->statusCode(), k200OK);
    auto body = body_of(unfiltered);
    ASSERT_EQ(body["data"].size(), 1U) << "виден только кадровый документ";
    EXPECT_EQ(body["data"][0]["doc_type"].get<std::string>(), "hr");
    EXPECT_EQ(body["total"].get<long>(), 1) << "total обязан считать ту же суженную выборку";

    // Явный ?type=payroll не обходит сужение — оно перезаписывается.
    auto asked = documents_list(hr, /*type=*/"payroll");
    ASSERT_NE(asked, nullptr);
    ASSERT_EQ(asked->statusCode(), k200OK);
    auto asked_body = body_of(asked);
    for (const auto& d : asked_body["data"])
        EXPECT_EQ(d["doc_type"].get<std::string>(), "hr");
    EXPECT_EQ(asked_body["total"].get<long>(), 1);

    // Бухгалтер видит все четыре — сужение принадлежит роли, а не реестру.
    auto acc = member("acc7@example.com", org_.id, "accountant");
    auto all = documents_list(acc, /*type=*/"");
    ASSERT_NE(all, nullptr);
    ASSERT_EQ(all->statusCode(), k200OK);
    EXPECT_EQ(body_of(all)["total"].get<long>(), 4);
}

// ── чтения, ОТКРЫТЫЕ для кадровика ──────────────────────────────────────────

TEST_F(OrgReadGatesTest, HrMayReadEmployeesAndHrDocuments) {
    // Гейт обязан различать, а не запрещать всем подряд: те же пять
    // маршрутов, что закрыты выше по другим ресурсам, кадровику открыты.
    auto hr = member("hr2@example.com", org_.id, "hr");

    HttpResponsePtr employees_list;
    employees_.list(authed(hr), [&](const HttpResponsePtr& r) { employees_list = r; });
    ASSERT_NE(employees_list, nullptr);
    EXPECT_EQ(employees_list->statusCode(), k200OK) << "GET /employees";

    HttpResponsePtr employee_one;
    employees_.get(
        authed(hr), [&](const HttpResponsePtr& r) { employee_one = r; }, kAbsentId);
    ASSERT_NE(employee_one, nullptr);
    // Не 403: роль пропущена, дальше отвечает уже сам ресурс.
    EXPECT_EQ(employee_one->statusCode(), k404NotFound) << "GET /employees/{id}";

    HttpResponsePtr orders;
    hr_.listOrders(authed(hr), [&](const HttpResponsePtr& r) { orders = r; });
    ASSERT_NE(orders, nullptr);
    EXPECT_EQ(orders->statusCode(), k200OK) << "GET /hr-orders";

    auto contracts_req = authed(hr);
    contracts_req->setParameter("employee_id", kAbsentId);
    HttpResponsePtr contracts;
    hr_.listContracts(contracts_req, [&](const HttpResponsePtr& r) { contracts = r; });
    ASSERT_NE(contracts, nullptr);
    EXPECT_EQ(contracts->statusCode(), k200OK) << "GET /labor-contracts";

    HttpResponsePtr vacations;
    hr_.listVacations(authed(hr), [&](const HttpResponsePtr& r) { vacations = r; });
    ASSERT_NE(vacations, nullptr);
    EXPECT_EQ(vacations->statusCode(), k200OK) << "GET /vacations";
}

// ── записи, закрытые для кадровика (регрессия на задачу 6) ──────────────────

TEST_F(OrgReadGatesTest, HrCannotWriteToTheLedgerTaxesOrPayroll) {
    auto hr = member("hr3@example.com", org_.id, "hr");

    HttpResponsePtr entry;
    journal_.create(authed(hr, Post), [&](const HttpResponsePtr& r) { entry = r; });
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->statusCode(), k403Forbidden) << "POST /journal-entries";

    HttpResponsePtr counterparty;
    counterparties_.create(authed(hr, Post), [&](const HttpResponsePtr& r) { counterparty = r; });
    ASSERT_NE(counterparty, nullptr);
    EXPECT_EQ(counterparty->statusCode(), k403Forbidden) << "POST /counterparties";

    HttpResponsePtr run;
    payroll_.calculate(authed(hr, Post), [&](const HttpResponsePtr& r) { run = r; });
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run->statusCode(), k403Forbidden) << "POST /payroll-runs";

    HttpResponsePtr posted;
    payroll_.postToJournal(
        authed(hr, Post), [&](const HttpResponsePtr& r) { posted = r; }, kAbsentId);
    ASSERT_NE(posted, nullptr);
    EXPECT_EQ(posted->statusCode(), k403Forbidden) << "POST /payroll-runs/{id}/post-to-journal";

    HttpResponsePtr calculation;
    tax_.createCalculation(authed(hr, Post), [&](const HttpResponsePtr& r) { calculation = r; });
    ASSERT_NE(calculation, nullptr);
    EXPECT_EQ(calculation->statusCode(), k403Forbidden) << "POST /tax/calculations";

    HttpResponsePtr generated;
    docgen_.generate(authed(hr, Post), [&](const HttpResponsePtr& r) { generated = r; });
    ASSERT_NE(generated, nullptr);
    EXPECT_EQ(generated->statusCode(), k403Forbidden) << "POST /documents/generate";
}

// ── сужение реестра документов ──────────────────────────────────────────────

TEST_F(OrgReadGatesTest, DocumentsListIsNarrowedToHrDocumentsForTheHrRole) {
    // В организации есть и первичка, и кадровые документы.
    seed_document("invoice", "generated");
    seed_document("tax_invoice", "generated");
    seed_document("hr", "generated");
    auto hr = member("hr4@example.com", org_.id, "hr");

    // БЕЗ фильтра — главный случай: сужение обязано быть свойством роли, а
    // не следствием того, что клиент любезно прислал ?type=hr. Если бы оно
    // зависело от параметра запроса, дыра открывалась бы пустым запросом.
    auto unfiltered = documents_list(hr, /*type=*/"");
    ASSERT_NE(unfiltered, nullptr);
    ASSERT_EQ(unfiltered->statusCode(), k200OK);
    auto unfiltered_body = body_of(unfiltered);
    ASSERT_FALSE(unfiltered_body["data"].empty());
    for (const auto& d : unfiltered_body["data"])
        EXPECT_EQ(d["doc_type"].get<std::string>(), "hr");
    // И пагинационный total считает ту же суженную выборку, иначе кадровик
    // узнаёт количество первички, не видя её.
    EXPECT_EQ(unfiltered_body["total"].get<long>(), 1);

    // С фильтром на чужой тип — перезапись, а не обход: кадровые документы,
    // а не счета.
    auto filtered = documents_list(hr, /*type=*/"invoice");
    ASSERT_NE(filtered, nullptr);
    ASSERT_EQ(filtered->statusCode(), k200OK);
    auto filtered_body = body_of(filtered);
    for (const auto& d : filtered_body["data"])
        EXPECT_EQ(d["doc_type"].get<std::string>(), "hr");
    EXPECT_EQ(filtered_body["total"].get<long>(), 1);

    // Точечное чтение чужого документа — 403, а не 404: документ есть, роль
    // не та.
    const std::string invoice_id = seed_document("invoice", "generated");
    auto one = documents_get(hr, invoice_id);
    ASSERT_NE(one, nullptr);
    EXPECT_EQ(one->statusCode(), k403Forbidden);
    EXPECT_EQ(body_of(one)["error"].get<std::string>(), "org_role_denied");

    // Бухгалтер по тому же запросу видит всё — контрольный случай: гейт
    // различает роли, а не режет реестр всем.
    auto acc = member("acc4@example.com", org_.id, "accountant");
    auto all = documents_list(acc, /*type=*/"");
    ASSERT_NE(all, nullptr);
    ASSERT_EQ(all->statusCode(), k200OK);
    auto all_body = body_of(all);
    EXPECT_GT(all_body["data"].size(), unfiltered_body["data"].size());
    EXPECT_EQ(all_body["total"].get<long>(), 4);
}

}  // namespace
