/**
 * @file RenderJob.hpp
 * @brief Background job handler for `docgen.render`: validates a document's
 *        input against its template's JSON Schema, renders LaTeX via inja
 *        (auto-escaped), compiles it to PDF with XeLaTeX, stores the PDF,
 *        and marks the document `final`.
 *
 * Payload: `{org_id, document_id, slug, input}`. On ANY failure (schema
 * rejection, missing template, XeLaTeX exit != 0, storage/DB error) this
 * throws — the job framework's retry/DLQ machinery takes over
 * (src/jobs/Dispatcher.hpp) and the document is left exactly as it was
 * (typically `draft`): `set_version_file`/`set_current_version`/`set_status`
 * only run after the PDF has already been compiled and durably stored, so
 * there is no partial-success state to unwind.
 *
 * P3 (migrations/018_document_versions.sql): the PDF's metadata belongs to a
 * VERSION, not to the document row. The job fills in the document's LATEST
 * version and only then moves `current_version_id` onto it — that pointer
 * move IS the publication of the render, and until it happens readers keep
 * seeing the previous version's file rather than a half-written one.
 *
 * The XeLaTeX invocation (`docgen.latex_cmd` / `DOCGEN_LATEX_CMD`, default
 * `xelatex`) runs twice under `/usr/bin/timeout 60` — once for content, once
 * more so a future template's `\ref`/`\pageref`/totals that depend on a
 * first pass resolve (the shipped invoice template needs none, but the
 * two-pass contract is cheap insurance for templates that do). Unit/
 * integration tests never invoke the real xelatex: DOCGEN_LATEX_CMD points
 * at a stub script that copies a canned PDF — see
 * tests/integration/test_render_job.cpp. The real XeLaTeX only runs in the
 * `template-render` CI job, on the worker image (the only place TeX Live is
 * installed — see docker/Dockerfile).
 */

#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/wait.h>

#include <nlohmann/json.hpp>

#include "docgen/Renderer.hpp"
#include "docgen/TemplateRegistry.hpp"
#include "files/FileKeys.hpp"
#include "jobs/Dispatcher.hpp"
#include "ledger/DocumentRepository.hpp"
#include "storage/Storage.hpp"
#include "utils/Config.hpp"
#include "utils/Crypto.hpp"

namespace Docgen {

using json = nlohmann::json;

/// Job type the worker subscribes to (WORKER_TYPES) to render a document to PDF.
inline constexpr const char* kJobType = "docgen.render";

/**
 * @brief RAII scratch directory. Created via `mkdtemp` under the system temp
 *        root; removed recursively on every exit path (success, exception,
 *        early return) so a failed render never leaks a `main.tex`/`main.pdf`
 *        pair — or, worse, the input JSON snapshot baked into `main.tex`.
 */
class ScopedTempDir {
public:
    explicit ScopedTempDir(const std::string& prefix) {
        std::string tmpl = (std::filesystem::temp_directory_path() / (prefix + "XXXXXX")).string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (mkdtemp(buf.data()) == nullptr)
            throw std::runtime_error("docgen: mkdtemp failed");
        path_ = buf.data();
    }
    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

/// Resolve the configured XeLaTeX command: `docgen.latex_cmd` /
/// `DOCGEN_LATEX_CMD`, default `xelatex`. Works even without Config
/// initialized (falls back to a raw getenv), so the worker's
/// `--render-template` CLI smoke-test mode (no Core::initialize) can use it
/// too.
inline std::string latex_cmd() {
    if (Config::is_initialized())
        return Config::get().get<std::string>("docgen.latex_cmd", "DOCGEN_LATEX_CMD", "xelatex");
    if (const char* env = std::getenv("DOCGEN_LATEX_CMD"))
        return env;
    return "xelatex";
}

/**
 * @brief Run one shell command, draining its merged stdout+stderr so the
 *        child never blocks on a full pipe, and return its exit code.
 * @param output When non-null, receives the captured output (used to build
 *        an informative error message on a nonzero exit).
 * @throws std::runtime_error only if the command could not even be spawned
 *         (`popen`/`pclose` failure) — a nonzero exit is reported via the
 *         return value, not an exception.
 */
inline int run_command(const std::string& cmd, std::string* output = nullptr) {
    const std::string full = cmd + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    if (pipe == nullptr)
        throw std::runtime_error("docgen: failed to spawn: " + cmd);

    std::array<char, 4096> buf{};
    std::ostringstream captured;
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr)
        captured << buf.data();

