#!/usr/bin/env python3
"""Content, geometry and ink gate over ONE rendered document PDF.

Why this exists
---------------
v0.3.0 shipped payslips and ФНО declarations that had lost their entire
amounts column: a printed 910.00 read `10 000` where the true figure was
`10 000 000,00 ₸`. The data was correct; only the PDF was wrong. CI stayed
green because an overfull box is a *warning* in LaTeX, not an error.

That was patched by grepping the XeLaTeX transcript for `Overfull \\hbox`.
That check reads the engine's opinion of the artifact, works only for LaTeX,
and only catches one failure shape — the Typst migration spike
(.superpowers/sdd/typst-migration-spike.md) measured that the intended
replacement engine clips silently with exit 0 and no transcript at all.

So this gate reads the **artifact** instead of the engine's log, in five
layers, and knows nothing about which engine produced the PDF:

  LAYER 0 (oracle)    every printed amount in the fixture must be DERIVED
                      from an integer number of tiyn, by this file's `money()`
                      — the same algorithm the server's
                      Money::format_tiyn_ru uses. A fixture may not hand-write
                      the printed form of an amount. See "Why amounts are
                      derived" below.
  LAYER 1 (content)   every scalar in the fixture, and every static label the
                      template prints (declared in `expected.txt`), must
                      appear in the PDF's extracted text. Amounts are checked
                      in the form they are actually PRINTED — thousands
                      separators and the decimal comma — and a `*_tiyn`
                      integer is checked as the money string it must have
                      been formatted into, never as a raw integer.
  LAYER 2 (geometry)  every word's bounding box from `pdftotext -bbox` must
                      lie inside the page's margin box. Catches content that
                      survives extraction but is drawn off the paper.
  LAYER 3 (syntax)    no word in the extracted text may look like TEMPLATE
                      SYNTAX unless the fixture or an expectation file
                      accounts for it. Catches a control construct that got
                      typeset as body text instead of being executed.
  LAYER 4 (ink)       no word may have a drawn rule crossing it. The page is
                      rasterised and a word is failed when a full-width dark
                      band runs through its box with the word's OWN ink both
                      above and below it. Catches ink that text extraction is
                      blind to by construction.

Every layer names what went wrong: which fixture, which value or label,
which word crossed which margin by how much, which token leaked, which rule
struck through which word.

Why layers 3 and 4 exist
------------------------
The Typst conversion of the labour contract rebuilt, and re-measured, the two
defects the migration spike had hit. Layers 0-2 reported PASS, exit 0, on
BOTH of them:

  * `] else [` broken across two physical lines. Typst closes the
    if-expression at the `]` and then TYPESETS the word `else` and its whole
    second branch as literal body text — exit 0, no warning. The contract read
    "…и действует до 17.08.2027 else [и заключён на неопределённый срок]".
    291 word boxes instead of 280, all inside the margins, every declared
    label and value present: the leak only ADDS words, and layers 0-2 only
    ever ask whether something is missing.
  * `line()` used for a signature rule inside a grid. `line` contributes no
    height, so the row collapsed to zero and the rule was drawn THROUGH the
    party identifiers — "БСН/БИН 104332181962" and "ЖСН/ИИН 900112350487"
    struck through. The glyphs extract perfectly; the geometry layer compares
    word boxes to the PAGE MARGIN and never to other ink. Same 280 word
    boxes as the healthy document.

Both are the defect class this migration produces, every template still
holding a `template.tex` is a conversion that can rebuild one of them, and
until now the only defence was a human looking at the raster.

The hard part of layer 3 is that a fixture may legitimately CARRY template
syntax as data — the `special-chars` fixtures deliberately ship `#panic("x")`,
`#read("/etc/passwd")`, `\б`, `{б}`, backticks and tildes, and those must
still pass. So the layer does not ask "does this look like syntax", it asks
"does this look like syntax that NOTHING accounted for": every extracted
token that matches a syntax signature must occur inside a fixture value or
inside a declared label. Syntax that arrived as data is attributable; syntax
that leaked out of the template is not. See `check_leaked_syntax`.

The hard part of layer 4 is that healthy documents are full of rules very
close to glyphs — an underline, a `\\midrule` immediately below a header row,
a signature line under a name. The discriminator is not distance, it is
which side the ink is on: a rule that is UNDER text has the word's ink only
above it, a rule ABOVE text only below it, and a rule THROUGH text has the
same glyph columns inked on both sides. Measured over all 22 shipped
fixtures, both engines: zero findings. See `check_struck_through`.

Why amounts are derived
-----------------------
v0.4.2 shipped ФНО declarations, payslips and labour contracts that printed
`450000.00` where a Kazakhstani legal document must read `450 000,00` — the
server was calling Ledger::format_tiyn (the machine form that belongs in the
database, the API and the ФНО XML) on the input it hands to the PRINT
templates. Layers 1 and 2 could not see it, and the reason is structural:
they compare the PDF against the fixture, and the fixture hand-wrote the
money string. Fixture and PDF agree perfectly while both are wrong. The
fixture was the oracle, and an oracle you write by hand can encode the very
mistake you are gating for.

So a printed amount no longer has a hand-written expected form at all. It has
an INTEGER, declared as

    amount <json-path> <tiyn>

and the gate computes the printed string from it. `amount balance_tenge
45000000` can only ever mean `450 000,00`; there is no way to spell
`450000.00` in a directive. Enforced in both directions: a directive whose
path holds nothing in the fixture is rot, and a fixture scalar that LOOKS
like an amount but carries no directive is refused, so an amount cannot be
smuggled in undeclared. A fixture scalar in the machine form is refused
outright and named as this defect.

What this can catch: any printed amount whose FORMAT is wrong — in the
fixture, in the template, or lost/mangled by the engine. What it cannot: it
does not prove the SERVER formats with Money::format_tiyn_ru, because a
fixture is an input to the template, not an output of a controller. That gap
is closed on the C++ side, where the controllers' `input_snapshot` is
asserted to be the human form AND asserted not to be the machine form
(tests/integration/test_tax_api.cpp, test_payroll_api.cpp, test_hr_api.cpp).
Nor is `money()` below the C++ formatter itself — it is a port of it; the two
are pinned to each other by MoneyFormatRu.MatchesEveryAmountDirective
(tests/unit/test_money_format.cpp), which re-derives every directive in this
repo through the real Money::format_tiyn_ru.

Usage
-----
    check-render.py <template-dir> <fixture.json> <rendered.pdf>

    check-render.py templates/latex/payslip/v1 \\
        templates/latex/payslip/v1/fixtures/basic.json /tmp/out/main.pdf

Exit status: 0 = PASS, 1 = the render lost content, leaked template syntax,
drew a rule through its own text or overran the page, 2 = the gate itself
could not run (missing expectation file, no `pdftotext`/`pdftoppm`, unreadable
PDF). 2 is a failure too — silently skipping the check is how this class of
defect reached production once already.

Expectation files
-----------------
`<template-dir>/expected.txt` is REQUIRED, and `<template-dir>/fixtures/
<fixture>.expected.txt` is an optional per-fixture supplement for labels that
only one branch of the template prints. Format, one directive per line:

    # comment
    margin 18mm             page margin box, exactly one across both files
    amount <path> <tiyn>    the fixture's value at <path> is an AMOUNT, and
                            its printed form is derived from <tiyn> — an
                            integer number of tiyn — rather than written out.
                            Repeat the line once per element for an array
                            path (`rows[].a_debit`); the declared amounts and
                            the fixture's values at that path are compared as
                            multisets.
    unprinted kind          a fixture path the template deliberately never
                            prints (a control value like an order `kind`)
    known-defect <path>     content that SHOULD be printed and is not,
    known-defect <label>    because of an open bug. Excluded from the check
                            and re-announced on stderr on every single run,
                            passing or failing, so it cannot fade into the
                            background. Always write the reason and the
                            follow-up in a comment above it.
    <anything else>         a static label that MUST appear in the PDF

Every label is additionally required to occur verbatim in the template
source, so an expectation file cannot rot into asserting something the
template stopped printing years ago.
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import unicodedata

# --- Tolerances --------------------------------------------------------------
# Measured, not guessed. Rendering all ten templates x both fixtures (20 PDFs)
# and taking the worst overshoot of any word box past the declared margin box:
#
#   left   +0.00pt      right  +0.01pt      bottom  +0.00pt      top  +4.15pt
#
# Horizontally LaTeX justifies to exactly \textwidth, so the honest number is
# zero and 0.5pt is pure side-bearing allowance. This is STRICTER than the
# `Overfull \hbox` grep it replaces (which passed anything under 1.0pt) and
# still catches the 1.59pt overhang that pushed a ₸ off the invoice.
#
# The spike's prototype allowed 2.5pt on a word ending in a hyphen, because
# Typst hangs the hyphen into the margin by design (~1.7pt measured). That
# tolerance does NOT transfer: no word in any of the twenty LaTeX renders
# hangs a hyphen at all, so the exception is not carried over. Re-measure it
# when the engine changes rather than assuming.
SIDE_SLACK_PT = 0.5
# Vertically the reported box is the FONT's ascent/descent box, not the ink,
# and it sits above the top of the text body for every title line in the
# corpus (+3.92pt .. +4.15pt for \Large on a 10pt base). 6.0pt clears that
# artifact with headroom while staying far below a real overflow, which costs
# at least a full ~12pt line.
VERT_SLACK_PT = 6.0

MM_TO_PT = 72.0 / 25.4

# Characters the typesetter is allowed to substitute for the ones in the
# fixture. XeLaTeX turns ASCII quotes into typographic ones ("к" -> ”к”), and
# the various dashes are interchangeable at this level of comparison. Nothing
# else is folded: `%`, `&`, `#`, `_`, `{}`, `\`, `^`, `~`, `<>` were all
# verified to survive escaping and extraction byte-for-byte.
_FOLD = {c: '"' for c in '\u201c\u201d\u201e\u201f\u00ab\u00bb\u2033\u2018\u2019\u201a\u2032\u0060\u00b4\''}
_FOLD.update({c: "-" for c in "\u2010\u2011\u2012\u2013\u2014\u2015\u2212"})
_SPACES = "\u00a0\u2007\u2009\u202f\u2002\u2003\u2005"

WORD_RE = re.compile(
    r'<word xMin="([0-9.eE+-]+)" yMin="([0-9.eE+-]+)"'
    r' xMax="([0-9.eE+-]+)" yMax="([0-9.eE+-]+)">(.*)</word>')
PAGE_RE = re.compile(r'<page width="([0-9.]+)" height="([0-9.]+)"')
# A value that must survive on ONE line: digits, thousands separators and a
# decimal comma, i.e. every printed amount. A prose sentence may legitimately
# wrap; an amount that wraps has been mangled.
UNBREAKABLE_RE = re.compile(r"^\d[\d ,.]*$")
GEOMETRY_MARGIN_RE = re.compile(r"\\usepackage\[[^]]*?margin=([0-9.]+)mm[^]]*?\]\{geometry\}")
# The Typst equivalent of \usepackage[margin=18mm]{geometry}: the margin in
# `#set page(..., margin: 18mm)`. Same purpose — an expected.txt that claims a
# margin the template does not set would gate against the wrong box.
TYPST_MARGIN_RE = re.compile(r"#set\s+page\((?:[^()]|\([^()]*\))*?margin:\s*([0-9.]+)mm")
# A fixture scalar that is nothing but an amount in the form the templates
# print (`money()` below). Anything matching this MUST carry an `amount`
# directive — that is what stops a hand-written expected form from creeping
# back in. Free text that merely CONTAINS an amount ("Оклад 450 000,00
# тенге") does not match and stays free text.
PRINTED_MONEY_RE = re.compile(r"^\d{1,3}( \d{3})*,\d{2}$")
# The machine form — Ledger::format_tiyn's "450000.00". It belongs in the
# database, the API and the ФНО XML, and NEVER on a page. It is refused
# wherever it appears in a fixture, declared or not.
MACHINE_MONEY_RE = re.compile(r"^-?\d+\.\d{2}$")
AMOUNT_DIRECTIVE_RE = re.compile(r"^amount\s+(\S+)\s+(-?\d+)$")

# --- LAYER 3: what "template syntax" looks like once it is on the page -------
# Applied to whole extracted TOKENS (whitespace-separated), never to raw
# characters, because a token is the unit that can be attributed back to a
# fixture value. Each entry is (what to call it, pattern).
#
# The list is deliberately short and shape-based rather than a keyword
# dictionary: these are the sequences that CANNOT be produced by typesetting
# Kazakh/Russian business prose, in either engine.
SYNTAX_SIGNATURES = (
    # Typst's `#` sigil followed by an identifier: `#let`, `#if`, `#d.employer`,
    # `#panic`. `#1` (a LaTeX macro parameter, and a special-chars payload)
    # deliberately does NOT match — a digit cannot start an identifier.
    ("a Typst `#` sigil", re.compile(r"#[A-Za-z_][A-Za-z0-9_-]*")),
    # A Typst content block `[...]`. No shipped template prints a square
    # bracket and no shipped fixture carries one; the `] else [` leak prints
    # four of them.
    ("a content-block bracket", re.compile(r"[\[\]]")),
    # inja's delimiters, for the templates still on LaTeX.
    ("an inja delimiter", re.compile(r"\{\{|\}\}|\{%|%\}")),
    # A LaTeX control sequence that reached the page instead of being run.
    # `{2,}` and `[A-Za-z]` together spare the special-chars fixtures' `\б`
    # (Cyrillic) and a lone trailing backslash.
    ("a LaTeX control sequence", re.compile(r"\\[A-Za-z]{2,}")),
)
# Control words that leak WITHOUT a sigil, because the engine consumed the
# sigil before deciding the construct was over — `] else [` is exactly this.
# Kept narrow on purpose: `if`, `for`, `in` and `set` are ordinary English
# words, they buy nothing (a leaked `#if` already carries its sigil) and they
# would put the check one English-language line item away from crying wolf.
SYNTAX_KEYWORDS = frozenset((
    "else", "elif", "endif", "endfor", "endwhile",
    "let", "show", "import", "include", "context",
))
# Punctuation a leaked keyword may pick up from the text around it.
KEYWORD_TRIM = "[](){}.,;:!?\"'-/\\ "

# --- LAYER 4: rasterisation ---------------------------------------------------
# 200dpi is what the labour-contract conversion measured both variants of the
# signature rule at (551.9pt through the ink vs 574.9pt in white space), and it
# puts ~2.8px under a 1pt rule — thick enough that antialiasing cannot dissolve
# it, cheap enough that all 22 fixtures rasterise in well under a second.
RASTER_DPI = 200
# "Not white". A hairline rule antialiases to mid-grey rather than black, and a
# glyph's edge pixels do the same, so the threshold is generous; the check does
# not rely on it being tight, it relies on the ink being on BOTH sides.
INK_LEVEL = 200
# How far past the word box a band must reach on BOTH sides before it counts as
# a rule rather than as part of a glyph. An em dash is the widest single mark
# in this corpus and it still lives inside its own word box, so any overhang at
# all is disqualifying for glyph ink; 2pt is margin against rounding.
RULE_OVERHANG_PT = 2.0
# A struck-through line hits every word on it. Report enough to identify the
# rule and stop — a hundred identical findings are not a hundred defects.
MAX_STRIKE_FINDINGS = 8


def die(msg):
    sys.stderr.write("check-render: %s\n" % msg)
    sys.exit(2)


def fold(text):
    """Normalize for comparison: NFC, fold substitutable glyphs, drop soft
    hyphens, collapse runs of whitespace that are not line breaks."""
    text = unicodedata.normalize("NFC", text)
    text = "".join(_FOLD.get(c, " " if c in _SPACES else c) for c in text)
    text = text.replace("\u00ad", "")
    text = re.sub(r"[^\S\n]+", " ", text)
    return text


def flow_variants(text):
    """Two whole-document strings a multi-line value may be matched against:
    one that rejoins a hyphenated line break (`ос-\\nновании` -> `основании`)
    and one that keeps the hyphen (`Есеп-\\nтөлем` -> `Есеп-төлем`)."""
    joined = re.sub(r"-[ ]*\n[ ]*", "", text)
    kept = re.sub(r"-[ ]*\n[ ]*", "-", text)
    return (re.sub(r"\s+", " ", joined), re.sub(r"\s+", " ", kept))


def money(tiyn):
    """Render an integer amount in tiyn the way the templates print it:
    thousands separated by a space, two decimals after a comma.

    A port of Money::format_tiyn_ru (src/money/MoneyFormat.hpp), which is what
    the server writes into every printed document input. The separator is a
    plain ASCII space U+0020, exactly as in the C++ and in
    frontend/src/lib/money.ts::formatTiynRu — deliberately not
    toLocaleString/ICU, which yields U+00A0 and would stop the three from
    agreeing byte for byte. Kept honest against the C++ by
    MoneyFormatRu.MatchesEveryAmountDirective in tests/unit/test_money_format.cpp.

    Negatives get a leading `-` here because a raw `*_tiyn` fixture integer is
    unconstrained; the `amount` directive refuses them, matching
    format_tiyn_ru's own contract (the sign is a separate domain field)."""
    sign = "-" if tiyn < 0 else ""
    whole, frac = divmod(abs(int(tiyn)), 100)
    groups = []
    while whole >= 1000:
        whole, rest = divmod(whole, 1000)
        groups.append("%03d" % rest)
    groups.append(str(whole))
    return "%s%s,%02d" % (sign, " ".join(reversed(groups)), frac)


