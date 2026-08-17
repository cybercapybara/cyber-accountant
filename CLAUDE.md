# CLAUDE.md — agent guide for this repo

C++20 REST service template: Drogon + PostgreSQL + Redis, vcpkg/CMake,
React SPA in `frontend/`, Helm charts in `helm/`. `docs/INDEX.md` is the
map of all documentation; `docs/CONVENTIONS.md` is the pattern reference.

## Prime directive: scaffold, don't hand-roll

- Full CRUD resource: `./scripts/new-resource.sh Name` — migration + DTO +
  repository + controller + registry row + OpenAPI block + integration test.
- Single endpoint: `./scripts/new-endpoint.sh FooController Get /api/v1/foo
  [--with-test] [--patch-openapi]`
- Background job: `./scripts/new-job.sh <type>`
- Migration: `make new-migration SLUG=<slug>`
- React page: `./scripts/new-react-page.sh`

The generators encode the invariants below — their output passes the CI
gates by construction. Hand-rolled versions usually don't.

## Invariants the CI gates enforce

1. **Route triple-sync:** every `ADD_METHOD_TO` in a controller must also
   appear in `Api::get_endpoints()` (`src/api/Endpoints.hpp`) **and** in
   `docs/openapi.yaml`. `scripts/check-openapi-drift.sh` and
   `scripts/check-routes-registered.sh` fail CI on any mismatch.
2. **API versioning (ADR 0006):** business routes live under `/api/v1`;
   `new-endpoint.sh` rejects unversioned paths. Probe routes (`/healthz`,
   `/ready`, `/health`, `/metrics`) stay unversioned.
3. **Header-only src/ (ADR 0003):** implementation lives in `.hpp`; don't
   add `.cpp` files except the existing binary entry points.
4. **One error shape:** `{error, status, message, ...}` everywhere — use
   `ErrorResponse::*` / `Api::Validation::*`, never hand-rolled error JSON.
5. **Test buckets by directory** (`scripts/check-test-buckets.sh`):
   `tests/unit` (no services), `tests/integration` (real Postgres/Redis),
   `tests/api` (controller via `TestHelpers::make_request`), `tests/e2e`
   (real HTTP server, separate binary).
6. **Migrations:** `migrations/NNN_slug.sql`, sequential numbering, no
   `BEGIN`/`COMMIT` (the runner wraps them; use the
   `-- migrate:no-transaction` marker for `CREATE INDEX CONCURRENTLY`).
7. **No secrets in tracked files:** `config/config.json` holds `${VAR}`
   placeholders, env overrides everything (`docs/CONFIG.md` is the full
   table). gitleaks gates CI; `make prod-check` gates the prod profile.
8. **Commits:** conventional commits, no AI-attribution trailers.
9. **Document templates carry their own expectations:** every
   `templates/docs/<slug>/v<N>/` needs an `expected.txt` listing the static
   labels it prints and its `margin <N>mm`, plus optional per-fixture
   `fixtures/<name>.expected.txt`. `scripts/check-render.py` fails if it is
   missing — a template with no declared expectations cannot be gated. It
   also needs a `fixtures/special-chars.json` whose data carries the hostile
   payload: `#panic(`, `#read(`, `*`, `` ` ``, `@`, plus the bytes the
   deleted LaTeX escaper used to rewrite (`% & # $ _ { } \ ^ ~`), which stay
   on the list because the requirement was never "the escaper handles them"
   but "the engine PRINTS them". That fixture is what makes `template-render`
   an injection guard, and
   `ShippedTemplatesTest.EveryTemplateShipsAHostileSpecialCharsFixture`
   (`tests/unit/test_template_registry.cpp`) fails if one goes bland.
10. **Printed money is `Money::format_tiyn_ru`; stored/served/filed money is
   `Ledger::format_tiyn`.** The rule is by DESTINATION, not by module. Every
   `input` handed to a docgen template — the ФНО forms (TaxController), the
   payslip (PayrollController), the labour contract (HrController), the
   первичка (`Docgen::InputPolicy`) — carries `12 345,67`. `journal_lines
   .amount`, API responses and the ФНО XML (which uses neither formatter:
   `FnoXml::tenge_amount`, whole tenge) keep `1234.56`. In a fixture a printed
   amount is never written out at all — it is declared `amount <path> <tiyn>`
   and derived (invariant 9, and `templates/docs/README.md`).

## Gate sequence — run cheapest-first before pushing

