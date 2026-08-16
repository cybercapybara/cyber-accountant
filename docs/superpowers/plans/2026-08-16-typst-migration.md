# Typst Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move document generation from LaTeX/XeLaTeX + inja to Typst, one template at a time, without ever leaving production with a half-swapped engine or a weakened render gate.

**Architecture:** The engine is selected **per template directory** by which source file exists: `template.typ` → Typst, `template.tex` → XeLaTeX. `Docgen::TemplateRegistry` reports that choice on `TemplateInfo`, and `Docgen::render_and_compile` dispatches on it. For the whole middle of the migration the worker image carries **both** engines — deliberately heavier, so that every single commit leaves a shippable system and every template lands in its own reviewable diff. Typst reads the schema-validated, normalized input straight from an `input.json` written next to the template, so inja and LaTeX escaping are dropped rather than ported: a tenant value can never be code. TeX Live comes out of the image only after the tenth template is converted and a human has looked at the rendered pages.

**Tech Stack:** C++20 (header-only `src/`, Drogon, nlohmann/json + json-schema-validator), Typst 0.15.1 (pinned static musl binary), poppler `pdftotext`, Python 3 (`scripts/check-render.py`), Docker multi-stage build, GitHub Actions.

**Spec:**
- `.superpowers/sdd/typst-migration-spike.md` — the feasibility spike: measured sizes and timings, the three conversion defects, the "gate first, engine second" recommendation. **Read §5, §7 and §9 before touching a template.**
- `~/.claude/projects/-Users-moveeeax-Public-cybercapybara/memory/cyber-accountant-docgen-typst.md` — the owner's decision and its two corrections.
- `templates/latex/README.md` — the current template/gate contract.
- `CLAUDE.md` — repo invariants 3, 8, 9, 10 and the gate sequence.

## Global Constraints

Every task's requirements implicitly include this section.

### Project invariants (do not violate, do not "improve")

- **All money is integer tiyn (`long long`); no floats; never parse a formatted money string back into a number.** Printed money uses the human form (`450 000,00`) via `Money::format_tiyn_ru` (`src/money/MoneyFormat.hpp`); the ФНО **XML** filed to the tax authority uses neither formatter (whole tenge, `FnoXml::tenge_amount`) and **must not change** in this migration.
- **Amounts in words are computed server-side** (`Money::to_words_ru` / `Money::to_words_kk`); client-supplied `*_words` are rejected 422. No task here touches that path.
- **`schema.json` files are an API contract — the server's allowlists derive from them; they do not change in this migration.** Not one byte, in any of the ten templates.
- **The render gate must pass over every fixture at every step, and its self-test must keep catching its deliberate breakages.** **Amounts are declared to the gate as `amount <path> <tiyn>` and the gate derives the expected string itself** — fixtures must never hand-write a money string.
- **Header-only `src/` (ADR 0003), no new `.cpp`**; conventional commits with **NO AI-attribution trailers** (no `Co-Authored-By`, no "Generated with"); builds and tests run **only in GitHub Actions**; clang-format 17.0.6 (`make fmt`).
- **The worker image is where the engine lives**; `template-render` is the CI job that renders every template against every fixture.
- `TemplateRegistry::normalize_input` **stays and becomes more load-bearing**: Typst hard-errors on a missing optional key (`dictionary does not contain key "address"`) where inja tolerated it. Never "simplify" a template to `d.employer.at("address", default: "")` — that pushes the schema contract into template bodies, which a tenant-editable constructor must not allow.
- `inja` stays in `vcpkg.json`. `src/email/Templates.hpp` uses it for e-mail templates. Only **docgen** stops using it.
- Typst is **pre-1.0 with breaking changes each minor release**: the binary version is pinned in `docker/Dockerfile` (never `latest`), and the engine version is recorded on every rendered document version alongside `template_version`.

### How you verify (builds and tests run only in GitHub Actions)

There is no local build, no local Docker, no local `typst`, no local `xelatex`. For every task:

```bash
git push -u origin <branch>
gh run watch --exit-status          # blocks until the run finishes
gh run view --log-failed            # on failure, read the failing job's log
```

- C++ unit/integration changes are proven by the **`build-and-test`** job in `.github/workflows/ci.yml`.
- Template, `src/docgen/**`, `docker/Dockerfile` and gate-script changes are proven by the **`template-render`** job (it is path-filtered to exactly those paths).
- Formatting is proven by **`lint-format`**; run `make fmt` before pushing anyway.

### Typst crib sheet (the conversion idioms, all measured in the spike)

Every converted template starts with these two lines, in this order:

```typst
// <slug> v1 — Typst. Input is the schema-validated, NORMALIZED JSON the
// worker writes next to this file. Values are data, never source.
#let d = json("input.json")
```

Preamble mapping (all ten LaTeX templates are `\documentclass[a4paper,10pt]` with `\setmainfont[Scale=0.92]{Noto Sans}` and `\setmainlanguage{russian}`, i.e. an effective 9.2pt):

```typst
#set page(paper: "a4", margin: 18mm)            // margin: copy the exact mm from expected.txt
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru")
#set par(justify: true)                          // Typst does NOT justify by default; LaTeX does
```

`landscape` in the `geometry` options becomes `#set page(paper: "a4", flipped: true, margin: <N>mm)`.

| LaTeX / inja | Typst |
|---|---|
| `{{ x }}` | `#d.x` |
| `{{ a.b }}` | `#d.a.b` |
| `{% if X != "" %}…{% endif %}` | `#if d.X != "" […]` — the `!= ""` idiom carries over unchanged, and Typst rejects a non-boolean condition outright, so the inja truthiness trap cannot be written |
| `{% for row in rows %}…{% endfor %}` in a table | `..d.rows.map(r => (r.date, r.doc, r.a_debit)).flatten()` spliced into `#table(…)` |
| `\textbf{X}` / `\field{X}` | `*X*` (drop the `\newcommand{\field}` idiom entirely) |
| `\toprule` / `\midrule` / `\bottomrule` | `table.hline(stroke: 1pt)` / `table.hline(stroke: 0.6pt)` / `table.hline(stroke: 1pt)` |
| `\multicolumn{2}{c}{X}` | `table.cell(colspan: 2, align: center)[X]` |
| `\cmidrule(lr){3-4}` | `table.hline(start: 2, end: 4)` — **0-based** column indices |
| `\hphantom{\field{Сторона А:} }` | `#let phantom(l) = context h(measure(l).width)` then `#phantom[*Сторона А:* ]` — `measure` outside a `context` is a hard error |
| `\hrulefill` on a signature line | `#box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))` |
| `((# comment #))` | `// comment` |
| `\vspace{4mm}` | `#v(4mm)` |
| `\\` (line break) | ` \` at end of line |
| `\par` + "tables start their own paragraph" | **nothing** — a Typst `table` is block-level and cannot be typeset inline. That whole class of bug is gone. |
| `\emergencystretch=2em` | **nothing** — Typst loosens or hyphenates instead of overhanging |

Three rules that exist because the spike was bitten by them:

1. **`] else [` must stay on ONE source line.** Split across two lines, Typst closes the `if`, then typesets the word `else` and the entire second branch **as literal text**, exit 0, no warning. This printed the word "else" into a labour contract.
2. **`line(length: 100%)` inside a `grid`/`table` contributes no height** — the row collapses and the rule is drawn *through* the text above it. Invisible to `pdftotext`, visible only in a raster. Use the `box(… stroke: (bottom: …))` form above.
3. **Never give a data-bearing column a fixed width.** `columns: (80mm, 80mm)` clips silently (exit 0, no log); `columns: (1fr, auto)` shrinks the label column and keeps the amount whole. Measured both ways.

In template body text, a literal `#`, `$`, `*`, `_`, `@`, `<`, `>`, `\` or backtick must be written with a leading backslash (`\#`). This applies to the template's own static text only — **fixture values are never escaped anywhere, by anyone**.

### Money in templates

A template **never formats an amount**. The server hands it the finished string (`450 000,00`) in the input JSON; the template prints `#d.balance_tenge`. `*_tiyn` integers in a fixture are checked by the gate as the money string they must have been formatted into. There is no arithmetic in any template.

---

## File Structure

**New files**

| File | Responsibility |
|---|---|
| `templates/latex/<slug>/v1/template.typ` × 10 | The Typst source for each template. Replaces `template.tex`, which is deleted in the same commit. |
| `migrations/024_document_versions_render_engine.sql` | Adds `render_engine TEXT` to `document_versions`. |

**Modified**

| File | Change |
|---|---|
| `docker/Dockerfile` (worker-runtime stage, ~line 200-240) | Add pinned Typst; later remove the five TeX Live packages. |
| `src/docgen/TemplateRegistry.hpp` | `TemplateInfo::engine` + `source_path` (replaces `tex_path`); discover `template.typ` before `template.tex`. |
| `src/docgen/Renderer.hpp` | Add `write_typst_inputs()`; later shrink to Typst only (inja/escaping deleted). |
| `src/docgen/RenderJob.hpp` | `typst_cmd()`, `compile_typst()`, engine dispatch in `render_and_compile()`, engine version returned and stored. |
| `src/worker_main.cpp` | `--render-template` follows `render_and_compile`'s new return type. |
| `src/ledger/DocumentVersion.hpp`, `src/ledger/DocumentRepository.hpp` | `render_engine` column: model field, column lists, `set_version_file` parameter. |
| `docs/openapi.yaml`, `frontend/src/lib/api/schema.gen.ts` | `render_engine` on the DocumentVersion schema. |
| `scripts/render-templates.sh` | Engine-aware: the overfull-`\hbox` tripwire applies only to LaTeX templates; PDFs are kept for human review. |
| `scripts/check-render.py` | Typst margin cross-check; hyphen slack only if measured. |
| `scripts/check-render-selftest.sh` | The three template-mutating breakages are rewritten per template as it converts. |
| `.github/workflows/ci.yml` (`template-render` job) | Assert the pinned Typst version; upload the rendered PDFs as an artifact. |
| `config/worker.json`, `docs/CONFIG.md` | `docgen.typst_cmd` / `DOCGEN_TYPST_CMD`; later drop `docgen.latex_cmd`. |
| `tests/unit/test_template_registry.cpp`, `tests/unit/test_money_format.cpp`, `tests/integration/test_render_job.cpp`, `tests/integration/test_documents_api.cpp` | Follow the engine change. |
| `templates/latex/README.md` → `templates/docs/README.md`, `CLAUDE.md`, `CHANGELOG.md` | Docs, and the final tree rename. |

**Deleted**

`src/docgen/LatexEscape.hpp`, `tests/unit/test_latex_escape.cpp`, all ten `template.tex`, `templates/typst-spike/` (throwaway).

---

## Sequencing, and where the human looks at the raster

**Ten templates convert one at a time behind a per-template engine selector, not all at once behind a flag.** The reasons, in order of weight:

1. A per-template selector is the only shape in which **every commit is shippable**. `TemplateRegistry` picks the engine from the file that is on disk; a template that has not been converted still has its `template.tex` and still renders through XeLaTeX. There is no moment where production has an engine that half the templates do not work with.
2. Ten templates in one commit is unreviewable, and this phase's dominant risk is a **silent** conversion defect — the spike found two of three that way. A reviewer (human, on the raster) can only do that job one document at a time.
3. The cost of the choice is a worker image that carries both engines for the duration — about 400 MiB heavier than the endpoint, for a handful of days, on an image that is not on the request path. That is a cheap price for "always shippable".

The gate the memory demanded be landed **first** already exists (`scripts/check-render.py` + `scripts/check-render-selftest.sh`, engine-agnostic, three layers, CLAUDE.md invariant 9). This plan's job is to keep it green and biting at every step, never to weaken it.

**The human raster review** — the phase's biggest risk, because one of the spike's three defects (a signature rule drawn *through* the party names) was invisible to `pdftotext` and visible only in a rendered image:

- **Who:** the instance owner. Not the implementing agent, and not a reviewer who cannot read Russian and Kazakh — the defects that matter here are "this legal document looks wrong", which is a judgement about a Kazakhstani document, not about code.
- **Where, per template:** Task 5 makes the `template-render` CI job upload every rendered PDF as a build artifact. Each of the ten conversion tasks (6-15) **ends** with the owner downloading that artifact and opening that template's two-to-four PDFs. A conversion task is not complete until that has happened; the implementing agent reports the artifact link and stops.
- **Where, as a sweep:** Task 17 (removing TeX Live — the point of no return) **begins** with a second pass over all 22 fixture PDFs at once, side by side. That is the last moment at which reverting a template to LaTeX is a one-line change.
- **What to look for**, every time: a rule or line drawn *through* text; a signature block whose rules have collapsed onto the names; a table whose right-hand column is cut off at the page edge; a heading merged into the paragraph below it; any word of English or any stray identifier (`else`, `to_pay`, `none`) in the body. The text gate cannot see any of these.

---

## Task 1: Pinned Typst in the worker image, alongside TeX Live

**Files:**
- Modify: `docker/Dockerfile` (the `worker-runtime` stage, immediately after the TeX Live `RUN apt-get install` block, ~line 234)
- Modify: `.github/workflows/ci.yml` (the `template-render` job, after the "Build worker image" step, ~line 405)

**Interfaces:**
- Produces: a `typst` binary at `/usr/local/bin/typst` in the `worker-runtime` and `worker-render-check` image stages, version **0.15.1**, invocable as `typst`. Later tasks call it as `typst compile --root <dir> main.typ main.pdf`.
- Produces: Dockerfile build args `TYPST_VERSION` and `TYPST_SHA256`.

**Context:** the worker image is the only place a document engine is installed (`docker/Dockerfile` stages `worker-runtime` and, on top of it, `worker-render-check`). `runtime-base` already provides `curl` and `ca-certificates`; `xz-utils` is not installed and the Typst release tarball is `.tar.xz`. Nothing else in the repo changes in this task — no template is converted, no C++ is touched, and XeLaTeX stays exactly where it is.

- [ ] **Step 1: Get the release checksum**

Run, and keep the 64-hex digest it prints:

```bash
curl -sL https://github.com/typst/typst/releases/download/v0.15.1/typst-x86_64-unknown-linux-musl.tar.xz | shasum -a 256
```

- [ ] **Step 2: Add the install block to the worker-runtime stage**

Insert immediately **after** the existing `texlive-*` / `fonts-noto-core` `RUN apt-get ... && rm -rf /var/lib/apt/lists/*` block in the `worker-runtime` stage, and before the `COPY --from=builder` lines. Replace `<DIGEST>` with the digest from Step 1 — nothing else in the block is a variable:

```dockerfile
# Typst — the document engine the docgen templates are migrating to
# (.superpowers/sdd/typst-migration-spike.md). Installed ALONGSIDE TeX Live
# for the duration of the migration: TemplateRegistry picks the engine per
# template directory (template.typ -> typst, template.tex -> xelatex), so
# both must exist until the tenth template is converted. TeX Live is removed
# in its own commit once none remains.
#
# Pinned, never `latest`: Typst is pre-1.0 and every minor release carries
# breaking changes and removals. A document rendered from v1 must still
# re-render for an audit years from now, so the engine version is recorded on
# every rendered document version (document_versions.render_engine) and an
# engine bump is a v<N+1> template event.
#
# One static-pie musl binary, no runtime deps, ~53 MiB installed. amd64 only:
# build-staging is linux/amd64 and nothing builds this image locally.
ARG TYPST_VERSION=0.15.1
ARG TYPST_SHA256=<DIGEST>
ARG TARGETARCH
RUN set -eux; \
    if [ "${TARGETARCH:-amd64}" != "amd64" ]; then \
        echo "typst install: unsupported TARGETARCH=${TARGETARCH} (amd64 only)" >&2; exit 1; \
    fi; \
    apt-get update && apt-get install -y --no-install-recommends xz-utils; \
    curl -fsSL -o /tmp/typst.tar.xz \
        "https://github.com/typst/typst/releases/download/v${TYPST_VERSION}/typst-x86_64-unknown-linux-musl.tar.xz"; \
    echo "${TYPST_SHA256}  /tmp/typst.tar.xz" | sha256sum -c -; \
    tar -xJf /tmp/typst.tar.xz -C /usr/local/bin --strip-components=1 \
        "typst-x86_64-unknown-linux-musl/typst"; \
    rm -f /tmp/typst.tar.xz; \
    chmod 0755 /usr/local/bin/typst; \
    apt-get purge -y xz-utils && apt-get autoremove -y; \
    rm -rf /var/lib/apt/lists/*; \
    typst --version
```

- [ ] **Step 3: Assert the pinned version in CI**

In `.github/workflows/ci.yml`, in the `template-render` job, insert this step directly **after** the "Build worker image (worker-render-check target — TeX Live + poppler)" step and **before** "Render every fixture and gate the PDF":

```yaml
      - name: Assert the pinned engine versions
        if: steps.changes.outputs.docgen == 'true'
        # The engine version is recorded on every rendered document version
        # (document_versions.render_engine) and is part of the render's
        # reproducibility contract. Typst is pre-1.0: a silent bump would
        # change layout under templates already in production use.
        run: |
          docker run --rm --entrypoint typst \
            cyber-accountant-template:worker-render --version | tee /tmp/typst-version
          grep -qx 'typst 0.15.1' /tmp/typst-version
```

- [ ] **Step 4: Push and verify CI**

```bash
git push -u origin <branch>
gh run watch --exit-status
```

Expected: `template-render` passes, its new "Assert the pinned engine versions" step prints `typst 0.15.1`, and the existing render + self-test steps still pass unchanged (nothing has been converted, so all ten templates still go through XeLaTeX).

- [ ] **Step 5: Commit**

```bash
git add docker/Dockerfile .github/workflows/ci.yml
git commit -m "build(worker): install pinned typst 0.15.1 alongside TeX Live"
```

---

## Task 2: Per-template engine discovery in TemplateRegistry

**Files:**
- Modify: `src/docgen/TemplateRegistry.hpp` (the `TemplateInfo` struct ~line 36-45, and `load()` ~line 330-355)
- Modify: `tests/unit/test_template_registry.cpp` (assertions on `tex_path` at ~line 127; the `printed_expressions` helper and `ShippedTemplatesTest.NeverPrintAnEnumPinnedField` at ~line 665-693)
- Modify: `src/docgen/Renderer.hpp` (the one use of `info.tex_path` in `render_tex`, ~line 185)

**Interfaces:**
- Produces:
  ```cpp
  namespace Docgen {
  enum class Engine { kLatex, kTypst };
  struct TemplateInfo {
      std::string slug;
      int version = 0;
      std::string version_str;
      std::filesystem::path dir;
      std::filesystem::path source_path;  // template.typ or template.tex
      Engine engine = Engine::kLatex;
      json schema;
  };
  inline const char* engine_name(Engine e);  // "typst" | "xelatex"
  }
  ```
- `TemplateInfo::tex_path` is **removed**; every reader uses `source_path`.
- Consumes: nothing from earlier tasks.

**Rules:** `template.typ` wins over `template.tex` when both are present (they never both are, except transiently while a template is being converted). If neither exists, `load()` throws `std::runtime_error("template registry: missing template.typ/template.tex for <slug>/<vN>")` — the existing "malformed template is a hard error, not a silent not-found" posture. `schema.json` handling is untouched.

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit/test_template_registry.cpp`. The fixture's existing helper `write_template(slug, version_dir, tex, schema)` writes `template.tex` + `schema.json`; add a sibling that writes `template.typ`:

```cpp
/// Write templates/<slug>/<version_dir>/template.typ + schema.json.
void write_typst_template(const std::string& slug, const std::string& version_dir,
                          const std::string& typ, const std::string& schema) {
    const auto dir = root_ / slug / version_dir;
    fs::create_directories(dir);
    std::ofstream(dir / "template.typ") << typ;
    std::ofstream(dir / "schema.json") << schema;
}
```

```cpp
TEST_F(TemplateRegistryTest, LatexTemplateReportsXelatexEngine) {
    write_template("invoice", "v1", "hello", R"({"type":"object"})");
    auto info = registry().latest("invoice");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->engine, Docgen::Engine::kLatex);
    EXPECT_EQ(info->source_path, root_ / "invoice" / "v1" / "template.tex");
    EXPECT_STREQ(Docgen::engine_name(info->engine), "xelatex");
}