def walk(node, path, out):
    """Flatten the fixture into (json-path, value) scalars. Array elements
    collapse to a `[]` path so an expectation can address them as a set."""
    if isinstance(node, dict):
        for key, val in node.items():
            walk(val, "%s.%s" % (path, key) if path else key, out)
    elif isinstance(node, list):
        for val in node:
            walk(val, path + "[]", out)
    else:
        out.append((path, node))


JSON_PATH_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(\[\]|\.[A-Za-z_][A-Za-z0-9_]*)*$")


def read_expectations(paths):
    """Parse the expectation files into (labels, unprinted, defects, amounts,
    margin).

    Every entry carries its "file:line" so a failure can point at the exact
    line that made the claim."""
    labels, unprinted, defects, margin, margin_src = [], set(), [], None, None
    amounts = []
    for path in paths:
        with open(path, encoding="utf-8") as handle:
            for lineno, raw in enumerate(handle, 1):
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                where = "%s:%d" % (path, lineno)
                if line.startswith("margin "):
                    value = line[len("margin "):].strip()
                    if not value.endswith("mm"):
                        die("%s: margin must be given in mm, got %r" % (where, value))
                    if margin is not None:
                        die("%s: margin already declared at %s" % (where, margin_src))
                    margin, margin_src = float(value[:-2]) * MM_TO_PT, where
                elif line.startswith("amount "):
                    found = AMOUNT_DIRECTIVE_RE.match(line)
                    if not found:
                        die("%s: expected `amount <json-path> <integer tiyn>`, got %r "
                            "— the printed form is DERIVED from the integer and is "
                            "never written out here" % (where, line))
                    if not JSON_PATH_RE.match(found.group(1)):
                        die("%s: %r is not a fixture path" % (where, found.group(1)))
                    tiyn = int(found.group(2))
                    if tiyn < 0:
                        die("%s: %d tiyn is negative — Money::format_tiyn_ru refuses a "
                            "negative amount by contract; the sign belongs to a separate "
                            "domain field (e.g. fno_300's balance_kind)" % (where, tiyn))
                    amounts.append((found.group(1), tiyn, where))
                elif line.startswith("unprinted "):
                    unprinted.add(line[len("unprinted "):].strip())
                elif line.startswith("known-defect "):
                    defects.append((line[len("known-defect "):].strip(), where))
                else:
                    labels.append((line, where))
    if margin is None:
        die("no `margin <N>mm` declared in %s" % ", ".join(paths))
    # A known defect either names a fixture path — dropped from the value
    # sweep the same way an `unprinted` one is — or quotes a static label,
    # which is dropped from the PDF sweep but still cross-checked against the
    # template source below, since the template does try to print it.
    defect_labels = []
    for text, where in defects:
        if JSON_PATH_RE.match(text):
            unprinted.add(text)
        else:
            defect_labels.append((text, where))
            labels = [item for item in labels if item[0] != text]
    return labels, unprinted, defects, defect_labels, amounts, margin, margin_src


