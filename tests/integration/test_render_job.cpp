/**
 * @file test_render_job.cpp
 * @brief Integration tests for the "docgen.render" job (Docgen::process_job,
 *        src/docgen/RenderJob.hpp) against real Postgres + Redis + a
 *        LocalStorage backend.
 *
 * THE REAL ENGINE IS NEVER INVOKED here: DOCGEN_TYPST_CMD is pointed at a
 * stub shell script (written to a temp file and chmod +x'd in SetUp) that
 * answers `--version`, and then either copies a hardcoded minimal PDF into
 * `main.pdf` (success case) or exits 1 without producing one (failure case).
 * It ignores its arguments, so it stands in for `typst compile main.typ`.
 *
 * The full pipeline — schema validation, the Typst staging (template copy +
 * input.json), the engine invocation, sha256, Storage::put,
 * DocumentRepository::version_render_state/set_version_file/
 * set_current_version/set_status_if — runs for real; only the compiler itself
 * is faked. A real engine only ever runs via scripts/render-templates.sh (the
 * `template-render` CI job, on the worker image — see docker/Dockerfile).
 */

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "docgen/DocumentTemplateRepository.hpp"
#include "docgen/RenderJob.hpp"
#include "docgen/TemplateRegistry.hpp"
#include "jobs/Job.hpp"
#include "ledger/DocumentRepository.hpp"
#include "repo_templates.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "storage/Storage.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"
#include "utils/Crypto.hpp"

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

/// Make @p path executable for owner/group/other — the stub script
/// DOCGEN_TYPST_CMD points at needs +x to run via popen()'s `sh -c`.
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
    std::unique_ptr<TestHelpers::ScopedEnv> typst_cmd_env_;

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

        // Engine-agnostic by construction: the directory and its schema.json,
        // never `invoice/v1/template.tex`. That file is what this guard used
        // to probe, and the invoice's Typst conversion (3c77b1e) deleted it —
        // every test in this file then skipped, silently, for the whole of
        // the migration. See tests/repo_templates.hpp.
        REQUIRE_REPO_TEMPLATE("invoice");

        // Centralized org-data wipe (TestHelpers::wipe_org_data(), in
        // test_helpers.hpp) — TRUNCATEs the journal/document tables (bypasses
        // journal_entries_immutability()) before a plain DELETE on
        // organizations, so fixed test data doesn't collide with a previous
        // run against a persistent local Postgres.
        TestHelpers::wipe_org_data();
    }

    void TearDown() override {
        typst_cmd_env_.reset();
        TestHelpers::CoreBackedTest::TearDown();
        std::error_code ec;
        fs::remove_all(storage_root_path(), ec);
        fs::remove_all(scripts_dir_path(), ec);
    }

    std::string make_org(const std::string& bin) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "RenderJob Test Org " + bin, "snr_simplified", false).id;
    }

    /// A real users row — documents.voided_by_user_id is a FK onto it, so a
    /// made-up uuid would trip 23503 instead of testing anything. Same idiom
    /// as tests/integration/test_documents.cpp's seed_user(). The email is
    /// made unique per call because this fixture does not wipe `users`
    /// (wipe_org_data() deliberately leaves them alone).
    std::string seed_user(const std::string& email) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name("User");
        if (!role) {
            ADD_FAILURE() << "role 'User' missing — seed migration?";
            throw std::runtime_error("seed role missing: User");
        }
        auto created = users.create(Jobs::generate_uuid() + "." + email,
                                    std::string("$argon2id$placeholder"),
                                    std::nullopt,
                                    std::nullopt,
                                    role->id,
                                    /*confirmed=*/true);
        return created.id;
    }

    /// A valid input for templates/docs/invoice/v1's schema.json.
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
            // total_tiyn is REQUIRED by templates/docs/invoice/v1/schema.json
            // since P3, and render_and_compile() runs
            // TemplateRegistry::validate() strictly BEFORE normalize_input()
            // — so a missing integer is a hard failure here, never a
            // silently defaulted zero on a printed document.
            {"total_tiyn", 100000},
            {"total", "1000.00"},
            {"total_words", "Одна тысяча тенге 00 тиын"},
        };
    }

    /// Overwrite the bytes the succeeding stub copies into `main.pdf`. Lets
    /// one test render TWICE and get DIFFERENT files, which is what makes
    /// "version 1's object still holds version 1's content" a real assertion
    /// instead of a tautology. Deliberately rewrites the canned PDF rather
    /// than pointing the engine variable at a second script: re-seating it
    /// would construct the new ScopedEnv and only then destroy the old one,
    /// and that destructor would restore the variable right back over the new
    /// value.
    void set_canned_pdf(const std::string& bytes) {
        std::ofstream(scripts_dir_path() / "canned.pdf", std::ios::binary) << bytes;
    }

    /// Point the engine command at @p script_path. Named for the ENGINE in
    /// the abstract, not for Typst: which binary a render reaches for is
    /// decided by the template directory on disk (Docgen::TemplateRegistry::
    /// load), and this fixture deliberately never spells out which one the
    /// invoice selects today.
    void use_engine_stub(const fs::path& script_path) {
        make_executable(script_path);
        typst_cmd_env_ = std::make_unique<TestHelpers::ScopedEnv>("DOCGEN_TYPST_CMD", script_path.string());
    }

    /// Points the engine command at a stub script that copies the canned
    /// PDF into `main.pdf` (cwd, per compile_typst's `cd <dir> && ...`) and
    /// exits 0. The stub ignores its arguments.
    void use_succeeding_engine_stub() {
        const fs::path pdf_path = scripts_dir_path() / "canned.pdf";
        set_canned_pdf(kFakePdfBytes);

        const fs::path script_path = scripts_dir_path() / "fake-engine-ok.sh";
        {
            std::ofstream script(script_path);
            // `--version` is answered rather than compiled: Docgen::
            // engine_version asks the Typst command for its version (once per
            // process, cached) and it does so from the test's OWN working
            // directory — a stub that blindly copied a PDF would drop a stray
            // `main.pdf` into the repo root on that call.
            script << "#!/bin/sh\n"
                   << "case \"$1\" in --version) echo 'typst 0.15.1 (stub)'; exit 0;; esac\n"
                   << "cp \"" << pdf_path.string() << "\" main.pdf\n"
                   << "exit 0\n";
        }
        use_engine_stub(script_path);
    }

    /// Points the engine command at a stub script that fails (exit 1)
    /// without producing a PDF — simulates a compile error from whichever
    /// engine the template selects.
    void use_failing_engine_stub() {
        const fs::path script_path = scripts_dir_path() / "fake-engine-fail.sh";
        {
            std::ofstream script(script_path);
            script << "#!/bin/sh\n"
                   << "echo 'simulated engine failure' >&2\n"
                   << "exit 1\n";
        }
        use_engine_stub(script_path);
    }
};

