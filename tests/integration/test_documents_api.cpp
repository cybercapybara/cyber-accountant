/**
 * @file test_documents_api.cpp
 * @brief Integration tests for LedgerDocumentsController — Task 12.
 *
 * Needs Postgres/Redis (TestHelpers::CoreBackedTest) AND a real S3-compatible
 * endpoint (MinIO in the test compose profile, same as
 * tests/integration/test_s3_storage.cpp) — presign()/exists() are exercised
 * against the real thing, not a fake, so the download-url/uploads/
 * confirm-upload round trip proves the signatures are actually valid to an
 * independent HTTP client (curl via popen), not just to this binary's own
 * libcurl usage. Skips (or FAILs under CI_REQUIRE_INFRA) when MinIO is
 * unavailable — same idiom as test_s3_storage.cpp and
 * TestHelpers::CoreBackedTest for Postgres/Redis.
 *
 * Follows the direct-controller-invocation idiom of test_organizations_api.cpp
 * for everything auth/org-context related.
 */

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/LedgerDocumentsController.hpp"
#include "cache/Cache.hpp"
#include "docgen/InputPolicy.hpp"
#include "docgen/RenderJob.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "files/FileKeys.hpp"
#include "hr/EmployeeRepository.hpp"
#include "hr/HrRepository.hpp"
#include "jobs/Job.hpp"
#include "jobs/Jobs.hpp"
#include "ledger/DocumentRepository.hpp"
#include "ledger/JournalRepository.hpp"
#include "ledger/JournalService.hpp"
#include "money/AmountInWords.hpp"
#include "money/MoneyFormat.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "storage/Storage.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"
#include "utils/Crypto.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-documents-api-padding";
constexpr const char* kRenderJobType = "docgen.render";

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

// Same defaults as test_s3_storage.cpp — match docker/docker-compose.yml's
// test-minio service.
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

/// Run a shell command, returning everything it wrote to stdout — same
/// popen-based idiom as test_s3_storage.cpp, used here to drive `curl`
/// against the presigned URLs the controller hands back.
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

class LedgerDocumentsApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::LedgerDocumentsController ctrl;
    std::unique_ptr<TestHelpers::ScopedEnv> latex_stub_;
    std::filesystem::path latex_stub_dir_;

    std::string config_file_name() const override { return "documents_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        // POST /documents/{id}/versions enqueues a docgen.render job, so the
        // queue has to be live or every edit would honestly answer 503
        // jobs_disabled. Nothing here RUNS the job — see
        // tests/integration/test_render_job.cpp for the render pipeline.
        cfg["jobs"]["enabled"] = true;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // Centralized org-data wipe (TestHelpers::wipe_org_data(), in
        // test_helpers.hpp) — TRUNCATEs journal_lines/journal_entries/
        // document_entries/documents FIRST (bypasses the
        // journal_entries_immutability trigger other suites sharing this
        // Postgres may have tripped), then a plain DELETE on organizations
        // (whose row-level ON DELETE CASCADE clears org_members without
        // touching the org_id IS NULL system chart-of-accounts seed).
        // TRUNCATE users CASCADE stays local to this fixture.
        TestHelpers::wipe_org_data();
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
        drain_queue();

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
        if (!::testing::Test::IsSkipped() && Cache::is_initialized())
            drain_queue();
        latex_stub_.reset();
        if (!latex_stub_dir_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(latex_stub_dir_, ec);
        }
        Storage::reset_for_testing();
        TestHelpers::CoreBackedTest::TearDown();
    }

    /// Point DOCGEN_LATEX_CMD at a stub that "compiles" anything into
    /// @p bytes, so a test can run the REAL Docgen::process_job (schema
    /// validation, inja render, storage, repository writes) without TeX Live
    /// — same idiom as tests/integration/test_render_job.cpp, which owns the
    /// render pipeline's own coverage. Only the tests that actually drive the
    /// worker call this.
    void use_latex_stub(const std::string& bytes) {
        latex_stub_dir_ = std::filesystem::temp_directory_path() / ("documents_api_latex_" + Jobs::generate_uuid());
        std::filesystem::create_directories(latex_stub_dir_);
        const std::filesystem::path pdf_path = latex_stub_dir_ / "canned.pdf";
        std::ofstream(pdf_path, std::ios::binary) << bytes;
        const std::filesystem::path script_path = latex_stub_dir_ / "fake-latex.sh";
        {
            std::ofstream script(script_path);
            script << "#!/bin/sh\n"
                   << "cp \"" << pdf_path.string() << "\" main.pdf\n"
                   << "exit 0\n";
        }
        std::error_code ec;
        std::filesystem::permissions(script_path,
                                     std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                         std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                                         std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::replace,
                                     ec);
        latex_stub_ = std::make_unique<TestHelpers::ScopedEnv>("DOCGEN_LATEX_CMD", script_path.string());
    }

    static void drain_queue() { TestHelpers::drain_jobs({kRenderJobType}); }

    struct Pair {
        Domain::User user;
        Security::Auth::AuthPrincipal principal;
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

    static Security::Auth::AuthPrincipal with_org(Security::Auth::AuthPrincipal p, const std::string& org_id) {
        p.org = org_id;
        return p;
    }

    Tenancy::Organization seed_org(const std::string& bin, const std::string& name) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, name, "snr_simplified", false);
    }

    /// Seed a user, add them to @p org_id with @p role, and return an
    /// org-scoped principal ready for authed()/authed_json().
    Security::Auth::AuthPrincipal member(const std::string& email, const std::string& org_id, const std::string& role) {
        auto seeded = seed_user(email);
        Tenancy::OrgMemberRepository members;
        members.add(org_id, seeded.user.id, role);
        return with_org(seeded.principal, org_id);
    }

    static HttpRequestPtr authed(const Security::Auth::AuthPrincipal& p, HttpMethod method = Get) {
        return TestHelpers::authed(p, method);
    }

    static HttpRequestPtr authed_json(const Security::Auth::AuthPrincipal& p,
                                      const json& body,
                                      HttpMethod method = Post) {
        return TestHelpers::authed_json(p, body, method);
    }

    // ── P3 task 9 (editing) fixtures ────────────────────────────────────────
    //
    // The edit tests need real templates on disk: createVersion() resolves the
    // slug through Docgen::TemplateRegistry and schema-validates the merged
    // input, so a working directory without templates/ would turn every one of
    // them into a 500 that says nothing about the allowlist.
    static bool templates_available() {
        return std::filesystem::exists("templates/latex/invoice/v1/schema.json") &&
               std::filesystem::exists("templates/latex/fno_910/v1/schema.json");
    }

    /// A REQUEST-shaped invoice input: money is the INTEGER tiyn field only.
    /// `total`/`total_words` are the SERVER's to write — supplying either is
    /// the 422 EditRejectsServerDerivedMoneyStrings asserts.
    static json make_invoice_input(long long total_tiyn) {
        return json{
            {"number", "1"},
            {"date", "14.08.2026"},
            {"seller", {{"name", "Cyber Capybara ТОО"}, {"identifier", "104332181962"}}},
            {"buyer", {{"name", "Покупатель ТОО"}, {"identifier", "001338908381"}}},
            {"items",
             json::array({json{{"name", "Консультации"},
                               {"qty", "1"},
                               {"unit", "шт"},
                               {"price", "1 000,00"},
                               {"amount", "1 000,00"}}})},
            {"total_tiyn", total_tiyn},
        };
    }

    /// A document in the state the render worker leaves behind: version 1 has
    /// a real object in MinIO, the current-version pointer is on it, and the
    /// snapshot is the DERIVED one (what docgen actually stored), not the
    /// request body.
    std::string seed_rendered_invoice(const std::string& org_id, long long total_tiyn, const std::string& pdf_bytes) {
        json input = make_invoice_input(total_tiyn);
        std::string bad_field, bad_code, bad_message;
        if (!Docgen::InputPolicy::apply_derived_amount("invoice", input, bad_field, bad_code, bad_message))
            throw std::runtime_error("fixture: apply_derived_amount rejected the seed input: " + bad_message);

        Ledger::DocumentRepository repo;
        auto doc = repo.create(org_id,
                               "invoice",
                               "generated",
                               "draft",
                               /*counterparty_id=*/std::nullopt,
                               /*template_slug=*/std::string("invoice"),
                               /*template_version=*/std::string("v1"),
                               std::optional<nlohmann::json>{input});
        const std::string key = Files::org_key(org_id, "generated", "invoice.pdf");
        Storage::get().put(key, pdf_bytes, "application/pdf");
        auto v1 = repo.latest_version(org_id, doc.id);
        if (!v1)
            throw std::runtime_error("fixture: create() left the document versionless");
        repo.set_version_file(org_id,
                              v1->id,
                              key,
                              Utils::Crypto::sha256_hex(pdf_bytes),
                              "application/pdf",
                              static_cast<long long>(pdf_bytes.size()));
        repo.set_current_version(org_id, doc.id, v1->id);
        repo.set_status(org_id, doc.id, "final");
        return doc.id;
    }

    /// The snapshot a ФНО 910.00 filing stores: every figure below was derived
    /// by POST /tax/filings from an authoritative calculation, which is exactly
    /// why an edit may not touch any of them (only `director`/`accountant` are
    /// in Docgen::InputPolicy::editable_fields("fno_910")).
    static json fno910_snapshot() {
        return json{
            {"org", {{"bin", "444240000031"}, {"name", "Filing Org LLP"}}},
            {"period", {{"year", "2026"}, {"half", "1"}}},
            {"income_tenge", "1 000 000,00"},
            {"rate_percent", "3"},
            {"tax_tenge", "30 000,00"},
            {"tax_words", Money::to_words_ru(3000000)},
            {"signed_on", "14.08.2026"},
            {"director", "Директор из расчёта"},
            {"accountant", "Бухгалтер из расчёта"},
        };
    }

    // ── P3 task 11 (delete vs void) fixtures ────────────────────────────────
    //
    // What separates a deletable document from a voidable one is the LINK to a
    // posted (or reversed) journal entry, not the status — so these two
    // fixtures build exactly that difference and nothing else.

    /// A balanced entry left in 'draft'. Deleting a document attached to it is
    /// allowed: document_entries cascades and the draft is left without its
    /// document, which is the accepted outcome (recorded in the audit log).
    std::string seed_draft_entry(const std::string& org_id, const std::string& user_id) {
        Ledger::JournalService svc;
        Ledger::JournalLine debit;
        debit.account_code = "1030";
        debit.side = "debit";
        debit.amount = "1000.00";
        Ledger::JournalLine credit;
        credit.account_code = "6010";
        credit.side = "credit";
        credit.amount = "1000.00";
        return svc.create_draft(org_id, user_id, "2026-01-15", "Delete-vs-void fixture", {debit, credit}).id;
    }

    /// The same entry, posted. A document linked to THIS can never be deleted:
    /// the ledger is insert-only and is corrected by storno, so the document
    /// stays as the evidence the entry was made on.
    std::string seed_posted_entry(const std::string& org_id, const std::string& user_id) {
        const std::string entry_id = seed_draft_entry(org_id, user_id);
        Ledger::JournalService svc;
        auto posted = svc.post(org_id, entry_id);
        if (!posted)
            throw std::runtime_error("fixture: could not post the seeded entry");
        return entry_id;
    }

    /// An HR document with NO journal link at all, but referenced by an HR
    /// order (hr_orders.document_id, a NO ACTION FK — migrations/012_hr.sql).
    /// Deleting it must surface as a 409, not as the raw SQLSTATE 23503 that
    /// would otherwise reach the client as a 500.
    std::string seed_hr_document_referenced_by_an_order(const std::string& org_id, const std::string& iin) {
        Ledger::DocumentRepository docs;
        auto doc = docs.create(org_id, "hr", "uploaded", "inbox");

        Hr::Employee draft;
        draft.iin = iin;
        draft.last_name = "Аннулиров";
        draft.first_name = "Тест";
        draft.position = "Бухгалтер";
        draft.salary_tiyn = 30000000;
        draft.hired_on = "2026-01-05";
        draft.payout_iik = "KZ000000000000000000";
        Hr::EmployeeRepository employees;
        auto employee = employees.create(org_id, draft);

        Hr::HrRepository hr;
        hr.create_order(org_id,
                        employee.id,
                        "hire",
                        "1",
                        "2026-01-05",
                        "2026-01-05",
                        /*effective_to=*/std::nullopt,
                        /*payload=*/std::nullopt,
                        /*document_id=*/doc.id);
        return doc.id;
    }

    static std::string seed_fno910_document(const std::string& org_id) {
        Ledger::DocumentRepository repo;
        auto doc = repo.create(org_id,
                               "fno",
                               "generated",
                               "draft",
                               /*counterparty_id=*/std::nullopt,
                               /*template_slug=*/std::string("fno_910"),
                               /*template_version=*/std::string("v1"),
                               std::optional<nlohmann::json>{fno910_snapshot()});
        return doc.id;
    }
};