TEST_F(TemplateRegistryTest, TypstTemplateReportsTypstEngine) {
    write_typst_template("payslip", "v1", "#let d = json(\"input.json\")",
                         R"({"type":"object"})");
    auto info = registry().latest("payslip");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->engine, Docgen::Engine::kTypst);
    EXPECT_EQ(info->source_path, root_ / "payslip" / "v1" / "template.typ");
    EXPECT_STREQ(Docgen::engine_name(info->engine), "typst");
}

// Transient state while a template is being converted: the .typ is authoritative.
TEST_F(TemplateRegistryTest, TypstWinsWhenBothSourcesExist) {
    write_template("payslip", "v1", "latex body", R"({"type":"object"})");
    std::ofstream(root_ / "payslip" / "v1" / "template.typ") << "typst body";
    auto info = registry().latest("payslip");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->engine, Docgen::Engine::kTypst);
}

TEST_F(TemplateRegistryTest, MissingBothSourcesThrows) {
    fs::create_directories(root_ / "broken" / "v1");
    std::ofstream(root_ / "broken" / "v1" / "schema.json") << R"({"type":"object"})";
    EXPECT_THROW((void)registry().latest("broken"), std::runtime_error);
}
```

Replace the existing `TEST_F(TemplateRegistryTest, MissingTemplateTexThrows)` body's expectation if it asserts on the old message; keep the test name or rename it to `MissingBothSourcesThrows` (do not keep two tests asserting the same thing). Update the `tex_path` assertion at ~line 127 (`LatestFindsSingleVersion`) to `source_path`.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
git push -u origin <branch> && gh run watch --exit-status
```

Expected: `build-and-test` FAILS to compile — `'engine' is not a member of 'Docgen::TemplateInfo'`, `'source_path' is not a member`, `'Docgen::Engine' has not been declared`.

- [ ] **Step 3: Implement in TemplateRegistry.hpp**

Replace the `TemplateInfo` struct with:

```cpp
/// Which document engine renders a template, decided by the source file that
/// is on disk. The migration converts one template at a time
/// (docs/superpowers/plans/2026-08-16-typst-migration.md), so both values are
/// live simultaneously and the worker image carries both engines.
enum class Engine { kLatex, kTypst };

inline const char* engine_name(Engine e) { return e == Engine::kTypst ? "typst" : "xelatex"; }

/// One resolved template version: where its source lives on disk, which
/// engine compiles it, and its parsed JSON Schema.
struct TemplateInfo {
    std::string slug;
    int version = 0;            ///< Numeric part of the `vN` directory name.
    std::string version_str;    ///< The `vN` directory name as it appears on disk.
    std::filesystem::path dir;  ///< `templates/latex/<slug>/<vN>/`
    std::filesystem::path source_path;  ///< `template.typ` (Typst) or `template.tex` (LaTeX)
    Engine engine = Engine::kLatex;
    json schema;
};
```

In `load()`, replace the `tex_path` block with:

```cpp
        // template.typ wins: while a template is mid-conversion both files can
        // exist for one commit, and the Typst source is the authoritative one.
        const fs::path typ_path = info.dir / "template.typ";
        const fs::path tex_path = info.dir / "template.tex";
        if (fs::exists(typ_path)) {
            info.source_path = typ_path;
            info.engine = Engine::kTypst;
        } else if (fs::exists(tex_path)) {
            info.source_path = tex_path;
            info.engine = Engine::kLatex;
        } else {
            throw std::runtime_error("template registry: missing template.typ/template.tex for " + slug + "/" +
                                     version_str);
        }
```

Update the file's Doxygen header: the layout line now reads `template.typ` (Typst) **or** `template.tex` (LaTeX, being retired). In `src/docgen/Renderer.hpp`, change `render_tex`'s `std::ifstream tex_file(info.tex_path, …)` and its error message to `info.source_path`.

- [ ] **Step 4: Make the enum-printing test engine-aware**

`ShippedTemplatesTest.NeverPrintAnEnumPinnedField` reads `info.tex_path` and scans for inja `{{ path }}` expressions. Once templates are Typst it would pass **vacuously** (no `{{ }}` anywhere) while still asserting `enum_fields > 0`. Change it to read `info.source_path`, and extend the `printed_expressions(const std::string& source)` helper to also collect Typst field references, so it keeps biting throughout the migration:

```cpp
/// Every field path the template PRINTS: inja `{{ a.b }}` expressions and
/// Typst `#d.a.b` references. Both forms are collected unconditionally — a
/// file only ever contains one of them, and a helper that silently found
/// nothing is how a shipped test rots into `EXPECT_TRUE(true)`.
inline std::vector<std::string> printed_expressions(const std::string& source) {
    std::vector<std::string> out;
    static const std::regex kInja(R"(\{\{\s*([A-Za-z_][A-Za-z0-9_.]*)\s*\}\})");
    static const std::regex kTypst(R"(#d\.([A-Za-z_][A-Za-z0-9_.]*))");
    for (const auto* re : {&kInja, &kTypst}) {
        for (std::sregex_iterator it(source.begin(), source.end(), *re), end; it != end; ++it)
            out.push_back((*it)[1].str());
    }
    return out;
}
```

(If the existing helper has a different name or signature, keep its name and add the Typst regex to it rather than introducing a second helper.)

- [ ] **Step 5: Run the tests to verify they pass**

```bash
make fmt && git push && gh run watch --exit-status
```

Expected: `build-and-test` PASSES (all four new registry tests green, everything else unchanged), `lint-format` PASSES, `template-render` PASSES — all ten templates are still LaTeX and still render identically.

- [ ] **Step 6: Commit**

```bash
git add src/docgen/TemplateRegistry.hpp src/docgen/Renderer.hpp tests/unit/test_template_registry.cpp
git commit -m "feat(docgen): select the render engine per template directory"
```

---

## Task 3: The Typst render path

**Files:**
- Modify: `src/docgen/Renderer.hpp` (add `write_typst_inputs`, keep `render_tex` untouched)
- Modify: `src/docgen/RenderJob.hpp` (`typst_cmd()`, `compile_typst()`, dispatch in `render_and_compile()`)
- Modify: `config/worker.json` (~line 61, next to `latex_cmd`)
- Modify: `docs/CONFIG.md` (~line 215, next to the `DOCGEN_LATEX_CMD` row)
- Test: `tests/unit/test_template_registry.cpp` (unit tests for `write_typst_inputs`)

**Interfaces:**
- Consumes: `Docgen::TemplateInfo{ source_path, engine, schema }`, `Docgen::Engine::{kLatex,kTypst}`, `Docgen::engine_name(Engine)` (Task 2).
- Produces:
  ```cpp
  // Renderer.hpp
  inline void write_typst_inputs(const TemplateInfo& info, const json& normalized_input,
                                 const std::filesystem::path& out_dir);
  // RenderJob.hpp
  inline std::string typst_cmd();                                        // "typst" by default
  inline void compile_typst(const std::filesystem::path& dir, const std::string& cmd);
  ```
- `render_and_compile(slug, input, out_dir)` keeps its signature and produces `<out_dir>/main.pdf` for both engines; for Typst it additionally leaves `<out_dir>/main.typ` and `<out_dir>/input.json`.

**How Typst is invoked, and why:** the worker writes the schema-validated, **normalized** input to `input.json` inside the same `ScopedTempDir`, copies the template there as `main.typ`, and runs `typst compile --root <dir> main.typ main.pdf`. The template opens the data with `#let d = json("input.json")`. `--root` confines file access to that directory — the spike verified `#read("/etc/passwd")` resolves *inside* the root and fails, and `#read("../../../../etc/passwd")` errors with "path would escape the project root". Passing the JSON on the command line via `--input` also works but puts an unbounded document on an `argv`; the file is safer. **There is no templating layer and nothing to escape**: a value containing `#panic("x") *bold* #read("/etc/passwd")` is printed verbatim.

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit/test_template_registry.cpp` (it already builds a temp templates root, and `write_typst_inputs` needs no engine binary):

```cpp
TEST_F(TemplateRegistryTest, WriteTypstInputsCopiesTemplateAndWritesNormalizedJson) {
    write_typst_template("payslip", "v1",
                         "#let d = json(\"input.json\")\n#d.employer.name",
                         R"({"type":"object","properties":{
                              "employer":{"type":"object","properties":{
                                "name":{"type":"string"},"address":{"type":"string"}}}}})");
    auto info = registry().latest("payslip");
    ASSERT_TRUE(info.has_value());

    const json input = {{"employer", {{"name", "ТОО \"Ромашка\""}}}};
    const json normalized = Docgen::TemplateRegistry::normalize_input(info->schema, input);

    const auto out = root_ / "out";
    fs::create_directories(out);
    Docgen::write_typst_inputs(*info, normalized, out);

    std::ifstream typ(out / "main.typ");
    std::string typ_body((std::istreambuf_iterator<char>(typ)), std::istreambuf_iterator<char>());
    EXPECT_EQ(typ_body, "#let d = json(\"input.json\")\n#d.employer.name");

    std::ifstream data(out / "input.json");
    json written;
    data >> written;
    EXPECT_EQ(written.at("employer").at("name"), "ТОО \"Ромашка\"");
    // normalize_input filled the declared-but-absent optional; Typst hard-errors
    // on a missing key, so this is load-bearing, not cosmetic.
    EXPECT_EQ(written.at("employer").at("address"), "");
}