def check_amounts(scalars, amounts, fixture_path):
    """LAYER 0. Returns (failures, printed-by-path).

    `printed-by-path` maps every declared path to the printed strings derived
    from its directives, so LAYER 1 can report those values as amounts rather
    than as anonymous strings."""
    failures = []
    declared = {}
    for path, tiyn, where in amounts:
        declared.setdefault(path, []).append((tiyn, where))

    by_path = {}
    for path, value in scalars:
        by_path.setdefault(path, []).append(value)

    for path, entries in declared.items():
        want = sorted(money(tiyn) for tiyn, _ in entries)
        # Empty cells are dropped, not compared: an array path collapses a
        # whole column, and a reconciliation row with no debit carries
        # `"a_debit": ""`. Layer 1 skips empty values for the same reason.
        # A cell that LOST its amount still fails — `want` then has one more
        # entry than `got`.
        got = sorted(v for v in by_path.get(path, []) if isinstance(v, str) and v.strip())
        where = ", ".join(sorted({w for _, w in entries}))
        if path not in by_path:
            failures.append(
                "EXPECTATION ROT  amount directive for %s (%s) but %s has no such "
                "value — the field was renamed or removed"
                % (path, where, fixture_path))
        elif want != got:
            failures.append(
                "FIXTURE FORMAT  %s (%s) declares %s tiyn, i.e. %s, but %s says %s. "
                "An amount's printed form is DERIVED from the integer here — if the "
                "integer is right, the fixture is what has to change"
                % (path, where,
                   " + ".join(str(t) for t, _ in entries),
                   " / ".join('"%s"' % s for s in want), fixture_path,
                   " / ".join('"%s"' % s for s in got) if got else "nothing"))
        else:
            by_path[path] = want
    for path, entries in declared.items():
        # The fixture may ALSO carry the integer (invoice's total_tiyn, the
        # счёт-фактура's totals.*_tiyn — the caller-authored forms, where the
        # server derives the string from exactly that field). Two hand-written
        # integers for one amount can disagree; they may not.
        sibling = by_path.get(path + "_tiyn", [])
        mine = sorted(t for t, _ in entries)
        theirs = sorted(v for v in sibling if isinstance(v, int) and not isinstance(v, bool))
        if theirs and mine != theirs:
            failures.append(
                "FIXTURE FORMAT  amount %s declares %s tiyn but the fixture's own "
                "%s_tiyn says %s — the document would be built from the integer, so "
                "the directive is the one that is wrong"
                % (path, mine, path, theirs))

    # The other direction: nothing that looks like an amount may go undeclared,
    # or the hand-written expected form is back.
    for path, value in scalars:
        if not isinstance(value, str):
            continue
        if MACHINE_MONEY_RE.match(value):
            # Deliberately NOT converted back into an amount to show the right
            # string: parsing a formatted money string into a number is
            # forbidden by the phase invariant, in a gate as much as in the
            # server. The fix is to write the integer down, not to translate.
            failures.append(
                'MACHINE MONEY FORM  %s = "%s" in %s. That is Ledger::format_tiyn — '
                "the form for journal_lines.amount, the API and the ФНО XML, and it "
                "must never be printed. Replace it with an `amount %s <tiyn>` "
                "directive and let the gate derive the printed string"
                % (path, value, fixture_path, path))
        elif PRINTED_MONEY_RE.match(value) and path not in declared:
            failures.append(
                'UNDECLARED AMOUNT  %s = "%s" in %s looks like a printed amount but '
                "has no `amount %s <tiyn>` directive. Every printed amount is derived "
                "from its integer so a fixture cannot encode a wrong money format"
                % (path, value, fixture_path, path))
    return failures, by_path