// Payload WITHOUT version_id on purpose: jobs enqueued by an older build and
// still sitting in Redis across a deploy must keep working. Such a job was
// enqueued by the request that CREATED the document, so what it means is
// version 1 — and the fallback is bounded to exactly that, untouched (see
// IsANoOpForALegacyPayloadWhoseDocumentWasEditedAcrossTheDeploy for the bound
// itself). Every producer sets version_id now (the tests below use it); this
// one pins the fallback so a future cleanup cannot quietly turn those jobs
// into skips.
TEST_F(RenderJobTest, RenderJobHappyPath) {
    use_succeeding_engine_stub();
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
    // The render wrote version 1 and published it — every file field above
    // is read through that pointer (migrations/018_document_versions.sql).
    ASSERT_TRUE(stored->current_version_id.has_value());
    EXPECT_EQ(stored->latest_version_no, 1);
    auto versions = documents.list_versions(org_id, doc.id);
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_EQ(*stored->current_version_id, versions[0].id);
    EXPECT_EQ(versions[0].s3_key.value_or(""), key);

    ASSERT_TRUE(Storage::get().exists(key));
    auto stored_bytes = Storage::get().get(key);
    ASSERT_TRUE(stored_bytes.has_value());
    EXPECT_EQ(*stored_bytes, kFakePdfBytes);
}

TEST_F(RenderJobTest, ACustomTemplateFromTheDatabaseIsRenderedInsteadOfTheBuiltInOne) {
    // Это тот момент, ради которого существует хранилище шаблонов: пока
    // разрешение из базы не встроено в рендер, шаблон можно завести, но он
    // никуда не попадёт.
    use_succeeding_engine_stub();
    auto org_id = make_org("111280000020");

    Docgen::DocumentTemplateRepository templates;
    Docgen::DocumentTemplate custom;
    custom.org_id = org_id;
    custom.slug = "invoice";
    custom.version = 1;
    custom.mode = Docgen::TemplateMode::kSource;
    custom.source = "#let d = json(\"input.json\")\n= Мой счёт #d.number\n";
    // Схема нарочно СЛАБЕЕ встроенной: требуется только `number`. Если бы
    // рендер продолжал валидировать по схеме с диска, этот ввод не прошёл бы.
    custom.schema = json{{"type", "object"},
                         {"required", json::array({"number"})},
                         {"properties", json{{"number", json{{"type", "string"}}}}}};
    custom.status = Docgen::TemplateStatus::kPublished;
    templates.create(custom, std::nullopt);

    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id, "invoice", "generated", "draft", std::nullopt, "invoice", "v1");

    json payload = {
        {"org_id", org_id}, {"document_id", doc.id}, {"slug", "invoice"}, {"input", json{{"number", "77"}}}};
    auto result = Docgen::process_job(payload);
    ASSERT_TRUE(result.contains("key"));

    // Собрался именно пользовательский шаблон: движок-заглушка получила
    // main.typ с его текстом. Проверяем по тому, что ввод, недостаточный для
    // встроенной схемы (нет date/seller/buyer/items/total_tiyn), прошёл.
    auto stored = documents.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->status, "final");
}