// The whole security argument in one test: a value that looks like Typst code
// reaches the engine as DATA, byte for byte, with no escaping applied.
TEST_F(TemplateRegistryTest, WriteTypstInputsNeverTransformsValues) {
    write_typst_template("payslip", "v1", "#let d = json(\"input.json\")",
                         R"({"type":"object","properties":{"note":{"type":"string"}}})");
    auto info = registry().latest("payslip");
    ASSERT_TRUE(info.has_value());

    const std::string payload = R"(#panic("pwned") *bold* $x^2$ #read("/etc/passwd") @l _it_)";
    const auto out = root_ / "out2";
    fs::create_directories(out);
    Docgen::write_typst_inputs(*info, json{{"note", payload}}, out);

    std::ifstream data(out / "input.json");
    json written;
    data >> written;
    EXPECT_EQ(written.at("note").get<std::string>(), payload);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
git push && gh run watch --exit-status
```

Expected: `build-and-test` FAILS to compile — `'write_typst_inputs' is not a member of 'Docgen'`.

- [ ] **Step 3: Implement `write_typst_inputs` in Renderer.hpp**

Add at the end of `namespace Docgen`, leaving `render_tex` and the escaping machinery untouched:

```cpp
/**
 * @brief Stage a Typst render in @p out_dir: `main.typ` (a copy of the
 *        template) + `input.json` (the normalized input).
 * @details There is no templating layer here and nothing to escape. Typst
 *          reads the JSON itself (`#let d = json("input.json")`), so a value
 *          is content and never source: the literal string
 *          `#panic("x") *bold* #read("/etc/passwd")` is typeset character for
 *          character. That is why `escape_latex` is deleted rather than
 *          ported once the last LaTeX template is gone — there is nothing for
 *          a Typst equivalent to do.
 * @param normalized_input MUST be the output of
 *        `TemplateRegistry::normalize_input` — Typst raises a hard error
 *        ("dictionary does not contain key") on a declared-but-absent
 *        optional key, where inja merely printed nothing.
 * @throws std::runtime_error if either file cannot be written.
 */
inline void write_typst_inputs(const TemplateInfo& info, const json& normalized_input,
                               const std::filesystem::path& out_dir) {
    std::error_code ec;
    std::filesystem::copy_file(
        info.source_path, out_dir / "main.typ", std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        throw std::runtime_error("write_typst_inputs: cannot copy " + info.source_path.string() + " to " +
                                 (out_dir / "main.typ").string() + ": " + ec.message());

    std::ofstream data(out_dir / "input.json", std::ios::binary | std::ios::trunc);
    if (!data)
        throw std::runtime_error("write_typst_inputs: cannot write input.json to " + out_dir.string());
    data << normalized_input.dump();
    data.close();
    if (!data)
        throw std::runtime_error("write_typst_inputs: failed writing input.json to " + out_dir.string());
}
```

- [ ] **Step 4: Implement the Typst compile + dispatch in RenderJob.hpp**

Add next to `latex_cmd()`:

```cpp
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
```

Add next to `compile_pdf()`:

```cpp
/**
 * @brief Compile `<dir>/main.typ` to `<dir>/main.pdf` with one `typst
 *        compile` pass under a 60s timeout.
 * @details `--root <dir>` confines every `read`/`include` the template can
 *          perform to the scratch directory that holds only main.typ and
 *          input.json (verified in the spike: an absolute path resolves
 *          inside the root and fails; `../` errors with "path would escape
 *          the project root"). One pass, not two — Typst has no aux-file
 *          fixpoint.
 * @note Typst exits 0 and writes NO log when content overflows the page: it
 *       clips or draws past the margin silently. There is deliberately
 *       nothing here that greps a transcript, because there is no transcript.
 *       Overflow is caught downstream by scripts/check-render.py, which reads
 *       the PDF instead of the engine's opinion of it.
 * @throws std::runtime_error on a nonzero exit, or if it reports success but
 *         `main.pdf` is missing.
 */
inline void compile_typst(const std::filesystem::path& dir, const std::string& cmd) {
    const std::string full_cmd = "cd " + dir.string() + " && /usr/bin/timeout 60 " + cmd +
                                 " compile --root " + dir.string() + " main.typ main.pdf";
    std::string output;
    const int rc = run_command(full_cmd, &output);
    if (rc != 0)
        throw std::runtime_error("docgen: typst compile failed (exit " + std::to_string(rc) + "): " + output);
    if (!std::filesystem::exists(dir / "main.pdf"))
        throw std::runtime_error("docgen: typst reported success but main.pdf is missing");
}
```

In `render_and_compile()`, replace the body after `normalize_input` with the dispatch (everything above it — `latest()`, `validate()`, `normalize_input()` — is unchanged and shared by both engines):

```cpp
    const json normalized = TemplateRegistry::normalize_input(info->schema, input);

    if (info->engine == Engine::kTypst) {
        write_typst_inputs(*info, normalized, out_dir);
        compile_typst(out_dir, typst_cmd());
        return;
    }

    const std::string tex = render_tex(*info, normalized);
    // … existing main.tex write + compile_pdf(out_dir, latex_cmd()) …
```

- [ ] **Step 5: Add the config key**

`config/worker.json`, in the `docgen` object next to `latex_cmd`:

```json
    "typst_cmd": "${DOCGEN_TYPST_CMD:-typst}"
```

`docs/CONFIG.md`, immediately after the `DOCGEN_LATEX_CMD` row:

```markdown
| `DOCGEN_TYPST_CMD` | `docgen.typst_cmd` | string | `typst` | The Typst binary `RenderJob` shells out to, once per render (`typst compile --root <tmpdir> main.typ main.pdf`, under `/usr/bin/timeout 60`). Pinned to 0.15.1 in the worker image — Typst is pre-1.0 and each minor release breaks layout. Tests point this at a stub script. |
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
make fmt && git push && gh run watch --exit-status
```

Expected: `build-and-test` PASSES with both new tests green; `template-render` PASSES unchanged (no template is Typst yet, so `compile_typst` is not yet exercised end to end — that happens in Task 6, where CI renders the converted payslip through the real binary).

- [ ] **Step 7: Commit**

```bash
git add src/docgen/Renderer.hpp src/docgen/RenderJob.hpp config/worker.json docs/CONFIG.md tests/unit/test_template_registry.cpp
git commit -m "feat(docgen): render Typst templates from input.json, no templating layer"
```

---

## Task 4: Record the engine version on every rendered version

**Files:**
- Create: `migrations/024_document_versions_render_engine.sql`
- Modify: `src/ledger/DocumentVersion.hpp` (struct field, `from_row`, `to_json`)
- Modify: `src/ledger/DocumentRepository.hpp` (`kVersionColumns` ~line 128, `set_version_file` ~line 509)
- Modify: `src/docgen/RenderJob.hpp` (`render_and_compile` return value, `engine_version()`, the `set_version_file` call)
- Modify: `src/worker_main.cpp` (~line 268, `run_render_template`)
- Modify: `docs/openapi.yaml` (the DocumentVersion schema, ~line 512-521)
- Modify: `frontend/src/lib/api/schema.gen.ts` (regenerated, not hand-edited)
- Test: `tests/integration/test_render_job.cpp`

**Interfaces:**
- Consumes: `Docgen::typst_cmd()`, `Docgen::compile_typst()`, `Docgen::Engine`, `Docgen::engine_name()` (Tasks 2-3); `Docgen::run_command(cmd, &output)`.
- Produces:
  ```cpp
  // RenderJob.hpp — the engine descriptor stored on the version, e.g.
  // "typst 0.15.1" or "xelatex".
  inline std::string engine_version(Engine engine);
  inline std::string render_and_compile(const std::string& slug, const json& input,
                                        const std::filesystem::path& out_dir);  // returns the descriptor
  // DocumentRepository.hpp — one added trailing parameter, defaulted so no
  // other caller changes.
  bool set_version_file(const std::string& org_id, const std::string& version_id,
                        const std::string& s3_key, const std::string& checksum_sha256,
                        const std::string& mime, long long size_bytes,
                        std::optional<std::string> render_engine = std::nullopt);
  ```

**Why:** Typst is pre-1.0 and each minor release changes layout. `document_versions.template_version` already snapshots *which template* produced a PDF; without the engine, "re-render this 2026 payslip for the audit" is not answerable. `documents.template_version` (the document-level projection) is **not** extended — one place is enough, and widening the Document shape would ripple through the whole documents API.

- [ ] **Step 1: Write the failing test**

In `tests/integration/test_render_job.cpp`, alongside `RenderJobTest.WritesTheFileIntoTheAddressedVersionAndPublishesIt`:

```cpp
// The engine that produced the bytes is part of the version's provenance:
// Typst is pre-1.0 and re-rendering a v1 template under a later engine can
// lay it out differently. The stub reports a fixed version string.
TEST_F(RenderJobTest, RecordsTheEngineVersionOnTheRenderedVersion) {
    use_succeeding_latex_stub();
    const auto ids = create_document_with_version();

    Docgen::process_job(render_payload(ids));

    Ledger::DocumentRepository documents;
    auto version = documents.find_version(kOrgId, ids.document_id, ids.version_id);
    ASSERT_TRUE(version.has_value());
    ASSERT_TRUE(version->render_engine.has_value());
    EXPECT_EQ(*version->render_engine, "xelatex");
}
```

Adapt the helper names (`create_document_with_version`, `render_payload`, `find_version`, `kOrgId`) to the ones this file already uses — read the file's existing happy-path test and mirror it exactly rather than inventing helpers.

- [ ] **Step 2: Run the test to verify it fails**

```bash
git push && gh run watch --exit-status
```

Expected: `build-and-test` FAILS to compile — `'struct Ledger::DocumentVersion' has no member named 'render_engine'`.

- [ ] **Step 3: Write the migration**

`migrations/024_document_versions_render_engine.sql` (sequential numbering after `023_payroll_doc_type.sql`; **no** `BEGIN`/`COMMIT` — the runner wraps migrations):

```sql
-- Which document engine produced this version's PDF, e.g. 'typst 0.15.1' or
-- 'xelatex'. NULL for every version rendered before this column existed, and
-- for versions whose file was uploaded rather than rendered.
--
-- Typst is pre-1.0 and each minor release carries breaking layout changes, so
-- "which template" (template_version) is not enough to reproduce a document:
-- an audit years from now needs the engine too. An engine bump is treated as
-- a v<N+1> template event.
ALTER TABLE document_versions ADD COLUMN IF NOT EXISTS render_engine TEXT;
```

- [ ] **Step 4: Thread the column through the model and repository**

`src/ledger/DocumentVersion.hpp`:
- add `std::optional<std::string> render_engine;` after `template_version`;
- in `from_row`, after the `template_version` block:
  ```cpp
        if (!r["render_engine"].is_null())
            v.render_engine = r["render_engine"].template as<std::string>();
  ```
- in `to_json`, after the `template_version` entry:
  ```cpp
        {"render_engine", v.render_engine ? nlohmann::json(*v.render_engine) : nlohmann::json(nullptr)},
  ```

`src/ledger/DocumentRepository.hpp`:
- `kVersionColumns`: add `render_engine` after `template_version`, so the list reads
  `"id, org_id, document_id, version_no, s3_key, checksum_sha256, mime, size_bytes, template_version, render_engine, input_snapshot, created_by_user_id, created_at, updated_at"`;
- `set_version_file`: add the trailing defaulted parameter and set the column:
  ```cpp
    bool set_version_file(const std::string& org_id,
                          const std::string& version_id,
                          const std::string& s3_key,
                          const std::string& checksum_sha256,
                          const std::string& mime,
                          long long size_bytes,
                          std::optional<std::string> render_engine = std::nullopt) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE document_versions SET s3_key = $3, checksum_sha256 = $4, mime = $5, size_bytes = $6, "
                "render_engine = COALESCE($7, render_engine) "
                "WHERE id = $1 AND org_id = $2 "
                "RETURNING id",
                version_id, org_id, s3_key, checksum_sha256, mime, size_bytes, render_engine);
            return !r.empty();
        });
    }
  ```
  `COALESCE` so the confirm-upload path (which passes no engine) cannot blank an engine that a render already recorded.

- [ ] **Step 5: Produce the descriptor in RenderJob.hpp**

```cpp
/**
 * @brief The engine descriptor stored on a rendered version, e.g.
 *        `typst 0.15.1` or `xelatex`.
 * @details For Typst the real binary is asked (`typst --version`), once per
 *          process (function-local static), because the pinned version is a
 *          property of the image, not of this source file — a Dockerfile bump
 *          that this code did not hear about must still be recorded truthfully.
 *          If the binary cannot be asked, the descriptor degrades to `typst`
 *          rather than failing the render: a document with a coarse engine
 *          note beats no document.
 */
inline std::string engine_version(Engine engine) {
    if (engine == Engine::kLatex)
        return "xelatex";
    static const std::string cached = [] {
        std::string out;
        if (run_command(typst_cmd() + " --version", &out) != 0)
            return std::string("typst");
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        return out.empty() ? std::string("typst") : out;
    }();
    return cached;
}
```

Change `render_and_compile` to `inline std::string render_and_compile(...)`, returning `engine_version(info->engine)` on both branches. In `process_job`, capture it —
`const std::string engine = render_and_compile(slug, input, tmp.path());` — and pass it as the seventh argument to `documents.set_version_file(...)`. Add `{"render_engine", engine}` to the success JSON `process_job` returns. In `src/worker_main.cpp`, `run_render_template` currently calls `Docgen::render_and_compile(slug, input, out_dir);` — keep the call and print the engine:
`const std::string engine = Docgen::render_and_compile(slug, input, out_dir);` then `std::cout << "PASS " << slug << " " << fixture_path << " [" << engine << "]" << std::endl;` (`scripts/render-templates.sh` only checks the exit status, so the extra text is safe).

- [ ] **Step 6: Update the OpenAPI schema and regenerate the frontend types**

`docs/openapi.yaml`, DocumentVersion schema (~line 512): add `render_engine` to the `required:` list right after `template_version`, and add the property next to it:

```yaml
        render_engine:      { type: ['string', 'null'], description: "The document engine that produced this PDF, e.g. 'typst 0.15.1'. NULL for uploaded files and for versions rendered before v0.5.0" }
```

Then:

```bash
make frontend-gen-api
```

Never hand-edit `frontend/src/lib/api/schema.gen.ts`.

- [ ] **Step 7: Run the tests to verify they pass**

```bash
make fmt && git push && gh run watch --exit-status
```

Expected: `build-and-test` PASSES (the new test asserts `xelatex`, since the invoice template is still LaTeX), `openapi-drift` PASSES, `frontend` PASSES, `lint-format` PASSES.

- [ ] **Step 8: Commit**

```bash
git add migrations/024_document_versions_render_engine.sql src/ledger/DocumentVersion.hpp \
        src/ledger/DocumentRepository.hpp src/docgen/RenderJob.hpp src/worker_main.cpp \
        docs/openapi.yaml frontend/src/lib/api/schema.gen.ts tests/integration/test_render_job.cpp
git commit -m "feat(docgen): record the render engine version on each document version"
```

---

## Task 5: Make the render gate engine-aware and publish the rendered PDFs

**Files:**
- Modify: `scripts/render-templates.sh` (the per-fixture loop, the `main.log` block and the trailing summary)
- Modify: `scripts/check-render.py` (the margin cross-check, ~line 169 and ~line 545)
- Modify: `.github/workflows/ci.yml` (the `template-render` job's render step + a new upload step)

**Interfaces:**
- Consumes: nothing from earlier tasks (the scripts never link against C++).
- Produces: for every fixture, a PDF at `render-out/<mangled-fixture-path>/main.pdf` on the CI runner, uploaded as the artifact **`rendered-documents`**. Tasks 6-15 and 17 send the owner to that artifact for the raster review.

**Why now:** the first converted template (Task 6) deletes its `template.tex`, so no `main.log` is produced for it and the script's "no transcript is a failure" branch would fail the job. The overfull-`\hbox` grep is a **LaTeX-only tripwire** for overflow in material that emits no text (a `\hrulefill` rule, an `\hline`); it must stay mandatory for the templates that are still LaTeX and be skipped for the ones that are not. **Do not weaken `check-render.py`'s three layers, and do not touch its tolerances in this task.**

- [ ] **Step 1: Make the transcript tripwire engine-conditional**

In `scripts/render-templates.sh`, replace the `if [[ ! -f "$outdir/main.log" ]]` block and the `offenders` block that follows it with:

```bash
    # The overfull-\hbox grep is a LaTeX-ONLY tripwire, kept for overflow in
    # material that produces no extractable text (a \hrulefill rule, an
    # \hline). A Typst template writes no transcript at all — the engine exits
    # 0 and logs nothing even when it clips — so for those the gate proper
    # (check-render.py, which reads the PDF) is the only check, by design.
    if [[ -f "$version_dir/template.tex" ]]; then
        if [[ ! -f "$outdir/main.log" ]]; then
            echo "FAIL $slug $fixture: no XeLaTeX transcript at $outdir/main.log" >&2
            overall=1
            continue
        fi
        offenders="$(overfull_offenders "$outdir/main.log")"
        if [[ -n "$offenders" ]]; then
            overfull=$((overfull + 1))
            overall=1
            echo "FAIL $slug $fixture: content overhangs the page (> ${OVERFULL_MAX_PT}pt)" >&2
            echo "$offenders" >&2
        fi
    elif [[ -f "$outdir/main.log" ]]; then
        echo "FAIL $slug $fixture: $version_dir has no template.tex but the render left a" >&2
        echo "  XeLaTeX transcript — the engine selection and the tree disagree" >&2
        overall=1
        continue
    fi