// ── GET /api/v1/documents ────────────────────────────────────────────────────

TEST_F(LedgerDocumentsApiTest, ListDocumentsPaginated) {
    auto org = seed_org("444240000001", "List Docs Org LLP");
    Ledger::DocumentRepository repo;
    repo.create(org.id, "invoice", "generated", "draft");
    repo.create(org.id, "bank_statement", "email", "inbox");

    auto viewer = member("viewer1@example.com", org.id, "viewer");
    HttpResponsePtr resp;
    ctrl.list(authed(viewer), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["total"].get<long>(), 2);
    ASSERT_TRUE(body["data"].is_array());
    EXPECT_EQ(body["data"].size(), 2U);
}

TEST_F(LedgerDocumentsApiTest, ListFiltersByTypeAndStatus) {
    auto org = seed_org("444240000002", "Filter Docs Org LLP");
    Ledger::DocumentRepository repo;
    repo.create(org.id, "invoice", "generated", "draft");
    repo.create(org.id, "bank_statement", "email", "inbox");

    auto viewer = member("viewer2@example.com", org.id, "viewer");
    auto req = authed(viewer);
    req->setParameter("type", "invoice");
    HttpResponsePtr resp;
    ctrl.list(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["total"].get<long>(), 1);
    ASSERT_EQ(body["data"].size(), 1U);
    EXPECT_EQ(body["data"][0]["doc_type"].get<std::string>(), "invoice");
}

TEST_F(LedgerDocumentsApiTest, ListInvalidTypeFilterRejected) {
    auto org = seed_org("444240000003", "Bad Type Filter Org LLP");
    auto viewer = member("viewer3@example.com", org.id, "viewer");
    auto req = authed(viewer);
    req->setParameter("type", "not_a_real_type");
    HttpResponsePtr resp;
    ctrl.list(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "type");
}

TEST_F(LedgerDocumentsApiTest, ListInvalidStatusFilterRejected) {
    auto org = seed_org("444240000004", "Bad Status Filter Org LLP");
    auto viewer = member("viewer4@example.com", org.id, "viewer");
    auto req = authed(viewer);
    req->setParameter("status", "nonexistent_status");
    HttpResponsePtr resp;
    ctrl.list(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "status");
}

// ── GET /api/v1/documents/{id} ───────────────────────────────────────────────

TEST_F(LedgerDocumentsApiTest, GetDocumentSucceeds) {
    auto org = seed_org("444240000005", "Get Doc Org LLP");
    Ledger::DocumentRepository repo;
    auto created = repo.create(org.id, "invoice", "generated", "draft");

    auto viewer = member("viewer5@example.com", org.id, "viewer");
    HttpResponsePtr resp;
    ctrl.get(
        authed(viewer), [&](const HttpResponsePtr& r) { resp = r; }, created.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["id"].get<std::string>(), created.id);
    EXPECT_EQ(body["data"]["doc_type"].get<std::string>(), "invoice");
}