TEST_F(RenderJobTest, ADraftCustomTemplateIsIgnoredAndTheBuiltInOneIsUsed) {
    // Незаконченный шаблон не должен попадать в документы. Проверяем не
    // «упало», а «взялся встроенный»: ввод, годный для встроенной схемы,
    // рендерится, хотя у организации есть черновик с тем же слагом.
    use_succeeding_engine_stub();
    auto org_id = make_org("111280000021");

    Docgen::DocumentTemplateRepository templates;
    Docgen::DocumentTemplate draft_tpl;
    draft_tpl.org_id = org_id;
    draft_tpl.slug = "invoice";
    draft_tpl.version = 1;
    draft_tpl.mode = Docgen::TemplateMode::kSource;
    draft_tpl.source = "#let d = json(\"input.json\")\n= Черновик\n";
    draft_tpl.schema = json{{"type", "object"}, {"required", json::array({"number"})}};
    draft_tpl.status = Docgen::TemplateStatus::kDraft;
    templates.create(draft_tpl, std::nullopt);

    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id, "invoice", "generated", "draft", std::nullopt, "invoice", "v1");
    json payload = {{"org_id", org_id}, {"document_id", doc.id}, {"slug", "invoice"}, {"input", valid_invoice_input()}};
    ASSERT_NO_THROW(Docgen::process_job(payload));

    auto stored = documents.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->status, "final");
}

TEST_F(RenderJobTest, AnotherOrgsCustomTemplateIsNeverPickedUp) {
    use_succeeding_engine_stub();
    auto mine = make_org("111280000022");
    auto theirs = make_org("111280000023");

    Docgen::DocumentTemplateRepository templates;
    Docgen::DocumentTemplate theirs_tpl;
    theirs_tpl.org_id = theirs;
    theirs_tpl.slug = "invoice";
    theirs_tpl.version = 1;
    theirs_tpl.mode = Docgen::TemplateMode::kSource;
    theirs_tpl.source = "#let d = json(\"input.json\")\n= Чужой шаблон\n";
    // Схема требует поле, которого во вводе нет. Если бы чужой шаблон
    // подхватился, рендер упал бы на валидации — и это ровно тот признак,
    // по которому утечка между арендаторами была бы видна.
    theirs_tpl.schema = json{{"type", "object"}, {"required", json::array({"their_only_field"})}};
    theirs_tpl.status = Docgen::TemplateStatus::kPublished;
    templates.create(theirs_tpl, std::nullopt);

    Ledger::DocumentRepository documents;
    auto doc = documents.create(mine, "invoice", "generated", "draft", std::nullopt, "invoice", "v1");
    json payload = {{"org_id", mine}, {"document_id", doc.id}, {"slug", "invoice"}, {"input", valid_invoice_input()}};
    ASSERT_NO_THROW(Docgen::process_job(payload));

    auto stored = documents.find_in_org(doc.id, mine, /*from_primary=*/true);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->status, "final");
}

TEST_F(RenderJobTest, RenderJobInvalidInputFails) {
    use_succeeding_engine_stub();  // never reached — schema validation fails first
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

// Named for the engine in the abstract: the stub fails whichever binary the
// invoice's directory currently selects.
TEST_F(RenderJobTest, RenderJobEngineFailureFails) {
    use_failing_engine_stub();
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

// Security: `slug` reaches Docgen::render_and_compile straight from the job
// payload — via the generic admin job-submission endpoint, that payload is
// attacker-reachable (admin-gated, but still a live path-traversal
// primitive if unchecked). TemplateRegistry::latest()'s allowlist must
// reject it before any filesystem access, and the job must throw (document
// stays draft) exactly like any other "no such template" failure — not
// silently succeed, and not read/write outside templates/docs.
TEST_F(RenderJobTest, RenderJobRejectsTraversalSlug) {
    use_succeeding_engine_stub();  // never reached — slug is rejected first
    auto org_id = make_org("111280000004");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id, "invoice", "generated", "draft");

    json payload = {{"org_id", org_id},
                    {"document_id", doc.id},
                    {"slug", "../../../../etc/passwd"},
                    {"input", valid_invoice_input()}};
    EXPECT_THROW(Docgen::process_job(payload), std::runtime_error);

    auto stored = documents.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->status, "draft");
    EXPECT_FALSE(stored->s3_key.has_value());
}

