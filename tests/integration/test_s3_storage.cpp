/**
 * @file test_s3_storage.cpp
 * @brief Integration coverage for Storage::S3Storage against a real
 *        S3-compatible endpoint (MinIO in the test compose profile — see
 *        docker/docker-compose.yml's test-minio/test-minio-init services).
 *
 * Suite name S3StorageTest → integration bucket (needs MinIO). The
 * presigned-URL cases deliberately shell out to the `curl` CLI rather than
 * reusing S3Storage's own libcurl client — the point is proving the
 * signature is valid to a fully independent HTTP client, not just to our
 * own code that produced it.
 *
 * OrgKeyLayoutTest below needs no S3 endpoint at all (pure string
 * formatting) but lives in this bucket rather than tests/unit/ per the
 * "don't pull Storage into unit" convention — it exercises the file-key
 * layout that every S3Storage caller in this suite also uses.
 */

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "files/FileKeys.hpp"
#include "jobs/Job.hpp"
#include "storage/Storage.hpp"

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

// Defaults match docker/docker-compose.yml's test-minio service so the
// suite runs unmodified under `make test` (docker) as well as against a
// hand-started MinIO on localhost for local iteration.
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

/// Run a shell command, returning everything it wrote to stdout. Used to
/// drive the `curl` CLI as a client fully independent of S3Storage's own
/// libcurl usage (see file header).
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

/// Same liveness probe as the test-minio healthcheck in docker-compose.yml —
/// unauthenticated, so this needs no signing to answer the "is it even up"
/// question before a signed request is attempted.
bool is_minio_available() {
    const std::string cmd =
        "curl -s -o /dev/null -w '%{http_code}' --max-time 2 '" + s3_endpoint() + "/minio/health/live' 2>/dev/null";
    try {
        return run_capture(cmd) == "200";
    } catch (...) {
        return false;
    }
}

/// Unique per-call key under a per-suite-run namespace — avoids collisions
/// between test cases and between repeated local runs against the same
/// (non-ephemeral, if a dev pointed S3_TEST_ENDPOINT elsewhere) bucket.
std::string test_key(const std::string& name) {
    return "s3test/" + name + "/" + Jobs::generate_uuid();
}

}  // namespace

class S3StorageTest : public ::testing::Test {
protected:
    std::unique_ptr<Storage::S3Storage> storage_;

    void SetUp() override {
        if (!is_minio_available()) {
            // Same "don't let a mis-wired CI job silently go all-green"
            // guard as CoreBackedTest (tests/test_helpers.hpp) for
            // Postgres/Redis — the test compose profile always brings up
            // test-minio + test-minio-init, so a CI run should never
            // legitimately hit this.
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
        storage_ = std::make_unique<Storage::S3Storage>(cfg);
    }
};

TEST_F(S3StorageTest, PutGetRoundTrip) {
    const std::string key = test_key("roundtrip");
    const std::string body = "hello minio " + Jobs::generate_uuid();

    storage_->put(key, body, "text/plain");
    const auto got = storage_->get(key);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, body);

    storage_->remove(key);
}

TEST_F(S3StorageTest, ExistsRemove) {
    const std::string key = test_key("existsremove");

    EXPECT_FALSE(storage_->exists(key));
    storage_->put(key, "payload", "text/plain");
    EXPECT_TRUE(storage_->exists(key));

    EXPECT_TRUE(storage_->remove(key));
    EXPECT_FALSE(storage_->exists(key));
}

TEST_F(S3StorageTest, ListPrefix) {
    const std::string prefix = "s3test/listprefix/" + Jobs::generate_uuid() + "/";
    storage_->put(prefix + "a.txt", "a", "text/plain");
    storage_->put(prefix + "b.txt", "b", "text/plain");

    const auto listed = storage_->list(prefix);
    ASSERT_EQ(listed.size(), 2u);

    std::set<std::string> keys;
    for (const auto& obj : listed)
        keys.insert(obj.key);
    EXPECT_EQ(keys.count(prefix + "a.txt"), 1u);
    EXPECT_EQ(keys.count(prefix + "b.txt"), 1u);

    storage_->remove(prefix + "a.txt");
    storage_->remove(prefix + "b.txt");
}

TEST_F(S3StorageTest, PresignedGetFetchesViaCurl) {
    const std::string key = test_key("presignget");
    const std::string body = "presigned-get-body-" + Jobs::generate_uuid();
    storage_->put(key, body, "text/plain");

    const std::string url = storage_->presign(key, "GET", 60);
    const std::string fetched = run_capture("curl -s '" + url + "'");
    EXPECT_EQ(fetched, body);

    storage_->remove(key);
}