TEST_F(LedgerDocumentsApiTest, GetDocumentCrossOrgNotFound) {
    auto org_a = seed_org("444240000006", "Doc Org A LLP");
    auto org_b = seed_org("444240000007", "Doc Org B LLP");
    Ledger::DocumentRepository repo;
    auto created = repo.create(org_a.id, "invoice", "generated", "draft");

    auto viewer_b = member("viewer6@example.com", org_b.id, "viewer");
    HttpResponsePtr resp;
    ctrl.get(
        authed(viewer_b), [&](const HttpResponsePtr& r) { resp = r; }, created.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

// ── POST /api/v1/documents/{id}/download-url ────────────────────────────────

TEST_F(LedgerDocumentsApiTest, DownloadUrlWithoutFileConflict) {
    auto org = seed_org("444240000008", "No File Org LLP");
    Ledger::DocumentRepository repo;
    // Version 1 exists (create() always makes one) but has no file in it,
    // and nothing published it — exactly the "render never finished" state.
    auto created = repo.create(org.id, "invoice", "generated", "draft");

    auto viewer = member("viewer7@example.com", org.id, "viewer");
    HttpResponsePtr resp;
    ctrl.downloadUrl(
        authed(viewer, Post), [&](const HttpResponsePtr& r) { resp = r; }, created.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "no_file");
}

TEST_F(LedgerDocumentsApiTest, DownloadUrlCrossOrgNotFound) {
    auto org_a = seed_org("444240000009", "DL Org A LLP");
    auto org_b = seed_org("444240000010", "DL Org B LLP");
    Ledger::DocumentRepository repo;
    auto created = repo.create(org_a.id, "invoice", "generated", "draft");

    auto viewer_b = member("viewer8@example.com", org_b.id, "viewer");
    HttpResponsePtr resp;
    ctrl.downloadUrl(
        authed(viewer_b, Post), [&](const HttpResponsePtr& r) { resp = r; }, created.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

// Positive counterpart to the RBAC tests elsewhere in this file: download-url
// is read-only (mints a URL, writes nothing), so it deliberately does NOT
// carry the viewer-mutation gate the other mutating routes do — this fixes
// that exception in place as an explicit, asserted behavior rather than
// something only visible by the ABSENCE of a 403 test.
TEST_F(LedgerDocumentsApiTest, DownloadUrlViewerAllowed) {
    auto org = seed_org("444240000022", "Viewer DL Allowed Org LLP");
    Ledger::DocumentRepository repo;
    auto created = repo.create(org.id, "invoice", "generated", "draft");
    // Stand in for the render worker: fill version 1's file AND publish it —
    // a document only reports the file of its CURRENT version.
    auto seeded_version = repo.latest_version(org.id, created.id);
    ASSERT_TRUE(seeded_version);
    ASSERT_TRUE(repo.set_version_file(org.id,
                                      seeded_version->id,
                                      Files::org_key(org.id, "generated", "report.pdf"),
                                      std::string(64, 'b'),
                                      "application/pdf",
                                      42));
    ASSERT_TRUE(repo.set_current_version(org.id, created.id, seeded_version->id));

    auto viewer = member("viewerdl@example.com", org.id, "viewer");
    HttpResponsePtr resp;
    ctrl.downloadUrl(
        authed(viewer, Post), [&](const HttpResponsePtr& r) { resp = r; }, created.id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_FALSE(body["url"].get<std::string>().empty());
}

// ── POST /api/v1/documents/uploads ──────────────────────────────────────────

TEST_F(LedgerDocumentsApiTest, UploadsViewerForbidden) {
    auto org = seed_org("444240000011", "Uploads Viewer Org LLP");
    auto viewer = member("viewer9@example.com", org.id, "viewer");

    auto req = authed_json(viewer, {{"filename", "invoice.pdf"}, {"mime", "application/pdf"}, {"doc_type", "invoice"}});
    HttpResponsePtr resp;
    ctrl.startUpload(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(LedgerDocumentsApiTest, UploadsInvalidDocTypeRejected) {
    auto org = seed_org("444240000012", "Uploads Bad Type Org LLP");
    auto accountant = member("accountant1@example.com", org.id, "accountant");

    auto req =
        authed_json(accountant, {{"filename", "invoice.pdf"}, {"mime", "application/pdf"}, {"doc_type", "not_a_type"}});
    HttpResponsePtr resp;
    ctrl.startUpload(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "doc_type");
}

// Fix round 2 regression: a genuinely malformed body (missing field) must
// still be a 400, not swept into the same 422 path as a semantic failure —
// this is what separating startUpload()'s structural/semantic Errors
// collectors into two phases has to keep working.
TEST_F(LedgerDocumentsApiTest, UploadsMissingFieldRejected) {
    auto org = seed_org("444240000024", "Uploads Missing Field Org LLP");
    auto accountant = member("accountant7@example.com", org.id, "accountant");

    auto req = authed_json(accountant, {{"filename", "invoice.pdf"}, {"mime", "application/pdf"}});  // no doc_type
    HttpResponsePtr resp;
    ctrl.startUpload(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "doc_type");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "missing");
}

// Security: filename must be a plain file name, checked BEFORE it ever
// reaches Files::org_key() — see LedgerDocumentsController.hpp's Security
// note. A traversal-shaped filename is rejected outright...
TEST_F(LedgerDocumentsApiTest, UploadsPathTraversalFilenameRejected) {
    auto org = seed_org("444240000018", "Uploads Traversal Org LLP");
    auto accountant = member("accountant5@example.com", org.id, "accountant");

    auto req = authed_json(
        accountant, {{"filename", "../../etc/passwd"}, {"mime", "application/octet-stream"}, {"doc_type", "other"}});
    HttpResponsePtr resp;
    ctrl.startUpload(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "filename");
}

// ...while a perfectly legitimate filename with non-ASCII characters,
// spaces, and parentheses is accepted (those get sanitized in the S3 KEY by
// Files::org_key, but they are valid FILE NAMES and must not be rejected).
// The original filename is preserved in input_snapshot as metadata.
TEST_F(LedgerDocumentsApiTest, UploadsUnicodeFilenameAccepted) {
    auto org = seed_org("444240000019", "Uploads Unicode Org LLP");
    auto accountant = member("accountant6@example.com", org.id, "accountant");

    auto req =
        authed_json(accountant, {{"filename", "отчёт (1).pdf"}, {"mime", "application/pdf"}, {"doc_type", "invoice"}});
    HttpResponsePtr resp;
    ctrl.startUpload(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k201Created);
    auto body = json::parse(std::string(resp->body()));
    const std::string doc_id = body["data"]["id"].get<std::string>();
    ASSERT_FALSE(body["data"]["s3_key"].is_null());
    // The S3 key itself is sanitized (org_key's [A-Za-z0-9._-] allowlist) —
    // the raw filename must not appear verbatim in it.
    EXPECT_EQ(body["data"]["s3_key"].get<std::string>().find("отчёт"), std::string::npos);

    Ledger::DocumentRepository repo;
    auto found = repo.find_in_org(doc_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->input_snapshot.has_value());
    EXPECT_EQ((*found->input_snapshot)["original_filename"].get<std::string>(), "отчёт (1).pdf");
}

// Fix round 1 (repository-level regression): DocumentRepository::
// set_pending_upload() must refuse to write on a non-draft document — see
// that method's doc comment in DocumentRepository.hpp for the data-loss
// scenario this guards against (a future re-upload/replace-file caller
// clobbering an already-confirmed, 'final' document's real s3_key/mime with
// a fresh presigned-but-unconfirmed pair). Exercised directly against the
// repository, not through the controller — LedgerDocumentsController never
// calls set_pending_upload() on anything but a document it just created as
// 'draft', so this invariant has no controller-level route to reach today.
TEST_F(LedgerDocumentsApiTest, SetPendingUploadGuardedToDraftStatus) {
    auto org = seed_org("444240000023", "Guard Org LLP");
    Ledger::DocumentRepository repo;
    auto created = repo.create(org.id, "invoice", "generated", "draft");
    const std::string confirmed_key = Files::org_key(org.id, "generated", "confirmed.pdf");
    auto seeded_version = repo.latest_version(org.id, created.id);
    ASSERT_TRUE(seeded_version);
    ASSERT_TRUE(
        repo.set_version_file(org.id, seeded_version->id, confirmed_key, std::string(64, 'a'), "application/pdf", 123));
    ASSERT_TRUE(repo.set_current_version(org.id, created.id, seeded_version->id));
    ASSERT_TRUE(repo.set_status(org.id, created.id, "final"));

    EXPECT_FALSE(repo.set_pending_upload(
        org.id, created.id, Files::org_key(org.id, "inbox", "malicious.bin"), "application/octet-stream"));

    auto after = repo.find_in_org(created.id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->status, "final");
    ASSERT_TRUE(after->s3_key.has_value());
    EXPECT_EQ(*after->s3_key, confirmed_key);
    EXPECT_EQ(after->mime.value_or(""), "application/pdf");
}

// ── POST /api/v1/documents/{id}/confirm-upload ──────────────────────────────

TEST_F(LedgerDocumentsApiTest, ConfirmUploadViewerForbidden) {
    auto org = seed_org("444240000013", "Confirm Viewer Org LLP");
    Ledger::DocumentRepository repo;
    auto created = repo.create(org.id, "invoice", "uploaded", "draft");
    auto viewer = member("viewer10@example.com", org.id, "viewer");

    auto req = authed_json(viewer, {{"size_bytes", 10}, {"checksum_sha256", std::string(64, 'a')}});
    HttpResponsePtr resp;
    ctrl.confirmUpload(
        req, [&](const HttpResponsePtr& r) { resp = r; }, created.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(LedgerDocumentsApiTest, ConfirmUploadObjectMissingConflict) {
    auto org = seed_org("444240000014", "Confirm Missing Org LLP");
    auto accountant = member("accountant2@example.com", org.id, "accountant");

    // Start an upload but never actually PUT bytes to the presigned URL.
    auto start_req =
        authed_json(accountant, {{"filename", "ghost.pdf"}, {"mime", "application/pdf"}, {"doc_type", "invoice"}});
    HttpResponsePtr start_resp;
    ctrl.startUpload(start_req, [&](const HttpResponsePtr& r) { start_resp = r; });
    ASSERT_EQ(start_resp->statusCode(), k201Created);
    auto doc_id = json::parse(std::string(start_resp->body()))["data"]["id"].get<std::string>();

    auto confirm_req = authed_json(accountant, {{"size_bytes", 10}, {"checksum_sha256", std::string(64, 'a')}});
    HttpResponsePtr resp;
    ctrl.confirmUpload(
        confirm_req, [&](const HttpResponsePtr& r) { resp = r; }, doc_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "object_missing");
}

TEST_F(LedgerDocumentsApiTest, ConfirmUploadCrossOrgNotFound) {
    auto org_a = seed_org("444240000015", "Confirm Org A LLP");
    auto org_b = seed_org("444240000016", "Confirm Org B LLP");
    Ledger::DocumentRepository repo;
    auto created = repo.create(org_a.id, "invoice", "uploaded", "draft");
    auto accountant_b = member("accountant3@example.com", org_b.id, "accountant");

    auto req = authed_json(accountant_b, {{"size_bytes", 10}, {"checksum_sha256", std::string(64, 'a')}});
    HttpResponsePtr resp;
    ctrl.confirmUpload(
        req, [&](const HttpResponsePtr& r) { resp = r; }, created.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

// Final pre-merge fix: lifecycle guard — confirm-upload must refuse anything
// that isn't a draft, uploaded document, checked BEFORE the s3_key/
// Storage::exists() checks (a generated document already has a real s3_key
// and an existing object, so those alone would not catch this). Without the
// guard, this would let the client's reported size_bytes/checksum_sha256
// overwrite docgen's real, reproducible audit metadata.
TEST_F(LedgerDocumentsApiTest, ConfirmUploadRejectedForGeneratedDocument) {
    auto org = seed_org("444240000027", "Confirm Generated Org LLP");
    Ledger::DocumentRepository repo;
    auto created = repo.create(org.id, "invoice", "generated", "draft");
    auto accountant = member("accountant11@example.com", org.id, "accountant");

    auto req = authed_json(accountant, {{"size_bytes", 42}, {"checksum_sha256", std::string(64, 'e')}});
    HttpResponsePtr resp;
    ctrl.confirmUpload(
        req, [&](const HttpResponsePtr& r) { resp = r; }, created.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "invalid_state");
}

// Same guard, the other lifecycle edge: an uploaded document that was ALREADY
// confirmed once (now 'final') must reject a second confirm-upload — proven
// end-to-end through a real startUpload -> curl PUT -> confirm (success) ->
// confirm again (rejected) sequence, not by hand-setting the row.
TEST_F(LedgerDocumentsApiTest, ConfirmUploadRejectedForFinalizedUpload) {
    auto org = seed_org("444240000028", "Confirm Twice Org LLP");
    auto accountant = member("accountant12@example.com", org.id, "accountant");

    auto start_req =
        authed_json(accountant, {{"filename", "twice.pdf"}, {"mime", "application/pdf"}, {"doc_type", "invoice"}});
    HttpResponsePtr start_resp;
    ctrl.startUpload(start_req, [&](const HttpResponsePtr& r) { start_resp = r; });
    ASSERT_EQ(start_resp->statusCode(), k201Created);
    auto start_body = json::parse(std::string(start_resp->body()));
    const std::string doc_id = start_body["data"]["id"].get<std::string>();
    const std::string upload_url = start_body["upload_url"].get<std::string>();

    const std::string payload = "twice-bytes-" + Jobs::generate_uuid();
    const auto tmp_path = std::filesystem::temp_directory_path() / ("docapi-twice-" + Jobs::generate_uuid() + ".bin");
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << payload;
    }
    const std::string put_code = run_capture("curl -s -o /dev/null -w '%{http_code}' -X PUT --data-binary @'" +
                                             tmp_path.string() + "' '" + upload_url + "'");
    std::filesystem::remove(tmp_path);
    ASSERT_EQ(put_code, "200");

    const json confirm_body = {{"size_bytes", static_cast<long long>(payload.size())},
                               {"checksum_sha256", Utils::Crypto::sha256_hex(payload)}};

    // First confirm — succeeds (document flips draft -> final).
    HttpResponsePtr first_resp;
    ctrl.confirmUpload(
        authed_json(accountant, confirm_body), [&](const HttpResponsePtr& r) { first_resp = r; }, doc_id);
    ASSERT_NE(first_resp, nullptr);
    ASSERT_EQ(first_resp->statusCode(), k200OK);

    // Second confirm — same document, now 'final' — must be rejected.
    HttpResponsePtr second_resp;
    ctrl.confirmUpload(
        authed_json(accountant, confirm_body), [&](const HttpResponsePtr& r) { second_resp = r; }, doc_id);
    ASSERT_NE(second_resp, nullptr);
    EXPECT_EQ(second_resp->statusCode(), k409Conflict);
    auto body = json::parse(std::string(second_resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "invalid_state");
}

// Fix round 2: checksum_sha256 present and a string (structural checks
// pass), but the wrong shape (not 64 lowercase hex chars) — a semantic
// failure, must be 422, not 400. Before the fix this was indistinguishable
// from a missing/wrong-type field.
TEST_F(LedgerDocumentsApiTest, ConfirmUploadInvalidChecksumFormatRejected) {
    auto org = seed_org("444240000025", "Confirm Bad Checksum Org LLP");
    Ledger::DocumentRepository repo;
    auto created = repo.create(org.id, "invoice", "uploaded", "draft");
    auto accountant = member("accountant8@example.com", org.id, "accountant");

    auto req = authed_json(accountant, {{"size_bytes", 10}, {"checksum_sha256", "not-a-valid-checksum"}});
    HttpResponsePtr resp;
    ctrl.confirmUpload(
        req, [&](const HttpResponsePtr& r) { resp = r; }, created.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "checksum_sha256");
}

// Fix round 2 regression (structural side): a genuinely malformed body
// (missing field) on confirm-upload must still be 400.
TEST_F(LedgerDocumentsApiTest, ConfirmUploadMissingFieldRejected) {
    auto org = seed_org("444240000026", "Confirm Missing Field Org LLP");
    Ledger::DocumentRepository repo;
    auto created = repo.create(org.id, "invoice", "uploaded", "draft");
    auto accountant = member("accountant9@example.com", org.id, "accountant");

    auto req = authed_json(accountant, {{"checksum_sha256", std::string(64, 'a')}});  // no size_bytes
    HttpResponsePtr resp;
    ctrl.confirmUpload(
        req, [&](const HttpResponsePtr& r) { resp = r; }, created.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "size_bytes");
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "missing");
}

// ── Full round trip: uploads -> (client PUT via curl) -> confirm-upload ->
// download-url -> (client GET via curl) ──────────────────────────────────────

TEST_F(LedgerDocumentsApiTest, UploadConfirmAndDownloadRoundTrip) {
    auto org = seed_org("444240000017", "Round Trip Org LLP");
    auto accountant = member("accountant4@example.com", org.id, "accountant");

    // 1. Start the upload — draft document + presigned PUT.
    auto start_req =
        authed_json(accountant, {{"filename", "invoice.pdf"}, {"mime", "application/pdf"}, {"doc_type", "invoice"}});
    HttpResponsePtr start_resp;
    ctrl.startUpload(start_req, [&](const HttpResponsePtr& r) { start_resp = r; });
    ASSERT_NE(start_resp, nullptr);
    ASSERT_EQ(start_resp->statusCode(), k201Created);
    auto start_body = json::parse(std::string(start_resp->body()));
    const std::string doc_id = start_body["data"]["id"].get<std::string>();
    const std::string upload_url = start_body["upload_url"].get<std::string>();
    EXPECT_EQ(start_body["data"]["status"].get<std::string>(), "draft");
    EXPECT_EQ(start_body["data"]["source"].get<std::string>(), "uploaded");
    ASSERT_FALSE(start_body["data"]["s3_key"].is_null());
    // The key is visible THROUGH the document, i.e. the current-version
    // pointer is already set — an upload has no async render job to move it
    // later, so if startUpload did not publish version 1 the confirm below
    // would answer 409 no_pending_upload instead of 200
    // (migrations/018_document_versions.sql, DocumentRepository::
    // set_pending_upload).
    ASSERT_FALSE(start_body["data"]["current_version_id"].is_null());
    EXPECT_EQ(start_body["data"]["latest_version_no"].get<int>(), 1);

    // 2. The "client" uploads bytes directly to S3 via the presigned PUT —
    // bare curl, fully independent of this binary's own libcurl usage.
    const std::string payload = "invoice-bytes-" + Jobs::generate_uuid();
    const auto tmp_path = std::filesystem::temp_directory_path() / ("docapi-put-" + Jobs::generate_uuid() + ".bin");
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << payload;
    }
    const std::string put_code = run_capture("curl -s -o /dev/null -w '%{http_code}' -X PUT --data-binary @'" +
                                             tmp_path.string() + "' '" + upload_url + "'");
    std::filesystem::remove(tmp_path);
    ASSERT_EQ(put_code, "200");

    // 3. Confirm the upload — Storage::exists() must now see the object.
    const std::string checksum = Utils::Crypto::sha256_hex(payload);
    auto confirm_req = authed_json(
        accountant, {{"size_bytes", static_cast<long long>(payload.size())}, {"checksum_sha256", checksum}});
    HttpResponsePtr confirm_resp;
    ctrl.confirmUpload(
        confirm_req, [&](const HttpResponsePtr& r) { confirm_resp = r; }, doc_id);
    ASSERT_NE(confirm_resp, nullptr);
    ASSERT_EQ(confirm_resp->statusCode(), k200OK);
    auto confirm_body = json::parse(std::string(confirm_resp->body()));
    EXPECT_EQ(confirm_body["data"]["status"].get<std::string>(), "final");
    EXPECT_EQ(confirm_body["data"]["checksum_sha256"].get<std::string>(), checksum);
    EXPECT_EQ(confirm_body["data"]["size_bytes"].get<long long>(), static_cast<long long>(payload.size()));
    EXPECT_EQ(confirm_body["data"]["mime"].get<std::string>(), "application/pdf");
    // Confirming an upload fills the EXISTING version in, it does not append
    // a second one — the bytes never changed.
    Ledger::DocumentRepository repo;
    EXPECT_EQ(repo.list_versions(org.id, doc_id).size(), 1u);

    // 4. Mint a download URL and fetch it back — round trip complete.
    HttpResponsePtr dl_resp;
    ctrl.downloadUrl(
        authed(accountant, Post), [&](const HttpResponsePtr& r) { dl_resp = r; }, doc_id);
    ASSERT_NE(dl_resp, nullptr);
    ASSERT_EQ(dl_resp->statusCode(), k200OK);
    const std::string download_url = json::parse(std::string(dl_resp->body()))["url"].get<std::string>();
    const std::string fetched = run_capture("curl -s '" + download_url + "'");
    EXPECT_EQ(fetched, payload);
}

// ── P3 task 9: editing through the creation allowlist + version history ──────
//
// These tests exist to prove ONE property: the edit endpoint accepts exactly
// the same fields creation does and re-derives everything else from what is
// already stored. If the allowlist were deleted,
// EditRejectsFieldsOutsideTheCreationAllowlist,
// EditRejectsServerDerivedMoneyStrings and
// EditOfAServerBuiltFormCarriesDerivedFiguresForward all fail — the first two
// because a forged figure would be accepted, the third because the carried-
// forward figure would come from the request instead of the previous version.

TEST_F(LedgerDocumentsApiTest, EditCreatesANewVersionAndKeepsTheOldPdf) {
    if (!templates_available())
        GTEST_SKIP() << "repo templates not reachable from this working directory";
    auto org = seed_org("444240000030", "Edit Versions Org LLP");
    auto p = member("edit@example.com", org.id, "accountant");
    const std::string original_pdf = "invoice-v1-bytes-" + Jobs::generate_uuid();
    const std::string doc_id = seed_rendered_invoice(org.id, /*total_tiyn=*/1234567, original_pdf);

    HttpResponsePtr before;
    ctrl.get(
        authed(p), [&](const HttpResponsePtr& r) { before = r; }, doc_id);
    ASSERT_NE(before, nullptr);
    ASSERT_EQ(before->statusCode(), k200OK);
    const std::string old_key = json::parse(std::string(before->body()))["data"]["s3_key"].get<std::string>();

    const json edit = {{"input", make_invoice_input(/*total_tiyn=*/200000)}};
    HttpResponsePtr resp;
    ctrl.createVersion(
        authed_json(p, edit), [&](const HttpResponsePtr& r) { resp = r; }, doc_id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted) << std::string(resp->body());
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["version_no"].get<int>(), 2);
    EXPECT_TRUE(body["render_queued"].get<bool>());

    HttpResponsePtr versions;
    ctrl.listVersions(
        authed(p), [&](const HttpResponsePtr& r) { versions = r; }, doc_id);
    ASSERT_NE(versions, nullptr);
    ASSERT_EQ(versions->statusCode(), k200OK);
    auto version_body = json::parse(std::string(versions->body()));
    ASSERT_EQ(version_body["data"].size(), 2U);
    EXPECT_EQ(version_body["data"][0]["version_no"].get<int>(), 1);
    EXPECT_EQ(version_body["data"][1]["version_no"].get<int>(), 2);
    // The new version has no file yet — nothing rendered it in this suite.
    EXPECT_TRUE(version_body["data"][1]["s3_key"].is_null());
    // input_snapshot is never in the list payload (Ledger::to_json).
    EXPECT_FALSE(version_body["data"][0].contains("input_snapshot"));

    // The OLD file is still in storage and still downloadable by its own
    // version number — this is the whole reason an edit appends instead of
    // overwriting. Fetched with bare curl, so the bytes are proven, not the
    // row.
    HttpResponsePtr url1;
    ctrl.versionDownloadUrl(
        authed(p, Post), [&](const HttpResponsePtr& r) { url1 = r; }, doc_id, "1");
    ASSERT_NE(url1, nullptr);
    ASSERT_EQ(url1->statusCode(), k200OK);
    const std::string url = json::parse(std::string(url1->body()))["url"].get<std::string>();
    ASSERT_FALSE(url.empty());
    EXPECT_EQ(run_capture("curl -s '" + url + "'"), original_pdf);

    // The current-version pointer has NOT moved: version 2's render has not
    // finished, so the document still reports version 1's file.
    Ledger::DocumentRepository repo;
    auto doc = repo.find_in_org(doc_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(doc.has_value());
    ASSERT_TRUE(doc->s3_key.has_value());
    EXPECT_EQ(*doc->s3_key, old_key);
    EXPECT_EQ(doc->latest_version_no, 2);

    // The new version stored the DERIVED snapshot, and the response's
    // version_id names that row (the render job's payload key task 10 reads).
    auto v2 = repo.find_version(org.id, doc_id, 2);
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(body["version_id"].get<std::string>(), v2->id);
    ASSERT_TRUE(v2->input_snapshot.has_value());
    EXPECT_EQ((*v2->input_snapshot)["total"].get<std::string>(), Money::format_tiyn_ru(200000));
    EXPECT_EQ((*v2->input_snapshot)["total_words"].get<std::string>(), Money::to_words_ru(200000));

    // A docgen.render job was ENQUEUED, not executed — and its payload names
    // the version, which is what lets task 10 land the PDF on the right one
    // instead of on "the newest version at the time the worker got round to
    // it". The rendered input is the stored snapshot, not the request body.
    std::vector<std::string> ids;
    Cache::get().get_client().lrange(Jobs::queue_key(kRenderJobType), 0, -1, std::back_inserter(ids));
    ASSERT_EQ(ids.size(), 1U);
    auto job = Jobs::get().get_status(ids[0]);
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->type, kRenderJobType);
    EXPECT_EQ(job->payload["org_id"], org.id);
    EXPECT_EQ(job->payload["document_id"], doc_id);
    EXPECT_EQ(job->payload["version_id"], v2->id);
    EXPECT_EQ(job->payload["slug"], "invoice");
    EXPECT_EQ(job->payload["input"], *v2->input_snapshot);
}

// ── P3 task 10: the RENDER of a new version leaves the old PDF alone ─────────
//
// EditCreatesANewVersionAndKeepsTheOldPdf proves the edit does not touch
// version 1. This proves the half that actually writes to object storage: the
// worker runs, for real, over the very payload that edit enqueued, against
// real MinIO — and afterwards version 1 is still fetchable through ITS OWN
// presigned URL and still contains ITS OWN bytes. Fetched with bare curl, so
// what is proven is the object, not a database row: if the render job keyed
// the object on the document instead of the version, version 2's bytes would
// come back here and this test would fail.
TEST_F(LedgerDocumentsApiTest, RenderingVersionTwoLeavesVersionOnesPdfDownloadable) {
    if (!templates_available())
        GTEST_SKIP() << "repo templates not reachable from this working directory";
    auto org = seed_org("444240000045", "Render Keeps Old Pdf Org LLP");
    auto p = member("render-v2@example.com", org.id, "accountant");
    const std::string original_pdf = "invoice-v1-bytes-" + Jobs::generate_uuid();
    const std::string doc_id = seed_rendered_invoice(org.id, /*total_tiyn=*/1234567, original_pdf);

    Ledger::DocumentRepository repo;
    const auto v1 = repo.find_version(org.id, doc_id, 1);
    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v1->s3_key.has_value());
    const std::string v1_key = *v1->s3_key;

    HttpResponsePtr resp;
    ctrl.createVersion(
        authed_json(p, json{{"input", make_invoice_input(/*total_tiyn=*/200000)}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        doc_id);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted) << std::string(resp->body());

    // Run the worker over the payload the endpoint actually enqueued — no
    // hand-built job, so the producer's version_id is part of what is tested.
    std::vector<std::string> ids;
    Cache::get().get_client().lrange(Jobs::queue_key(kRenderJobType), 0, -1, std::back_inserter(ids));
    ASSERT_EQ(ids.size(), 1U);
    auto job = Jobs::get().get_status(ids[0]);
    ASSERT_TRUE(job.has_value());

    const std::string rendered_pdf = "%PDF-1.1 rendered-version-2-" + Jobs::generate_uuid() + "\n";
    use_latex_stub(rendered_pdf);
    auto result = Docgen::process_job(job->payload);
    ASSERT_FALSE(result.contains("skipped")) << result.dump();
    const std::string v2_key = result["key"].get<std::string>();
    ASSERT_NE(v2_key, v1_key);

    // Version 1: its own presigned URL, its own bytes — unchanged.
    HttpResponsePtr url1;
    ctrl.versionDownloadUrl(
        authed(p, Post), [&](const HttpResponsePtr& r) { url1 = r; }, doc_id, "1");
    ASSERT_NE(url1, nullptr);
    ASSERT_EQ(url1->statusCode(), k200OK);
    EXPECT_EQ(run_capture("curl -s '" + json::parse(std::string(url1->body()))["url"].get<std::string>() + "'"),
              original_pdf);

    // Version 2: its own presigned URL, the freshly rendered bytes.
    HttpResponsePtr url2;
    ctrl.versionDownloadUrl(
        authed(p, Post), [&](const HttpResponsePtr& r) { url2 = r; }, doc_id, "2");
    ASSERT_NE(url2, nullptr);
    ASSERT_EQ(url2->statusCode(), k200OK);
    EXPECT_EQ(run_capture("curl -s '" + json::parse(std::string(url2->body()))["url"].get<std::string>() + "'"),
              rendered_pdf);

    // The document itself has moved on to version 2, and version 1's row
    // still names version 1's object.
    auto after = repo.find_in_org(doc_id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->s3_key.value_or(""), v2_key);
    EXPECT_EQ(after->status, "final");
    const auto v1_again = repo.find_version(org.id, doc_id, 1);
    ASSERT_TRUE(v1_again.has_value());
    EXPECT_EQ(v1_again->s3_key.value_or(""), v1_key);
}

TEST_F(LedgerDocumentsApiTest, EditRejectsFieldsOutsideTheCreationAllowlist) {
    if (!templates_available())
        GTEST_SKIP() << "repo templates not reachable from this working directory";
    auto org = seed_org("444240000031", "Edit Allowlist Org LLP");
    auto p = member("edit2@example.com", org.id, "accountant");
    const std::string filing_doc = seed_fno910_document(org.id);

    Ledger::DocumentRepository repo;
    const auto before = repo.find_version(org.id, filing_doc, 1);
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(before->input_snapshot.has_value());
    const json snapshot_before = *before->input_snapshot;

    // `director`/`accountant` ARE editable on a ФНО; `tax_tenge` is the figure
    // the filing's XML also states, and rewriting it here is precisely the P2
    // forgery this endpoint must make unrepresentable.
    const json edit = {{"input", {{"director", "С.С."}, {"accountant", "И.И."}, {"tax_tenge", "1,00"}}}};
    HttpResponsePtr resp;
    ctrl.createVersion(
        authed_json(p, edit), [&](const HttpResponsePtr& r) { resp = r; }, filing_doc);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "not_allowed_override");
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "tax_tenge");

    // Rejected means NOTHING was written — not "the good half was applied".
    EXPECT_EQ(repo.list_versions(org.id, filing_doc).size(), 1U);
    const auto after = repo.find_version(org.id, filing_doc, 1);
    ASSERT_TRUE(after.has_value());
    ASSERT_TRUE(after->input_snapshot.has_value());
    EXPECT_EQ(*after->input_snapshot, snapshot_before);
    EXPECT_EQ((*after->input_snapshot)["tax_tenge"].get<std::string>(),
              snapshot_before["tax_tenge"].get<std::string>());
}

// The other half of the same allowlist, on the caller-authored side: primary
// documents take the whole `input`, so what protects them is the money
// derivation — the printed sum and its words are the SERVER's, from an integer
// tiyn field, and a client-supplied string is refused exactly as at creation.
TEST_F(LedgerDocumentsApiTest, EditRejectsServerDerivedMoneyStrings) {
    if (!templates_available())
        GTEST_SKIP() << "repo templates not reachable from this working directory";
    auto org = seed_org("444240000032", "Edit Money Org LLP");
    auto p = member("edit6@example.com", org.id, "accountant");
    const std::string doc_id = seed_rendered_invoice(org.id, /*total_tiyn=*/1234567, "seed-bytes");

    json forged = make_invoice_input(/*total_tiyn=*/100);
    forged["total_words"] = "Один миллион тенге 00 тиын";
    HttpResponsePtr resp;
    ctrl.createVersion(
        authed_json(p, json{{"input", forged}}), [&](const HttpResponsePtr& r) { resp = r; }, doc_id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k422UnprocessableEntity);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["errors"][0]["code"].get<std::string>(), "not_allowed_override");
    EXPECT_EQ(body["errors"][0]["field"].get<std::string>(), "input.total_words");

    Ledger::DocumentRepository repo;
    EXPECT_EQ(repo.list_versions(org.id, doc_id).size(), 1U);
}

// Fix round 1: a body that is present but is not a JSON object used to be a
// 500, not a 400. `contains()` is false on a non-object json, so the `input`
// type guard was skipped and `body.value("input", …)` threw
// nlohmann type_error.306 — one line before with_repo_errors, so nothing
// mapped it. A malformed request SHAPE is a 400, always.
TEST_F(LedgerDocumentsApiTest, EditWithANonObjectBodyIsABadRequest) {
    auto org = seed_org("444240000045", "Edit Bad Body Org LLP");
    auto p = member("edit10@example.com", org.id, "accountant");
    const std::string doc_id = seed_rendered_invoice(org.id, /*total_tiyn=*/1234567, "seed-bytes");

    for (const json& bad : {json(nullptr), json::array(), json("x"), json(5)}) {
        HttpResponsePtr resp;
        ctrl.createVersion(
            authed_json(p, bad), [&](const HttpResponsePtr& r) { resp = r; }, doc_id);
        ASSERT_NE(resp, nullptr) << bad.dump();
        EXPECT_EQ(resp->statusCode(), k400BadRequest) << bad.dump();
        auto body = json::parse(std::string(resp->body()));
        EXPECT_EQ(body["error"].get<std::string>(), "invalid_json") << bad.dump();
    }
    // `input` present but of the wrong type stays the field-level 400 it was.
    HttpResponsePtr typed;
    ctrl.createVersion(
        authed_json(p, json{{"input", 5}}), [&](const HttpResponsePtr& r) { typed = r; }, doc_id);
    ASSERT_NE(typed, nullptr);
    EXPECT_EQ(typed->statusCode(), k400BadRequest);
    auto typed_body = json::parse(std::string(typed->body()));
    EXPECT_EQ(typed_body["errors"][0]["field"].get<std::string>(), "input");
    EXPECT_EQ(typed_body["errors"][0]["code"].get<std::string>(), "not_object");

    Ledger::DocumentRepository repo;
    EXPECT_EQ(repo.list_versions(org.id, doc_id).size(), 1U);
}

// The other half of parse_optional_body's contract, and what makes
// `requestBody: required: false` in docs/openapi.yaml true: NO body at all is
// a legitimate edit — it edits nothing and re-renders the stored input
// faithfully.
TEST_F(LedgerDocumentsApiTest, EditWithNoBodyRerendersTheStoredInputUnchanged) {
    if (!templates_available())
        GTEST_SKIP() << "repo templates not reachable from this working directory";
    auto org = seed_org("444240000046", "Edit Empty Body Org LLP");
    auto p = member("edit11@example.com", org.id, "accountant");
    const std::string filing_doc = seed_fno910_document(org.id);

    Ledger::DocumentRepository repo;
    const auto v1 = repo.find_version(org.id, filing_doc, 1);
    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v1->input_snapshot.has_value());

    HttpResponsePtr resp;
    ctrl.createVersion(
        authed(p, Post), [&](const HttpResponsePtr& r) { resp = r; }, filing_doc);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted) << std::string(resp->body());
    EXPECT_EQ(json::parse(std::string(resp->body()))["version_no"].get<int>(), 2);

    const auto v2 = repo.find_version(org.id, filing_doc, 2);
    ASSERT_TRUE(v2.has_value());
    ASSERT_TRUE(v2->input_snapshot.has_value());
    EXPECT_EQ(*v2->input_snapshot, *v1->input_snapshot);
}

TEST_F(LedgerDocumentsApiTest, EditOfAServerBuiltFormCarriesDerivedFiguresForward) {
    if (!templates_available())
        GTEST_SKIP() << "repo templates not reachable from this working directory";
    auto org = seed_org("444240000033", "Edit Carry Forward Org LLP");
    auto p = member("edit3@example.com", org.id, "accountant");
    const std::string filing_doc = seed_fno910_document(org.id);

    Ledger::DocumentRepository repo;
    const auto v1 = repo.find_version(org.id, filing_doc, 1);
    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v1->input_snapshot.has_value());
    const std::string tax_words = (*v1->input_snapshot)["tax_words"].get<std::string>();

    const json edit = {{"input", {{"director", "Новый директор"}}}};
    HttpResponsePtr resp;
    ctrl.createVersion(
        authed_json(p, edit), [&](const HttpResponsePtr& r) { resp = r; }, filing_doc);
    ASSERT_NE(resp, nullptr);
    ASSERT_EQ(resp->statusCode(), k202Accepted) << std::string(resp->body());

    const auto v2 = repo.find_version(org.id, filing_doc, 2);
    ASSERT_TRUE(v2.has_value());
    ASSERT_TRUE(v2->input_snapshot.has_value());
    EXPECT_EQ((*v2->input_snapshot)["director"].get<std::string>(), "Новый директор");
    // Untouched by the request, carried over from the version that WAS
    // rendered out of the authoritative calculation.
    EXPECT_EQ((*v2->input_snapshot)["tax_words"].get<std::string>(), tax_words);
    EXPECT_EQ((*v2->input_snapshot)["tax_tenge"], (*v1->input_snapshot)["tax_tenge"]);
    EXPECT_EQ((*v2->input_snapshot)["income_tenge"], (*v1->input_snapshot)["income_tenge"]);
    EXPECT_EQ((*v2->input_snapshot)["org"], (*v1->input_snapshot)["org"]);
    // The one editable field the request did NOT send keeps its old value —
    // the base is the previous snapshot, not an empty object.
    EXPECT_EQ((*v2->input_snapshot)["accountant"], (*v1->input_snapshot)["accountant"]);
}

TEST_F(LedgerDocumentsApiTest, UploadedAndEmailDocumentsCannotBeEdited) {
    auto org = seed_org("444240000034", "Edit Not Editable Org LLP");
    auto p = member("edit4@example.com", org.id, "accountant");
    Ledger::DocumentRepository repo;
    for (const auto* source : {"uploaded", "email"}) {
        auto doc = repo.create(org.id, "incoming", source, "inbox");
        HttpResponsePtr resp;
        ctrl.createVersion(
            authed_json(p, json{{"input", json::object()}}), [&](const HttpResponsePtr& r) { resp = r; }, doc.id);
        ASSERT_NE(resp, nullptr) << source;
        EXPECT_EQ(resp->statusCode(), k409Conflict) << source;
        auto body = json::parse(std::string(resp->body()));
        EXPECT_EQ(body["error"].get<std::string>(), "not_editable") << source;
        EXPECT_EQ(repo.list_versions(org.id, doc.id).size(), 1U) << source;
    }
}

TEST_F(LedgerDocumentsApiTest, VersionDownloadUrlIsFourOhFourForAnUnknownVersionNumber) {
    auto org = seed_org("444240000035", "Edit Unknown Version Org LLP");
    auto p = member("edit5@example.com", org.id, "accountant");
    const std::string doc_id = seed_rendered_invoice(org.id, /*total_tiyn=*/1234567, "seed-bytes");

    HttpResponsePtr resp;
    ctrl.versionDownloadUrl(
        authed(p, Post), [&](const HttpResponsePtr& r) { resp = r; }, doc_id, "99");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

// A version number that is not a number at all is a malformed request SHAPE
// (400), not a semantically wrong value (422) — and it must never reach SQL as
// an integer cast.
TEST_F(LedgerDocumentsApiTest, VersionDownloadUrlRejectsAMalformedVersionNumber) {
    auto org = seed_org("444240000036", "Edit Bad Version Org LLP");
    auto p = member("edit7@example.com", org.id, "accountant");
    const std::string doc_id = seed_rendered_invoice(org.id, /*total_tiyn=*/1234567, "seed-bytes");

    for (const auto* bad : {"abc", "0", "-1", "1.5", ""}) {
        HttpResponsePtr resp;
        ctrl.versionDownloadUrl(
            authed(p, Post), [&](const HttpResponsePtr& r) { resp = r; }, doc_id, bad);
        ASSERT_NE(resp, nullptr) << bad;
        EXPECT_EQ(resp->statusCode(), k400BadRequest) << bad;
        auto body = json::parse(std::string(resp->body()));
        EXPECT_EQ(body["error"].get<std::string>(), "invalid_version") << bad;
    }
}

// A version whose render has not landed has no file — an honest 409, the same
// answer the document-level download-url gives in that state.
TEST_F(LedgerDocumentsApiTest, VersionDownloadUrlIsAConflictWhileTheRenderIsPending) {
    if (!templates_available())
        GTEST_SKIP() << "repo templates not reachable from this working directory";
    auto org = seed_org("444240000037", "Edit Pending Render Org LLP");
    auto p = member("edit8@example.com", org.id, "accountant");
    const std::string doc_id = seed_rendered_invoice(org.id, /*total_tiyn=*/1234567, "seed-bytes");

    HttpResponsePtr edited;
    ctrl.createVersion(
        authed_json(p, json{{"input", make_invoice_input(200000)}}),
        [&](const HttpResponsePtr& r) { edited = r; },
        doc_id);
    ASSERT_NE(edited, nullptr);
    ASSERT_EQ(edited->statusCode(), k202Accepted) << std::string(edited->body());

    HttpResponsePtr resp;
    ctrl.versionDownloadUrl(
        authed(p, Post), [&](const HttpResponsePtr& r) { resp = r; }, doc_id, "2");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["error"].get<std::string>(), "no_file");
}

TEST_F(LedgerDocumentsApiTest, ViewerCannotEditButCanListVersions) {
    auto org = seed_org("444240000038", "Edit Viewer Org LLP");
    auto v = member("viewer11@example.com", org.id, "viewer");
    const std::string doc_id = seed_rendered_invoice(org.id, /*total_tiyn=*/1234567, "seed-bytes");

    HttpResponsePtr edit_resp;
    ctrl.createVersion(
        authed_json(v, json{{"input", json::object()}}), [&](const HttpResponsePtr& r) { edit_resp = r; }, doc_id);
    ASSERT_NE(edit_resp, nullptr);
    EXPECT_EQ(edit_resp->statusCode(), k403Forbidden);
    Ledger::DocumentRepository repo;
    EXPECT_EQ(repo.list_versions(org.id, doc_id).size(), 1U);

    HttpResponsePtr list_resp;
    ctrl.listVersions(
        authed(v), [&](const HttpResponsePtr& r) { list_resp = r; }, doc_id);
    ASSERT_NE(list_resp, nullptr);
    EXPECT_EQ(list_resp->statusCode(), k200OK);
}

TEST_F(LedgerDocumentsApiTest, VersionRoutesAreFourOhFourAcrossOrganizations) {
    auto org_a = seed_org("444240000039", "Versions Org A LLP");
    auto org_b = seed_org("444240000040", "Versions Org B LLP");
    const std::string doc_id = seed_rendered_invoice(org_a.id, /*total_tiyn=*/1234567, "seed-bytes");
    auto accountant_b = member("edit9@example.com", org_b.id, "accountant");

    HttpResponsePtr list_resp;
    ctrl.listVersions(
        authed(accountant_b), [&](const HttpResponsePtr& r) { list_resp = r; }, doc_id);
    ASSERT_NE(list_resp, nullptr);
    EXPECT_EQ(list_resp->statusCode(), k404NotFound);

    HttpResponsePtr edit_resp;
    ctrl.createVersion(
        authed_json(accountant_b, json{{"input", json::object()}}),
        [&](const HttpResponsePtr& r) { edit_resp = r; },
        doc_id);
    ASSERT_NE(edit_resp, nullptr);
    EXPECT_EQ(edit_resp->statusCode(), k404NotFound);

    HttpResponsePtr dl_resp;
    ctrl.versionDownloadUrl(
        authed(accountant_b, Post), [&](const HttpResponsePtr& r) { dl_resp = r; }, doc_id, "1");
    ASSERT_NE(dl_resp, nullptr);
    EXPECT_EQ(dl_resp->statusCode(), k404NotFound);

    Ledger::DocumentRepository repo;
    EXPECT_EQ(repo.list_versions(org_a.id, doc_id).size(), 1U);
}

// ── Repository defects inherited from task 8, closed here because these
// routes are what expose them ────────────────────────────────────────────────

// add_version() used to let the composite FK
// (document_id, org_id) -> documents(id, org_id) be the only wall, so a
// cross-org document id came back as a raw SQLSTATE 23503 — a 500 sitting
// behind POST /documents/{id}/versions. It is a domain 404 now.
TEST_F(LedgerDocumentsApiTest, AddVersionForAnotherOrganizationsDocumentIsANotFound) {
    auto org_a = seed_org("444240000041", "AddVersion Org A LLP");
    auto org_b = seed_org("444240000042", "AddVersion Org B LLP");
    Ledger::DocumentRepository repo;
    auto doc = repo.create(org_a.id, "invoice", "generated", "draft");

    EXPECT_THROW(repo.add_version(org_b.id,
                                  doc.id,
                                  std::optional<nlohmann::json>{json{{"forged", true}}},
                                  std::optional<std::string>{std::string("v1")},
                                  std::nullopt),
                 Repositories::NotFoundError);
    // An id that exists nowhere answers the same way, and neither wrote a row.
    EXPECT_THROW(
        repo.add_version(org_a.id, "00000000-0000-0000-0000-000000000000", std::nullopt, std::nullopt, std::nullopt),
        Repositories::NotFoundError);
    EXPECT_EQ(repo.list_versions(org_a.id, doc.id).size(), 1U);
}

// set_current_version() used to accept a version id from another tenant at
// statement time and only die at COMMIT (documents_current_version_fk is
// DEFERRABLE) — the caller saw a 500 for an ordinary "no such version here".
// The org predicate makes it this method's normal `false`.
TEST_F(LedgerDocumentsApiTest, SetCurrentVersionRejectsAVersionFromAnotherOrganization) {
    auto org_a = seed_org("444240000043", "Publish Org A LLP");
    auto org_b = seed_org("444240000044", "Publish Org B LLP");
    Ledger::DocumentRepository repo;
    auto doc_a = repo.create(org_a.id, "invoice", "generated", "draft");
    auto doc_b = repo.create(org_b.id, "invoice", "generated", "draft");
    auto version_a = repo.latest_version(org_a.id, doc_a.id);
    ASSERT_TRUE(version_a.has_value());

    // No throw, no 500 — a plain false.
    bool published = true;
    ASSERT_NO_THROW(published = repo.set_current_version(org_b.id, doc_b.id, version_a->id));
    EXPECT_FALSE(published);

    // Same document, another document's version of the SAME org: also false.
    auto version_b = repo.latest_version(org_b.id, doc_b.id);
    ASSERT_TRUE(version_b.has_value());
    auto other_doc_b = repo.create(org_b.id, "invoice", "generated", "draft");
    EXPECT_FALSE(repo.set_current_version(org_b.id, other_doc_b.id, version_b->id));

    // And the pointer really did not move.
    auto after = repo.find_in_org(doc_b.id, org_b.id, /*from_primary=*/true);
    ASSERT_TRUE(after.has_value());
    EXPECT_FALSE(after->current_version_id.has_value());
    // The legitimate publish still works.
    EXPECT_TRUE(repo.set_current_version(org_b.id, doc_b.id, version_b->id));
}

// ── P3 task 11: DELETE /documents/{id} vs POST /documents/{id}/void ──────────
//
// These assert the RULE, not a happy path: what may be destroyed is decided by
// the link to a posted journal entry, and everything that may not be destroyed
// must still be disposable — by voiding.

// An uploaded scan that never became the basis of anything is genuinely
// deletable. Note the status: 'inbox', never 'draft' — keying deletion on
// status='draft' (the spec's first draft) would have made this document
// undeletable forever, because only source='generated' rows ever reach that
// status.
TEST_F(LedgerDocumentsApiTest, DeletesADocumentWithNoPostedLink) {
    auto org = seed_org("444240000045", "Delete Plain Org LLP");
    auto p = member("del1@example.com", org.id, "accountant");
    Ledger::DocumentRepository repo;
    auto doc = repo.create(org.id, "incoming", "uploaded", "inbox");

    HttpResponsePtr resp;
    ctrl.remove(
        authed(p, Delete), [&](const HttpResponsePtr& r) { resp = r; }, doc.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k204NoContent);
    EXPECT_TRUE(resp->body().empty());
    // Physically gone, not merely hidden.
    EXPECT_FALSE(repo.find_in_org(doc.id, org.id, /*from_primary=*/true).has_value());
    EXPECT_TRUE(repo.list_versions(org.id, doc.id).empty());
}

// A link to a DRAFT entry does not protect the document: the entry has not
// entered the ledger yet. document_entries cascades, the draft survives
// without its basis, and the audit log is the only trace of that — which is
// why the handler records it.
TEST_F(LedgerDocumentsApiTest, DeletesADocumentLinkedOnlyToADraftEntry) {
    auto org = seed_org("444240000046", "Delete Draft-Link Org LLP");
    auto p = member("del2@example.com", org.id, "accountant");
    Ledger::DocumentRepository repo;
    auto doc = repo.create(org.id, "invoice", "generated", "draft");
    const std::string draft_entry = seed_draft_entry(org.id, p.subject);
    ASSERT_TRUE(repo.link_entry(org.id, doc.id, draft_entry));

    HttpResponsePtr resp;
    ctrl.remove(
        authed(p, Delete), [&](const HttpResponsePtr& r) { resp = r; }, doc.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k204NoContent);
    EXPECT_FALSE(repo.find_in_org(doc.id, org.id, /*from_primary=*/true).has_value());

    // Черновая проводка осталась — связь каскадится, это принято.
    Ledger::JournalRepository journal;
    EXPECT_TRUE(journal.find_in_org(draft_entry, org.id, /*from_primary=*/true).has_value());
    EXPECT_TRUE(repo.list_for_entry(org.id, draft_entry).empty());
}

// The rule itself: posted entry -> deletion is a 409 forever, and voiding is
// the way out. `status` survives the void, because "was it final or sent" is
// exactly what an audit asks.
TEST_F(LedgerDocumentsApiTest, RefusesToDeleteADocumentOnAPostedEntry) {
    auto org = seed_org("444240000047", "Posted-Link Org LLP");
    auto p = member("del3@example.com", org.id, "accountant");
    Ledger::DocumentRepository repo;
    auto doc = repo.create(org.id, "invoice", "generated", "final");
    ASSERT_TRUE(repo.link_entry(org.id, doc.id, seed_posted_entry(org.id, p.subject)));

    HttpResponsePtr resp;
    ctrl.remove(
        authed(p, Delete), [&](const HttpResponsePtr& r) { resp = r; }, doc.id);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    EXPECT_EQ(json::parse(std::string(resp->body()))["error"].get<std::string>(), "document_has_posted_entries");
    EXPECT_TRUE(repo.find_in_org(doc.id, org.id, /*from_primary=*/true).has_value());

    // ...и аннулирование при этом доступно.
    HttpResponsePtr voided;
    ctrl.voidDocument(
        authed_json(p, json{{"reason", "ошибка"}}), [&](const HttpResponsePtr& r) { voided = r; }, doc.id);
    ASSERT_NE(voided, nullptr);
    ASSERT_EQ(voided->statusCode(), k200OK);
    auto body = json::parse(std::string(voided->body()));
    EXPECT_FALSE(body["data"]["voided_at"].is_null());
    EXPECT_EQ(body["data"]["void_reason"].get<std::string>(), "ошибка");
    EXPECT_EQ(body["data"]["voided_by_user_id"].get<std::string>(), p.subject);
    // status НЕ затёрт — аудит видит, чем документ был.
    EXPECT_EQ(body["data"]["status"].get<std::string>(), "final");
}

// No journal link at all, and still not destroyable: an HR order points at it.
// hr_orders.document_id is NO ACTION, so without the 23503 -> kReferenced
// translation this would be a 500.
TEST_F(LedgerDocumentsApiTest, HrDocumentReferencedByAnOrderIsFourZeroNineNotFiveHundred) {
    auto org = seed_org("444240000048", "HR Referenced Org LLP");
    auto p = member("del4@example.com", org.id, "accountant");
    const std::string hr_doc = seed_hr_document_referenced_by_an_order(org.id, "870101300123");

    Ledger::DocumentRepository repo;
    ASSERT_FALSE(repo.has_posted_entry_link(org.id, hr_doc));  // не проводка держит документ

    HttpResponsePtr resp;
    ctrl.remove(
        authed(p, Delete), [&](const HttpResponsePtr& r) { resp = r; }, hr_doc);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    EXPECT_EQ(json::parse(std::string(resp->body()))["error"].get<std::string>(), "document_referenced");
    EXPECT_TRUE(repo.find_in_org(hr_doc, org.id, /*from_primary=*/true).has_value());

    // И здесь тоже остаётся аннулирование.
    HttpResponsePtr voided;
    ctrl.voidDocument(
        authed_json(p, json{{"reason", "скан не тот"}}), [&](const HttpResponsePtr& r) { voided = r; }, hr_doc);
    ASSERT_NE(voided, nullptr);
    EXPECT_EQ(voided->statusCode(), k200OK);
}

// The reason is not decoration: it is the whole audit value of the three
// columns. Missing field -> 400 (shape), blank -> 422 (value), repeat -> 409
// (state) — the file's usual three-way split.
TEST_F(LedgerDocumentsApiTest, VoidRequiresAReasonAndIsIdempotentlyRejected) {
    auto org = seed_org("444240000049", "Void Reason Org LLP");
    auto p = member("del5@example.com", org.id, "accountant");
    Ledger::DocumentRepository repo;
    auto doc = repo.create(org.id, "invoice", "generated", "final");

    HttpResponsePtr missing;
    ctrl.voidDocument(
        authed_json(p, json::object()), [&](const HttpResponsePtr& r) { missing = r; }, doc.id);
    ASSERT_NE(missing, nullptr);
    EXPECT_EQ(missing->statusCode(), k400BadRequest);

    HttpResponsePtr blank;
    ctrl.voidDocument(
        authed_json(p, json{{"reason", "  "}}), [&](const HttpResponsePtr& r) { blank = r; }, doc.id);
    ASSERT_NE(blank, nullptr);
    EXPECT_EQ(blank->statusCode(), k422UnprocessableEntity);

    HttpResponsePtr first;
    ctrl.voidDocument(
        authed_json(p, json{{"reason", "дубль"}}), [&](const HttpResponsePtr& r) { first = r; }, doc.id);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->statusCode(), k200OK);

    HttpResponsePtr again;
    ctrl.voidDocument(
        authed_json(p, json{{"reason", "ещё раз"}}), [&](const HttpResponsePtr& r) { again = r; }, doc.id);
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->statusCode(), k409Conflict);
    EXPECT_EQ(json::parse(std::string(again->body()))["error"].get<std::string>(), "already_voided");
    // Первое решение и его причина уцелели — второе не перезаписало их.
    auto stored = repo.find_in_org(doc.id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->void_reason.value_or(""), "дубль");
}