// ── P3 task 10: the job addresses a VERSION ──────────────────────────────────

TEST_F(RenderJobTest, WritesTheFileIntoTheAddressedVersionAndPublishesIt) {
    use_succeeding_engine_stub();
    auto org_id = make_org("111280000005");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id,
                                "invoice",
                                "generated",
                                "draft",
                                std::nullopt,
                                "invoice",
                                "v1",
                                std::optional<nlohmann::json>{valid_invoice_input()});
    auto v1 = documents.latest_version(org_id, doc.id);
    ASSERT_TRUE(v1);

    auto result = Docgen::process_job(json{{"org_id", org_id},
                                           {"document_id", doc.id},
                                           {"version_id", v1->id},
                                           {"slug", "invoice"},
                                           {"input", valid_invoice_input()}});
    EXPECT_FALSE(result.contains("skipped"));
    EXPECT_EQ(result["version_id"].get<std::string>(), v1->id);

    // The key names the VERSION and stays inside the org's own prefix — that
    // is what keeps version N+1's render off version N's object, and one
    // tenant's tree out of another's.
    const std::string key = result["key"].get<std::string>();
    EXPECT_EQ(key, "org/" + org_id + "/generated/" + v1->id + "/invoice.pdf");

    auto after = documents.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(after);
    ASSERT_TRUE(after->current_version_id);
    EXPECT_EQ(*after->current_version_id, v1->id);
    EXPECT_EQ(after->status, "final");
    ASSERT_TRUE(after->s3_key);
    EXPECT_EQ(*after->s3_key, key);
    EXPECT_TRUE(Storage::get().exists(key));
}

// ── the engine descriptor itself ─────────────────────────────────────────────
// Pure parsing, no fixture and no services — but it lives here rather than in
// tests/unit because the function under test is declared in RenderJob.hpp,
// which drags in the repository/storage/job stack that the unit bucket is
// defined by NOT having.

// Task 1 found that `typst --version` prints `typst 0.15.1 (9dfd3a08)`: the
// build's commit hash is appended at runtime. Storing that line verbatim
// would make "was this rendered by 0.15.1?" a substring match, and would
// change the recorded descriptor when the SAME release is rebuilt. So the
// version is parsed out and the hash dropped — the exact binary stays
// identifiable through TYPST_SHA256 in docker/Dockerfile.
TEST(DocgenEngineDescriptor, ParsesTheVersionAndDropsTheBuildHash) {
    EXPECT_EQ(Docgen::typst_descriptor("typst 0.15.1 (9dfd3a08)\n"), "typst 0.15.1");
    EXPECT_EQ(Docgen::typst_descriptor("typst 0.15.1\n"), "typst 0.15.1");
    EXPECT_EQ(Docgen::typst_descriptor("typst 0.16.0 (deadbeef)\r\nignored second line\n"), "typst 0.16.0");
}

// An unreadable answer degrades to the bare engine name — the render still
// produces a document, and a coarse engine note beats none. What it must NOT
// do is store a line whose shape we do not understand.
TEST(DocgenEngineDescriptor, DegradesToTheBareEngineNameOnUnparseableOutput) {
    EXPECT_EQ(Docgen::typst_descriptor(""), "typst");
    EXPECT_EQ(Docgen::typst_descriptor("command not found: typst\n"), "typst");
}

