/**
 * @file RenderJob.hpp
 * @brief Background job handler for `docgen.render`: validates a document's
 *        input against its template's JSON Schema, compiles it to PDF with
 *        the engine that template selects, stores the PDF, and marks the
 *        document `final`.
 *
 * Payload: `{org_id, document_id, version_id, slug, input}`. On ANY failure
 * (schema rejection, missing template, engine exit != 0, storage/DB error)
 * this throws — the job framework's retry/DLQ machinery takes over
 * (src/jobs/Dispatcher.hpp) and the document is left exactly as it was
 * (typically `draft`): `set_version_file`/`set_current_version`/
 * `set_status_if` only run after the PDF has already been compiled and
 * durably stored, so there is no partial-success state to unwind.
 *
 * P3 (migrations/018_document_versions.sql): the PDF's metadata belongs to a
 * VERSION, not to the document row. The job fills in the version the payload
 * NAMES and only then moves `current_version_id` onto it — that pointer move
 * IS the publication of the render, and until it happens readers keep seeing
 * the previous version's file rather than a half-written one.
 *
 * P3 task 10 — two properties this file is responsible for:
 *   * the stored object is keyed on `version_id` (Files::version_key), so a
 *     render of version N+1 cannot overwrite version N's PDF. A version is
 *     evidence; the bytes behind a link already handed out may not change;
 *   * a version that must not be rendered (already has a file, has been
 *     superseded by a newer version, belongs to a voided document, or does
 *     not exist in this org) is a NO-OP that returns `{..., skipped}` —
 *     nothing is uploaded, nothing is written. The guard is state-based, not
 *     a checksum comparison: nothing here sets SOURCE_DATE_EPOCH, so two
 *     renders of identical input produce DIFFERENT bytes and "did someone
 *     re-render this?" is not answerable from a digest.
 *
 * TWO ENGINES, one per template directory (see TemplateRegistry::load). The
 * migration converts one template at a time, so both are live:
 *
 *   * Typst (`docgen.typst_cmd` / `DOCGEN_TYPST_CMD`, default `typst`) —
 *     `write_typst_inputs` writes the normalized input to `input.json` and
 *     copies the template to `main.typ` in the same ScopedTempDir, then one
 *     `typst compile --root <dir> main.typ main.pdf` pass. The template reads
 *     the data itself (`#let d = json("input.json")`), so there is no
 *     templating layer and nothing to escape — a value is content, never
 *     source. The JSON goes in a FILE, not on the command line via
 *     `--input`: an unbounded document has no business on an argv;
 *   * XeLaTeX (`docgen.latex_cmd` / `DOCGEN_LATEX_CMD`, default `xelatex`),
 *     described next, for the templates not yet converted.
 *
 * The XeLaTeX invocation runs twice under `/usr/bin/timeout 60` — once for
 * content, once more so a future template's `\ref`/`\pageref`/totals that
 * depend on a first pass resolve (the shipped invoice template needs none,
 * but the two-pass contract is cheap insurance for templates that do). Unit/
 * integration tests never invoke the real xelatex: DOCGEN_LATEX_CMD points
 * at a stub script that copies a canned PDF — see
 * tests/integration/test_render_job.cpp. The real XeLaTeX only runs in the
 * `template-render` CI job, on the worker image (the only place TeX Live is
 * installed — see docker/Dockerfile).
 */

#pragma once

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
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
 *        pair — or, worse, the input JSON snapshot, baked into `main.tex` on
 *        the LaTeX path and sitting there verbatim as `input.json` on the
 *        Typst one. That directory is also the Typst sandbox: `--root` points
 *        at it, so it is the entire filesystem a template can read.
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