TEST_F(S3StorageTest, PresignedGetWithPublicEndpointFetchesViaCurl) {
    const std::string key = test_key("presign-public-endpoint");
    const std::string body = "presigned-get-public-endpoint-body-" + Jobs::generate_uuid();
    storage_->put(key, body, "text/plain");

    // public_endpoint set explicitly (equal to the real MinIO endpoint here,
    // since that's the only S3-compatible host reachable in this suite) —
    // proves presign() signs the canonical request against public_host_ and
    // still produces a signature MinIO accepts, not just that leaving
    // public_endpoint unset happens to fall back correctly (that path is
    // PresignedGetFetchesViaCurl above).
    Storage::S3Storage::Config cfg;
    cfg.endpoint = s3_endpoint();
    cfg.region = s3_region();
    cfg.bucket = s3_bucket();
    cfg.access_key = s3_access_key();
    cfg.secret_key = s3_secret_key();
    cfg.public_endpoint = s3_endpoint();
    Storage::S3Storage public_storage(cfg);

    const std::string url = public_storage.presign(key, "GET", 60);
    ASSERT_EQ(url.rfind(s3_endpoint(), 0), 0u) << "url=" << url;
    const std::string fetched = run_capture("curl -s '" + url + "'");
    EXPECT_EQ(fetched, body);

    storage_->remove(key);
}

TEST_F(S3StorageTest, PresignedUrlUsesDistinctPublicEndpointHost) {
    const std::string key = test_key("presign-distinct-public-host");
    // Not a reachable host in this suite — this case only checks the URL
    // string, not that curl can fetch through it (there's no live ingress
    // in the test environment for a public MinIO domain). The
    // signature-is-actually-valid case is covered above against the one
    // S3-compatible endpoint the suite does have.
    const std::string public_endpoint = "https://s3.cybercapybara.kz";

    Storage::S3Storage::Config cfg;
    cfg.endpoint = s3_endpoint();
    cfg.region = s3_region();
    cfg.bucket = s3_bucket();
    cfg.access_key = s3_access_key();
    cfg.secret_key = s3_secret_key();
    cfg.public_endpoint = public_endpoint;
    Storage::S3Storage public_storage(cfg);

    const std::string url = public_storage.presign(key, "GET", 60);
    EXPECT_EQ(url.rfind(public_endpoint, 0), 0u) << "url=" << url;
    EXPECT_EQ(url.find(s3_endpoint()), std::string::npos) << "url leaked the internal endpoint: " << url;
    EXPECT_NE(url.find("X-Amz-Signature="), std::string::npos);
}

TEST_F(S3StorageTest, PresignedPutUploads) {
    const std::string key = test_key("presignput");
    const std::string body = "presigned-put-body-" + Jobs::generate_uuid();
    const std::string url = storage_->presign(key, "PUT", 60);

    // --data-binary @file (rather than inlining body on the command line)
    // sidesteps shell-quoting the payload entirely.
    const auto tmp_path = std::filesystem::temp_directory_path() / ("s3test-put-" + Jobs::generate_uuid() + ".bin");
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f << body;
    }
    const std::string http_code = run_capture("curl -s -o /dev/null -w '%{http_code}' -X PUT --data-binary @'" +
                                              tmp_path.string() + "' '" + url + "'");
    std::filesystem::remove(tmp_path);
    EXPECT_EQ(http_code, "200");

    const auto got = storage_->get(key);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, body);

    storage_->remove(key);
}

// --- OrgKeyLayout: pure string formatting, no S3 endpoint needed ---

TEST(OrgKeyLayoutTest, OrgKeyLayout) {
    static const std::regex kUuidRe("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");

    // Format: org/{org_id}/{kind}/{uuid}-{sanitized-filename}, and disallowed
    // characters (spaces, parens) become '_' while '.', '_', '-' pass through.
    const std::string key = Files::org_key("org-123", "inbox", "My File (draft).pdf");
    const std::string prefix = "org/org-123/inbox/";
    ASSERT_EQ(key.rfind(prefix, 0), 0u);

    const std::string tail = key.substr(prefix.size());
    ASSERT_GT(tail.size(), 37u);  // 36-char uuid + '-' + at least one char
    const std::string uuid_part = tail.substr(0, 36);
    EXPECT_TRUE(std::regex_match(uuid_part, kUuidRe)) << "uuid_part=" << uuid_part;
    EXPECT_EQ(tail[36], '-');
    EXPECT_EQ(tail.substr(37), "My_File__draft_.pdf");

    // Two calls for the same (org, kind, filename) never collide — the uuid
    // makes every key unique even when callers reuse a filename.
    const std::string key2 = Files::org_key("org-123", "inbox", "My File (draft).pdf");
    EXPECT_NE(key, key2);

    // Sanitization: only [A-Za-z0-9._-] survive verbatim.
    EXPECT_EQ(Files::sanitize_filename("abc-DEF_123.txt"), "abc-DEF_123.txt");
    EXPECT_EQ(Files::sanitize_filename("weird name!@#.csv"), "weird_name___.csv");

    // Empty filename -> the literal "file", not an empty tail / dangling '-'.
    EXPECT_EQ(Files::sanitize_filename(""), "file");
    const std::string key3 = Files::org_key("org-1", "generated", "");
    EXPECT_EQ(key3.substr(key3.size() - 5), "-file");
}