// The engine that produced the bytes is part of the version's provenance:
// Typst is pre-1.0, and re-rendering a v1 template under a later engine can
// lay it out differently, so "which template" is not enough to reproduce a
// document.
//
// WHICH engine is deliberately not written here. It is read off the invoice's
// own TemplateInfo — the same directory-decided answer the render pipeline
// uses — because a literal engine name is a test that has to be edited by
// every conversion instead of surviving it; this one survived all ten. The
// property is the AGREEMENT: what got stored on the version is the engine the
// template on disk selects, prefix-matched because the descriptor carries a
// version suffix the bare name does not (see Docgen::engine_version). The
// engine command variable points at the stub script, which is exactly why the
// descriptor may not be derived from the command.
TEST_F(RenderJobTest, RecordsTheEngineOnTheRenderedVersion) {
    use_succeeding_engine_stub();
    Docgen::TemplateRegistry registry;
    const auto invoice_template = registry.latest("invoice");
    ASSERT_TRUE(invoice_template.has_value());
    const std::string expected_engine = Docgen::engine_name(invoice_template->engine);

    auto org_id = make_org("111280000014");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id,
                                "invoice",
                                "generated",
                                "draft",
                                std::nullopt,
                                "invoice",
                                "v1",
                                std::optional<nlohmann::json>{valid_invoice_input()});
    auto v1 = documents.latest_version(org_id, doc.id);
    ASSERT_TRUE(v1);
    // Nothing was rendered into it yet, so it carries no engine at all.
    EXPECT_FALSE(v1->render_engine.has_value());

    auto result = Docgen::process_job(json{{"org_id", org_id},
                                           {"document_id", doc.id},
                                           {"version_id", v1->id},
                                           {"slug", "invoice"},
                                           {"input", valid_invoice_input()}});
    ASSERT_FALSE(result.contains("skipped")) << result.dump();
    const std::string reported = result["render_engine"].get<std::string>();
    EXPECT_EQ(reported.rfind(expected_engine, 0), 0U)
        << "the job reported engine '" << reported << "' for a template the registry resolves to '" << expected_engine
        << "'";

    auto version = documents.find_version(org_id, doc.id, 1);
    ASSERT_TRUE(version.has_value());
    ASSERT_TRUE(version->render_engine.has_value());
    EXPECT_EQ(*version->render_engine, reported);
    // …and it is served, not just stored: this field is the answer to "why
    // does this PDF differ from the one I printed in March".
    EXPECT_EQ(nlohmann::json(*version)["render_engine"].get<std::string>(), reported);
}

// The upload path shares set_version_file with the render path, and it must
// neither invent an engine for a human-uploaded file nor blank one a render
// already recorded — hence COALESCE on the column. Both halves are asserted
// here because a plain `render_engine = $7` would pass the first.
TEST_F(RenderJobTest, ConfirmUploadStyleWriteDoesNotTouchTheRecordedEngine) {
    // No engine stub: nothing is rendered here, the repository is driven
    // directly — this is about the WRITE, not about a compile.
    auto org_id = make_org("111280000015");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id,
                                "invoice",
                                "generated",
                                "draft",
                                std::nullopt,
                                "invoice",
                                "v1",
                                std::optional<nlohmann::json>{valid_invoice_input()});
    auto v1 = documents.latest_version(org_id, doc.id);
    ASSERT_TRUE(v1);

    // No engine argument — the shape LedgerDocumentsController::confirmUpload
    // calls with. A version that never had one keeps NULL.
    ASSERT_TRUE(documents.set_version_file(
        org_id, v1->id, "org/" + org_id + "/uploaded/x.pdf", std::string(64, 'a'), "application/pdf", 12));
    auto after_upload = documents.find_version(org_id, doc.id, 1);
    ASSERT_TRUE(after_upload);
    EXPECT_FALSE(after_upload->render_engine.has_value());

    // Now record one, then repeat the engine-less write: it survives.
    ASSERT_TRUE(documents.set_version_file(org_id,
                                           v1->id,
                                           "org/" + org_id + "/generated/x.pdf",
                                           std::string(64, 'b'),
                                           "application/pdf",
                                           34,
                                           std::string("typst 0.15.1")));
    ASSERT_TRUE(documents.set_version_file(
        org_id, v1->id, "org/" + org_id + "/uploaded/y.pdf", std::string(64, 'c'), "application/pdf", 56));
    auto after = documents.find_version(org_id, doc.id, 1);
    ASSERT_TRUE(after);
    ASSERT_TRUE(after->render_engine.has_value());
    EXPECT_EQ(*after->render_engine, "typst 0.15.1");
}