/// Resolve the configured Typst command: `docgen.typst_cmd` /
/// `DOCGEN_TYPST_CMD`, default `typst`. Same Config-optional shape as
/// latex_cmd() so the worker's `--render-template` CLI mode (no
/// Core::initialize) can use it too.
inline std::string typst_cmd() {
    if (Config::is_initialized())
        return Config::get().get<std::string>("docgen.typst_cmd", "DOCGEN_TYPST_CMD", "typst");
    if (const char* env = std::getenv("DOCGEN_TYPST_CMD"))
        return env;
    return "typst";
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
 * @brief Compile `<dir>/main.typ` to `<dir>/main.pdf` with one `typst
 *        compile` pass under a 60s timeout.
 * @details `--root <dir>` confines every `read`/`include` the template can
 *          perform to the scratch directory that holds only main.typ and
 *          input.json. Verified against typst 0.15.1: an ABSOLUTE path is
 *          resolved relative to the root (`#read("/etc/passwd")` fails with
 *          "file not found (searched at <dir>/etc/passwd)") and a relative
 *          climb errors with "path would escape the project root". One pass,
 *          not two — Typst has no aux-file fixpoint to converge.
 * @note Typst exits 0 and writes NO log when content overflows the page: it
 *       clips or draws past the margin silently. There is deliberately
 *       nothing here that greps a transcript, because there is no transcript.
 *       Overflow is caught downstream by scripts/check-render.py, which reads
 *       the PDF instead of the engine's opinion of it.
 * @note A genuine failure IS loud: Typst writes its diagnostic (with source
 *       line and caret) to stderr and produces no PDF at all, so the nonzero
 *       exit below carries the engine's own message — which is what a
 *       developer needs, since the most common failure is a template dotting
 *       into a key the input does not have. `run_command` merges stderr into
 *       the captured output for exactly this reason. A hang is bounded by
 *       `timeout 60`: exit 124, or a signal, which `run_command` reports as
 *       -1 — both nonzero, both throw.
 * @throws std::runtime_error on a nonzero exit, or if it reports success but
 *         `main.pdf` is missing.
 */
inline void compile_typst(const std::filesystem::path& dir, const std::string& cmd) {
    const std::string full_cmd = "cd " + dir.string() + " && /usr/bin/timeout 60 " + cmd + " compile --root " +
                                 dir.string() + " main.typ main.pdf";
    std::string output;
    const int rc = run_command(full_cmd, &output);
    if (rc != 0)
        throw std::runtime_error("docgen: typst compile failed (exit " + std::to_string(rc) + "): " + output);
    if (!std::filesystem::exists(dir / "main.pdf"))
        throw std::runtime_error("docgen: typst reported success but main.pdf is missing");
}

/**
 * @brief Turn the raw output of `typst --version` into the storable
 *        descriptor: `typst 0.15.1` out of `typst 0.15.1 (9dfd3a08)`.
 * @details Split out from engine_version() below purely so it is TESTABLE:
 *          engine_version caches its answer in a function-local static (one
 *          subprocess per worker, not one per render), and a cache cannot be
 *          re-exercised with a second input inside one test binary. The
 *          parsing is the part with a decision in it, so it is the part that
 *          gets a test.
 *
 *          Takes the first whitespace-separated token that STARTS WITH A
 *          DIGIT, which drops both the program name Typst prints first and
 *          the `(9dfd3a08)` build hash it appends. Anything unrecognizable —
 *          empty output, a line with no numeric token — degrades to the bare
 *          engine name rather than storing a line whose shape we do not
 *          understand.
 * @return `typst <version>`, or the bare `typst` if no version is parseable.
 */
inline std::string typst_descriptor(const std::string& version_output) {
    const std::string base = engine_name(Engine::kTypst);
    std::string first_line = version_output;
    if (const std::size_t eol = first_line.find_first_of("\r\n"); eol != std::string::npos)
        first_line.erase(eol);

    std::istringstream tokens(first_line);
    std::string token;
    while (tokens >> token) {
        if (!token.empty() && std::isdigit(static_cast<unsigned char>(token.front())) != 0)
            return base + " " + token;
    }
    return base;
}

/**
 * @brief The engine descriptor recorded on a rendered version:
 *        `xelatex`, or `typst` plus its parsed version — `typst 0.15.1`.
 * @details The engine's stable NAME is never spelled out here: it comes from
 *          `Docgen::engine_name()` (TemplateRegistry.hpp), the single place
 *          that decides what a stored engine is called. This function only
 *          appends the version.
 *
 *          Only Typst carries one, and that asymmetry is deliberate rather
 *          than an omission: Typst is pre-1.0 and every minor release
 *          restyles the same template, so its version is the difference
 *          between a reproducible document and a guess. XeLaTeX's layout has
 *          not moved in years, and the TeX Live release that pins it is
 *          already recorded — by `docker/Dockerfile`, at the commit the image
 *          was built from.
 *
 *          The real binary is asked (`typst --version`), once per process
 *          (function-local static), NOT read off a constant in this file: the
 *          pin is a property of the worker image, and a Dockerfile bump this
 *          source never heard about must still be recorded truthfully.
 *
 *          What is stored is the PARSED version, not the raw line. `typst
 *          --version` prints `typst 0.15.1 (9dfd3a08)` — the trailing commit
 *          hash of the build, which would turn every "was this rendered by
 *          0.15.1?" into a substring match, and would change the stored
 *          descriptor for a rebuild of the very same release. That hash is
 *          not lost: the version tag plus `TYPST_SHA256` in docker/Dockerfile
 *          identify the downloaded binary exactly, which is a stronger record
 *          than the hash alone and one this column does not have to carry.
 *
 *          If the binary cannot be asked — it is absent, it fails, or popen
 *          itself fails — the descriptor degrades to a bare `typst` instead
 *          of failing the render: a document with a coarse engine note beats
 *          no document.
 */
inline std::string engine_version(Engine engine) {
    if (engine != Engine::kTypst)
        return engine_name(engine);

    static const std::string cached = [] {
        try {
            std::string out;
            if (run_command(typst_cmd() + " --version", &out) != 0)
                return std::string(engine_name(Engine::kTypst));
            return typst_descriptor(out);
        } catch (const std::exception&) {
            return std::string(engine_name(Engine::kTypst));
        }
    }();
    return cached;
}

/**
 * @brief Validate, render and compile @p slug's latest template against
 *        @p input, producing `<out_dir>/main.pdf` whichever engine the
 *        template selects.
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
 *
 *          The engine is `info->engine`, decided by the template directory
 *          (TemplateRegistry::load), never by a caller or a payload field:
 *          - `kTypst`  -> `write_typst_inputs` + `compile_typst`, leaving
 *            `main.typ` and `input.json` beside the PDF;
 *          - `kLatex`  -> `render_tex` + `compile_pdf`, leaving `main.tex`.
 * @return the engine descriptor that produced the PDF (`engine_version`),
 *         for the caller to store on the version alongside the file. It is
 *         returned rather than recomputed by the caller because the engine
 *         is resolved HERE, from the template on disk, and nowhere else.
 * @throws std::runtime_error on a missing template, schema-validation
 *         failure, or compile failure.
 */
inline std::string render_and_compile(const std::string& slug,
                                      const json& input,
                                      const std::filesystem::path& out_dir) {
    TemplateRegistry registry;
    auto info = registry.latest(slug);
    if (!info)
        throw std::runtime_error("docgen: no template found for slug '" + slug + "'");
    if (auto err = TemplateRegistry::validate(*info, input))
        throw std::runtime_error("docgen: schema validation failed: " + *err);

    const json normalized = TemplateRegistry::normalize_input(info->schema, input);

    // Everything above is shared by both engines — the template lookup, the
    // schema validation, and the default-fill. normalize_input is MORE
    // load-bearing for Typst, not less: inja printed nothing for a missing
    // optional, Typst hard-errors with "dictionary does not contain key".
    if (info->engine == Engine::kTypst) {
        write_typst_inputs(*info, normalized, out_dir);
        compile_typst(out_dir, typst_cmd());
        return engine_version(info->engine);
    }

    const std::string tex = render_tex(*info, normalized);
    std::ofstream out(out_dir / "main.tex", std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("docgen: cannot write main.tex to " + out_dir.string());
    out << tex;
    out.close();
    if (!out)
        throw std::runtime_error("docgen: failed writing main.tex to " + out_dir.string());

    compile_pdf(out_dir, latex_cmd());
    return engine_version(info->engine);
}

/**
 * @brief Worker-side handler for `docgen.render`.
 * @details On success: stores the compiled PDF under
 *          `Files::version_key(org_id, "generated", version_id, slug + ".pdf")`
 *          — a key that belongs to the VERSION, not to the document — fills
 *          that version's file metadata, publishes it, and moves a still-draft
 *          document to `final`. Throws on any failure — the document stays in
 *          whatever status it already had (never touched until the PDF is
 *          durably stored).
 *
 *          Returns `{document_id, version_id, skipped}` instead, WITHOUT
 *          storing anything, when the version must not receive a render (see
 *          Ledger::VersionRenderState). That is a normal completion, not a
 *          failure: retrying could not change the answer, and the job
 *          framework's DLQ is for things a human should look at.
 */
inline json process_job(const json& payload) {
    const std::string org_id = payload.at("org_id").get<std::string>();
    const std::string document_id = payload.at("document_id").get<std::string>();
    const std::string slug = payload.at("slug").get<std::string>();
    const json input = payload.value("input", json::object());
    // The version these bytes belong to. Every producer of `docgen.render`
    // puts it in the payload (DocgenController, TaxController,
    // PayrollController, HrController, LedgerDocumentsController::
    // createVersion). Absent = a job enqueued by an older build and still in
    // Redis across the deploy: those are resolved below, but only to an
    // UNTOUCHED version 1 — see that block for why the fallback may not
    // simply take whatever is newest. `.value`, not `.at`, only for that
    // window.
    std::string version_id = payload.value("version_id", std::string{});

    ScopedTempDir tmp("docgen-");
    // Which engine, and at which version, produced these exact bytes — stored
    // on the version below. Taken from the render itself, never re-derived
    // from the slug afterwards: the template on disk is what chooses the
    // engine, and it may have been converted between the two.
    const std::string engine = render_and_compile(slug, input, tmp.path());

    const auto pdf_path = tmp.path() / "main.pdf";
    std::ifstream pdf_file(pdf_path, std::ios::binary);
    if (!pdf_file)
        throw std::runtime_error("docgen: cannot open compiled PDF at " + pdf_path.string());
    std::ostringstream pdf_ss;
    pdf_ss << pdf_file.rdbuf();
    const std::string pdf_bytes = pdf_ss.str();

    Ledger::DocumentRepository documents;
    if (version_id.empty()) {
        // Legacy payload (see the version_id comment above): create() made
        // version 1 in the same transaction as the document, so there is
        // always at least one version to look at.
        auto latest = documents.latest_version(org_id, document_id, /*from_primary=*/true);
        if (!latest)
            throw std::runtime_error("docgen: no version found for document " + document_id + " in org " + org_id);
        // …but the fallback is BOUNDED to "version 1, no file yet", and that
        // bound is the whole point. Such a job was enqueued by a build that
        // did not name its version, i.e. before the deploy; its `input` is
        // version 1's snapshot. Resolving "the newest version" at RENDER time
        // would let an edit that landed in between receive version 1's input:
        // the job would render stale input into version 2, publish it, and
        // version 2's own job would then skip as already_rendered — leaving a
        // document whose PDF contradicts its own input_snapshot. That is the
        // exact class of defect versioning exists to remove, so anything
        // other than the untouched version 1 is skipped rather than guessed
        // at. A skipped legacy job is re-enqueueable by hand with the right
        // version_id; a wrong PDF under a right-looking version number is
        // not detectable at all.
        if (latest->version_no != 1 || latest->s3_key.has_value()) {
            const char* reason = latest->version_no != 1 ? "superseded" : "already_rendered";
            spdlog::warn(
                "docgen: payload for document {} carries no version_id and its newest version is {} (no {}); "
                "skipping as {} rather than rendering stale input into it",
                document_id,
                latest->id,
                latest->version_no != 1 ? "longer version 1" : "empty file slot",
                reason);
            return json{{"document_id", document_id}, {"version_id", latest->id}, {"skipped", reason}};
        }
        version_id = latest->id;
        spdlog::warn(
            "docgen: payload for document {} carries no version_id; falling back to its untouched version 1 {}",
            document_id,
            version_id);
    }

    // State is checked BEFORE the upload: a voided, superseded or
    // already-rendered version must get neither an object in storage nor a
    // row in the database. Returning (not throwing) is the point — the job
    // ran fine, its result is simply no longer wanted, and a retry cannot
    // change that.
    const auto state = documents.version_render_state(org_id, document_id, version_id);
    if (state != Ledger::VersionRenderState::kRenderable) {
        const char* reason = state == Ledger::VersionRenderState::kMissing           ? "missing"
                             : state == Ledger::VersionRenderState::kVoided          ? "voided"
                             : state == Ledger::VersionRenderState::kAlreadyRendered ? "already_rendered"
                                                                                     : "superseded";
        spdlog::info("docgen: skipping render of version {} of document {}: {}", version_id, document_id, reason);
        return json{{"document_id", document_id}, {"version_id", version_id}, {"skipped", reason}};
    }

    const std::string checksum = Utils::Crypto::sha256_hex(pdf_bytes);
    // Keyed on the VERSION, not on the document: with one key per document,
    // rendering version 2 would overwrite the object version 1's row still
    // points at — silently replacing the bytes of a PDF someone may already
    // have cited, which is precisely what document_versions exists to
    // prevent. See Files::version_key for the layout and its tenant safety.
    const std::string key = Files::version_key(org_id, "generated", version_id, slug + ".pdf");
    Storage::get().put(key, pdf_bytes, "application/pdf");

    if (!documents.set_version_file(
            org_id, version_id, key, checksum, "application/pdf", static_cast<long long>(pdf_bytes.size()), engine))
        throw std::runtime_error("docgen: set_version_file found no version " + version_id + " in org " + org_id);

    // Publish: only now does the document report this file — and only while
    // this version is still the newest one. "Version N+1 created, its render
    // unfinished" keeps the pointer on N; a lost race (N+2 appeared while we
    // rendered) is not an error, the next job publishes its own version.
    //
    // Accepted consequence, documented rather than fixed: if the FIRST render
    // of a document loses this race and the newer version's render never
    // succeeds (a template that stopped compiling, a job lost before its
    // retry), `current_version_id` stays NULL — the document reports no file
    // even though version 1's PDF is sitting in storage under its own key,
    // and `download-url` honestly answers 409 no_file while
    // `versions/1/download-url` still serves it. That is the deliberate
    // trade: publishing version 1 here would make the document report an
    // OLD file under a NEWER version number, which for evidence is worse
    // than reporting none. The state is pinned by
    // RenderJobTest::IsANoOpForASupersededVersion.
    if (!documents.set_current_version(org_id, document_id, version_id)) {
        // set_current_version() returns false for the whole family of "this
        // (document, version) pair is not publishable right now": the version
        // is no longer the document's newest (the expected race, and why this
        // is an info, not an error), or the document/version pair does not
        // exist in this org at all (the org predicate task 9 added). The
        // former is the only one reachable from here — version_render_state()
        // just confirmed the pair — so the skip code stays "superseded", but
        // the message no longer claims to know which.
        spdlog::info(
            "docgen: not publishing version {} of document {}: it is no longer the document's newest "
            "version, or the (document, version) pair no longer exists in org {}",
            version_id,
            document_id,
            org_id);
        return json{{"document_id", document_id}, {"version_id", version_id}, {"skipped", "superseded"}};
    }
    // Status moves only for a still-draft document: 'sent' and the inbound
    // lifecycle (inbox/recognized/linked/archived) are not this job's to roll
    // back, and neither is an aborted transition someone made while we
    // rendered.
    documents.set_status_if(org_id, document_id, "draft", "final");

    return json{{"document_id", document_id},
                {"version_id", version_id},
                {"slug", slug},
                {"key", key},
                {"checksum_sha256", checksum},
                {"size_bytes", pdf_bytes.size()},
                {"render_engine", engine}};
}

// Self-registration — the worker dispatches "docgen.render" jobs here as
// soon as this header is #included (see src/worker_main.cpp), the same
// JobHandlerRegistrar idiom scripts/new-job.sh scaffolds (Dispatcher.hpp).
inline const Jobs::JobHandlerRegistrar k_docgen_render_job{kJobType, &process_job};

}  // namespace Docgen