```

Update the script's header comment: the tripwire now runs only for templates that still have a `template.tex`.

- [ ] **Step 2: Cross-check the declared margin for Typst too**

In `scripts/check-render.py`, next to `GEOMETRY_MARGIN_RE` (~line 169) add:

```python
# The Typst equivalent of \usepackage[margin=18mm]{geometry}: the margin in
# `#set page(..., margin: 18mm)`. Same purpose — an expected.txt that claims a
# margin the template does not set would gate against the wrong box.
TYPST_MARGIN_RE = re.compile(r"#set\s+page\((?:[^()]|\([^()]*\))*?margin:\s*([0-9.]+)mm")
```

and at the margin cross-check (~line 545-552) replace the `declared = GEOMETRY_MARGIN_RE.search(source)` line with:

```python
    # Cross-check the declared margin against the template's own page setup,
    # whichever engine it is written for.
    declared = GEOMETRY_MARGIN_RE.search(source) or TYPST_MARGIN_RE.search(source)
```

The comment above it that says "A Typst template simply has no such line" is now wrong — replace it with the two lines above.

- [ ] **Step 3: Keep the PDFs and upload them**

In `.github/workflows/ci.yml`, replace the "Render every fixture and gate the PDF (content + geometry)" step's `run:` with:

```yaml
        run: |
          mkdir -p render-out && chmod 777 render-out
          docker run --rm \
            -v "$PWD/scripts:/scripts:ro" \
            -v "$PWD/render-out:/render-out" \
            -e KEEP_RENDERS=/render-out \
            -w /app \
            --entrypoint /bin/bash \
            cyber-accountant-template:worker-render \
            /scripts/render-templates.sh
```

(`chmod 777` because the container runs as uid 1000 `appuser` and the runner's workspace is owned by a different uid.)

Then add, directly after that step:

```yaml
      - name: Upload the rendered documents for human review
        # Not decoration. The text gate cannot see a rule drawn THROUGH the
        # party names, or a signature block whose rules collapsed onto them —
        # the Typst migration spike hit exactly that, and it was visible only
        # in a raster. Every converted template gets one human look at these
        # PDFs before its task is signed off.
        if: steps.changes.outputs.docgen == 'true' && always()
        uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02 # v4
        with:
          name: rendered-documents
          path: render-out/**/main.pdf
          if-no-files-found: error
          retention-days: 14
```

- [ ] **Step 4: Verify in CI**

```bash
git push && gh run watch --exit-status
```

Expected: `template-render` PASSES with all ten templates still on LaTeX (so every one of them still goes through the overfull tripwire), and the run now carries a `rendered-documents` artifact with 22 PDFs. Download it and confirm the PDFs open:

```bash
gh run download --name rendered-documents --dir /tmp/rendered && find /tmp/rendered -name main.pdf | wc -l
```

Expected: `22`.

- [ ] **Step 5: Commit**

```bash
git add scripts/render-templates.sh scripts/check-render.py .github/workflows/ci.yml
git commit -m "ci(template-render): make the gate engine-aware and publish rendered PDFs"
```

---

## Task 6: Convert `payslip`

**Files:**
- Create: `templates/latex/payslip/v1/template.typ`
- Delete: `templates/latex/payslip/v1/template.tex`
- Modify: `templates/latex/payslip/v1/fixtures/special-chars.json` (the `period_label` payload only)
- Modify: `scripts/check-render-selftest.sh` (the `break_amounts_column` mutator)
- Reference (do not edit, do not ship): `templates/typst-spike/payslip/v1/template.typ`

**Interfaces:**
- Consumes: the engine selector (Task 2), `write_typst_inputs`/`compile_typst` (Task 3), the engine-aware gate (Task 5).
- Produces: the first template rendered by Typst in CI — this is the end-to-end proof of Tasks 1-3.

**Unchanged, and not negotiable:** `templates/latex/payslip/v1/schema.json` (API contract), `expected.txt`, and `fixtures/basic.expected.txt` with its nine `amount` directives (`gross_tenge 30000000`, `opv 3000000`, `vosms 600000`, `ipn 1342500`, `net 25057500`, `opvr 1050000`, `so 1350000`, `osms 900000`, `social_tax 1584000`). The template must print `#d.gross_tenge` etc. — the server-formatted string — and must never compute or reformat an amount.

Page setup: `margin 18mm` (from `expected.txt`), A4 portrait, `Noto Sans` at 9.2pt, `lang: "ru"`.

- [ ] **Step 1: Write `template.typ`**

The spike's `templates/typst-spike/payslip/v1/template.typ` converted this template with 154/154 tokens matching on the first attempt. Start from it and make exactly these changes: keep the comment header pointing at this plan rather than at the spike, and replace the signature line's `#box(width: 1fr, repeat[\_])` with the height-bearing rule form. The result:

```typst
// payslip v1 — Typst. Input is the schema-validated, NORMALIZED JSON the
// worker writes next to this file (src/docgen/Renderer.hpp::write_typst_inputs).
// Values are data, never source: nothing here escapes anything.
#let d = json("input.json")

#set page(paper: "a4", margin: 18mm)
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru")
#set par(justify: true)

#align(center)[
  #text(size: 14.4pt, weight: "bold")[Есеп-төлем парағы / Расчётный листок] \
  #d.period_label
]
#v(4mm)

*Жұмыс беруші / Работодатель:* #d.employer.name, БСН/БИН #d.employer.bin \
*Қызметкер / Работник:* #d.employee.full_name, ЖСН/ИИН #d.employee.iin \
*Лауазымы / Должность:* #d.employee.position

#v(4mm)

// columns: (1fr, auto) is load-bearing. A fixed width clips silently (typst
// exits 0 and writes no log); with 1fr the label column shrinks and the amount
// is never lost. Measured in the migration spike, §5.
#let money(v) = align(right, v)
#table(
  columns: (1fr, auto),
  align: (left, right),
  stroke: none,
  inset: (x: 0pt, y: 3pt),
  table.hline(stroke: 1pt),
  table.header([Көрсеткіш / Показатель], [Сомасы, ₸ / Сумма, ₸]),
  table.hline(stroke: 0.6pt),
  [Есептелген жалақы (жалпы сома) / Начислено (гросс)], money(d.gross_tenge),
  table.hline(stroke: 0.6pt),
  [Жеке табыс салығы (ИПН) / Индивидуальный подоходный налог (ИПН)], money(d.ipn),
  [Міндетті зейнетақы жарналары (ОПВ) / Обязательные пенсионные взносы (ОПВ)], money(d.opv),
  [Жұмыскердің ӘМСС жарнасы (ВОСМС) / Взнос работника на ОСМС (ВОСМС)], money(d.vosms),
  table.hline(stroke: 0.6pt),
  strong[Қолға берілетін сома / К выплате (нетто)], money(strong(d.net)),
  table.hline(stroke: 0.6pt),
  [Жұмыс беруші есебінен зейнетақы жарналары (ОПВР) / Взносы работодателя в ЕНПФ (ОПВР)], money(d.opvr),
  [Әлеуметтік аударым (СО) / Социальные отчисления (СО)], money(d.so),
  [Жұмыс берушінің ӘМСС жарнасы (ОСМС) / Взнос работодателя на ОСМС (ОСМС)], money(d.osms),
  [Әлеуметтік салық / Социальный налог], money(d.social_tax),
  table.hline(stroke: 1pt),
)

#v(3mm)
Қолға берілетін сома: #d.net ₸ \
(#d.net_words)

#v(12mm)
// A `line()` inside a grid contributes NO height: the row collapses and the
// rule is drawn through the text. A stroked box with an explicit height does
// not. Spike defect 3 — invisible to pdftotext, visible only in a raster.
#grid(
  columns: (1fr, 1fr),
  gutter: 8mm,
  [Бас бухгалтер / Гл. бухгалтер #box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))],
  [Қызметкер / Работник #box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))],
)
```

Then `git rm templates/latex/payslip/v1/template.tex`.

Every label in `expected.txt` must occur **verbatim** in this file or the gate reports `EXPECTATION ROT` — check every one of them, including `Бас бухгалтер / Гл. бухгалтер` and `Сомасы, ₸ / Сумма, ₸`.

- [ ] **Step 2: Rewrite the `special-chars` payload for Typst**

`templates/latex/payslip/v1/fixtures/special-chars.json` currently carries a LaTeX-escaping payload in `period_label`. Under Typst that asserts nothing, so it must carry a **Typst** payload. Set `period_label` to exactly:

```json
  "period_label": "50% & \"к\" #1 _t_ {б} \\б $5 ^верх ~тильда <уголки> *ж* `код` @м #panic(\"x\") #read(\"/etc/passwd\")",
```

This is the security property under test: layer 1 of the gate requires every fixture scalar to appear in the PDF, so if Typst had *executed* `#panic("x")` the compile would fail, and if it had *interpreted* `*ж*` the literal asterisks would be missing. Passing means the value was typeset character for character.

Do not touch any other value in the fixture, and do not touch `schema.json`.

- [ ] **Step 3: Rewrite the self-test's payslip breakage**

`scripts/check-render-selftest.sh`'s `break_amounts_column` empties the amounts column with a `sed` over LaTeX syntax; against `template.typ` it would match nothing, the render would be healthy, and the case would fail with "the gate PASSED a payslip that lost content". Replace the function with:

```bash
# 1. The v0.3.0 symptom, reproduced exactly: the payslip's amounts column is
#    emptied. Every money cell in the table body loses its value while the
#    column and its "Сомасы, ₸ / Сумма, ₸" header stay, so the document still
#    looks like a payslip and still compiles cleanly (typst exit 0).
#    python3, not sed: this MUST fail loudly if it matched nothing. A mutator
#    that silently changes nothing turns its case into "the gate passed a
#    healthy document", which reads as a gate failure and is not one.
break_amounts_column() {
    python3 - "$1/payslip/v1/template.typ" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
text, n1 = re.subn(r"money\(strong\(d\.[a-z_]+\)\)", "money(strong[])", text)
text, n2 = re.subn(r"money\(d\.[a-z_]+\)", "money[]", text)
if n1 + n2 < 5:
    sys.exit("break_amounts_column: only %d amount cell(s) emptied in %s — the "
             "template no longer uses the money(d.<field>) shape this case mutates" % (n1 + n2, path))
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}
```

The `run_case amounts-column payslip basic.json break_amounts_column …` invocation and its three expected substrings (`CONTENT LOST`, `amount gross_tenge = 30000000 tiyn, printed as "300 000,00"`, `amount social_tax = 1584000 tiyn, printed as "15 840,00"`) stay **exactly** as they are.

- [ ] **Step 4: Verify in CI**

```bash
git push && gh run watch --exit-status
```

Expected: `template-render` PASSES. Both payslip fixtures render through the real `typst` binary; `check-render.py` passes all three layers on both; `check-render-selftest.sh` still reports `4 deliberate breakages, all caught, all named`; the other nine templates still go through XeLaTeX and its overfull tripwire.

If the run fails with `OFF-MARGIN` findings on words **ending in a hyphen**, that is Typst hanging the hyphen into the margin by design (~1.7pt, measured in the spike) and not a real overflow. Only then, and with the number the failure actually reports, add to `scripts/check-render.py` next to `SIDE_SLACK_PT`:

```python
# Typst hangs a line-breaking hyphen into the margin by design. Measured on
# this corpus: <the worst overshoot the gate reported>pt. XeLaTeX never did
# this, which is why the tolerance is scoped to hyphen-terminated words
# rather than raised globally.
HYPHEN_SLACK_PT = 2.5
```

and use it in the geometry layer only for a word whose text ends in `-`. Do not raise `SIDE_SLACK_PT`.

- [ ] **Step 5: Human raster review (blocking)**

```bash
gh run download --name rendered-documents --dir /tmp/rendered
open /tmp/rendered/templates_latex_payslip_v1_fixtures_basic.json/main.pdf
open /tmp/rendered/templates_latex_payslip_v1_fixtures_special-chars.json/main.pdf
```

The **instance owner** opens both PDFs and confirms: the signature rules sit *below* "Бас бухгалтер / Гл. бухгалтер" and "Қызметкер / Работник" and do not strike through them; the amounts column is right-aligned and complete; the header rule, the four inner rules and the bottom rule are all drawn; the `special-chars` payload is typeset literally, with no bold and no missing characters. Report the artifact link and **stop here** — the task is not complete until that review has happened.

- [ ] **Step 6: Commit**

```bash
git add templates/latex/payslip/v1/template.typ templates/latex/payslip/v1/fixtures/special-chars.json \
        scripts/check-render-selftest.sh
git rm templates/latex/payslip/v1/template.tex
git commit -m "feat(templates): convert payslip to Typst"
```

---

## Task 7: Convert `labor_contract`

**Files:**
- Create: `templates/latex/labor_contract/v1/template.typ`
- Delete: `templates/latex/labor_contract/v1/template.tex`
- Modify: `templates/latex/labor_contract/v1/fixtures/special-chars.json` (the `work_schedule` payload only)
- Reference (do not edit, do not ship): `templates/typst-spike/labor_contract/v1/template.typ`

**Interfaces:**
- Consumes: the engine selector, the Typst render path, the engine-aware gate.
- Produces: nothing other tasks consume.

**Unchanged:** `schema.json`, `expected.txt` (margin 18mm), both `.expected.txt` files and their `amount` directives.

**This is the template that produced all three of the spike's defects — read them before writing a line:**

1. `#let w = measure[…]` outside a `context` is a hard error. `\hphantom{…}` becomes `#let phantom(l) = context h(measure(l).width)`.
2. **`] else [` split across two source lines** made Typst close the `if` and typeset the word `else` and its whole branch as literal text, into a labour contract, with exit 0 and no warning. Keep every `] else [` on one line.
3. `line(length: 100%)` in a grid contributes no height, so the signature rules were drawn **through** the party names. Use `box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))`.

- [ ] **Step 1: Write `template.typ`**

Start from `templates/typst-spike/labor_contract/v1/template.typ`, which is a complete working conversion of this template, and make exactly these changes:
- retarget the header comment at this plan instead of the spike;
- replace every signature rule with the height-bearing `box(… stroke: (bottom: 0.4pt))` form (the spike file already documents this fix — confirm it is applied, not just described);
- keep `#let phantom(label) = context h(measure(label).width)` and both `#phantom[…]` uses;
- keep the `#if d.ends_on != "" [и действует до …] else [и заключён на неопределённый срок]` lines each on **one** physical line, with the warning comment above them;
- verify the preamble is `#set page(paper: "a4", margin: 18mm)`, `#set text(font: "Noto Sans", size: 9.2pt, lang: "ru")`, `#set par(justify: true)`, and that `\emergencystretch` has no counterpart (Typst loosens the line itself).

Then `git rm templates/latex/labor_contract/v1/template.tex`.

Every label in `expected.txt` must occur verbatim in the `.typ` — including the section headings — or the gate reports `EXPECTATION ROT`.

- [ ] **Step 2: Rewrite the `special-chars` payload for Typst**

In `templates/latex/labor_contract/v1/fixtures/special-chars.json`, set `work_schedule` to exactly:

```json
  "work_schedule": "50% & \"к\" #1 _t_ {б} \\б $5 ^верх ~тильда <уголки> *ж* `код` @м #panic(\"x\") #read(\"/etc/passwd\")",
```

Change nothing else in the fixture; do not touch `schema.json`. Note that this fixture also omits `employer.address`, `employee.address`, `ends_on` and `probation_months` — that is deliberate: it is the case that proves `normalize_input` fills declared-but-absent optionals, without which Typst raises `dictionary does not contain key "address"`.

- [ ] **Step 3: Verify in CI**