def template_source(template_dir):
    """The template body, engine-agnostically: whichever source file is there."""
    for name in ("template.tex", "template.typ"):
        path = os.path.join(template_dir, name)
        if os.path.exists(path):
            with open(path, encoding="utf-8") as handle:
                return path, handle.read()
    die("no template.tex or template.typ in %s" % template_dir)
    return None, None  # unreachable; keeps linters happy


def pdf_text(pdf, mode):
    cmd = ["pdftotext", mode, pdf, "-"]
    try:
        done = subprocess.run(cmd, capture_output=True, check=False)
    except OSError as exc:
        die("cannot run %s: %s" % (" ".join(cmd), exc))
    if done.returncode != 0:
        die("%s failed (exit %d): %s" % (" ".join(cmd), done.returncode,
                                         done.stderr.decode("utf-8", "replace").strip()))
    return done.stdout.decode("utf-8", "replace")


def check_content(pdf, fixture_path, scalars, labels, unprinted, defect_labels,
                  declared_tiyn, template_path, source):
    """LAYER 1. Returns a list of human-readable failures.

    Both poppler orderings are consulted, and content counts as present if it
    survives in either. `-layout` reconstructs the visual page, which is what
    a reader sees, but it interleaves a wrapped table cell with the cell to
    its right ("… в ЕНПФ  10 500,00 / (ОПВР)"); `-raw` emits the text in the
    order the engine drew it, which keeps that cell contiguous but scrambles
    columns. Requiring a match in both would fail healthy documents; requiring
    it in neither is what "lost" means."""
    failures = []
    texts = [fold(pdf_text(pdf, mode)) for mode in ("-layout", "-raw")]
    lines = [ln for text in texts for ln in text.split("\n") if ln.strip()]
    flow = [variant for text in texts for variant in flow_variants(text)]
    folded_source = re.sub(r"\s+", " ", fold(source))

    def present(needle):
        return any(needle in variant for variant in flow)

    def on_one_line(needle):
        return any(needle in ln for ln in lines)

    for label, where in labels + defect_labels:
        needle = re.sub(r"\s+", " ", fold(label))
        if needle not in folded_source:
            failures.append(
                'EXPECTATION ROT  label "%s" (%s) does not occur in %s — the '
                "template stopped printing it, or the expectation is a typo"
                % (label, where, template_path))
        elif (label, where) in defect_labels:
            continue
        elif not present(needle):
            failures.append(
                'CONTENT LOST  static label "%s" (%s) is printed by the '
                "template but is not in the PDF text" % (label, where))

    checked = 0
    remaining = dict(declared_tiyn)
    for path, value in scalars:
        if path in unprinted or value is None:
            continue
        if isinstance(value, bool):
            continue
        if isinstance(value, (int, float)):
            if not path.split(".")[-1].endswith("_tiyn"):
                continue
            # An amount is never printed as a raw integer: the server formats
            # it, and THAT string is what has to be on the page.
            printed, kind = money(value), "amount %s = %d tiyn, printed as" % (path, value)
        elif any(money(t) == value for t in remaining.get(path, ())):
            # LAYER 0 has already proven this string IS money(tiyn); report it
            # as the amount it is, so a failure names the integer a reader can
            # go and check against the payslip/declaration. Paired by VALUE,
            # not by position: an array path collapses several directives onto
            # one path and the fixture's order is not theirs.
            tiyn = next(t for t in remaining[path] if money(t) == value)
            remaining[path].remove(tiyn)
            printed, kind = value, "amount %s = %d tiyn, printed as" % (path, tiyn)
        else:
            if not value.strip():
                continue
            printed, kind = value, "value %s =" % path
        checked += 1
        needle = re.sub(r"\s+", " ", fold(printed))
        if UNBREAKABLE_RE.match(printed.strip()):
            if on_one_line(needle):
                continue
            failures.append(
                'CONTENT LOST  %s "%s" is in the fixture but not in the PDF text%s'
                % (kind, printed,
                   " on any single line — it survives only broken across a line "
                   "break, i.e. the column that holds it was mangled"
                   if present(needle) else ""))
        elif not present(needle):
            failures.append('CONTENT LOST  %s "%s" is in the fixture but not in the PDF text'
                            % (kind, printed))
    return failures, len(labels), checked


