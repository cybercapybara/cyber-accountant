#!/usr/bin/env bash
#
# Proves the render gate bites.
#
# A gate that only ever runs against healthy input is indistinguishable from a
# gate that returns 0 unconditionally, and this repo has already shipped tests
# named for mechanisms they did not exercise. So: take the real templates,
# break them six different ways — the amounts column emptied, one amount
# silently truncated, a table widened until its right-hand columns fall off
# the page, every amount of a ФНО 300.00 rewritten into the machine money
# form, a Typst control construct typeset as body text, and a signature rule
# drawn through the party identifiers — render each through the SAME worker
# pipeline the real job uses, and require that scripts/check-render.py fails
# on each one AND says what went wrong.
#
# The fourth case is not like the first three. Those break the TEMPLATE and
# the gate notices the PDF no longer matches the fixture. The fourth breaks
# the FIXTURE, in the one way the gate could not see before v0.4.2: fixture
# and PDF agree perfectly, and both are wrong, because the fixture hand-wrote
# the printed form of an amount. It is the regression test for the gate's own
# blind spot, and it must fail at LAYER 0 — before any pixel is compared.
#
# The fifth and sixth are the two blind spots the labour-contract conversion
# proved by experiment: the gate reported PASS, exit 0, on BOTH of them, and
# both are the defect class the Typst migration produces. They ADD ink rather
# than lose it, which is precisely what layers 0-2 were built not to notice —
# every declared label and value is still there in each. Case 5 must fail at
# LAYER 3 (the `else` and its whole branch typeset as literal text) and case 6
# at LAYER 4 (a rule struck through "БСН/БИН 104332181962" and "ЖСН/ИИН
# 900112350487", invisible to text extraction by construction). Eight
# templates are still to convert; if either mutator stops mutating, or either
# layer stops biting, the migration goes back to being guarded by a human
# squinting at a raster.
#
# Every case also renders the UNMUTATED copy of the same template first and
# requires a PASS. That control is the point: without it, a case could "pass"
# because the expectation files were wrong, or because the render crashed, or
# because the gate rejects everything.
#
# Needs the same environment as scripts/render-templates.sh (a real typst,
# pdftotext, pdftoppm and python3) — i.e. the worker-render-check image. All
# ten templates are Typst now, so no case reaches xelatex any more; the image
# still carries TeX Live and this script would still drive it if a
# `template.tex` came back.
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

# --- the six breakages --------------------------------------------------------
# Each takes the root of a private copy of the templates tree and edits one
# template (case 4: one fixture) in place.
#
# EVERY MUTATOR IS NOW python3 AND EVERY ONE COUNTS ITS SUBSTITUTIONS. That is
# not style. Two of them used to `sed` LaTeX syntax — a `{{ }}` placeholder in
# fno_910, a `tabularx` width in tax_invoice — and when those templates were
# converted the patterns stopped matching: `sed` cheerfully copied the file
# through unchanged and the case would have "passed" because the gate was
# handed an UNMUTATED template twice, which is the green-rubber-stamp failure
# this whole script exists to prevent. The tree now holds zero `template.tex`,
# so ANY surviving LaTeX pattern would no-op silently. Two defences, and keep
# both: each mutator exits nonzero when its own count is wrong, AND run_case
# fingerprints the tree around every mutator and fails loudly when a mutation
# changes nothing. See tree_files/digest_of and the "changed NOTHING" branch
# below.
#
# `sed -i` was avoided here for a second reason worth keeping in mind if one
# ever comes back: its argument differs between GNU and BSD sed and this
# script is run on both.

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

# 3. Content pushed past the right margin: the tax invoice's nine-column table
#    is given fixed, oversized columns instead of the (auto, 1fr, auto…) set,
#    which walks its VAT columns off the right edge. Typst does this SILENTLY —
#    exit 0, no warning, no log at all — which is precisely why the gate reads
#    the PDF and not the engine's opinion of it.
#    44mm is measured, not arbitrary. The column pitch is 44+3mm, so on a 297mm
#    landscape page columns 7-9 land past the PAPER edge (their content is
#    dropped and lost, `Ставка НДС` among it) while column 6's right-aligned
#    figures land between the 285mm margin and the paper edge, where pdftotext
#    still sees them and layer 2 can flag them. Both halves are needed: the
#    run_case below asserts CONTENT LOST *and* OFF-MARGIN. A wider set (60mm)
#    pushes EVERY overflowing column clean off the paper, leaving no word for
#    the geometry layer to find and no OFF-MARGIN line at all.
break_over_wide_table() {
    python3 - "$1/tax_invoice/v1/template.typ" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
# count=1: the template has exactly two `columns: (…)` — the nine-column
# `#table` and the signature `#grid` — and the table is first.
text, n = re.subn(r"columns:\s*\([^)]*\)",
                  "columns: (44mm, 44mm, 44mm, 44mm, 44mm, 44mm, 44mm, 44mm, 44mm)",
                  text, count=1)
if n != 1:
    sys.exit("break_over_wide_table: no `columns: (...)` found in %s" % path)
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
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
text, n = re.subn(r': "(\d{1,3}(?: \d{3})*),(\d{2})"',
                  lambda m: ': "%s.%s"' % (m.group(1).replace(" ", ""), m.group(2)), text)
if n < 1:
    sys.exit("break_machine_money_form: no printed-form amount in %s — the "
             "fixture no longer carries the money strings this case rewrites" % path)
with open(path, "w", encoding="utf-8") as fh:
    fh.write(text)
PY
}