0. **Check your clang-format version first.** CI pins **clang-format 17**
   (`.github/workflows/ci.yml`), and a different major version reformats code
   CI then rejects — running `make fmt` with clang-format 22 rewrote 18
   unrelated files and cost three red runs. If `clang-format --version` is not
   17, do NOT run `make fmt`; get the exact version from a wheel instead and
   format only the files you touched:

   ```
   python3 -m venv /tmp/cf17 && /tmp/cf17/bin/pip install -q clang-format==17.0.6
   /tmp/cf17/bin/clang-format -i <your files>
   git ls-files | grep -E '^(src|tests)/.*\.(hpp|cpp)$' | xargs /tmp/cf17/bin/clang-format --dry-run --Werror
   ```

1. `make fmt` — clang-format in place (only when your version is 17)
2. `./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh
   && ./scripts/check-test-buckets.sh` — seconds, no build
3. `make lint-openapi` — spectral over `docs/openapi.yaml`
4. `make test-quick` — cached test image, ~5 s
5. `make test` — full rebuild + suite, ~2 min; what CI runs
6. `make helm-lint` — only if `helm/` was touched
7. `make ci-local` — full local reproduction of CI

CI additionally runs clang-tidy, ASan+UBSan (+TSAN), gitleaks, Trivy,
helm-render and the OpenAPI-drift gate. `template-render` renders every
template fixture on the worker image (`worker-render-check` target — the
production worker image plus `pdftotext`/`python3`) and then **gates the PDF
it produced**. It first asserts the pinned engine (see "Document engines"
below), then runs three steps:

1. `scripts/render-templates.sh` — compiles every
   `templates/docs/*/v*/fixtures/*.json` through the worker's
   `--render-template` mode, i.e. the real render pipeline. A nonzero engine
   exit fails the job, and that is **all** it can catch: Typst clips silently,
   exits 0 and writes no transcript, so there is nothing to grep. The
   overfull-`\hbox` tripwire went with the last `template.tex` and was not
   replaced — step 2 reads the PDF instead, which is why it exists.
2. `scripts/check-render.py` — the gate proper, engine-agnostic, run per
   fixture. **Oracle:** every printed amount is DERIVED from an integer
   declared `amount <path> <tiyn>` in the per-fixture expectation file, by the
   same algorithm as `Money::format_tiyn_ru`; a money-shaped fixture string
   with no directive, or one in the machine form (`450000.00`), fails. A
   fixture may not hand-write the printed form of an amount — that is how
   v0.4.2 printed `450000.00` on a filed ФНО 300.00 with the gate green.
   **Content:** every scalar in the fixture and every label in `expected.txt`
   must appear in the PDF's extracted text; a `*_tiyn` integer is checked as
   the money string it must have been formatted into (`1234567` →
   `12 345,67`), never as a raw integer, and an amount that survives only
   broken across a line break counts as lost. **Geometry:** every word box
   from `pdftotext -bbox` must lie inside the declared margin box (0.5pt of
   slack sideways, 6.0pt vertically for font ascent). The `margin <N>mm` in
   `expected.txt` is cross-checked against the template's own page setup
   (`#set page(..., margin: 18mm)`). **Syntax:** no extracted token may look
   like template syntax (a Typst `#` sigil, a content-block bracket, a bare
   `else`/`endif`; inja delimiters and LaTeX control sequences are still
   screened so a stray one cannot creep back in) unless the
   fixture or a declared label contains it — the `special-chars` fixtures
   ship `#panic("x")` and `#read("/etc/passwd")` as DATA and must keep
   passing, so the test is provenance, not appearance. **Ink:** the page is
   rasterised (`pdftoppm`, 200dpi) and no word may have a rule drawn through
   it — a solid band spanning the word box and past both sides, with the
   word's own ink above AND below it in the same pixel columns. An underline,
   a `\midrule` or a signature line has ink on one side only; measured over
   all 23 fixtures, zero findings.
