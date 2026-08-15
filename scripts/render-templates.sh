#!/usr/bin/env bash
#
# Actually renders every templates/latex/<slug>/v<N>/fixtures/*.json through
# XeLaTeX, via the worker binary's `--render-template <slug> <fixture> <outdir>`
# CLI mode (src/worker_main.cpp). That mode runs the exact same
# validate -> render_tex -> compile_pdf pipeline as the "docgen.render" job
# (src/docgen/RenderJob.hpp), so a PASS here means a real document render
# would succeed too.
#
# Needs a real `xelatex` on PATH — run this on the worker image (the only
# stage with TeX Live installed, see docker/Dockerfile), not on a bare
# checkout. The unit/integration test suites never invoke real XeLaTeX
# (DOCGEN_LATEX_CMD is stubbed there); this script is the one place that does
# — wired into CI as the `template-render` job.
#
# A successful compile is NOT enough, though — see the overfull-\hbox gate
# below.
#
# Usage:
#   WORKER_BIN=/app/cyber_accountant_worker ./scripts/render-templates.sh
#
# Env overrides:
#   WORKER_BIN      path to the worker binary (default: /app/cyber_accountant_worker,
#                    matching the worker-runtime image's layout)
#   TEMPLATES_ROOT   templates root to scan (default: templates/latex, relative to cwd)
#   OVERFULL_MAX_PT  overfull-\hbox tolerance in TeX points (default: 1.0)
set -uo pipefail

WORKER_BIN="${WORKER_BIN:-/app/cyber_accountant_worker}"
TEMPLATES_ROOT="${TEMPLATES_ROOT:-templates/latex}"

# --- Why this job also greps the TeX log -------------------------------------
# v0.3.0 shipped a defect this job did NOT catch: in payslip/fno_910/fno_300
# the line before the amounts table wasn't terminated with \par, so the
# \textwidth tabularx was typeset INLINE, continuing that paragraph, and hung
# off the right edge of the page. XeLaTeX only reports that as
# "Overfull \hbox (89.25517pt too wide)" — a warning, exit code 0 — so the job
# stayed green while the printed ФНО 910.00 showed "10 000" in place of
# 10 000 000,00 and the printed payslip lost its whole amounts column plus its
# static header. The underlying data was correct everywhere; only the PDF was
# wrong, which is the worst kind of wrong to hand an employee or the КГД.
#
# So: fail on any overfull \hbox wider than OVERFULL_MAX_PT. The threshold is
# not zero because TeX reports sub-point overfulls that are invisible in print,
# but it is deliberately small — the invoice/tax_invoice/avr/reconciliation/
# waybill variant of this same bug only measured 1.59pt, and a 1.59pt overhang
# was still enough to push a "₸" glyph past the margin. Anything at or above
# ~1pt means a box genuinely does not fit its column and money can be clipped.
# Underfull boxes are NOT gated: they are loose-looking lines, not lost data.
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

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

overall=0
count=0
overfull=0

shopt -s nullglob
for fixture in "$TEMPLATES_ROOT"/*/v*/fixtures/*.json; do
    count=$((count + 1))
    # fixture = templates/latex/<slug>/v<N>/fixtures/<name>.json
    slug="$(basename "$(dirname "$(dirname "$(dirname "$fixture")")")")"
    outdir="$WORKDIR/${fixture//\//_}"
    mkdir -p "$outdir"

    if ! "$WORKER_BIN" --render-template "$slug" "$fixture" "$outdir"; then
        overall=1
        continue
    fi

    # Compiled — now check it compiled to something that fits on the page.
    # render_and_compile() writes main.tex into $outdir and runs XeLaTeX there,
    # so the transcript is $outdir/main.log. No transcript means the gate could
    # not run at all, which is a failure rather than a pass: silently skipping
    # the check is how this class of defect reached production once already.
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

echo "render-templates: $count fixture(s) checked"
exit "$overall"
