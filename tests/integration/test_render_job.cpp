/**
 * @file test_render_job.cpp
 * @brief Integration tests for the "docgen.render" job (Docgen::process_job,
 *        src/docgen/RenderJob.hpp) against real Postgres + Redis + a
 *        LocalStorage backend.
 *
 * The real XeLaTeX binary is NEVER invoked here: DOCGEN_LATEX_CMD is pointed
 * at a stub shell script (written to a temp file and chmod +x'd in SetUp)
 * that either copies a hardcoded minimal PDF into `main.pdf` (success case)
 * or exits 1 without producing one (failure case). The full pipeline —
 * schema validation, inja rendering + LaTeX escaping, the two-pass "latex"
 * invocation, sha256, Storage::put, DocumentRepository::set_file/
 * set_status — runs for real; only the compiler itself is faked. Real
 * XeLaTeX only ever runs via scripts/render-templates.sh (the
 * `template-render` CI job, on the worker image — see docker/Dockerfile).
 */

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "docgen/RenderJob.hpp"
#include "ledger/DocumentRepository.hpp"
#include "storage/Storage.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// A syntactically minimal PDF (the textbook "smallest possible PDF" — no
// xref table, but every reader that matters accepts it). Nothing downstream
// of RenderJob parses PDF structure; this only has to be non-empty,
// recognizable bytes that survive a storage round-trip intact.
constexpr const char* kFakePdfBytes =
    "%PDF-1.1\n"
    "1 0 obj  << /Type /Catalog /Pages 2 0 R >> endobj\n"
    "2 0 obj  << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj  << /Type /Page /Parent 2 0 R /Resources << >> /MediaBox [0 0 300 144] >> endobj\n"
    "trailer  << /Root 1 0 R >>\n"
    "%%EOF\n";

fs::path storage_root_path() {
    return fs::temp_directory_path() / "docgen_render_job_storage";
}

fs::path scripts_dir_path() {
    return fs::temp_directory_path() / "docgen_render_job_scripts";
}

/// Make @p path executable for owner/group/other — the stub scripts
/// DOCGEN_LATEX_CMD points at need +x to run via popen()'s `sh -c`.
void make_executable(const fs::path& path) {
    std::error_code ec;
    fs::permissions(path,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                        fs::perms::others_exec,
                    fs::perm_options::replace,
                    ec);
}

class RenderJobTest : public TestHelpers::CoreBackedTest {
protected:
    std::unique_ptr<TestHelpers::ScopedEnv> latex_cmd_env_;

    std::string config_file_name() const override { return "render_job_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["jobs"]["enabled"] = true;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
        cfg["storage"]["backend"] = "local";
        cfg["storage"]["local"]["root"] = storage_root_path().string();
    }