// THE property the whole versioning work exists for: a later render must not
// touch an earlier version's bytes. Both renders produce genuinely different
// PDFs (set_canned_pdf), so equal-looking objects could not hide a rewrite.
TEST_F(RenderJobTest, RenderOfVersionTwoLeavesVersionOnesObjectIntact) {
    use_succeeding_engine_stub();
    auto org_id = make_org("111280000006");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id,
                                "invoice",
                                "generated",
                                "draft",
                                std::nullopt,
                                "invoice",
                                "v1",
                                std::optional<nlohmann::json>{valid_invoice_input()});
    auto v1 = documents.latest_version(org_id, doc.id);
    ASSERT_TRUE(v1);

    const std::string v1_bytes = std::string(kFakePdfBytes) + "% version one\n";
    set_canned_pdf(v1_bytes);
    auto first = Docgen::process_job(json{{"org_id", org_id},
                                          {"document_id", doc.id},
                                          {"version_id", v1->id},
                                          {"slug", "invoice"},
                                          {"input", valid_invoice_input()}});
    ASSERT_FALSE(first.contains("skipped"));
    const std::string v1_key = first["key"].get<std::string>();

    auto v2 = documents.add_version(
        org_id, doc.id, std::optional<nlohmann::json>{valid_invoice_input()}, std::string("v1"), std::nullopt);
    const std::string v2_bytes = std::string(kFakePdfBytes) + "% version two\n";
    set_canned_pdf(v2_bytes);
    auto second = Docgen::process_job(json{{"org_id", org_id},
                                           {"document_id", doc.id},
                                           {"version_id", v2.id},
                                           {"slug", "invoice"},
                                           {"input", valid_invoice_input()}});
    ASSERT_FALSE(second.contains("skipped")) << second.dump();
    const std::string v2_key = second["key"].get<std::string>();

    // The keys are not merely DIFFERENT — each names its own version. Two
    // distinct keys prove nothing on their own: the old per-document scheme
    // also produced distinct paths (a random uuid per call), so a byte
    // comparison alone would still pass after a revert to it. Asserting the
    // relationship is what actually pins the scheme.
    ASSERT_NE(v1_key, v2_key);
    EXPECT_EQ(v1_key, "org/" + org_id + "/generated/" + v1->id + "/invoice.pdf");
    EXPECT_EQ(v2_key, "org/" + org_id + "/generated/" + v2.id + "/invoice.pdf");
    EXPECT_NE(v1_key.find(v1->id), std::string::npos);
    EXPECT_NE(v2_key.find(v2.id), std::string::npos);
    EXPECT_EQ(v1_key.find(v2.id), std::string::npos);
    EXPECT_EQ(v2_key.find(v1->id), std::string::npos);

    // Version 1's object still holds version 1's bytes — not version 2's.
    auto v1_stored = Storage::get().get(v1_key);
    ASSERT_TRUE(v1_stored.has_value());
    EXPECT_EQ(*v1_stored, v1_bytes);
    auto v2_stored = Storage::get().get(v2_key);
    ASSERT_TRUE(v2_stored.has_value());
    EXPECT_EQ(*v2_stored, v2_bytes);

    // …and version 1's ROW still points at version 1's object, while the
    // document now reports version 2's.
    auto v1_row = documents.find_version(org_id, doc.id, 1);
    ASSERT_TRUE(v1_row);
    EXPECT_EQ(v1_row->s3_key.value_or(""), v1_key);
    EXPECT_EQ(v1_row->checksum_sha256.value_or(""), Utils::Crypto::sha256_hex(v1_bytes));
    auto after = documents.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(after);
    ASSERT_TRUE(after->current_version_id);
    EXPECT_EQ(*after->current_version_id, v2.id);
    EXPECT_EQ(after->s3_key.value_or(""), v2_key);
}

TEST_F(RenderJobTest, IsANoOpForASupersededVersion) {
    use_succeeding_engine_stub();
    auto org_id = make_org("111280000007");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id,
                                "invoice",
                                "generated",
                                "draft",
                                std::nullopt,
                                "invoice",
                                "v1",
                                std::optional<nlohmann::json>{valid_invoice_input()});
    auto v1 = documents.latest_version(org_id, doc.id);
    ASSERT_TRUE(v1);
    documents.add_version(org_id,
                          doc.id,
                          std::optional<nlohmann::json>{valid_invoice_input()},
                          std::string("v1"),
                          std::nullopt);  // v2 вытесняет v1

    auto result = Docgen::process_job(json{{"org_id", org_id},
                                           {"document_id", doc.id},
                                           {"version_id", v1->id},
                                           {"slug", "invoice"},
                                           {"input", valid_invoice_input()}});
    EXPECT_EQ(result["skipped"].get<std::string>(), "superseded");

    auto after = documents.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(after);
    // The NULL pointer here is the ACCEPTED state, not an oversight, and this
    // assertion is what pins it (RenderJob.hpp's set_current_version comment
    // points back at this test): if a first render loses the race to an edit
    // and the newer version's render never succeeds, the document keeps
    // reporting no file at all. Publishing this version instead would make
    // the document report an OLD file under a NEWER version number, which for
    // evidence is worse; the earlier version's PDF, once it exists, stays
    // reachable through versions/{n}/download-url either way.
    EXPECT_FALSE(after->current_version_id.has_value());
    EXPECT_EQ(after->status, "draft");
    // Nothing was stored — the skip happens BEFORE the upload.
    auto v1_row = documents.find_version(org_id, doc.id, 1);
    ASSERT_TRUE(v1_row);
    EXPECT_FALSE(v1_row->s3_key.has_value());
}

