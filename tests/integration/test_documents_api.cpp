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
#include <optional>
#include <stdexcept>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/LedgerDocumentsController.hpp"
#include "domain/Role.hpp"
#include "domain/User.hpp"
#include "files/FileKeys.hpp"
#include "jobs/Job.hpp"
#include "ledger/DocumentRepository.hpp"
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

    std::string config_file_name() const override { return "documents_api_test_config.json"; }

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
        TestHelpers::CoreBackedTest::TearDown();
    }

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
    auto created = repo.create(org.id, "invoice", "generated", "draft");  // never had set_file() called

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
    ASSERT_TRUE(repo.set_file(org.id,
                              created.id,
                              Files::org_key(org.id, "generated", "report.pdf"),
                              std::string(64, 'b'),
                              "application/pdf",
                              42));

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
    ASSERT_TRUE(repo.set_file(org.id, created.id, confirmed_key, std::string(64, 'a'), "application/pdf", 123));
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

}  // namespace