def word_boxes(pdf):
    """`pdftotext -bbox`, parsed once: a list of (page width, page height,
    [(xMin, yMin, xMax, yMax, word), ...]) in page order.

    Layers 2 and 4 both need it, and running poppler twice over the same PDF
    to build the same list twice is how the two would drift apart."""
    pages = []
    for line in pdf_text(pdf, "-bbox").splitlines():
        found = PAGE_RE.search(line)
        if found:
            pages.append((float(found.group(1)), float(found.group(2)), []))
            continue
        found = WORD_RE.search(line)
        if not found or not pages:
            continue
        pages[-1][2].append(tuple(float(found.group(i)) for i in range(1, 5))
                            + (found.group(5),))
    return pages


def check_geometry(pages, margin_pt, margin_src):
    """LAYER 2. Returns (failures, words seen)."""
    failures, words = [], 0
    for page_no, (page_w, page_h, page_words) in enumerate(pages, 1):
        for x_min, y_min, x_max, y_max, word in page_words:
            words += 1
            for edge, over, limit, slack in (
                    ("LEFT", margin_pt - x_min, margin_pt, SIDE_SLACK_PT),
                    ("RIGHT", x_max - (page_w - margin_pt), page_w - margin_pt, SIDE_SLACK_PT),
                    ("TOP", margin_pt - y_min, margin_pt, VERT_SLACK_PT),
                    ("BOTTOM", y_max - (page_h - margin_pt), page_h - margin_pt, VERT_SLACK_PT)):
                if over > slack:
                    failures.append(
                        'OFF-MARGIN  word "%s" on page %d crosses the %s margin by %.2fpt '
                        "(box x=[%.1f,%.1f] y=[%.1f,%.1f], %s limit %.1fpt on a %.1fx%.1fpt page, "
                        "margin %.1fpt declared at %s, tolerance %.1fpt)"
                        % (word, page_no, edge, over, x_min, x_max, y_min, y_max, edge.lower(),
                           limit, page_w, page_h, margin_pt, margin_src, slack))
    return failures, words