TEST_F(RenderJobTest, DoesNotOverwriteAnAlreadyRenderedVersionOnRerun) {
    // ВАЖНО про форму проверки: PDF в этом дереве НЕ байт-стабилен —
    // SOURCE_DATE_EPOCH нигде не выставляется, движок штампует в файл дату
    // сборки. Поэтому «повтор ничего не перезаписал» доказывается тем, что
    // джоба вернула skipped и байты в хранилище не изменились, а НЕ тем, что
    // второй рендер дал ту же контрольную сумму: без гварда суммы разошлись
    // бы, но и с наивным сравнением тест флакал бы на любом изменении
    // шаблона. Второй рендер здесь намеренно выдаёт ДРУГИЕ байты — если бы
    // гвард пропал, объект и checksum поехали бы, и тест это увидит.
    use_succeeding_engine_stub();
    auto org_id = make_org("111280000008");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id,
                                "invoice",
                                "generated",
                                "draft",
                                std::nullopt,
                                "invoice",
                                "v1",
                                std::optional<nlohmann::json>{valid_invoice_input()});
    auto v1 = documents.latest_version(org_id, doc.id);
    ASSERT_TRUE(v1);
    const json payload = {{"org_id", org_id},
                          {"document_id", doc.id},
                          {"version_id", v1->id},
                          {"slug", "invoice"},
                          {"input", valid_invoice_input()}};

    auto first_result = Docgen::process_job(payload);
    EXPECT_FALSE(first_result.contains("skipped"));
    const auto first = documents.find_version(org_id, doc.id, 1);
    ASSERT_TRUE(first);
    ASSERT_TRUE(first->s3_key);
    const std::string first_key = *first->s3_key;
    const std::string first_checksum = first->checksum_sha256.value_or("");
    ASSERT_FALSE(first_checksum.empty());

    set_canned_pdf(std::string(kFakePdfBytes) + "% a SECOND, different render\n");
    auto second_result = Docgen::process_job(payload);  // повтор той же джобы
    EXPECT_EQ(second_result["skipped"].get<std::string>(), "already_rendered");

    const auto again = documents.find_version(org_id, doc.id, 1);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->s3_key.value_or(""), first_key);
    EXPECT_EQ(again->checksum_sha256.value_or(""), first_checksum);
    EXPECT_EQ(documents.list_versions(org_id, doc.id).size(), 1u);
    // И содержимое объекта в хранилище то же — Storage::put повторно не звался.
    EXPECT_EQ(Utils::Crypto::sha256_hex(Storage::get().get(first_key).value_or("")), first_checksum);
}

TEST_F(RenderJobTest, IsANoOpForAVersionOfAnotherOrg) {
    use_succeeding_engine_stub();
    auto org_id = make_org("111280000009");
    auto other_org_id = make_org("111280000010");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id,
                                "invoice",
                                "generated",
                                "draft",
                                std::nullopt,
                                "invoice",
                                "v1",
                                std::optional<nlohmann::json>{valid_invoice_input()});
    auto v1 = documents.latest_version(org_id, doc.id);
    ASSERT_TRUE(v1);

    // Same version id, another tenant's org_id: the version is simply not
    // there as far as that org is concerned, so nothing is rendered into it
    // and no object lands under either org's prefix.
    auto result = Docgen::process_job(json{{"org_id", other_org_id},
                                           {"document_id", doc.id},
                                           {"version_id", v1->id},
                                           {"slug", "invoice"},
                                           {"input", valid_invoice_input()}});
    EXPECT_EQ(result["skipped"].get<std::string>(), "missing");

    auto v1_row = documents.find_version(org_id, doc.id, 1);
    ASSERT_TRUE(v1_row);
    EXPECT_FALSE(v1_row->s3_key.has_value());
}

// Fix round 1 — the version_id-less fallback must not GUESS which version a
// legacy job meant. This is the crossing-a-deploy shape: the job was
// enqueued by the old build (its `input` IS version 1's snapshot), sat in
// Redis, and by the time the new worker picked it up an edit had appended
// version 2. Resolving "the newest version" here would render version 1's
// input into version 2, publish it, and make version 2's own job skip as
// already_rendered — a document whose PDF contradicts its own
// input_snapshot, undetectably. Skipping is the only safe answer: the job
// can be re-enqueued by hand with the right version_id.
TEST_F(RenderJobTest, IsANoOpForALegacyPayloadWhoseDocumentWasEditedAcrossTheDeploy) {
    use_succeeding_engine_stub();
    auto org_id = make_org("111280000012");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id,
                                "invoice",
                                "generated",
                                "draft",
                                std::nullopt,
                                "invoice",
                                "v1",
                                std::optional<nlohmann::json>{valid_invoice_input()});
    auto v1 = documents.latest_version(org_id, doc.id);
    ASSERT_TRUE(v1);
    auto v2 = documents.add_version(
        org_id, doc.id, std::optional<nlohmann::json>{valid_invoice_input()}, std::string("v1"), std::nullopt);

    // No version_id — exactly what the old build enqueued.
    auto result = Docgen::process_job(
        json{{"org_id", org_id}, {"document_id", doc.id}, {"slug", "invoice"}, {"input", valid_invoice_input()}});
    EXPECT_EQ(result["skipped"].get<std::string>(), "superseded");
    EXPECT_EQ(result["version_id"].get<std::string>(), v2.id);

    // Neither version received the stale render, and nothing was published.
    auto v1_row = documents.find_version(org_id, doc.id, 1);
    ASSERT_TRUE(v1_row);
    EXPECT_FALSE(v1_row->s3_key.has_value());
    auto v2_row = documents.find_version(org_id, doc.id, 2);
    ASSERT_TRUE(v2_row);
    EXPECT_FALSE(v2_row->s3_key.has_value());
    auto after = documents.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(after);
    EXPECT_FALSE(after->current_version_id.has_value());
    EXPECT_EQ(after->status, "draft");
}

