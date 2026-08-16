#!/usr/bin/env bash
#
# The `template-render` CI job: renders every
# templates/latex/<slug>/v<N>/fixtures/*.json through the worker binary's
# `--render-template <slug> <fixture> <outdir>` CLI mode (src/worker_main.cpp),
# i.e. the exact same validate -> normalize -> write_typst_inputs -> compile
# pipeline the "docgen.render" job runs (src/docgen/RenderJob.hpp) — and then
# GATES THE PDF IT PRODUCED.
#
# "It compiled" has never been enough, and there is no transcript to ask
# instead:
#
#   * v0.3.0 shipped payslips and ФНО declarations that had lost their whole
#     amounts column. The engine of the day called that an `Overfull \hbox` —
#     a warning, exit 0 — so CI stayed green while a printed 910.00 read
#     `10 000` in place of 10 000 000,00 ₸.
#   * The fix at the time was to grep the XeLaTeX transcript for that warning.
#     That reads the engine's opinion of the artifact rather than the artifact.
#     It also does not survive the engine: Typst CLIPS silently, exiting 0 and
#     writing no log at all (measured in
#     .superpowers/sdd/typst-migration-spike.md). The grep went with the last
#     `template.tex`; nothing replaced it, because nothing needed to.
#
# So scripts/check-render.py is the ONLY gate here, and that is exactly why it
# reads the PDF: every fixture value and every static label must be in its
# extracted text, every word box must sit inside the page's margin box, no
# token may look like template syntax the fixture cannot account for, and no
# word may have a rule rasterised through it. It knows nothing about which
# engine produced the PDF.
#
# Needs a real `typst`, plus `pdftotext`, `pdftoppm` and `python3` on PATH —
# run this on the worker-render-check image (docker/Dockerfile), not on a bare
# checkout. No C++ test bucket ever invokes a real engine (tests/unit takes no
# services at all; tests/integration stubs DOCGEN_TYPST_CMD), so this script is
# the only place a template is actually compiled — including the one property
# that needs a live engine to mean anything: that the hostile values in every
# `special-chars.json` are TYPESET and not executed. The unit suite pins
# everything up to the engine's door (byte-exact `input.json` staging) and
# keeps those fixtures hostile — see tests/unit/test_template_registry.cpp,
# ShippedTemplatesTest.
#
# Usage:
#   WORKER_BIN=/app/cyber_accountant_worker ./scripts/render-templates.sh
#
# Env overrides:
#   WORKER_BIN       path to the worker binary (default: /app/cyber_accountant_worker,
#                     matching the worker image's layout)
#   TEMPLATES_ROOT   templates root to scan (default: templates/latex, relative to cwd)
#   CHECK_RENDER     path to the PDF gate (default: alongside this script)
#   KEEP_RENDERS     directory to keep every rendered main.typ/json/pdf in
#                     (default: a temp dir, deleted on exit)
set -uo pipefail

WORKER_BIN="${WORKER_BIN:-/app/cyber_accountant_worker}"
TEMPLATES_ROOT="${TEMPLATES_ROOT:-templates/latex}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CHECK_RENDER="${CHECK_RENDER:-$SCRIPT_DIR/check-render.py}"

if [[ ! -x "$WORKER_BIN" ]]; then
    echo "render-templates: worker binary not found/executable at '$WORKER_BIN'" >&2
    echo "  (set WORKER_BIN to override)" >&2
    exit 1
fi
if [[ ! -f "$CHECK_RENDER" ]]; then
    echo "render-templates: the PDF gate is missing at '$CHECK_RENDER'" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "render-templates: no python3 on PATH — scripts/check-render.py cannot run," >&2
    echo "  and skipping the content/geometry gate is how a lost amounts column" >&2
    echo "  reached production once already. Use the worker-render-check image." >&2
    exit 1
fi
if ! command -v pdftoppm >/dev/null 2>&1; then
    echo "render-templates: no pdftoppm on PATH — scripts/check-render.py's ink layer" >&2
    echo "  cannot rasterise the page, and that is the ONLY check that sees a rule" >&2
    echo "  drawn through the text (text extraction is blind to it by construction)." >&2
    echo "  It ships in poppler-utils, next to pdftotext. Use the worker-render-check" >&2
    echo "  image." >&2
    exit 1
fi

if [[ -n "${KEEP_RENDERS:-}" ]]; then
    WORKDIR="$KEEP_RENDERS"
    mkdir -p "$WORKDIR"
else
    WORKDIR="$(mktemp -d)"
    trap 'rm -rf "$WORKDIR"' EXIT
fi

overall=0
count=0
lost=0

shopt -s nullglob
for fixture in "$TEMPLATES_ROOT"/*/v*/fixtures/*.json; do
    count=$((count + 1))
    # fixture = templates/latex/<slug>/v<N>/fixtures/<name>.json
    version_dir="$(dirname "$(dirname "$fixture")")"
    slug="$(basename "$(dirname "$version_dir")")"
    outdir="$WORKDIR/${fixture//\//_}"
    mkdir -p "$outdir"

    # The worker always renders TemplateRegistry::latest(slug). A fixture that
    # lives under an older version directory would therefore be rendered by a
    # different template than the one whose expectation files sit next to it,
    # and the gate would be comparing against the wrong document. Refuse
    # rather than compare the wrong things.
    latest_dir=""
    for candidate in "$TEMPLATES_ROOT/$slug"/v*/; do
        latest_dir="${candidate%/}"
    done
    if [[ "$version_dir" != "$latest_dir" ]]; then
        echo "FAIL $slug $fixture: fixture lives in $version_dir but the worker only" >&2
        echo "  renders $latest_dir (TemplateRegistry::latest) — move or delete it" >&2
        overall=1
        continue
    fi

    if ! "$WORKER_BIN" --render-template "$slug" "$fixture" "$outdir"; then
        overall=1
        continue
    fi

    # The gate proper: does the PDF still say everything the fixture and the
    # template promised, and does all of it fit inside the margin box?
    if ! python3 "$CHECK_RENDER" "$version_dir" "$fixture" "$outdir/main.pdf"; then
        lost=$((lost + 1))
        overall=1
    fi
done
shopt -u nullglob

if [[ "$count" -eq 0 ]]; then
    echo "render-templates: no fixtures found under '$TEMPLATES_ROOT'" >&2
    exit 1
fi

if [[ "$lost" -gt 0 ]]; then
    echo "render-templates: $lost fixture(s) lost content or overran the margin box." >&2
    echo "  Each finding above names the fixture and the exact value, label or word." >&2
    echo "  A value that is in the fixture but not in the PDF is a document that would" >&2
    echo "  have been handed to an employee or the КГД with a number missing from it." >&2
    echo "  See scripts/check-render.py and templates/latex/README.md." >&2
fi

echo "render-templates: $count fixture(s) checked"
if [[ -n "${KEEP_RENDERS:-}" ]]; then
    echo "render-templates: rendered documents kept under $WORKDIR"
fi
exit "$overall"
