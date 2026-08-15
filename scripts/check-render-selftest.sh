#!/usr/bin/env bash
#
# Proves the render gate bites.
#
# A gate that only ever runs against healthy input is indistinguishable from a
# gate that returns 0 unconditionally, and this repo has already shipped tests
# named for mechanisms they did not exercise. So: take the real templates,
# break them four different ways — the amounts column emptied, one amount
# silently truncated, a table widened until its right-hand columns fall off
# the page, and every amount of a ФНО 300.00 rewritten into the machine money
# form — render each through the SAME worker pipeline the real job uses, and
# require that scripts/check-render.py fails on each one AND says what was
# lost.
#
# The fourth case is not like the first three. Those break the TEMPLATE and
# the gate notices the PDF no longer matches the fixture. The fourth breaks
# the FIXTURE, in the one way the gate could not see before v0.4.2: fixture
# and PDF agree perfectly, and both are wrong, because the fixture hand-wrote
# the printed form of an amount. It is the regression test for the gate's own
# blind spot, and it must fail at LAYER 0 — before any pixel is compared.
#
# Every case also renders the UNMUTATED copy of the same template first and
# requires a PASS. That control is the point: without it, a case could "pass"
# because the expectation files were wrong, or because the render crashed, or
# because the gate rejects everything.
#
# Needs the same environment as scripts/render-templates.sh (a real xelatex,
# pdftotext and python3) — i.e. the worker-render-check image.
#
# Usage:
#   WORKER_BIN=/app/cyber_accountant_worker ./scripts/check-render-selftest.sh
set -uo pipefail

WORKER_BIN="${WORKER_BIN:-/app/cyber_accountant_worker}"
TEMPLATES_ROOT="${TEMPLATES_ROOT:-templates/latex}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CHECK_RENDER="${CHECK_RENDER:-$SCRIPT_DIR/check-render.py}"

if [[ ! -x "$WORKER_BIN" ]]; then
    echo "check-render-selftest: worker binary not found/executable at '$WORKER_BIN'" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "check-render-selftest: no python3 on PATH" >&2
    exit 1
fi

TEMPLATES_ABS="$(cd -- "$TEMPLATES_ROOT" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
failures=0

# --- the three breakages -----------------------------------------------------
# Each takes the root of a private copy of the templates tree and edits one
# template in place. `sed -i` is avoided: its argument differs between GNU and
# BSD sed and this script is run on both.

# 1. The v0.3.0 symptom, reproduced exactly: the payslip's amounts column is
#    emptied. Every money cell in the table body loses its placeholder while
#    the column and its "Сомасы, ₸ / Сумма, ₸" header stay, so the document
#    still looks like a payslip and still compiles cleanly.
break_amounts_column() {
    local tex="$1/payslip/v1/template.tex"
    sed -E '/&/ s/\{\{ *[a-z_]+ *\}\} \\\\$/\\\\/' "$tex" >"$tex.new" && mv "$tex.new" "$tex"
}

# 2. One amount silently truncated: ФНО 910.00's income line prints a literal
#    `10 000` instead of the fixture's 10 000 000,00. This is the exact defect
#    that was printed and filed in v0.3.0, and it is the case that proves
#    amounts are compared in the form they are PRINTED — `10 000` is a prefix
#    of `10 000 000,00`, so a sloppy check would pass it.
break_truncated_amount() {
    local tex="$1/fno_910/v1/template.tex"
    sed 's/{{ income_tenge }}/10 000/' "$tex" >"$tex.new" && mv "$tex.new" "$tex"
}

# 3. Content pushed past the right margin: the tax invoice's nine-column table
#    is widened to 1.25\textwidth, which walks its VAT columns off the right
#    edge of the paper — the printed document loses them entirely.
break_over_wide_table() {
    local tex="$1/tax_invoice/v1/template.tex"
    sed 's/\\begin{tabularx}{\\textwidth}/\\begin{tabularx}{1.25\\textwidth}/' \
        "$tex" >"$tex.new" && mv "$tex.new" "$tex"
}