// A voided document is not editable: a new version would be rendered and would
// hand the document a live file back.
TEST_F(LedgerDocumentsApiTest, AVoidedDocumentCannotBeEdited) {
    if (!templates_available())
        GTEST_SKIP() << "repo templates not reachable from this working directory";
    auto org = seed_org("444240000050", "Void Then Edit Org LLP");
    auto p = member("del6@example.com", org.id, "accountant");
    const std::string doc_id = seed_rendered_invoice(org.id, /*total_tiyn=*/1234567, "seed-bytes");

    HttpResponsePtr voided;
    ctrl.voidDocument(
        authed_json(p, json{{"reason", "выписан не тому"}}), [&](const HttpResponsePtr& r) { voided = r; }, doc_id);
    ASSERT_NE(voided, nullptr);
    ASSERT_EQ(voided->statusCode(), k200OK);

    HttpResponsePtr edit;
    ctrl.createVersion(
        authed_json(p, json{{"input", make_invoice_input(1234567)}}),
        [&](const HttpResponsePtr& r) { edit = r; },
        doc_id);
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(edit->statusCode(), k409Conflict);
    EXPECT_EQ(json::parse(std::string(edit->body()))["error"].get<std::string>(), "document_voided");
    Ledger::DocumentRepository repo;
    EXPECT_EQ(repo.list_versions(org.id, doc_id).size(), 1U);
}