    const int status = pclose(pipe);
    if (output != nullptr)
        *output = captured.str();
    if (status == -1)
        throw std::runtime_error("docgen: pclose failed for: " + cmd);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;  // killed by signal (e.g. the timeout(1) wrapper firing)
}

/**
 * @brief Compile `<tex_dir>/main.tex` to `<tex_dir>/main.pdf` by running
 *        @p cmd twice, each under a 60s timeout with shell-escape disabled.
 * @throws std::runtime_error on any nonzero exit, or if compilation reports
 *         success but `main.pdf` is missing.
 */
inline void compile_pdf(const std::filesystem::path& tex_dir, const std::string& cmd) {
    const std::string invocation =
        "/usr/bin/timeout 60 " + cmd + " -interaction=nonstopmode -halt-on-error -no-shell-escape main.tex";
    const std::string full_cmd = "cd " + tex_dir.string() + " && " + invocation;

    for (int pass = 1; pass <= 2; ++pass) {
        std::string output;
        const int rc = run_command(full_cmd, &output);
        if (rc != 0)
            throw std::runtime_error("docgen: latex pass " + std::to_string(pass) + " failed (exit " +
                                     std::to_string(rc) + "): " + output);
    }
    if (!std::filesystem::exists(tex_dir / "main.pdf"))
        throw std::runtime_error("docgen: latex reported success but main.pdf is missing");
}

/**
 * @brief Validate, render and compile @p slug's latest template against
 *        @p input into `<out_dir>/main.tex` / `<out_dir>/main.pdf`.
 * @details Shared by the render job and the worker's `--render-template`
 *          CLI smoke-test mode (src/worker_main.cpp), so both paths exercise
 *          the exact same validate -> normalize -> render -> compile
 *          pipeline. `TemplateRegistry::normalize_input` runs strictly AFTER
 *          `validate()` succeeds — it schema-driven-fills every optional
 *          field the caller omitted (so `{{ }}`/`{% if %}` in the template
 *          can dot into it without inja's "variable not found" render
 *          error) — see that function's doc comment for why templates
 *          additionally had to switch `{% if X %}` to `{% if X != "" %}` on
 *          optional string fields.
 * @throws std::runtime_error on a missing template, schema-validation
 *         failure, or compile failure.
 */
inline void render_and_compile(const std::string& slug, const json& input, const std::filesystem::path& out_dir) {
    TemplateRegistry registry;
    auto info = registry.latest(slug);
    if (!info)
        throw std::runtime_error("docgen: no template found for slug '" + slug + "'");
    if (auto err = TemplateRegistry::validate(*info, input))
        throw std::runtime_error("docgen: schema validation failed: " + *err);

    const json normalized = TemplateRegistry::normalize_input(info->schema, input);
    const std::string tex = render_tex(*info, normalized);
    std::ofstream out(out_dir / "main.tex", std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("docgen: cannot write main.tex to " + out_dir.string());
    out << tex;
    out.close();
    if (!out)
        throw std::runtime_error("docgen: failed writing main.tex to " + out_dir.string());

    compile_pdf(out_dir, latex_cmd());
}

/**
 * @brief Worker-side handler for `docgen.render`.
 * @details On success: stores the compiled PDF under
 *          `Files::org_key(org_id, "generated", slug + ".pdf")` and marks
 *          the document `final`. Throws on any failure — the document stays
 *          in whatever status it already had (never touched until the PDF
 *          is durably stored).
 */
inline json process_job(const json& payload) {
    const std::string org_id = payload.at("org_id").get<std::string>();
    const std::string document_id = payload.at("document_id").get<std::string>();
    const std::string slug = payload.at("slug").get<std::string>();
    const json input = payload.value("input", json::object());

    ScopedTempDir tmp("docgen-");
    render_and_compile(slug, input, tmp.path());

    const auto pdf_path = tmp.path() / "main.pdf";
    std::ifstream pdf_file(pdf_path, std::ios::binary);
    if (!pdf_file)
        throw std::runtime_error("docgen: cannot open compiled PDF at " + pdf_path.string());
    std::ostringstream pdf_ss;
    pdf_ss << pdf_file.rdbuf();
    const std::string pdf_bytes = pdf_ss.str();

    const std::string checksum = Utils::Crypto::sha256_hex(pdf_bytes);
    const std::string key = Files::org_key(org_id, "generated", slug + ".pdf");
    Storage::get().put(key, pdf_bytes, "application/pdf");

    Ledger::DocumentRepository documents;
    // The version this render belongs to is the newest one — create() made
    // version 1 in the same transaction as the document, so there is always
    // at least one. Task 10 hardens this against a second render finishing
    // out of order; today the only writer of a document's versions is the
    // single job that renders it.
    auto version = documents.latest_version(org_id, document_id);
    if (!version)
        throw std::runtime_error("docgen: no version found for document " + document_id + " in org " + org_id);
    if (!documents.set_version_file(
            org_id, version->id, key, checksum, "application/pdf", static_cast<long long>(pdf_bytes.size())))
        throw std::runtime_error("docgen: set_version_file found no version " + version->id + " in org " + org_id);
    // Publish: only now does the document report this file.
    if (!documents.set_current_version(org_id, document_id, version->id))
        throw std::runtime_error("docgen: set_current_version found no document " + document_id + " in org " + org_id);
    if (!documents.set_status(org_id, document_id, "final"))
        throw std::runtime_error("docgen: set_status found no document " + document_id + " in org " + org_id);

    return json{{"document_id", document_id},
                {"slug", slug},
                {"key", key},
                {"checksum_sha256", checksum},
                {"size_bytes", pdf_bytes.size()}};
}

// Self-registration — the worker dispatches "docgen.render" jobs here as
// soon as this header is #included (see src/worker_main.cpp), the same
// JobHandlerRegistrar idiom scripts/new-job.sh scaffolds (Dispatcher.hpp).
inline const Jobs::JobHandlerRegistrar k_docgen_render_job{kJobType, &process_job};

}  // namespace Docgen
