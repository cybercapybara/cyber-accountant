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
# Usage:
#   WORKER_BIN=/app/cyber_accountant_worker ./scripts/render-templates.sh
#
# Env overrides:
#   WORKER_BIN      path to the worker binary (default: /app/cyber_accountant_worker,
#                    matching the worker-runtime image's layout)
#   TEMPLATES_ROOT   templates root to scan (default: templates/latex, relative to cwd)
set -uo pipefail

WORKER_BIN="${WORKER_BIN:-/app/cyber_accountant_worker}"
TEMPLATES_ROOT="${TEMPLATES_ROOT:-templates/latex}"

if [[ ! -x "$WORKER_BIN" ]]; then
    echo "render-templates: worker binary not found/executable at '$WORKER_BIN'" >&2
    echo "  (set WORKER_BIN to override)" >&2
    exit 1
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

overall=0
count=0

shopt -s nullglob
for fixture in "$TEMPLATES_ROOT"/*/v*/fixtures/*.json; do
    count=$((count + 1))
    # fixture = templates/latex/<slug>/v<N>/fixtures/<name>.json
    slug="$(basename "$(dirname "$(dirname "$(dirname "$fixture")")")")"
    outdir="$WORKDIR/${fixture//\//_}"
    mkdir -p "$outdir"

    if ! "$WORKER_BIN" --render-template "$slug" "$fixture" "$outdir"; then
        overall=1
    fi
done
shopt -u nullglob

if [[ "$count" -eq 0 ]]; then
    echo "render-templates: no fixtures found under '$TEMPLATES_ROOT'" >&2
    exit 1
fi

echo "render-templates: $count fixture(s) checked"
exit "$overall"