def check_leaked_syntax(pdf, fixture_path, scalars, labels, defect_labels):
    """LAYER 3. Returns (failures, tokens screened).

    Every whitespace-separated token of the extracted text is matched against
    SYNTAX_SIGNATURES / SYNTAX_KEYWORDS. A token that matches is a finding
    ONLY if nothing accounts for it, where "accounts for it" means the token
    occurs inside a fixture value or inside a declared static label.

    That attribution rule is the whole design. `#panic("x")` and
    `#read("/etc/passwd")` are shipped on purpose in every `special-chars`
    fixture — they are the test that a value is DATA and never source — so a
    check that flagged Typst syntax on sight would fail six healthy documents
    on day one and be switched off by the end of the week. It is not the
    characters that are wrong, it is characters with no provenance.

    Values are joined with NUL so a token cannot be attributed to two
    different fixture values it happens to straddle.

    The escape hatch is the same one the rest of this file uses and it is
    deliberately not a new one: a template that genuinely prints a bracket
    declares that text in `expected.txt`, where it is ALSO cross-checked
    against the template source by LAYER 1. Nothing can be silenced without
    writing down, in a tracked file, what is being silenced."""
    corpus = "\x00".join(
        [re.sub(r"\s+", " ", fold(v)) for _p, v in scalars if isinstance(v, str)]
        + [re.sub(r"\s+", " ", fold(text)) for text, _w in labels + defect_labels])

    tokens = set()
    lines = []
    for mode in ("-layout", "-raw"):
        text = fold(pdf_text(pdf, mode))
        tokens.update(text.split())
        lines.extend(ln.strip() for ln in text.split("\n") if ln.strip())

    failures = []
    for token in sorted(tokens):
        kind = None
        for name, pattern in SYNTAX_SIGNATURES:
            if pattern.search(token):
                kind = name
                break
        if kind is None and token.strip(KEYWORD_TRIM).lower() in SYNTAX_KEYWORDS:
            kind = "a control keyword"
        if kind is None:
            continue
        # `token[:-1]` is the second chance for a value the typesetter
        # hyphenated at a line break: the fragment that reached the page is
        # then the start of a fixture value plus a hyphen the fixture never
        # had. Attribution must survive line breaking or it would fail
        # documents for being long.
        if token in corpus or (token.endswith("-") and token[:-1] in corpus):
            continue
        context = next((ln for ln in lines if token in ln), token)
        if len(context) > 110:
            at = context.find(token)
            context = "..." + context[max(0, at - 40):at + 70] + "..."
        failures.append(
            'LEAKED SYNTAX  "%s" is printed in the PDF and is %s, but nothing in %s '
            "or in the expectation files contains it — so it did not arrive as DATA, "
            "it escaped from the template and got typeset. On the page: %r. If the "
            "template really is meant to print it, declare it as a static label; if "
            "a fixture value really carries it, the value is what has to say so"
            % (token, kind, fixture_path, context))
    return failures, len(tokens)