```bash
git push && gh run watch --exit-status
```

Expected: `template-render` PASSES. In particular, grep the job log for the word `else` appearing in a `CONTENT LOST` or extracted-text context — the `] else [` defect surfaces as the literal word `else` in the PDF, which the gate catches only indirectly. If the raster in Step 4 shows the word `else` or the phrase `и заключён на неопределённый срок` in a contract that has an `ends_on`, that is defect 2, and the fix is to rejoin the `] else [` onto one line.

- [ ] **Step 4: Human raster review (blocking)**

```bash
gh run download --name rendered-documents --dir /tmp/rendered
open /tmp/rendered/templates_latex_labor_contract_v1_fixtures_basic.json/main.pdf
open /tmp/rendered/templates_latex_labor_contract_v1_fixtures_special-chars.json/main.pdf
```

The **instance owner** confirms: no signature rule strikes through a party name; the term clause reads as one sentence with no stray `else` and no duplicated branch; in the `special-chars` render, the two address lines are absent (not printed as empty labels) and the probation sentence is absent; the wrapped address line in `basic` is indented to align under the label above it. Report the artifact link and stop.

- [ ] **Step 5: Commit**

```bash
git add templates/latex/labor_contract/v1/template.typ \
        templates/latex/labor_contract/v1/fixtures/special-chars.json
git rm templates/latex/labor_contract/v1/template.tex
git commit -m "feat(templates): convert labor_contract to Typst"
```

---

## Task 8: Convert `invoice`

**Files:**
- Create: `templates/latex/invoice/v1/template.typ`
- Delete: `templates/latex/invoice/v1/template.tex`
- Modify: `templates/latex/invoice/v1/fixtures/special-chars.json` (the `items[].name` payload only)
- Modify: `tests/unit/test_template_registry.cpp` (`NormalizeInputTest.RenderTexOfRealInvoiceTemplateOmitsEmptyConditionalBlocks`, ~line 402)
- Modify: `tests/integration/test_render_job.cpp` (the SetUp existence check, ~line 101, and the stub env var)
- Modify: `tests/integration/test_documents_api.cpp` (the `DOCGEN_LATEX_CMD` stub, ~line 193-218)

**Interfaces:**
- Consumes: the engine selector, the Typst render path, the engine-aware gate.
- Produces: the `{% for %}` → `#table(…, ..rows.map(…).flatten())` idiom that `avr`, `waybill`, `tax_invoice` and `reconciliation` reuse.

**Unchanged:** `schema.json`, `expected.txt` (margin 18mm), both `.expected.txt` files and their `amount` directives.

`invoice` is the template the C++ test suite renders. It is the first conversion that moves tests off the LaTeX stub, which is why it comes before the other loop templates.

- [ ] **Step 1: Write `template.typ`**

Read `templates/latex/invoice/v1/template.tex` first — it is 36 lines. Convert it with the crib sheet in Global Constraints. The constructs it uses and their targets:

- `\usepackage[margin=18mm]{geometry}` → `#set page(paper: "a4", margin: 18mm)`;
- `\usepackage{fancyhdr}\pagestyle{empty}` → nothing (Typst pages have no header/footer unless `#set page(header: …)` asks for one);
- `\newcommand{\field}[1]{\textbf{#1}}` and every `\field{X}` → `*X*`;
- the items table: `#table(columns: (auto, 1fr, auto, auto, auto), …)` with the data rows spliced in as
  ```typst
  ..d.items.map(it => (it.no, it.name, it.qty, it.price, it.amount)).flatten(),
  ```
  using this template's actual `items[]` property names, read from `schema.json`. **Never give the name column or an amount column a fixed width** — `1fr` for the description, `auto` for the amounts;
- every `{% if X != "" %}…{% endif %}` → `#if d.X != "" […]`, each `] else [` (if any) on one line;
- `\hrulefill` on any signature line → `#box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))`.

Preamble: `#set page(paper: "a4", margin: 18mm)`, `#set text(font: "Noto Sans", size: 9.2pt, lang: "ru")`, `#set par(justify: true)`. Header comment as in the other templates. Then `git rm templates/latex/invoice/v1/template.tex`.

Every label in `expected.txt` must occur verbatim in the `.typ`.

- [ ] **Step 2: Rewrite the `special-chars` payload for Typst**

In `templates/latex/invoice/v1/fixtures/special-chars.json`, the payload lives in an `items[].name`. Set that item's `name` to exactly:

```json
      "name": "50% & \"к\" #1 _t_ {б} \\б $5 ^верх ~тильда <уголки> *ж* `код` @м #panic(\"x\") #read(\"/etc/passwd\")",
```

Change nothing else; do not touch `schema.json`. If the gate then reports an `OFF-MARGIN` finding on a word of this payload, the fix is to insert a space inside the payload (it must stay one fixture value and must stay printable) — **never** to widen the template's margin or the column.

- [ ] **Step 3: Move the C++ tests onto the Typst stub**

Three test files render `invoice` through a stubbed engine and will break the moment its `.tex` is gone:

1. `tests/unit/test_template_registry.cpp`, `NormalizeInputTest.RenderTexOfRealInvoiceTemplateOmitsEmptyConditionalBlocks` (~line 402): it guards on `fs::exists("templates/latex/invoice/v1/template.tex")` and calls `render_tex` on the real template. `render_tex` is inja-only and this template is no longer inja. **Delete this test** and put a comment where it was:
   ```cpp
   // RenderTexOfRealInvoiceTemplateOmitsEmptyConditionalBlocks was deleted with
   // the invoice's LaTeX source: the property it pinned (an omitted optional
   // field must not leave an empty label behind) is now enforced where it is
   // observable — scripts/check-render.py over invoice/v1/fixtures/*.json,
   // which reads the rendered PDF rather than the intermediate source.
   ```
   Leave `NormalizeInputTest.FillsMissingOptionalFieldsOfRealInvoiceSchema` alone: it reads `schema.json`, which does not change.

2. `tests/integration/test_render_job.cpp` (~line 101): change the SetUp guard from `templates/latex/invoice/v1/template.tex` to `templates/latex/invoice/v1/template.typ`, and change `use_succeeding_latex_stub()` / `use_failing_latex_stub()` to point `DOCGEN_TYPST_CMD` (not `DOCGEN_LATEX_CMD`) at their stub scripts. Rename the helpers to `use_succeeding_typst_stub()` / `use_failing_typst_stub()` and the script files to `fake-typst-ok.sh` / `fake-typst-fail.sh`; update every call site in the file. The succeeding stub must write `main.pdf` in the directory it is invoked from, exactly as the LaTeX one does. Update `RecordsTheEngineVersionOnTheRenderedVersion` (Task 4) to expect the stub's `--version` output rather than `"xelatex"`: make the stub print `typst 0.15.1` when its first argument is `--version`, and assert `EXPECT_EQ(*version->render_engine, "typst 0.15.1")`.

3. `tests/integration/test_documents_api.cpp` (~line 193-218): the same stub, same rename — `DOCGEN_LATEX_CMD` → `DOCGEN_TYPST_CMD` — and the `fs::exists("templates/latex/invoice/v1/schema.json")` guards at ~line 282 are fine as they are (schemas do not move).

- [ ] **Step 4: Verify in CI**

```bash
make fmt && git push && gh run watch --exit-status
```

Expected: `build-and-test` PASSES (the render-job and documents-api suites now drive the Typst path through a stub), `template-render` PASSES (invoice's two fixtures render through the real `typst`; the remaining seven LaTeX templates still go through the overfull tripwire).

- [ ] **Step 5: Human raster review (blocking)**

```bash
gh run download --name rendered-documents --dir /tmp/rendered
open /tmp/rendered/templates_latex_invoice_v1_fixtures_basic.json/main.pdf
open /tmp/rendered/templates_latex_invoice_v1_fixtures_special-chars.json/main.pdf
```

The **instance owner** confirms: the items table has all its columns, its rules are drawn and none crosses text; every line item from the fixture is present with its amount; the totals row is intact; no signature rule strikes through a name. Report the artifact link and stop.

- [ ] **Step 6: Commit**

```bash
git add templates/latex/invoice/v1/template.typ templates/latex/invoice/v1/fixtures/special-chars.json \
        tests/unit/test_template_registry.cpp tests/integration/test_render_job.cpp \
        tests/integration/test_documents_api.cpp
git rm templates/latex/invoice/v1/template.tex
git commit -m "feat(templates): convert invoice to Typst"
```

---

## Task 9: Convert `avr`

**Files:**
- Create: `templates/latex/avr/v1/template.typ`
- Delete: `templates/latex/avr/v1/template.tex`
- Modify: `templates/latex/avr/v1/fixtures/special-chars.json` (the `items[].name` payload only)

**Interfaces:** consumes the engine selector, the Typst render path and the engine-aware gate. Produces nothing other tasks consume.

**Unchanged:** `schema.json`, `expected.txt` (margin 18mm), both `.expected.txt` files and their `amount` directives.

- [ ] **Step 1: Write `template.typ`**

Read `templates/latex/avr/v1/template.tex` (44 lines) and convert it. Preamble:

```typst
// avr v1 — Typst. Input is the schema-validated, NORMALIZED JSON the worker
// writes next to this file. Values are data, never source.
#let d = json("input.json")

#set page(paper: "a4", margin: 18mm)
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru")
#set par(justify: true)
```

Construct mapping for what this template uses: `\field{X}`/`\textbf{X}` → `*X*`; `{{ x }}` → `#d.x`; `{% if X != "" %}…{% endif %}` → `#if d.X != "" […]` (any `] else [` on one line); `\toprule`/`\midrule`/`\bottomrule` → `table.hline(stroke: 1pt)`/`(0.6pt)`/`(1pt)`; `\hrulefill` on a signature line → `#box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))`; `\vspace{Nmm}` → `#v(Nmm)`; `\\` → a trailing ` \`.

The one `{% for %}` loop becomes data spliced into the table, using this template's own `items[]` property names from `schema.json`:

```typst
#table(
  columns: (auto, 1fr, auto, auto, auto),   // description 1fr, amounts auto — never a fixed mm width
  align: (left, left, right, right, right),
  stroke: none,
  inset: (x: 2pt, y: 3pt),
  table.hline(stroke: 1pt),
  table.header(/* the header cells, verbatim from expected.txt */),
  table.hline(stroke: 0.6pt),
  ..d.items.map(it => (it.no, it.name, it.qty, it.price, it.amount)).flatten(),
  table.hline(stroke: 1pt),
)
```

Then `git rm templates/latex/avr/v1/template.tex`. Every label in `expected.txt` must occur verbatim in the `.typ`.

- [ ] **Step 2: Rewrite the `special-chars` payload for Typst**

In `templates/latex/avr/v1/fixtures/special-chars.json`, set the `items[].name` that carries the payload to exactly:

```json
      "name": "50% & \"к\" #1 _t_ {б} \\б $5 ^верх ~тильда <уголки> *ж* `код` @м #panic(\"x\") #read(\"/etc/passwd\")",
```

Nothing else changes; `schema.json` is untouched.

- [ ] **Step 3: Verify in CI**

```bash
git push && gh run watch --exit-status
```

Expected: `template-render` PASSES; `avr`'s two fixtures render through `typst`, the remaining LaTeX templates are unaffected.

- [ ] **Step 4: Human raster review (blocking)**

```bash
gh run download --name rendered-documents --dir /tmp/rendered
open /tmp/rendered/templates_latex_avr_v1_fixtures_basic.json/main.pdf
open /tmp/rendered/templates_latex_avr_v1_fixtures_special-chars.json/main.pdf
```

The **instance owner** confirms: every line item is present with its amount; the table rules are drawn and none crosses text; the signature rules sit below the names, not through them; the acceptance wording is intact. Report the artifact link and stop.

- [ ] **Step 5: Commit**

```bash
git add templates/latex/avr/v1/template.typ templates/latex/avr/v1/fixtures/special-chars.json
git rm templates/latex/avr/v1/template.tex
git commit -m "feat(templates): convert avr to Typst"
```

---

## Task 10: Convert `waybill`

**Files:**
- Create: `templates/latex/waybill/v1/template.typ`
- Delete: `templates/latex/waybill/v1/template.tex`
- Modify: `templates/latex/waybill/v1/fixtures/special-chars.json` (the `items[].name` payload only)

**Interfaces:** consumes the engine selector, the Typst render path and the engine-aware gate. Produces nothing other tasks consume.

**Unchanged:** `schema.json`, `expected.txt` (**margin 14mm**), both `.expected.txt` files and their `amount` directives.

**This template is LANDSCAPE** (`\usepackage[margin=14mm,landscape]{geometry}`).

- [ ] **Step 1: Write `template.typ`**

Read `templates/latex/waybill/v1/template.tex` (39 lines) and convert it. Preamble — note `flipped: true` and the 14mm margin, which must match `expected.txt`'s `margin 14mm` or `check-render.py` refuses to run:

```typst
// waybill v1 — Typst. Input is the schema-validated, NORMALIZED JSON the
// worker writes next to this file. Values are data, never source.
#let d = json("input.json")

#set page(paper: "a4", flipped: true, margin: 14mm)
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru")
#set par(justify: true)
```

Construct mapping: `\field{X}`/`\textbf{X}` → `*X*`; `{{ x }}` → `#d.x`; `{% if X != "" %}…{% endif %}` → `#if d.X != "" […]` (any `] else [` on one line); `\toprule`/`\midrule`/`\bottomrule` → `table.hline(stroke: 1pt)`/`(0.6pt)`/`(1pt)`; `\hrulefill` → `#box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))`; `\vspace{Nmm}` → `#v(Nmm)`; `\\` → a trailing ` \`.

The one `{% for %}` loop becomes `..d.items.map(it => (…)).flatten()` spliced into `#table(…)`, with the item property names read from `schema.json`, the description column `1fr` and every amount column `auto`. **No fixed `mm` column width anywhere** — that is the one shape Typst clips silently.

Then `git rm templates/latex/waybill/v1/template.tex`. Every label in `expected.txt` must occur verbatim in the `.typ`.

- [ ] **Step 2: Rewrite the `special-chars` payload for Typst**

In `templates/latex/waybill/v1/fixtures/special-chars.json`, set the `items[].name` that carries the payload to exactly:

```json
      "name": "50% & \"к\" #1 _t_ {б} \\б $5 ^верх ~тильда <уголки> *ж* `код` @м #panic(\"x\") #read(\"/etc/passwd\")",
```

Nothing else changes; `schema.json` is untouched.

- [ ] **Step 3: Verify in CI**

```bash
git push && gh run watch --exit-status
```

Expected: `template-render` PASSES. If the geometry layer reports the page is portrait-shaped, `flipped: true` is missing.

- [ ] **Step 4: Human raster review (blocking)**

```bash
gh run download --name rendered-documents --dir /tmp/rendered
open /tmp/rendered/templates_latex_waybill_v1_fixtures_basic.json/main.pdf
open /tmp/rendered/templates_latex_waybill_v1_fixtures_special-chars.json/main.pdf
```

The **instance owner** confirms: the page is landscape; every line item and every amount is present; no column is cut at the right edge; the "отпустил / получил" signature rules sit below the names. Report the artifact link and stop.

- [ ] **Step 5: Commit**

```bash
git add templates/latex/waybill/v1/template.typ templates/latex/waybill/v1/fixtures/special-chars.json
git rm templates/latex/waybill/v1/template.tex
git commit -m "feat(templates): convert waybill to Typst"
```

---

## Task 11: Convert `tax_invoice`

**Files:**
- Create: `templates/latex/tax_invoice/v1/template.typ`
- Delete: `templates/latex/tax_invoice/v1/template.tex`
- Modify: `templates/latex/tax_invoice/v1/fixtures/special-chars.json` (the `items[].name` payload only)
- Modify: `scripts/check-render-selftest.sh` (the `break_over_wide_table` mutator)