    void SetUp() override {
        std::error_code ec;
        fs::remove_all(storage_root_path(), ec);
        fs::remove_all(scripts_dir_path(), ec);
        fs::create_directories(scripts_dir_path());

        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;

        if (!fs::exists("templates/latex/invoice/v1/template.tex"))
            GTEST_SKIP() << "repo templates not reachable from this working directory";

        // Same idiom as test_documents.cpp's SetUp: clear organizations
        // (documents cascades off it) so fixed test data doesn't collide
        // with a previous run against a persistent local Postgres.
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE journal_lines, journal_entries CASCADE");
            txn.exec("DELETE FROM organizations");
            return 0;
        });
    }

    void TearDown() override {
        latex_cmd_env_.reset();
        TestHelpers::CoreBackedTest::TearDown();
        std::error_code ec;
        fs::remove_all(storage_root_path(), ec);
        fs::remove_all(scripts_dir_path(), ec);
    }

    std::string make_org(const std::string& bin) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "RenderJob Test Org " + bin, "snr_simplified", false).id;
    }

    /// A valid input for templates/latex/invoice/v1's schema.json.
    static json valid_invoice_input() {
        return json{
            {"number", "1"},
            {"date", "14.08.2026"},
            {"seller", {{"name", "Cyber Capybara ТОО"}, {"identifier", "104332181962"}}},
            {"buyer", {{"name", "Покупатель ТОО"}, {"identifier", "001338908381"}}},
            {"items",
             json::array({json{{"name", "Консультации"},
                               {"qty", "1"},
                               {"unit", "шт"},
                               {"price", "1000.00"},
                               {"amount", "1000.00"}}})},
            {"total", "1000.00"},
            {"total_words", "Одна тысяча тенге 00 тиын"},
        };
    }

    /// Points DOCGEN_LATEX_CMD at a stub script that copies the canned PDF
    /// into `main.pdf` (cwd, per compile_pdf's `cd <tex_dir> && ...`) and
    /// exits 0.
    void use_succeeding_latex_stub() {
        const fs::path pdf_path = scripts_dir_path() / "canned.pdf";
        std::ofstream(pdf_path, std::ios::binary) << kFakePdfBytes;

        const fs::path script_path = scripts_dir_path() / "fake-latex-ok.sh";
        {
            std::ofstream script(script_path);
            script << "#!/bin/sh\n"
                   << "cp \"" << pdf_path.string() << "\" main.pdf\n"
                   << "exit 0\n";
        }
        make_executable(script_path);
        latex_cmd_env_ = std::make_unique<TestHelpers::ScopedEnv>("DOCGEN_LATEX_CMD", script_path.string());
    }

    /// Points DOCGEN_LATEX_CMD at a stub script that fails (exit 1) without
    /// producing a PDF — simulates a XeLaTeX compile error.
    void use_failing_latex_stub() {
        const fs::path script_path = scripts_dir_path() / "fake-latex-fail.sh";
        {
            std::ofstream script(script_path);
            script << "#!/bin/sh\n"
                   << "echo 'simulated latex failure' >&2\n"
                   << "exit 1\n";
        }
        make_executable(script_path);
        latex_cmd_env_ = std::make_unique<TestHelpers::ScopedEnv>("DOCGEN_LATEX_CMD", script_path.string());
    }
};

TEST_F(RenderJobTest, RenderJobHappyPath) {
    use_succeeding_latex_stub();
    auto org_id = make_org("111280000001");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id, "invoice", "generated", "draft", std::nullopt, "invoice", "v1");
    ASSERT_EQ(doc.status, "draft");

    json payload = {{"org_id", org_id}, {"document_id", doc.id}, {"slug", "invoice"}, {"input", valid_invoice_input()}};
    auto result = Docgen::process_job(payload);

    EXPECT_EQ(result["document_id"], doc.id);
    EXPECT_EQ(result["slug"], "invoice");
    ASSERT_TRUE(result.contains("key"));
    const std::string key = result["key"].get<std::string>();

    auto stored = documents.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->status, "final");
    ASSERT_TRUE(stored->s3_key.has_value());
    EXPECT_EQ(*stored->s3_key, key);
    ASSERT_TRUE(stored->checksum_sha256.has_value());
    EXPECT_EQ(*stored->checksum_sha256, result["checksum_sha256"].get<std::string>());
    ASSERT_TRUE(stored->mime.has_value());
    EXPECT_EQ(*stored->mime, "application/pdf");

    ASSERT_TRUE(Storage::get().exists(key));
    auto stored_bytes = Storage::get().get(key);
    ASSERT_TRUE(stored_bytes.has_value());
    EXPECT_EQ(*stored_bytes, kFakePdfBytes);
}

TEST_F(RenderJobTest, RenderJobInvalidInputFails) {
    use_succeeding_latex_stub();  // never reached — schema validation fails first
    auto org_id = make_org("111280000002");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id, "invoice", "generated", "draft", std::nullopt, "invoice", "v1");

    // Missing every required field.
    json payload = {{"org_id", org_id}, {"document_id", doc.id}, {"slug", "invoice"}, {"input", json::object()}};
    EXPECT_THROW(Docgen::process_job(payload), std::runtime_error);

    auto stored = documents.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->status, "draft");
    EXPECT_FALSE(stored->s3_key.has_value());
}

TEST_F(RenderJobTest, RenderJobLatexFailureFails) {
    use_failing_latex_stub();
    auto org_id = make_org("111280000003");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id, "invoice", "generated", "draft", std::nullopt, "invoice", "v1");

    json payload = {{"org_id", org_id}, {"document_id", doc.id}, {"slug", "invoice"}, {"input", valid_invoice_input()}};
    EXPECT_THROW(Docgen::process_job(payload), std::runtime_error);

    auto stored = documents.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->status, "draft");
    EXPECT_FALSE(stored->s3_key.has_value());
}

}  // namespace
