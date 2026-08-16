#!/usr/bin/env bash
#
# The `template-render` CI job: renders every
# templates/latex/<slug>/v<N>/fixtures/*.json through the engine that
# template's own directory declares — XeLaTeX for a `template.tex`, Typst for
# a `template.typ` — via the worker binary's
# `--render-template <slug> <fixture> <outdir>` CLI mode (src/worker_main.cpp),
# i.e. the exact same validate -> normalize -> (render_tex | write_typst_inputs)
# -> compile pipeline the "docgen.render" job runs (src/docgen/RenderJob.hpp)
# — and then GATES THE PDF IT PRODUCED.
#
# "It compiled" has never been enough, and "the engine didn't complain" is not
# enough either:
#
#   * v0.3.0 shipped payslips and ФНО declarations that had lost their whole
#     amounts column. XeLaTeX called that an `Overfull \hbox` — a warning,
#     exit 0 — so CI stayed green while a printed 910.00 read `10 000` in
#     place of 10 000 000,00 ₸.
#   * The fix at the time was to grep the transcript for that warning. That
#     reads the engine's opinion of the artifact rather than the artifact, and
#     it only exists in LaTeX: the Typst migration spike
#     (.superpowers/sdd/typst-migration-spike.md) measured that the intended
#     replacement engine drops content silently with exit 0 and NO transcript
#     at all.
#
# So the gate proper is scripts/check-render.py, which reads the PDF: every
# fixture value and every static label must be in its extracted text, every
# word box must sit inside the page's margin box, no token may look like
# template syntax the fixture cannot account for, and no word may have a rule
# rasterised through it. It knows nothing about which engine produced the PDF
# and survives the migration unchanged.
#
# The transcript grep is KEPT, demoted to a supplementary LaTeX-only tripwire.
# It is not redundant: it is the only check here that sees overflow in
# material `pdftotext` cannot report — a `\hrulefill` signature rule, a
# `\hline`, an image — because those emit no words. It goes away with XeLaTeX:
# it runs only for a version directory that still has a `template.tex`, and a
# `template.typ` one is gated by check-render.py alone, by design. The
# converse is also an error and is reported: a directory with no
# `template.tex` that nevertheless left a XeLaTeX transcript means the engine
# selection (src/docgen, per-template) and the template tree disagree.
#
# Needs a real `xelatex` AND a real `typst`, plus `pdftotext`, `pdftoppm` and
# `python3` on PATH — run this on the worker-render-check image
# (docker/Dockerfile), not on a bare checkout. No C++ test bucket ever invokes
# a real engine (tests/unit takes no services at all; tests/integration stubs
# DOCGEN_LATEX_CMD), so this script is the only place a template is actually
# compiled — including the one property that needs a live engine to mean
# anything: that the hostile values in every `special-chars.json` are TYPESET
# and not executed. The unit suite pins everything up to the engine's door
# (Docgen::render_tex escaping for LaTeX, byte-exact `input.json` staging for
# Typst) and keeps those fixtures hostile — see
# tests/unit/test_template_registry.cpp, ShippedTemplatesTest.
#
# Usage:
#   WORKER_BIN=/app/cyber_accountant_worker ./scripts/render-templates.sh
#
# Env overrides:
#   WORKER_BIN       path to the worker binary (default: /app/cyber_accountant_worker,
#                     matching the worker image's layout)
#   TEMPLATES_ROOT   templates root to scan (default: templates/latex, relative to cwd)
#   CHECK_RENDER     path to the PDF gate (default: alongside this script)
#   KEEP_RENDERS     directory to keep every rendered main.tex/pdf/log in
#                     (default: a temp dir, deleted on exit)
#   OVERFULL_MAX_PT  overfull-\hbox tolerance in TeX points (default: 1.0)
set -uo pipefail

