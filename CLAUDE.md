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
   `templates/latex/<slug>/v<N>/` needs an `expected.txt` listing the static
   labels it prints and its `margin <N>mm`, plus optional per-fixture
   `fixtures/<name>.expected.txt`. `scripts/check-render.py` fails if it is
   missing — a template with no declared expectations cannot be gated.
10. **Printed money is `Money::format_tiyn_ru`; stored/served/filed money is
   `Ledger::format_tiyn`.** The rule is by DESTINATION, not by module. Every
   `input` handed to a docgen template — the ФНО forms (TaxController), the
   payslip (PayrollController), the labour contract (HrController), the
   первичка (`Docgen::InputPolicy`) — carries `12 345,67`. `journal_lines
   .amount`, API responses and the ФНО XML (which uses neither formatter:
   `FnoXml::tenge_amount`, whole tenge) keep `1234.56`. In a fixture a printed
   amount is never written out at all — it is declared `amount <path> <tiyn>`
   and derived (invariant 9, and `templates/latex/README.md`).

## Gate sequence — run cheapest-first before pushing

1. `make fmt` — clang-format in place
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
   `templates/latex/*/v*/fixtures/*.json` through the worker's
   `--render-template` mode, i.e. the real render pipeline. A nonzero XeLaTeX
   exit fails the job; so does an overfull `\hbox` over `OVERFULL_MAX_PT`
   (1.0pt), kept as a LaTeX-only tripwire for overflow in material that
   produces no text (a `\hrulefill` rule, an `\hline`).
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
   slack sideways, 6.0pt vertically for font ascent).
3. `scripts/check-render-selftest.sh` — breaks payslip, fno_910, tax_invoice
   and fno_300 on purpose and fails unless the gate catches all four and names
   what was lost. The fno_300 case breaks the FIXTURE, not the template, and
   is the regression test for the gate's own fixture-as-oracle blind spot.

It triggers on changes under `templates/**`, `src/docgen/**`,
`docker/Dockerfile` or the three gate scripts.

## Document engines

The worker image (`docker/Dockerfile`, stage `worker-runtime`) is the only
image with a document engine, and it currently carries **two**: TeX Live
(XeLaTeX, what all ten templates render through today) and **Typst, pinned to
0.15.1** at `/usr/local/bin/typst`. Both stay until the last template is
converted (`.superpowers/sdd/typst-migration-spike.md`); do not drop a TeX
package before then.

- The Typst pin is not decorative. Typst is pre-1.0 and every minor release
  changes layout, so the version is a build arg (`TYPST_VERSION`), the
  download is checksum-verified against `TYPST_SHA256`, and `template-render`
  fails if `typst --version` on the built image is not 0.15.1. Bumping it is a
  deliberate act with a template-version consequence, never a drive-by.
- Neither engine ships fonts. Both find **Noto Sans** — the family with the
  Kazakh glyph coverage (ә ғ қ ң ө ұ ү һ і) — from `fonts-noto-core` in
  `/usr/share/fonts`, XeLaTeX via fontspec and Typst by scanning that
  directory. The same CI step asserts `typst fonts` still lists it.

## Don'ts

- Don't edit the `builtin-baseline` in `vcpkg.json` or `ARG VCPKG_REF` in
  `docker/Dockerfile` by hand — Renovate owns them, and a baseline bump
  rebuilds the entire dependency world.
- Don't weaken `config/config.production.json` — `make prod-check` gates it.
- Don't change the error-response shape without updating `docs/openapi.yaml`.

## Self-maintenance

When a PR adds or changes a CI gate, scaffolding script, or invariant,
update this file in the same PR.