3. `scripts/check-render-selftest.sh` — breaks payslip, fno_910, tax_invoice,
   fno_300 and labor_contract on purpose and fails unless the gate catches all
   six and names what went wrong. The fno_300 case breaks the FIXTURE, not the
   template, and is the regression test for the gate's own fixture-as-oracle
   blind spot; the two labor_contract cases (`] else [` split across lines so
   the branch is typeset as body text, and the signature rules turned back
   into a zero-height `line()`) are the regression tests for the two blind
   spots that were measured PASSING at exit 0 before the syntax and ink
   layers existed. **A mutation that changes nothing is itself a failure:**
   a mutator that edits engine-specific syntax silently no-ops against a
   template converted to the other engine, so the script fingerprints the
   template tree around every mutator and fails loudly ("changed NOTHING")
   rather than re-testing the healthy document and reporting OK. That guard
   fired for real when the migration retired the last `template.tex`:
   `break_truncated_amount` and `break_over_wide_table` were still `sed`ing a
   `{{ }}` placeholder and a `tabularx` width, and both were ported to the
   Typst source. **All six mutators are now python3 and every one counts its
   own substitutions and exits nonzero on a miscount** — keep both defences
   when adding a case, and never replace a mutator with one that cannot fail
   loudly. The tax-invoice mutator's `44mm` column width is MEASURED, not
   arbitrary: it is the width that reproduces both halves of the original
   LaTeX defect (columns 7-9 off the paper => `CONTENT LOST`, column 6 in the
   band between margin and paper edge => `OFF-MARGIN`). A wider set pushes
   everything clean off the paper, `pdftotext` sees no word, and the geometry
   layer reports nothing.

The job keeps every rendered PDF and uploads it as the **`rendered-documents`**
artifact (`render-out/<mangled-fixture-path>/main.pdf`, 14-day retention), so
every template conversion can still get one human look at the raster:
`gh run download --name rendered-documents`. The two defects that used to make
that review load-bearing — a rule drawn *through* the text, a control construct
typeset as body text — are now gated by layers 3 and 4 above.

It triggers on changes under `templates/**`, `src/docgen/**`,
`docker/Dockerfile` or the three gate scripts.

## Document engines

**Typst, pinned to 0.15.1**, is the only document engine. All ten templates are
`template.typ`; the XeLaTeX render path, the inja templating layer and
`escape_latex` were deleted with the last `template.tex`, and **TeX Live is no
longer installed anywhere**. The worker image (`docker/Dockerfile`, stage
`worker-runtime`) is the only image carrying an engine, and it now carries one
53 MiB static binary plus `fonts-noto-core` where it used to carry ~62
TeX packages and ~417 MiB (measured from the noble apt closure; the built
image is measured by the `Report the worker image size` step of
`template-render`). Reverting a template to LaTeX is no longer a template
change — it means putting that layer back.

- **Data is never code.** A Typst template opens its own input
  (`#let d = json("input.json")`), which `write_typst_inputs` writes beside the
  copied template, so a tenant-supplied value is content and cannot be parsed
  as source. That is why there is no escaping layer and why adding one would be
  a regression, not a hardening: escaping BEFORE templating is what shipped ФНО
  300.00 forms without their closing line and hr_order orders with no body at
  all. Every template's `special-chars` fixture pins the property through the
  real engine in CI.
- **Which engine a template uses is still a property of its directory, not a
  list anywhere.** `TemplateRegistry::load()` decides it per version directory
  (`template.typ` ⇒ Typst) and `engine_name()` produces the string stored on
  the document version. `Docgen::Engine` has one value today and the enum stays
  — `document_versions.render_engine` still holds `xelatex` on rows rendered
  before the migration, and a second engine must be a change to `load()` rather
  than to every call site.
- The Typst pin is not decorative. Typst is pre-1.0 and every minor release
  changes layout, so the version is a build arg (`TYPST_VERSION`), the
  download is checksum-verified against `TYPST_SHA256`, and `template-render`
  fails if `typst --version` on the built image is not 0.15.1. Bumping it is a
  deliberate act with a template-version consequence, never a drive-by.
- Typst ships no fonts. It finds **Noto Sans** — the family with the Kazakh
  glyph coverage (ә ғ қ ң ө ұ ү һ і) — from `fonts-noto-core` in
  `/usr/share/fonts`, by scanning that directory (no fontconfig, no
  `--font-path`). `fonts-noto-core` was installed next to TeX Live but was
  never a TeX package: it **stayed** when TeX Live went, and removing it as
  part of some later "TeX cleanup" would blank the Cyrillic in every document.
  The same CI step asserts `typst fonts` still lists Noto Sans, which is what
  would catch that.

## Don'ts

- Don't edit the `builtin-baseline` in `vcpkg.json` or `ARG VCPKG_REF` in
  `docker/Dockerfile` by hand — Renovate owns them, and a baseline bump
  rebuilds the entire dependency world.
- Don't weaken `config/config.production.json` — `make prod-check` gates it.
- Don't change the error-response shape without updating `docs/openapi.yaml`.

## Self-maintenance

When a PR adds or changes a CI gate, scaffolding script, or invariant,
update this file in the same PR.