# 5. Spike defect 2, reproduced exactly: `] else [` is broken across two
#    physical lines in the labour contract's clause 2. Typst closes the
#    if-expression at the `]` and then TYPESETS the word `else` and its entire
#    second branch as literal body text — exit 0, no error, no warning, and a
#    contract that reads "…и действует до 17.08.2027 else [и заключён на
#    неопределённый срок]", brackets and all. Measured on the real template:
#    291 word boxes instead of 280, every one inside the margins, every
#    declared label and value present. LAYER 3 is the only thing that sees it.
#    python3, not sed, and it counts: this must fail loudly rather than hand
#    the gate an unmutated template.
break_leaked_control_syntax() {
    python3 - "$1/labor_contract/v1/template.typ" <<'PY'
import sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    lines = fh.read().split("\n")
hits = 0
for i, line in enumerate(lines):
    # Clause 2's two parallel sentences, Kazakh then Russian.
    if "] else [" in line and line.startswith(("Шарт ", "Договор ")):
        lines[i] = line.replace("] else [", "]\nelse [", 1)
        hits += 1
if hits != 2:
    sys.exit("break_leaked_control_syntax: split %d of the expected 2 `] else [` "
             "constructs in %s — clause 2 no longer has the shape this case "
             "mutates" % (hits, path))
with open(path, "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
PY
}

# 6. Spike defect 3, reproduced exactly: the labour contract's signature rules
#    go back to a bare `line()`. `line` contributes no height, so the grid row
#    collapses to zero and the rule is drawn THROUGH the two rows above it,
#    striking out both party identifiers. Text extraction cannot see it — same
#    280 word boxes as the healthy document, all inside the margins — so LAYER
#    4 rasterises and looks for ink instead.
break_rule_through_text() {
    python3 - "$1/labor_contract/v1/template.typ" <<'PY'
import re, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as fh:
    text = fh.read()
text, n = re.subn(r"box\(width: 1fr, height: [0-9.]+mm, stroke: \(bottom: [0-9.]+pt\)\)",
                  "line(length: 100%)", text)
if n != 1:
    sys.exit("break_rule_through_text: replaced %d of the expected 1 stroked "
             "signature box in %s — the \\hrulefill equivalent no longer has the "
             "shape this case mutates" % (n, path))
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

# tree_files <dir> — every regular file under <dir>, one per line, sorted.
tree_files() {
    find "$1" -type f | LC_ALL=C sort
}

# digest_of <<< <paths> — a "<path> <cksum> <bytes>" line per path read on
# stdin, or "<path> GONE" for one that no longer exists. POSIX tools only
# (find/sort/cksum), because this runs on the Debian worker image and on the
# maintainer's macOS.
#
# It fingerprints a FIXED path list captured before the mutation, deliberately
# ignoring files that appeared after it. A mutator whose pattern matches
# nothing can still leave litter behind — the retired `sed ... >"$tex.new"`
# form created an empty `template.tex.new` through the shell redirect before
# sed even ran — and a whole-tree fingerprint would read that litter as
# "something changed" and wave the no-op through. No mutator writes a sidecar
# today, but keep the fixed list: only a change to a file the render actually
# reads may count as a mutation.
digest_of() {
    local file
    while IFS= read -r file; do
        if [[ -f "$file" ]]; then
            printf '%s %s\n' "$file" "$(cksum <"$file")"
        else
            printf '%s GONE\n' "$file"
        fi
    done
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

    # Now break it — and prove the breakage happened. A mutator that matches
    # nothing (its `sed` written for LaTeX, the template since converted to
    # `template.typ`) would otherwise hand the gate the SAME document it just
    # passed, and the case would report OK while testing nothing at all.
    local tracked digest_before digest_after
    tracked="$(tree_files "$latex")"
    digest_before="$(printf '%s\n' "$tracked" | digest_of)"
    "$mutator" "$latex"
    digest_after="$(printf '%s\n' "$tracked" | digest_of)"
    if [[ "$digest_before" == "$digest_after" ]]; then
        echo "SELFTEST FAIL [$label]: the mutator $mutator changed NOTHING under" >&2
        echo "  $vdir — every file is byte-identical, so this case would have" >&2
        echo "  re-tested the healthy document and reported OK. A mutator that" >&2
        echo "  stops mutating is a gate that stops gating." >&2
        if [[ -f "$vdir/template.typ" ]]; then
            echo "  $slug is a Typst template ($vdir/template.typ). Either the mutator" >&2
            echo "  still edits LaTeX syntax and needs porting to the Typst source, or" >&2
            echo "  the Typst construct it targets was rewritten — find the shape it" >&2
            echo "  now has and mutate THAT. Do not delete the case." >&2
        else
            echo "  $vdir still holds a template.tex — the mutator's pattern no" >&2
            echo "  longer matches the template it was written against." >&2
        fi
        failures=$((failures + 1))
        return
    fi
    out="$tree/broken"
    if ! render "$tree" "$slug" "$fixture" "$out"; then
        echo "SELFTEST FAIL [$label]: the broken $slug/$fixture_name failed to COMPILE," >&2
        echo "  so this case no longer tests the gate — it tests the engine. Pick a" >&2
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

run_case leaked-control-syntax labor_contract basic.json break_leaked_control_syntax \
    'LEAKED SYNTAX' \
    '"else" is printed in the PDF and is a control keyword' \
    'is a content-block bracket'

run_case rule-through-text labor_contract basic.json break_rule_through_text \
    'RULE THROUGH TEXT' \
    'word "104332181962"' \
    'word "900112350487"' \
    "ink above AND below"

if [[ "$failures" -gt 0 ]]; then
    echo "check-render-selftest: $failures case(s) failed — the render gate is NOT" >&2
    echo "  proven to catch anything. Fix it before trusting a green template-render." >&2
    exit 1
fi
echo "check-render-selftest: 6 deliberate breakages, all applied, all caught, all named"