// Both routes are writes, so a viewer is refused on both — and the document is
// untouched by either attempt.
TEST_F(LedgerDocumentsApiTest, ViewerCanNeitherDeleteNorVoid) {
    auto org = seed_org("444240000051", "Void Viewer Org LLP");
    auto v = member("viewer12@example.com", org.id, "viewer");
    Ledger::DocumentRepository repo;
    auto doc = repo.create(org.id, "invoice", "generated", "final");

    HttpResponsePtr del;
    ctrl.remove(
        authed(v, Delete), [&](const HttpResponsePtr& r) { del = r; }, doc.id);
    ASSERT_NE(del, nullptr);
    EXPECT_EQ(del->statusCode(), k403Forbidden);

    HttpResponsePtr voided;
    ctrl.voidDocument(
        authed_json(v, json{{"reason", "нет"}}), [&](const HttpResponsePtr& r) { voided = r; }, doc.id);
    ASSERT_NE(voided, nullptr);
    EXPECT_EQ(voided->statusCode(), k403Forbidden);

    auto still = repo.find_in_org(doc.id, org.id, /*from_primary=*/true);
    ASSERT_TRUE(still.has_value());
    EXPECT_FALSE(still->voided_at.has_value());
}

// The кадровик's grant is per-RESOURCE, and one table holds two resources: hr
// documents are hr_docs (rw), everything else is `documents` (no grant at
// all). Both routes go through the same task-7 helper, so both split the same
// way — a fourth copy of the doc_type condition would have drifted.
TEST_F(LedgerDocumentsApiTest, HrRoleMayVoidHrDocumentsButNotPrimaryOnes) {
    auto org = seed_org("444240000052", "HR Role Void Org LLP");
    auto hr_user = member("hr-void@example.com", org.id, "hr");
    Ledger::DocumentRepository repo;
    auto hr_doc = repo.create(org.id, "hr", "uploaded", "inbox");
    auto invoice = repo.create(org.id, "invoice", "generated", "final");

    HttpResponsePtr hr_resp;
    ctrl.voidDocument(
        authed_json(hr_user, json{{"reason", "приказ отменён"}}),
        [&](const HttpResponsePtr& r) { hr_resp = r; },
        hr_doc.id);
    ASSERT_NE(hr_resp, nullptr);
    EXPECT_EQ(hr_resp->statusCode(), k200OK);

    HttpResponsePtr invoice_resp;
    ctrl.voidDocument(
        authed_json(hr_user, json{{"reason", "не моё"}}),
        [&](const HttpResponsePtr& r) { invoice_resp = r; },
        invoice.id);
    ASSERT_NE(invoice_resp, nullptr);
    EXPECT_EQ(invoice_resp->statusCode(), k403Forbidden);

    HttpResponsePtr invoice_del;
    ctrl.remove(
        authed(hr_user, Delete), [&](const HttpResponsePtr& r) { invoice_del = r; }, invoice.id);
    ASSERT_NE(invoice_del, nullptr);
    EXPECT_EQ(invoice_del->statusCode(), k403Forbidden);
    EXPECT_TRUE(repo.find_in_org(invoice.id, org.id, /*from_primary=*/true).has_value());

    // А свой кадровый документ он и удалить может — если тот ничем не занят.
    auto spare_hr_doc = repo.create(org.id, "hr", "uploaded", "inbox");
    HttpResponsePtr hr_del;
    ctrl.remove(
        authed(hr_user, Delete), [&](const HttpResponsePtr& r) { hr_del = r; }, spare_hr_doc.id);
    ASSERT_NE(hr_del, nullptr);
    EXPECT_EQ(hr_del->statusCode(), k204NoContent);
}

}  // namespace