def read_pgm(data, source):
    """Parse a binary PGM (`P5`) — what `pdftoppm -gray` writes — into
    (width, height, pixels). One byte per pixel, 0 = black, 255 = white."""
    if data[:2] != b"P5":
        die("%s did not produce a binary PGM (got %r)" % (source, data[:16]))
    fields, pos = [], 2
    while len(fields) < 3:
        if data[pos:pos + 1] == b"#":            # PGM comments run to EOL
            pos = data.index(b"\n", pos) + 1
        elif data[pos:pos + 1].isspace():
            pos += 1
        else:
            end = pos
            while not data[end:end + 1].isspace():
                end += 1
            fields.append(int(data[pos:end]))
            pos = end
    pos += 1                                      # exactly one whitespace byte
    width, height, maxval = fields
    if maxval != 255:
        die("%s wrote a %d-level PGM; this reader assumes 8-bit" % (source, maxval))
    if len(data) - pos < width * height:
        die("%s wrote a truncated PGM (%d bytes for %dx%d)"
            % (source, len(data) - pos, width, height))
    return width, height, data[pos:pos + width * height]


def rasterize(pdf, page_no, workdir):
    """One page of @p pdf as an 8-bit greyscale raster at RASTER_DPI."""
    prefix = os.path.join(workdir, "page")
    cmd = ["pdftoppm", "-gray", "-r", str(RASTER_DPI),
           "-f", str(page_no), "-l", str(page_no), "-singlefile", pdf, prefix]
    try:
        done = subprocess.run(cmd, capture_output=True, check=False)
    except OSError as exc:
        die("cannot run %s: %s — the ink check cannot be skipped, install "
            "poppler-utils (the worker-render-check image has it)"
            % (" ".join(cmd), exc))
    if done.returncode != 0:
        die("%s failed (exit %d): %s" % (" ".join(cmd), done.returncode,
                                         done.stderr.decode("utf-8", "replace").strip()))
    with open(prefix + ".pgm", "rb") as handle:
        return read_pgm(handle.read(), " ".join(cmd))


def check_struck_through(pdf, pages):
    """LAYER 4. Returns a list of human-readable failures.

    A word is struck through when some row inside its box is INK all the way
    across the box AND at least RULE_OVERHANG_PT past both of its sides — a
    rule, since no glyph reaches outside its own word box — and the word's own
    ink appears both above and below that band IN THE SAME PIXEL COLUMNS.

    The column condition is what keeps this quiet on healthy documents. A
    signature line under a name, a `\\midrule` under a table header and an
    underline all have ink on one side of the rule only. Requiring the SAME
    columns to be inked above and below additionally rejects the case where
    the ink "above" is really the descender of the previous line clipping into
    this word's ascent box, which is common in tight tables. Measured over all
    22 shipped fixtures (10 templates, both engines): zero findings."""
    failures = []
    with tempfile.TemporaryDirectory() as workdir:
        for page_no, (page_w, page_h, page_words) in enumerate(pages, 1):
            if not page_words:
                continue
            width, height, px = rasterize(pdf, page_no, workdir)
            scale_x, scale_y = width / page_w, height / page_h
            overhang = max(1, int(round(RULE_OVERHANG_PT * scale_x)))
            for x_min, y_min, x_max, y_max, word in page_words:
                x0 = max(0, int(x_min * scale_x))
                x1 = min(width - 1, int(x_max * scale_x + 0.999))
                y0 = max(0, int(y_min * scale_y))
                y1 = min(height - 1, int(y_max * scale_y + 0.999))
                if x1 <= x0 or y1 <= y0:
                    continue
                ex0, ex1 = max(0, x0 - overhang), min(width - 1, x1 + overhang)
                # The word box, row by row, and the rows a rule could be in.
                rows = {y: px[y * width + x0:y * width + x1 + 1]
                        for y in range(y0, y1 + 1)}
                seed = next((y for y in range(y0, y1 + 1)
                             if max(px[y * width + ex0:y * width + ex1 + 1]) < INK_LEVEL),
                            None)
                if seed is None:
                    continue
                # Grow the seed row into the rule's full thickness, including
                # the antialiased rows that flank it, so they are not mistaken
                # for the word's own ink.
                top = seed
                while top - 1 >= y0 and max(rows[top - 1]) < INK_LEVEL:
                    top -= 1
                bottom = seed
                while bottom + 1 <= y1 and max(rows[bottom + 1]) < INK_LEVEL:
                    bottom += 1
                above = set()
                for y in range(y0, top):
                    above.update(i for i, v in enumerate(rows[y]) if v < INK_LEVEL)
                if not above:
                    continue
                below = set()
                for y in range(bottom + 1, y1 + 1):
                    below.update(i for i, v in enumerate(rows[y]) if v < INK_LEVEL)
                shared = above & below
                if not shared:
                    continue
                if len(failures) >= MAX_STRIKE_FINDINGS:
                    failures.append(
                        "RULE THROUGH TEXT  ... and more words on page %d; only the "
                        "first %d are listed, one rule strikes every word on its line"
                        % (page_no, MAX_STRIKE_FINDINGS))
                    return failures
                failures.append(
                    'RULE THROUGH TEXT  word "%s" on page %d has a rule drawn THROUGH '
                    "it: a solid band %.2fpt thick at y=%.1f..%.1fpt runs the full "
                    "width of the word box (x=[%.1f,%.1f] y=[%.1f,%.1f]) and past both "
                    "sides of it, with the word's own ink above AND below it in %d "
                    "shared pixel column(s). `pdftotext` cannot see this — the glyphs "
                    "extract perfectly and every other layer passes. Rasterised at "
                    "%ddpi. In Typst a `line()` inside a grid or table contributes NO "
                    "height, so its row collapses and the rule lands on the rows above"
                    % (word, page_no, (bottom - top + 1) / scale_y, top / scale_y,
                       (bottom + 1) / scale_y, x_min, x_max, y_min, y_max,
                       len(shared), RASTER_DPI))
    return failures