**Interfaces:** consumes the engine selector, the Typst render path and the engine-aware gate. Produces nothing other tasks consume.

**Unchanged:** `schema.json`, `expected.txt` (**margin 12mm**), both `.expected.txt` files and their `amount` directives. Note `items[].vat_rate` carries `"16%"` — a percentage, not an amount; it stays a plain string.

**This template is LANDSCAPE** (`\usepackage[margin=12mm,landscape]{geometry}`) and has the widest table in the repo (nine columns).

- [ ] **Step 1: Write `template.typ`**

Read `templates/latex/tax_invoice/v1/template.tex` (38 lines) and convert it. Preamble:

```typst
// tax_invoice v1 — Typst. Input is the schema-validated, NORMALIZED JSON the
// worker writes next to this file. Values are data, never source.
#let d = json("input.json")

#set page(paper: "a4", flipped: true, margin: 12mm)
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru")
#set par(justify: true)
```

Construct mapping: `\field{X}`/`\textbf{X}` → `*X*`; `{{ x }}` → `#d.x`; `{% if X != "" %}…{% endif %}` → `#if d.X != "" […]` (any `] else [` on one line); `\toprule`/`\midrule`/`\bottomrule` → `table.hline(stroke: 1pt)`/`(0.6pt)`/`(1pt)`; `\hrulefill` → `#box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))`; `\vspace{Nmm}` → `#v(Nmm)`; `\\` → a trailing ` \`.

The nine-column table: `columns: (auto, 1fr, auto, auto, auto, auto, auto, auto, auto)` with the description column as the only `1fr`, and the rows spliced in as `..d.items.map(it => (…)).flatten()` using the property names from `schema.json`. With 12mm margins on a landscape A4 this is the tightest layout in the corpus — if the geometry layer complains, shrink `inset` (e.g. `inset: (x: 1.5pt, y: 2.5pt)`) or the header text size, **never** the margin, and never a fixed column width.

Then `git rm templates/latex/tax_invoice/v1/template.tex`. Every label in `expected.txt` must occur verbatim in the `.typ` — including `Ставка НДС`, which the self-test's third case asserts on.

- [ ] **Step 2: Rewrite the `special-chars` payload for Typst**

In `templates/latex/tax_invoice/v1/fixtures/special-chars.json`, set the `items[].name` that carries the payload to exactly:

```json
      "name": "50% & \"к\" #1 _t_ {б} \\б $5 ^верх ~тильда <уголки> *ж* `код` @м #panic(\"x\") #read(\"/etc/passwd\")",
```

Leave `items[].vat_rate` (`"16%"`) alone; nothing else changes; `schema.json` is untouched.

- [ ] **Step 3: Rewrite the self-test's tax_invoice breakage**

`scripts/check-render-selftest.sh`'s `break_over_wide_table` widens a `tabularx` to `1.25\textwidth`; against a `.typ` it matches nothing. The Typst equivalent of "walk the right-hand columns off the page" is a fixed, oversized column set — which is exactly the shape the spike proved Typst clips **silently, exit 0**. Replace the function with:

```bash
# 3. Content pushed past the right margin: the tax invoice's nine-column table
#    is given fixed, oversized columns instead of the (1fr, auto…) set, which
#    walks its VAT columns off the right edge. Typst does this SILENTLY —
#    exit 0, no warning, no log at all — which is precisely why the gate reads
#    the PDF and not the engine's opinion of it.
break_over_wide_table() {
    local typ="$1/tax_invoice/v1/template.typ"
    python3 - "$typ" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
new, n = re.subn(r"columns:\s*\([^)]*\)", "columns: (60mm, 60mm, 60mm, 60mm, 60mm, 60mm, 60mm, 60mm, 60mm)",
                 text, count=1)
if n != 1:
    sys.exit("break_over_wide_table: no `columns: (...)` found in %s" % path)
with open(path, "w", encoding="utf-8") as fh:
    fh.write(new)
PY
}
```

The `run_case over-wide-table tax_invoice basic.json break_over_wide_table 'static label "Ставка НДС"' 'OFF-MARGIN' 'crosses the RIGHT margin'` invocation stays **exactly** as it is. The mutator's own `sys.exit` on a failed substitution is deliberate: a self-test case that silently mutates nothing is worse than no case.

- [ ] **Step 4: Verify in CI**

```bash
git push && gh run watch --exit-status
```

Expected: `template-render` PASSES, and `check-render-selftest.sh` still reports `4 deliberate breakages, all caught, all named` — with the third case now proving the gate catches Typst's *silent* clipping, which has no transcript at all.

- [ ] **Step 5: Human raster review (blocking)**

```bash
gh run download --name rendered-documents --dir /tmp/rendered
open /tmp/rendered/templates_latex_tax_invoice_v1_fixtures_basic.json/main.pdf
open /tmp/rendered/templates_latex_tax_invoice_v1_fixtures_special-chars.json/main.pdf
```

The **instance owner** confirms: the page is landscape; all nine columns are on the paper with none cut at the right edge; the VAT rate and VAT amount columns are present and legible; no rule crosses text. Report the artifact link and stop.

- [ ] **Step 6: Commit**

```bash
git add templates/latex/tax_invoice/v1/template.typ \
        templates/latex/tax_invoice/v1/fixtures/special-chars.json scripts/check-render-selftest.sh
git rm templates/latex/tax_invoice/v1/template.tex
git commit -m "feat(templates): convert tax_invoice to Typst"
```

---

## Task 12: Convert `fno_910`

**Files:**
- Create: `templates/latex/fno_910/v1/template.typ`
- Delete: `templates/latex/fno_910/v1/template.tex`
- Modify: `templates/latex/fno_910/v1/fixtures/special-chars.json` (the `director` payload only)
- Modify: `scripts/check-render-selftest.sh` (the `break_truncated_amount` mutator)

**Interfaces:** consumes the engine selector, the Typst render path and the engine-aware gate. Produces nothing other tasks consume.

**Unchanged:** `schema.json`, `expected.txt` (margin 18mm), both `.expected.txt` files and their `amount` directives — including `amount income_tenge 1000000000` (`10 000 000,00`), which the self-test's second case asserts on.

**This is a filed tax form.** It prints the human money form (`10 000 000,00`) that the server put in the input. The ФНО **XML** filed to the tax authority is produced by a completely different path (`FnoXml::tenge_amount`, whole tenge) and is not touched by this task in any way.

- [ ] **Step 1: Write `template.typ`**

Read `templates/latex/fno_910/v1/template.tex` (37 lines) and convert it. Preamble:

```typst
// fno_910 v1 — Typst. A readable printed form of the simplified declaration,
// not a facsimile of the КГД blank. Input is the schema-validated, NORMALIZED
// JSON the worker writes next to this file. Values are data, never source.
// The XML actually FILED with the tax authority is produced elsewhere
// (FnoXml, whole tenge) and has nothing to do with this template.
#let d = json("input.json")

#set page(paper: "a4", margin: 18mm)
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru")
#set par(justify: true)
```

Construct mapping: the multi-line centred title (`\begin{center}\Large\bfseries … \\ (форма 910.00) \\ …`) → `#align(center)[#text(size: 14.4pt, weight: "bold")[…] \ … ]`; `\field{X}` → `*X*`; `{{ x }}` → `#d.x`; `{% if period.half == "1" %}…{% endif %}` → `#if d.period.half == "1" […]` — identical semantics, and `half` stays `unprinted` in `expected.txt` (it is an `enum`-pinned control value; `ShippedTemplatesTest.NeverPrintAnEnumPinnedField` fails the build if it is printed); the amounts table → `#table(columns: (1fr, auto), …)` with `table.hline` rules; `\hrulefill` → `#box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))`.

Then `git rm templates/latex/fno_910/v1/template.tex`. Every label in `expected.txt` must occur verbatim in the `.typ`.

- [ ] **Step 2: Rewrite the `special-chars` payload for Typst**

In `templates/latex/fno_910/v1/fixtures/special-chars.json`, set `director` to exactly:

```json
  "director": "100% & \"итог\" #3 _над_ {скобка} \\слэш $7 ^степень ~волна <знак> *ж* `код` @м #panic(\"x\") #read(\"/etc/passwd\")",
```

Nothing else changes; `schema.json` is untouched.

- [ ] **Step 3: Rewrite the self-test's fno_910 breakage**

`scripts/check-render-selftest.sh`'s `break_truncated_amount` replaces the inja expression `{{ income_tenge }}` with the literal `10 000`. Under Typst the expression is `#d.income_tenge`. Replace the function with:

```bash
# 2. One amount silently truncated: ФНО 910.00's income line prints a literal
#    `10 000` instead of the fixture's 10 000 000,00. This is the exact defect
#    that was printed and filed in v0.3.0, and it is the case that proves
#    amounts are compared in the form they are PRINTED — `10 000` is a prefix
#    of `10 000 000,00`, so a sloppy check would pass it.
#    The reference may be written `#d.income_tenge` (markup position) or
#    `d.income_tenge` (inside a table cell, already code position), so both are
#    matched — and the mutator fails loudly if it matched neither, because a
#    mutator that changes nothing turns its case into "the gate passed a
#    healthy document".
break_truncated_amount() {
    python3 - "$1/fno_910/v1/template.typ" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
# `[10 000]` and not a bare `10 000`: in a code position a bare number is code,
# not text. A content block prints the literal either way.
text, n = re.subn(r"#?d\.income_tenge", "[10 000]", text)
if n < 1:
    sys.exit("break_truncated_amount: no d.income_tenge reference in %s" % path)
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}
```

The `run_case truncated-amount fno_910 basic.json break_truncated_amount 'CONTENT LOST' 'amount income_tenge = 1000000000 tiyn, printed as "10 000 000,00"'` invocation stays **exactly** as it is.

- [ ] **Step 4: Verify in CI**

```bash
git push && gh run watch --exit-status
```

Expected: `template-render` PASSES and `check-render-selftest.sh` reports `4 deliberate breakages, all caught, all named`.

- [ ] **Step 5: Human raster review (blocking)**

```bash
gh run download --name rendered-documents --dir /tmp/rendered
open /tmp/rendered/templates_latex_fno_910_v1_fixtures_basic.json/main.pdf
open /tmp/rendered/templates_latex_fno_910_v1_fixtures_special-chars.json/main.pdf
```

The **instance owner** confirms: the two-line title is intact; the half-year clause reads correctly and shows no stray `else` or identifier; every amount is in the `10 000 000,00` form with a space thousands separator and a decimal comma; the signature rule sits below the director's name. Report the artifact link and stop.

- [ ] **Step 6: Commit**

```bash
git add templates/latex/fno_910/v1/template.typ templates/latex/fno_910/v1/fixtures/special-chars.json \
        scripts/check-render-selftest.sh
git rm templates/latex/fno_910/v1/template.tex
git commit -m "feat(templates): convert fno_910 to Typst"
```

---

## Task 13: Convert `fno_300`

**Files:**
- Create: `templates/latex/fno_300/v1/template.typ`
- Delete: `templates/latex/fno_300/v1/template.tex`
- Modify: `templates/latex/fno_300/v1/fixtures/special-chars.json` (the `director` payload only)
- Modify: `tests/unit/test_template_registry.cpp` (`ShippedTemplatesTest.Fno300PrintsItsClosingLineForBothBalanceKinds`, ~line 698)

**Interfaces:** consumes the engine selector, the Typst render path and the engine-aware gate. Produces nothing other tasks consume.

**Unchanged:** `schema.json`, `expected.txt` (margin 18mm), and both per-fixture expectation files, including `basic.expected.txt`'s `Сумма НДС, подлежащая уплате в бюджет:` label and its four `amount` directives (`sales_tenge 500000000`, `vat_charged_tenge 80000000`, `vat_credited_tenge 35000000`, `balance_tenge 45000000`).

**The two branches are already covered by fixtures:** `basic.json` has `"balance_kind": "to_pay"` and `special-chars.json` has `"balance_kind": "to_refund"`, each with its own `.expected.txt` naming the closing label that branch prints. `balance_kind` is `enum`-pinned: branch on it, never print it, and keep it declared `unprinted` in `expected.txt`.

**Do not touch `scripts/check-render-selftest.sh` in this task.** Its fourth case, `break_machine_money_form`, is the only one that breaks the **fixture** rather than the template: it rewrites every amount of `fno_300/v1/fixtures/basic.json` into the machine money form and requires the gate to refuse it at layer 0, before a pixel is compared. That mutator is a Python rewrite of JSON and is entirely engine-agnostic — it works identically against a Typst template and must stay byte-for-byte as it is. (The other three cases mutate templates and were rewritten in the payslip, tax_invoice and fno_910 tasks.)

- [ ] **Step 1: Write `template.typ`**

Read `templates/latex/fno_300/v1/template.tex` (44 lines) and convert it. Preamble:

```typst
// fno_300 v1 — Typst. A readable printed form of the VAT declaration, not a
// facsimile of the КГД blank. Input is the schema-validated, NORMALIZED JSON
// the worker writes next to this file. Values are data, never source. The XML
// actually FILED with the tax authority is produced elsewhere (FnoXml, whole
// tenge) and has nothing to do with this template.
#let d = json("input.json")

#set page(paper: "a4", margin: 18mm)
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru")
#set par(justify: true)
```

Construct mapping: the multi-line centred title → `#align(center)[#text(size: 14.4pt, weight: "bold")[…] \ … ]`; `\field{X}` → `*X*`; `{{ x }}` → `#d.x`; the amounts table → `#table(columns: (1fr, auto), …)` with `table.hline` rules; `\hrulefill` → `#box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))`.

The closing block branches on `balance_kind`. Write it as a single `#if`/`else` with **`] else [` on one physical line**:

```typst
#if d.balance_kind == "to_pay" [Сумма НДС, подлежащая уплате в бюджет: #d.balance_tenge ₸] else [/* the to_refund sentence, verbatim from template.tex */ #d.balance_tenge ₸]
```

Both closing sentences and the `balance_words` line must appear exactly as the LaTeX template printed them — this block is the one that shipped empty for every ФНО 300.00 in an earlier release, and the labels are asserted per fixture.

Then `git rm templates/latex/fno_300/v1/template.tex`. Every label in `expected.txt` must occur verbatim in the `.typ`.

- [ ] **Step 2: Rewrite the `special-chars` payload for Typst**

In `templates/latex/fno_300/v1/fixtures/special-chars.json`, set `director` to exactly:

```json
  "director": "100% & \"итог\" #3 _над_ {скобка} \\слэш $7 ^степень ~волна <знак> *ж* `код` @м #panic(\"x\") #read(\"/etc/passwd\")",
```

Leave `"balance_kind": "to_refund"` alone — it selects the second branch, and that is the whole point of this fixture now. Nothing else changes; `schema.json` is untouched.

- [ ] **Step 3: Retire the source-level branch test**

`tests/unit/test_template_registry.cpp`'s `ShippedTemplatesTest.Fno300PrintsItsClosingLineForBothBalanceKinds` calls `render_tex` on the real fno_300 template and asserts on the rendered **source**. Typst has no rendered source — the branch is taken inside the engine — so the property is only observable in the PDF. Delete the test and leave this comment in its place:

```cpp
// Fno300PrintsItsClosingLineForBothBalanceKinds was deleted with the ФНО
// 300.00 LaTeX source: under Typst the branch is taken inside the engine, so
// there is no intermediate source to assert on. The property did NOT go
// unguarded — it moved to scripts/check-render.py, which renders
// fno_300/v1/fixtures/basic.json (balance_kind "to_pay") and
// special-chars.json ("to_refund") and requires each fixture's own
// .expected.txt closing label to be present in the PDF.
```