# 4. The v0.4.2 defect, reproduced exactly: every amount of the ФНО 300.00
#    fixture is rewritten from the printed form into Ledger::format_tiyn's
#    machine form, so the declaration reads "Сумма НДС, подлежащая уплате в
#    бюджет: 450000.00 ₸". This is what the server actually produced, and the
#    gate PASSED it — fixture and PDF agreed. Layer 0 must now refuse it
#    without looking at the PDF at all. python3 rather than sed: only the
#    whole-value amounts may be rewritten, not the digits inside free text.
break_machine_money_form() {
    python3 - "$1/fno_300/v1/fixtures/basic.json" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
text = re.sub(r': "(\d{1,3}(?: \d{3})*),(\d{2})"',
              lambda m: ': "%s.%s"' % (m.group(1).replace(" ", ""), m.group(2)), text)
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

# --- driver ------------------------------------------------------------------

# render <tree> <slug> <fixture> <outdir> — run the real pipeline with the
# tree's own templates/latex (TemplateRegistry resolves it relative to cwd).
render() {
    local tree="$1" slug="$2" fixture="$3" outdir="$4"
    mkdir -p "$outdir"
    (cd "$tree" && "$WORKER_BIN" --render-template "$slug" "$fixture" "$outdir") >/dev/null 2>&1
}

version_dir() {
    local latest=""
    for candidate in "$1/$2"/v*/; do
        latest="${candidate%/}"
    done
    echo "$latest"
}

# run_case <label> <slug> <fixture-name> <mutator> <expected substring>...
run_case() {
    local label="$1" slug="$2" fixture_name="$3" mutator="$4"
    shift 4
    local tree="$TMP/$label"
    local latex="$tree/templates/latex"
    mkdir -p "$tree/templates"
    cp -R "$TEMPLATES_ABS" "$latex"
    local vdir fixture out output status
    vdir="$(version_dir "$latex" "$slug")"
    fixture="$vdir/fixtures/$fixture_name"

    # Control: the untouched template must PASS, or the case proves nothing.
    out="$tree/control"
    if ! render "$tree" "$slug" "$fixture" "$out"; then
        echo "SELFTEST FAIL [$label]: the UNMUTATED $slug/$fixture_name did not even render" >&2
        failures=$((failures + 1))
        return
    fi
    if ! output="$(python3 "$CHECK_RENDER" "$vdir" "$fixture" "$out/main.pdf" 2>&1)"; then
        echo "SELFTEST FAIL [$label]: the gate rejects the UNMUTATED $slug/$fixture_name," >&2
        echo "  so a failure on the broken one would prove nothing:" >&2
        echo "$output" | sed 's/^/    /' >&2
        failures=$((failures + 1))
        return
    fi

    # Now break it.
    "$mutator" "$latex"
    out="$tree/broken"
    if ! render "$tree" "$slug" "$fixture" "$out"; then
        echo "SELFTEST FAIL [$label]: the broken $slug/$fixture_name failed to COMPILE," >&2
        echo "  so this case no longer tests the gate — it tests XeLaTeX. Pick a" >&2
        echo "  breakage that still compiles, the way the v0.3.0 defect did." >&2
        failures=$((failures + 1))
        return
    fi
    output="$(python3 "$CHECK_RENDER" "$vdir" "$fixture" "$out/main.pdf" 2>&1)"
    status=$?
    if [[ "$status" -eq 0 ]]; then
        echo "SELFTEST FAIL [$label]: the gate PASSED a $slug that lost content." >&2
        echo "$output" | sed 's/^/    /' >&2
        failures=$((failures + 1))
        return
    fi

    local missing=0 needle
    for needle in "$@"; do
        if ! printf '%s\n' "$output" | grep -qF -- "$needle"; then
            echo "SELFTEST FAIL [$label]: the gate failed, but never said $needle" >&2
            missing=1
        fi
    done
    if [[ "$missing" -ne 0 ]]; then
        echo "$output" | sed 's/^/    /' >&2
        failures=$((failures + 1))
        return
    fi
    echo "SELFTEST OK   [$label]: $slug/$fixture_name broken -> gate exit $status, named what was lost"
}

run_case amounts-column payslip basic.json break_amounts_column \
    'CONTENT LOST' \
    'amount gross_tenge = 30000000 tiyn, printed as "300 000,00"' \
    'amount social_tax = 1584000 tiyn, printed as "15 840,00"'

run_case truncated-amount fno_910 basic.json break_truncated_amount \
    'CONTENT LOST' \
    'amount income_tenge = 1000000000 tiyn, printed as "10 000 000,00"'

run_case over-wide-table tax_invoice basic.json break_over_wide_table \
    'static label "Ставка НДС"' \
    'OFF-MARGIN' \
    'crosses the RIGHT margin'

run_case machine-money-form fno_300 basic.json break_machine_money_form \
    'MACHINE MONEY FORM' \
    'balance_tenge = "450000.00"' \
    'FIXTURE FORMAT' \
    'declares 45000000 tiyn'

if [[ "$failures" -gt 0 ]]; then
    echo "check-render-selftest: $failures case(s) failed — the render gate is NOT" >&2
    echo "  proven to catch anything. Fix it before trusting a green template-render." >&2
    exit 1
fi
echo "check-render-selftest: 4 deliberate breakages, all caught, all named"