WORKER_BIN="${WORKER_BIN:-/app/cyber_accountant_worker}"
TEMPLATES_ROOT="${TEMPLATES_ROOT:-templates/latex}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CHECK_RENDER="${CHECK_RENDER:-$SCRIPT_DIR/check-render.py}"

# The threshold is not zero because TeX reports sub-point overfulls that are
# invisible in print, but it is deliberately small — the
# invoice/tax_invoice/avr/reconciliation/waybill variant of the v0.3.0 bug
# only measured 1.59pt, and 1.59pt was still enough to push a "₸" glyph past
# the margin. Underfull boxes are NOT gated: loose-looking lines, not lost
# data. check-render.py's own geometry layer is stricter still (0.5pt of ink
# past the margin box), so this threshold only matters for the non-text
# material described in the header.
OVERFULL_MAX_PT="${OVERFULL_MAX_PT:-1.0}"

# Print every "Overfull \hbox (<N>pt too wide) ..." line in $1 whose <N>
# exceeds OVERFULL_MAX_PT. Empty output means the render is clean.
overfull_offenders() {
    sed -n 's/^Overfull \\hbox (\([0-9.]*\)pt too wide)\(.*\)$/\1\2/p' "$1" |
        awk -v max="$OVERFULL_MAX_PT" '$1 + 0 > max + 0 {
            printf "    Overfull \\hbox (%spt too wide)%s\n", $1, substr($0, length($1) + 1)
        }'
}

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
overfull=0
lost=0
# Per-engine tallies, printed in the summary. During the Typst migration the
# split is the one number that says which engine each fixture actually went
# through — a template that was converted but still renders through XeLaTeX
# (or the reverse) shows up here before anyone squints at a PDF.
latex_count=0
typst_count=0

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

    # Compiled. For a LaTeX template render_and_compile() writes main.tex into
    # $outdir and runs XeLaTeX there, so the transcript is $outdir/main.log,
    # and no transcript means the tripwire could not run at all — a failure
    # rather than a pass.
    #
    # The overfull-\hbox grep is a LaTeX-ONLY tripwire, kept for overflow in
    # material that produces no extractable text (a \hrulefill rule, an
    # \hline). A Typst template writes no transcript at all — the engine exits
    # 0 and logs nothing even when it clips — so for those the gate proper
    # (check-render.py, which reads the PDF) is the only check, by design.
    if [[ -f "$version_dir/template.tex" ]]; then
        latex_count=$((latex_count + 1))
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
    else
        typst_count=$((typst_count + 1))
        if [[ -f "$outdir/main.log" ]]; then
            echo "FAIL $slug $fixture: $version_dir has no template.tex but the render left a" >&2
            echo "  XeLaTeX transcript — the engine selection and the tree disagree" >&2
            overall=1
            continue
        fi
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

if [[ "$overfull" -gt 0 ]]; then
    echo "render-templates: $overfull fixture(s) produced an overfull \\hbox over ${OVERFULL_MAX_PT}pt." >&2
    echo "  A \\textwidth table must start its own paragraph — terminate the block above it" >&2
    echo "  with an unconditional \\par (never one that lives inside an {% if %})." >&2
    echo "  See templates/latex/README.md, \"Tables start their own paragraph\"." >&2
fi

if [[ "$lost" -gt 0 ]]; then
    echo "render-templates: $lost fixture(s) lost content or overran the margin box." >&2
    echo "  Each finding above names the fixture and the exact value, label or word." >&2
    echo "  A value that is in the fixture but not in the PDF is a document that would" >&2
    echo "  have been handed to an employee or the КГД with a number missing from it." >&2
    echo "  See scripts/check-render.py and templates/latex/README.md." >&2
fi

echo "render-templates: $count fixture(s) checked ($latex_count via XeLaTeX, $typst_count via Typst)"
if [[ -n "${KEEP_RENDERS:-}" ]]; then
    echo "render-templates: rendered documents kept under $WORKDIR"
fi
exit "$overall"