// The fallback's other bound, on the same legacy shape: version 1 is still
// the newest, but a duplicate of the same old job already rendered it. The
// file stays as it is — a re-run overwrites nothing whether or not the
// payload names its version.
TEST_F(RenderJobTest, IsANoOpForALegacyPayloadWhoseVersionOneAlreadyHasAFile) {
    use_succeeding_engine_stub();
    auto org_id = make_org("111280000013");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id,
                                "invoice",
                                "generated",
                                "draft",
                                std::nullopt,
                                "invoice",
                                "v1",
                                std::optional<nlohmann::json>{valid_invoice_input()});
    const json legacy_payload = {
        {"org_id", org_id}, {"document_id", doc.id}, {"slug", "invoice"}, {"input", valid_invoice_input()}};

    auto first = Docgen::process_job(legacy_payload);
    ASSERT_FALSE(first.contains("skipped")) << first.dump();
    const std::string key = first["key"].get<std::string>();

    set_canned_pdf(std::string(kFakePdfBytes) + "% a SECOND, different render\n");
    auto second = Docgen::process_job(legacy_payload);
    EXPECT_EQ(second["skipped"].get<std::string>(), "already_rendered");

    auto v1_row = documents.find_version(org_id, doc.id, 1);
    ASSERT_TRUE(v1_row);
    EXPECT_EQ(v1_row->s3_key.value_or(""), key);
    EXPECT_EQ(Utils::Crypto::sha256_hex(Storage::get().get(key).value_or("")), v1_row->checksum_sha256.value_or(""));
}

// Задача 11: аннулирование существует, и рендер не имеет права его отменять.
// Джоба, поставленная в очередь ДО аннулирования, доедет до воркера уже
// после — и если бы она отработала, документ получил бы файл и статус
// 'final' поверх пометки «недействителен», то есть аннулирование можно было
// бы обойти простым ожиданием. Единственное, что этого не допускает, —
// (d.voided_at IS NOT NULL) в version_render_state() и ветка kVoided.
TEST_F(RenderJobTest, DoesNotResurrectAVoidedDocument) {
    use_succeeding_engine_stub();
    auto org_id = make_org("111280000011");
    auto user_id = seed_user("voider@example.com");
    Ledger::DocumentRepository documents;
    auto doc = documents.create(org_id,
                                "invoice",
                                "generated",
                                "draft",
                                std::nullopt,
                                "invoice",
                                "v1",
                                std::optional<nlohmann::json>{valid_invoice_input()});
    auto v1 = documents.latest_version(org_id, doc.id);
    ASSERT_TRUE(v1);

    ASSERT_TRUE(documents.void_document(org_id, doc.id, user_id, "выписан по ошибке"));

    auto result = Docgen::process_job(json{{"org_id", org_id},
                                           {"document_id", doc.id},
                                           {"version_id", v1->id},
                                           {"slug", "invoice"},
                                           {"input", valid_invoice_input()}});
    EXPECT_EQ(result["skipped"].get<std::string>(), "voided");

    // Ни файла в версии, ни опубликованного указателя, ни воскрешённого
    // статуса — документ остался ровно тем, чем его оставил человек.
    auto version = documents.find_version(org_id, doc.id, 1);
    ASSERT_TRUE(version);
    EXPECT_FALSE(version->s3_key.has_value());
    auto after = documents.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(after);
    EXPECT_FALSE(after->current_version_id.has_value());
    EXPECT_EQ(after->status, "draft");
    ASSERT_TRUE(after->voided_at.has_value());
    EXPECT_EQ(after->void_reason.value_or(""), "выписан по ошибке");
}

}  // namespace