Do not delete `ShippedTemplatesTest.NeverPrintAnEnumPinnedField` — Task 2 made it engine-aware and it still applies.

- [ ] **Step 4: Verify in CI**

```bash
make fmt && git push && gh run watch --exit-status
```

Expected: `build-and-test` PASSES; `template-render` PASSES with both branches exercised — `basic` must contain `Сумма НДС, подлежащая уплате в бюджет:` and `special-chars` its own closing label.

- [ ] **Step 5: Human raster review (blocking)**

```bash
gh run download --name rendered-documents --dir /tmp/rendered
open /tmp/rendered/templates_latex_fno_300_v1_fixtures_basic.json/main.pdf
open /tmp/rendered/templates_latex_fno_300_v1_fixtures_special-chars.json/main.pdf
```

The **instance owner** confirms: each PDF has exactly ONE closing sentence (`basic` the "to pay" one, `special-chars` the "to refund" one) — not both, not neither, and no stray `else`, `to_pay` or `to_refund` anywhere on the page; the amount-in-words line is present; every amount is in the `450 000,00` form and none reads `450000.00`. Report the artifact link and stop.

- [ ] **Step 6: Commit**

```bash
git add templates/latex/fno_300/v1/template.typ templates/latex/fno_300/v1/fixtures/special-chars.json \
        tests/unit/test_template_registry.cpp
git rm templates/latex/fno_300/v1/template.tex
git commit -m "feat(templates): convert fno_300 to Typst"
```

---

## Task 14: Convert `hr_order`

**Files:**
- Create: `templates/latex/hr_order/v1/template.typ`
- Delete: `templates/latex/hr_order/v1/template.tex`
- Modify: `templates/latex/hr_order/v1/fixtures/special-chars.json` (the `reason` payload only)

**Interfaces:** consumes the engine selector, the Typst render path and the engine-aware gate. Produces nothing other tasks consume.

**Unchanged:** `schema.json`, `expected.txt` (margin 18mm), and all **four** fixtures with their expectation files: `basic.json`, `business_trip.json`, `salary_change.json`, `special-chars.json`. This template has the most fixtures in the repo because `kind` selects a different body for each.

`kind` is `enum`-pinned: branch on it, never print it, keep it `unprinted` in `expected.txt`. The `business_trip` and `salary_change` bodies both shipped **empty** in an earlier release; each fixture's own `.expected.txt` names the labels its branch prints, and those are the assertions that catch a regression.

- [ ] **Step 1: Write `template.typ`**

Read `templates/latex/hr_order/v1/template.tex` (52 lines) and convert it. Preamble:

```typst
// hr_order v1 — Typst. Input is the schema-validated, NORMALIZED JSON the
// worker writes next to this file. Values are data, never source.
#let d = json("input.json")

#set page(paper: "a4", margin: 18mm)
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru")
#set par(justify: true)
```

Construct mapping: the two-line centred title (`Бұйрық № {{ number }} / Приказ № {{ number }}` over `{{ issued_on }} ж. / от {{ issued_on }}`) → `#align(center)[#text(size: 14.4pt, weight: "bold")[Бұйрық № #d.number / Приказ № #d.number] \ #d.issued_on ж. / от #d.issued_on]`; `\field{X}` → `*X*`; `{{ x }}` → `#d.x`; `\hrulefill` → `#box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))`.

The `kind` branches — write them as a chain and keep every `] else [` on **one** physical line:

```typst
#if d.kind == "hire" [/* the hire body, verbatim */] else if d.kind == "business_trip" [/* … */] else if d.kind == "salary_change" [/* … */] else [/* the remaining kind's body */]
```

Take the exact set of `kind` values from `schema.json`'s `enum` — do not guess it from the fixture names. Then `git rm templates/latex/hr_order/v1/template.tex`. Every label in `expected.txt` **and** in each of the four `fixtures/*.expected.txt` must occur verbatim in the `.typ`.

- [ ] **Step 2: Rewrite the `special-chars` payload for Typst**

In `templates/latex/hr_order/v1/fixtures/special-chars.json`, set `reason` to exactly:

```json
  "reason": "50% & \"к\" #1 _t_ {б} \\б $5 ^верх ~тильда <уголки> *ж* `код` @м #panic(\"x\") #read(\"/etc/passwd\")",
```

Leave its `kind` alone; nothing else changes; `schema.json` is untouched.

- [ ] **Step 3: Verify in CI**

```bash
git push && gh run watch --exit-status
```

Expected: `template-render` PASSES over all **four** hr_order fixtures, each matching its own branch's labels.

- [ ] **Step 4: Human raster review (blocking)**

```bash
gh run download --name rendered-documents --dir /tmp/rendered
for f in basic business_trip salary_change special-chars; do
  open "/tmp/rendered/templates_latex_hr_order_v1_fixtures_$f.json/main.pdf"
done
```

The **instance owner** opens all four and confirms: each order has a body — none is an empty order with only a heading and a signature line; each body is the RIGHT one for its `kind`; no `kind` identifier (`hire`, `business_trip`, `salary_change`) appears anywhere on the page; no stray `else`; the signature rules sit below the names. Report the artifact link and stop.

- [ ] **Step 5: Commit**

```bash
git add templates/latex/hr_order/v1/template.typ templates/latex/hr_order/v1/fixtures/special-chars.json
git rm templates/latex/hr_order/v1/template.tex
git commit -m "feat(templates): convert hr_order to Typst"
```

---

## Task 15: Convert `reconciliation`

**Files:**
- Create: `templates/latex/reconciliation/v1/template.typ`
- Delete: `templates/latex/reconciliation/v1/template.tex`
- Modify: `templates/latex/reconciliation/v1/fixtures/special-chars.json` (the `rows[].doc` payload only)

**Interfaces:** consumes the engine selector, the Typst render path and the engine-aware gate. Produces nothing other tasks consume.

**Unchanged:** `schema.json`, `expected.txt` (**margin 16mm**), both `.expected.txt` files and their `amount` directives — including the array-path ones (`amount rows[].a_debit <tiyn>` style), which the gate compares as multisets.

**The hardest remaining template** (spike §8): six columns, a two-party spanning header, two `\cmidrule`s, an optional opening-balance row, a `{% for %}` body, and a two-column signature block. It also has cells that are legitimately **empty strings** (`"a_debit": ""`), which the gate skips — the template must print them as empty cells, not as the word `none`.

- [ ] **Step 1: Write `template.typ`**

Read `templates/latex/reconciliation/v1/template.tex` (38 lines) first. Preamble:

```typst
// reconciliation v1 — Typst. Input is the schema-validated, NORMALIZED JSON
// the worker writes next to this file. Values are data, never source.
#let d = json("input.json")

#set page(paper: "a4", margin: 16mm)
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru")
#set par(justify: true)

// \hphantom{\field{Сторона А:} } — indent a wrapped address line by the exact
// width of the label above it. `measure` needs a context; without it Typst
// errors with "can only be used when context is known".
#let phantom(label) = context h(measure(label).width)
```

The party blocks:

```typst
*Сторона А:* #d.party_a.name, БИН/ИИН #d.party_a.identifier \
#if d.party_a.address != "" [#phantom[*Сторона А:* ]#d.party_a.address \ ]
```

and the same for `party_b`. (`\par`/`\noindent` have no counterpart — a Typst table is block-level and cannot be typeset inline, so the whole "tables start their own paragraph" discipline disappears.)

The six-column table — this is where `\multicolumn` and `\cmidrule` land, with **0-based** hline indices:

```typst
#table(
  columns: (auto, 1fr, auto, auto, auto, auto),   // Документ is the only 1fr
  align: (left, left, right, right, right, right),
  stroke: none,
  inset: (x: 2pt, y: 3pt),
  table.hline(stroke: 1pt),                                   // \toprule
  [], [],
  table.cell(colspan: 2, align: center)[По данным #d.party_a.name],
  table.cell(colspan: 2, align: center)[По данным #d.party_b.name],
  table.hline(start: 2, end: 4, stroke: 0.4pt),               // \cmidrule(lr){3-4}
  table.hline(start: 4, end: 6, stroke: 0.4pt),               // \cmidrule(lr){5-6}
  [Дата], [Документ], [Дебет, ₸], [Кредит, ₸], [Дебет, ₸], [Кредит, ₸],
  table.hline(stroke: 0.6pt),                                 // \midrule
  ..(if d.opening_balance.a_debit != "" or d.opening_balance.a_credit != "" {
      (table.cell(colspan: 2)[Сальдо на начало периода],
       d.opening_balance.a_debit, d.opening_balance.a_credit, [], [])
    } else { () }),
  ..d.rows.map(r => (r.date, r.doc, r.a_debit, r.a_credit, r.b_debit, r.b_credit)).flatten(),
  table.hline(stroke: 1pt),                                   // \bottomrule
)
```

Then the closing lines (`#d.closing.a_says`, `#d.closing.b_says`) and the two-party signature block, using the height-bearing rule form — `line()` in a grid contributes no height and would draw the rules **through** the party names:

```typst
#v(12mm)
#grid(
  columns: (1fr, 1fr),
  gutter: 8mm,
  [От #d.party_a.name: #box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))],
  [От #d.party_b.name: #box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))],
)
```

Then `git rm templates/latex/reconciliation/v1/template.tex`. Every label in `expected.txt` must occur verbatim in the `.typ`, including `Сальдо на начало периода` and both `Дебет, ₸` / `Кредит, ₸` headers.

- [ ] **Step 2: Rewrite the `special-chars` payload for Typst**

In `templates/latex/reconciliation/v1/fixtures/special-chars.json`, set the `rows[].doc` that carries the payload to exactly:

```json
      "doc": "50% & \"к\" #1 _t_ {б} \\б $5 ^верх ~тильда <уголки> *ж* `код` @м #panic(\"x\") #read(\"/etc/passwd\")",
```

Nothing else changes; `schema.json` is untouched. If the payload's length pushes a word past the right margin, add a space inside the payload — never widen the margin or fix a column width.

- [ ] **Step 3: Verify in CI**

```bash
git push && gh run watch --exit-status
```

Expected: `template-render` PASSES. This is the last template, so **every** fixture in the repo now renders through Typst and `scripts/render-templates.sh`'s LaTeX tripwire no longer fires for anything.

- [ ] **Step 4: Human raster review (blocking)**

```bash
gh run download --name rendered-documents --dir /tmp/rendered
open /tmp/rendered/templates_latex_reconciliation_v1_fixtures_basic.json/main.pdf
open /tmp/rendered/templates_latex_reconciliation_v1_fixtures_special-chars.json/main.pdf
```

The **instance owner** confirms: the two "По данным …" headers each span exactly two columns and sit over the right pair; the two short rules under them cover columns 3-4 and 5-6 and nothing else; the opening-balance row appears in the fixture that declares one and is absent (not blank-labelled) in the one that does not; empty cells are empty — no `none`, no `""`; every row of the fixture is on the page; the two signature rules sit below the party names, not through them. Report the artifact link and stop.

- [ ] **Step 5: Commit**

```bash
git add templates/latex/reconciliation/v1/template.typ \
        templates/latex/reconciliation/v1/fixtures/special-chars.json
git rm templates/latex/reconciliation/v1/template.tex
git commit -m "feat(templates): convert reconciliation to Typst"
```

---

## Task 16: Delete inja, LaTeX escaping and the XeLaTeX code path

**Files:**
- Delete: `src/docgen/LatexEscape.hpp`, `tests/unit/test_latex_escape.cpp`
- Modify: `src/docgen/Renderer.hpp` (delete `render_tex`, `detail::escape_tree`, `detail::is_control_literal`, `detail::k_no_schema`, the inja include and the whole file header)
- Modify: `src/docgen/TemplateRegistry.hpp` (drop `Engine::kLatex` and the `template.tex` discovery branch)
- Modify: `src/docgen/RenderJob.hpp` (delete `latex_cmd()`, `compile_pdf()`, the LaTeX branch of `render_and_compile`, the `Engine` switch in `engine_version`)
- Modify: `tests/unit/test_template_registry.cpp` (delete the `RenderTex*` and escaping tests, and the LaTeX-engine registry tests)
- Modify: `scripts/render-templates.sh` (delete `overfull_offenders`, `OVERFULL_MAX_PT`, the transcript block and the overfull summary)
- Modify: `config/worker.json`, `docs/CONFIG.md` (drop `docgen.latex_cmd` / `DOCGEN_LATEX_CMD`)

**Interfaces:**
- Consumes: `Docgen::write_typst_inputs`, `Docgen::compile_typst`, `Docgen::typst_cmd`, `Docgen::engine_version` (Task 3-4).
- Produces: `Docgen::Engine` collapses to a single value. Keep the enum and `engine_name()` — `document_versions.render_engine` and the registry's discovery both still describe an engine, and a future engine change should not have to reinvent the concept. `TemplateInfo::engine` stays, always `Engine::kTypst`.

**Do not remove `inja` from `vcpkg.json`.** `src/email/Templates.hpp` renders e-mail templates with it and is not part of this migration. Only docgen stops using inja.

- [ ] **Step 1: Confirm no LaTeX source remains**

```bash
find templates -name 'template.tex' -o -name '*.tex' | tee /tmp/tex-left
test ! -s /tmp/tex-left
```

Expected: empty output, exit 0. If anything is listed, stop — a template was missed and this task must not proceed.

- [ ] **Step 2: Delete the escaping layer**

```bash
git rm src/docgen/LatexEscape.hpp tests/unit/test_latex_escape.cpp
```

(`tests/unit/*.cpp` is globbed by `CMakeLists.txt` with `CONFIGURE_DEPENDS`, so no build file changes.)

Rewrite `src/docgen/Renderer.hpp` down to `write_typst_inputs` alone. Delete `#include <inja/inja.hpp>` and `#include "docgen/LatexEscape.hpp"`, the whole `namespace detail` block, and `render_tex`. Replace the file header with:

```cpp
/**
 * @file Renderer.hpp
 * @brief Stage a Typst render: the template plus the normalized input JSON,
 *        written side by side into a scratch directory.
 *
 * There is no templating layer and nothing to escape. Typst opens the data
 * itself (`#let d = json("input.json")`), so a tenant-supplied value is
 * content and never source: the literal string
 * `#panic("x") *bold* #read("/etc/passwd")` is typeset character for
 * character (verified in .superpowers/sdd/typst-migration-spike.md §3 and
 * pinned by templates/docs/*/v1/fixtures/special-chars.json, whose values
 * carry exactly that payload and must appear verbatim in the rendered PDF).
 *
 * This replaced an inja + escape_latex pipeline whose two failure modes are
 * worth remembering, because the constructor phase must not reintroduce
 * them: escaping BEFORE templating made a template's own control flow run
 * against escaped data (`{% if balance_kind == "to_pay" %}` tested
 * `to\_pay`, and every ФНО 300.00 silently lost its closing line), and
 * escaping AFTER it mangled the template's own markup. Under Typst the
 * question does not arise — data never reaches a parser.
 *
 * `TemplateRegistry::normalize_input` is MORE load-bearing than it was:
 * Typst raises a hard error on a declared-but-absent optional key where inja
 * merely printed nothing.
 */
```

- [ ] **Step 3: Collapse the engine selection**

`src/docgen/TemplateRegistry.hpp`, in `load()`:

```cpp
        info.source_path = info.dir / "template.typ";
        info.engine = Engine::kTypst;
        if (!fs::exists(info.source_path))
            throw std::runtime_error("template registry: missing template.typ for " + slug + "/" + version_str);
```

Keep `enum class Engine { kTypst };` and `engine_name()` (returning `"typst"`), and delete `kLatex`. Update the file header's layout comment to `template.typ`.

`src/docgen/RenderJob.hpp`: delete `latex_cmd()` and `compile_pdf()`; `render_and_compile` keeps only the Typst branch; `engine_version` loses its `kLatex` early return. Update the file header — it currently describes inja, XeLaTeX and a two-pass compile.

- [ ] **Step 4: Prune the tests that tested LaTeX**

In `tests/unit/test_template_registry.cpp` delete: `RenderTexSubstitutesVariables`, `RenderTexAutoEscapesSpecialCharactersInBody`, `RenderTexLeavesNumbersAndBooleansUnescaped`, `RenderTexDoesNotMutateInput`, `RenderTexCoexistsWithLatexMacroParameters`, `RenderTexCommentMarkersAreStripped`, the escaping-vs-templating block that follows them, `LatexTemplateReportsXelatexEngine` and `TypstWinsWhenBothSourcesExist` (Task 2 — both describe a choice that no longer exists), and the `write_template` helper if nothing still calls it. Keep `TypstTemplateReportsTypstEngine`, `MissingBothSourcesThrows` (rename to `MissingTemplateTypThrows`), every `NormalizeInputTest`, both `WriteTypstInputs*` tests and `ShippedTemplatesTest.NeverPrintAnEnumPinnedField`. In that last test's `printed_expressions` helper, delete the inja regex and keep the Typst one.

Leave `tests/unit/test_money_format.cpp` alone — `MoneyFormatRu.MatchesEveryAmountDirective` walks expectation files, not templates, and must keep passing.

- [ ] **Step 5: Drop the transcript tripwire and the LaTeX config key**

In `scripts/render-templates.sh` delete `OVERFULL_MAX_PT`, `overfull_offenders()`, the `if [[ -f "$version_dir/template.tex" ]]` block Task 5 added, the `overfull` counter and its summary paragraph. Rewrite the header comment: there is no transcript to grep — Typst clips with exit 0 and writes no log — so `scripts/check-render.py` is the only gate, which is exactly why it reads the PDF.

Remove `"latex_cmd": "${DOCGEN_LATEX_CMD:-xelatex}"` from `config/worker.json` and the `DOCGEN_LATEX_CMD` row from `docs/CONFIG.md`.

- [ ] **Step 6: Verify in CI**

```bash
make fmt && git push && gh run watch --exit-status
```

Expected: `build-and-test` PASSES, `lint-format` PASSES, `template-render` PASSES with all 22 fixtures and `check-render-selftest.sh` still reporting `4 deliberate breakages, all caught, all named`. Also confirm nothing still references the deleted symbols:

```bash
grep -rn "escape_latex\|LatexEscape\|render_tex\|DOCGEN_LATEX_CMD\|OVERFULL_MAX_PT" src tests scripts config docs .github
```

Expected: no hits outside `docs/superpowers/plans/` and `CHANGELOG.md`. `grep -rn "inja" src` must still hit `src/email/Templates.hpp` and nothing under `src/docgen/`.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "refactor(docgen): drop inja, LaTeX escaping and the XeLaTeX code path"
```

---

## Task 17: Remove TeX Live from the worker image

**Files:**
- Modify: `docker/Dockerfile` (the `worker-runtime` stage's TeX Live `RUN apt-get install`, ~line 205-236)
- Modify: `.github/workflows/ci.yml` (the `template-render` job: the version assertion and the job comment/timeout)

**Interfaces:** consumes the Typst-only render path (Task 16). Produces the migration's headline saving.

**This is the point of no return** — after it, reverting a template to LaTeX means putting 496.7 MiB of TeX Live back. It does not start until the raster sweep below is done.

- [ ] **Step 1: Raster sweep over all ten templates (blocking, human)**

```bash
gh run download --name rendered-documents --dir /tmp/sweep
find /tmp/sweep -name main.pdf | sort
```

Expected: 22 PDFs. The **instance owner** opens all 22, in one sitting, and checks each against the list in this plan's "Sequencing" section: no rule through text, no collapsed signature block, no column cut at the page edge, no heading merged into its paragraph, no stray English word or identifier. This is the second look — the per-template reviews caught what they caught; this one catches what only shows up when ten documents are compared side by side. Record the sign-off in the commit message of this task.

- [ ] **Step 2: Remove the TeX Live packages**

In `docker/Dockerfile`'s `worker-runtime` stage, delete the entire comment block and `RUN apt-get install` that installs `texlive-xetex`, `texlive-latex-recommended`, `texlive-latex-extra`, `texlive-lang-cyrillic` and `fonts-noto-core`, and replace it with a font-only install placed **before** the Typst block:

```dockerfile
# Fonts for the docgen render job. `fonts-noto-core` ships NotoSans-Regular/
# Bold, which every template selects with `#set text(font: "Noto Sans")` and
# which covers Cyrillic AND Kazakh (ә ғ қ ң ө ұ ү һ і) plus ₸ and №. Typst
# discovers system fonts itself; no --font-path is needed.
#
# TeX Live used to live here: 68 packages, 154.2 MiB downloaded, 496.7 MiB
# installed, and — transitively, for a service that uses neither — a full Perl
# and libpdfbox-java in Trivy's scan surface. Measured in
# .superpowers/sdd/typst-migration-spike.md §4. Typst plus these fonts is
# 95.9 MiB, an 81% cut, and ~137 MiB less to push to GHCR per release and
# pull per deploy. (The spike also measured a 54.0 MiB variant that ships only
# the two Noto faces via --font-path; not taken, because it makes "which faces
# may a template use" a standing decision for every future template.)
RUN apt-get update && apt-get install -y --no-install-recommends \
    fonts-noto-core \
    && rm -rf /var/lib/apt/lists/*
```

- [ ] **Step 3: Update the CI job's comments and version assertion**

In `.github/workflows/ci.yml`'s `template-render` job: the version-assertion step's name loses the mention of TeX Live (`Assert the pinned engine version`); the "Build worker image" step's name becomes `Build worker image (worker-render-check target — Typst + poppler)`; the job's leading comment block, which describes XeLaTeX warnings and `Overfull \hbox`, is rewritten to describe Typst's silent clipping and the three gate layers. Lower `timeout-minutes: 45` to `30` — the comment justifying 45 says the ceiling was raised because the image carries TeX Live.

- [ ] **Step 4: Verify in CI, and record the size**

```bash
git push && gh run watch --exit-status
```

Expected: `template-render` PASSES within the 30-minute ceiling. Add the measurement the spike could not make — a step at the end of the job, after the render steps:

```yaml
      - name: Report the worker image size
        if: steps.changes.outputs.docgen == 'true'
        run: docker image inspect cyber-accountant-template:worker-render --format '{{.Size}}'
```

Read the number from the run log and put it in the commit message.

- [ ] **Step 5: Commit**

```bash
git add docker/Dockerfile .github/workflows/ci.yml
git commit -m "build(worker): drop TeX Live, Typst is the only document engine

Raster sweep over all 22 fixture renders signed off by the instance owner.
worker-render-check image size after: <bytes from step 4>."
```

---

## Task 18: Rename the template tree, rewrite the docs

**Files:**
- Rename: `templates/latex/` → `templates/docs/` (all ten template directories and their contents)
- Rename + rewrite: `templates/latex/README.md` → `templates/docs/README.md`
- Delete: `templates/typst-spike/`
- Modify: `src/docgen/TemplateRegistry.hpp` (the default constructor's root, and every comment naming the path)
- Modify: `scripts/render-templates.sh`, `scripts/check-render-selftest.sh`, `scripts/check-render.py` (the `TEMPLATES_ROOT` defaults and the usage examples)
- Modify: `tests/unit/test_template_registry.cpp`, `tests/unit/test_money_format.cpp`, `tests/integration/test_render_job.cpp`, `tests/integration/test_docgen_api.cpp`, `tests/integration/test_documents_api.cpp` (the `templates/latex/...` path literals)
- Modify: `src/api/TaxController.hpp`, `src/api/PayrollController.hpp`, `src/api/HrController.hpp`, `src/docgen/InputPolicy.hpp` (comments only)
- Modify: `frontend/src/lib/schemas/hr.ts`, `frontend/src/lib/schemas/documents.ts`, `frontend/src/lib/money.ts` (comments only)
- Modify: `.github/workflows/ci.yml` (the `docgen` paths filter — `templates/**` already covers the rename, but the comments name `templates/latex`)
- Modify: `CLAUDE.md`, `CHANGELOG.md`

**Interfaces:** consumes everything. Produces the final on-disk layout: `templates/docs/<slug>/v<N>/{template.typ,schema.json,expected.txt,fixtures/}`.

`templates/latex/` holding no LaTeX is a lie that every future reader has to decode. The rename is mechanical and the gates catch a missed reference immediately.

- [ ] **Step 1: Move the tree**

```bash
git mv templates/latex templates/docs
git rm -r templates/typst-spike
```

- [ ] **Step 2: Update every reference**

```bash
grep -rln 'templates/latex' src tests scripts docs frontend/src .github CLAUDE.md Makefile config
```

Change each hit to `templates/docs`, **except** files under `docs/superpowers/plans/` and `docs/superpowers/specs/` — those are historical records of what was true when they were written and must not be rewritten. The `Makefile`'s `clean-docs` target mentions `docs/latex`: that is **Doxygen output**, not the template tree, and must not be touched. The functional ones are: `src/docgen/TemplateRegistry.hpp`'s `explicit TemplateRegistry(std::filesystem::path root = "templates/latex")` → `"templates/docs"`; `scripts/render-templates.sh`'s `TEMPLATES_ROOT="${TEMPLATES_ROOT:-templates/latex}"`; `scripts/check-render-selftest.sh`'s same default; `scripts/check-render.py`'s usage example; and the `fs::exists("templates/latex/...")` guards and `recursive_directory_iterator("templates/latex")` in the five test files.

- [ ] **Step 3: Rewrite `templates/docs/README.md`**

The file is 252 lines and roughly half of it documents LaTeX traps that no longer exist. Rewrite it around the Typst reality, keeping the sections that are still true:

- **Layout**: `templates/docs/<slug>/v<N>/` with `template.typ`, `schema.json`, `expected.txt`, `fixtures/`. `TemplateRegistry::latest(slug)` picks the highest `vN`; ship a new version rather than editing one in place.
- **Writing a template**: the crib sheet from this plan's Global Constraints — the `#let d = json("input.json")` preamble, the `#set page`/`#set text`/`#set par` block, `#d.x`, `#if d.X != "" […]`, `..d.rows.map(…).flatten()` in a `#table`, `table.hline`, `table.cell(colspan:)`, `context h(measure(l).width)`.
- **The three rules that cost the spike a day**: `] else [` on one line; `box(… stroke: (bottom: …))` instead of `line()` in a grid; never a fixed `mm` width on a data column.
- **Values are data, never code** — replace the entire escaping section. No `escape_latex`, no escaping at all: Typst reads the JSON, so a value is printed verbatim. `special-chars.json` fixtures carry Typst payloads (`#panic(…)`, `*ж*`, backticks, `@`) and the gate requiring them to appear verbatim in the PDF is what pins the property.
- **`normalize_input` is load-bearing**: Typst hard-errors on a declared-but-absent optional key. Never write `d.x.at("y", default: "")` in a template — that moves the schema contract into the template body.
- **Branch on `enum` fields, never print one** — keep this section but drop the escaping rationale; the reason is now simply that a control identifier is not something a reader should see, and `ShippedTemplatesTest.NeverPrintAnEnumPinnedField` still enforces it.
- **DELETE the whole "Tables start their own paragraph" section** — a Typst table is block-level, there is no `\par` to forget, and the failure mode is structurally impossible.
- **The render gate**: keep the three-layer description and the "Amounts are declared as integers" section **verbatim** — none of it changed — but note that the engine writes no log at all, so the PDF is the only evidence there is.
- **Testing a template**: `tests/unit/test_template_registry.cpp` covers discovery/validation/staging without an engine; the real `typst` runs only in the `template-render` CI job; every render is uploaded as the `rendered-documents` artifact and **every new or changed template gets one human look at the raster** — the text gate cannot see a rule drawn through a name.
- **Engine version**: pinned in `docker/Dockerfile` (`TYPST_VERSION`), asserted in CI, recorded per render in `document_versions.render_engine`. Typst is pre-1.0; an engine bump is a `v<N+1>` template event.

- [ ] **Step 4: Update CLAUDE.md**

- Invariant 9: `templates/latex/<slug>/v<N>/` → `templates/docs/<slug>/v<N>/`.
- Invariant 10: the parenthetical `templates/latex/README.md` → `templates/docs/README.md`. Everything else in it — printed money is `Money::format_tiyn_ru`, stored/served/filed money is `Ledger::format_tiyn`, the ФНО XML uses `FnoXml::tenge_amount`, an amount is declared `amount <path> <tiyn>` and never written out — is unchanged and must stay word for word.
- The gate-sequence section: `template-render` now renders `templates/docs/*/v*/fixtures/*.json` through Typst; step 1's "A nonzero XeLaTeX exit fails the job; so does an overfull `\hbox`" becomes "A nonzero `typst compile` exit fails the job — and nothing else does, because Typst clips silently with exit 0 and no log, which is why steps 2 and 3 read the PDF"; steps 2 and 3 are unchanged.
- The trigger list: `src/docgen/**`, `docker/Dockerfile` and the three gate scripts are unchanged; `templates/**` already covers the rename.

- [ ] **Step 5: Add the CHANGELOG entry**

Under the unreleased heading, in the style the file already uses:

```markdown
### Changed

- **Document generation moved from LaTeX/XeLaTeX to Typst.** All ten templates
  (`invoice`, `avr`, `waybill`, `tax_invoice`, `payslip`, `fno_910`, `fno_300`,
  `labor_contract`, `hr_order`, `reconciliation`) are now `template.typ` under
  `templates/docs/`. The worker image drops 68 TeX Live packages (496.7 MiB
  installed, and with them a transitive Perl and `libpdfbox-java`) for one
  pinned 53 MiB Typst binary; per-document render time falls from ~520 ms to
  ~55 ms. `schema.json` files are unchanged — they are the API contract.
- **Template inputs are no longer escaped, because they are no longer code.**
  Typst reads the render input from `input.json` directly, so inja and
  `Docgen::escape_latex` are deleted rather than ported: a value containing
  `#panic(…) *bold* #read("/etc/passwd")` is typeset verbatim.
- `document_versions.render_engine` records the engine (e.g. `typst 0.15.1`)
  that produced each PDF, alongside `template_version`. Typst is pre-1.0 and
  the binary version is pinned in `docker/Dockerfile`.
```

- [ ] **Step 6: Verify in CI**

```bash
make fmt && git push && gh run watch --exit-status
```

Expected: every job PASSES — `build-and-test`, `template-render` (22 fixtures + the four self-test breakages), `lint-format`, `openapi-drift`, `frontend`. Then confirm the old name is gone from live code:

```bash
grep -rn 'templates/latex' src tests scripts frontend/src .github CLAUDE.md Makefile config
```

Expected: no hits.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "docs(templates): rename templates/latex to templates/docs and rewrite the guide"
```

---

## Done when

- All ten templates are `templates/docs/<slug>/v1/template.typ`; no `.tex` file exists in the repo.
- `template-render` renders 22 fixtures through Typst 0.15.1 and gates all three layers of `check-render.py`; `check-render-selftest.sh` still reports `4 deliberate breakages, all caught, all named`.
- The instance owner has looked at every rendered page twice — once per template, once as a sweep.
- The worker image carries one pinned engine and no TeX Live; its size is recorded in the commit that removed it.
- `document_versions.render_engine` is populated on every new render.
- `schema.json`, `Money::format_tiyn_ru`, `Money::to_words_ru`/`to_words_kk` and `FnoXml::tenge_amount` are byte-identical to where this plan started.