def main(argv):
    if len(argv) != 4:
        die("usage: check-render.py <template-dir> <fixture.json> <rendered.pdf>")
    template_dir, fixture_path, pdf = argv[1], argv[2], argv[3]
    for path in (template_dir, fixture_path, pdf):
        if not os.path.exists(path):
            die("no such path: %s" % path)

    expectation_files = [os.path.join(template_dir, "expected.txt")]
    if not os.path.exists(expectation_files[0]):
        die("%s is missing — every template version must declare the static "
            "labels it prints and its page margin, or the gate cannot tell a "
            "lost column from a template that never had one"
            % expectation_files[0])
    per_fixture = os.path.join(
        template_dir, "fixtures",
        os.path.basename(fixture_path)[:-len(".json")] + ".expected.txt")
    if os.path.exists(per_fixture):
        expectation_files.append(per_fixture)

    (labels, unprinted, defects, defect_labels, amounts,
     margin_pt, margin_src) = read_expectations(expectation_files)
    template_path, source = template_source(template_dir)

    # Cross-check the declared margin against the template's own page setup,
    # whichever engine it is written for.
    declared = GEOMETRY_MARGIN_RE.search(source) or TYPST_MARGIN_RE.search(source)
    if declared and abs(float(declared.group(1)) * MM_TO_PT - margin_pt) > 0.01:
        die("%s declares margin=%smm but %s says %s — fix the expectation file"
            % (template_path, declared.group(1), margin_src,
               "%.4gmm" % (margin_pt / MM_TO_PT)))

    name = "%s %s" % (template_dir.rstrip("/"), os.path.basename(fixture_path))
    with open(fixture_path, encoding="utf-8") as handle:
        fixture = json.load(handle)
    scalars = []
    walk(fixture, "", scalars)

    # LAYER 0 runs FIRST and its findings are reported first: if the fixture's
    # own idea of an amount is wrong, everything layer 1 says about that amount
    # is noise.
    oracle, _ = check_amounts(scalars, amounts, fixture_path)
    declared_tiyn = {}
    for path, tiyn, _where in amounts:
        declared_tiyn.setdefault(path, []).append(tiyn)
    content, label_count, value_count = check_content(
        pdf, fixture_path, scalars, labels, unprinted, defect_labels,
        declared_tiyn, template_path, source)
    # LAYER 3 sits with the content findings: a construct that leaked into the
    # body is a defect in what the page SAYS, not in where it sits.
    syntax, tokens = check_leaked_syntax(
        pdf, fixture_path, scalars, labels, defect_labels)
    content = oracle + content + syntax
    pages = word_boxes(pdf)
    geometry, words = check_geometry(pages, margin_pt, margin_src)
    # LAYER 4 needs the same word boxes and a raster of the same pages.
    geometry += check_struck_through(pdf, pages)
    if words == 0:
        content.append("EMPTY PDF  pdftotext extracted no words at all from %s" % pdf)

    # Announced on every run, passing or failing: a suppression that is only
    # visible in a diff is a suppression nobody will ever remove.
    for text, where in defects:
        print('check-render: KNOWN DEFECT, NOT CHECKED in %s: "%s" (%s)'
              % (name, text, where), file=sys.stderr)

    if content or geometry:
        print("FAIL %s (%s)" % (name, pdf), file=sys.stderr)
        for failure in content + geometry:
            print("  " + failure, file=sys.stderr)
        return 1
    print("PASS %s — %d label(s), %d value(s) (%d amount(s) derived from tiyn), "
          "%d token(s) screened for leaked template syntax, %d word box(es) inside "
          "the %.1fpt margin box and none struck through by a rule%s"
          % (name, label_count, value_count, len(amounts), tokens, words, margin_pt,
             ", %d known defect(s) NOT checked" % len(defects) if defects else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
