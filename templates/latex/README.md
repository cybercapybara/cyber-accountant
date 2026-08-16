# LaTeX document templates (docgen)

Each subdirectory is one **template slug** (a document type: `invoice`,
`avr`, `waybill`, `tax_invoice`, `reconciliation`, ...). `Docgen::TemplateRegistry`
(`src/docgen/TemplateRegistry.hpp`) scans this tree at render time — nothing
here is baked into a database or a compiled registry, so adding a template is
just adding files in the right layout.

## Layout

```
templates/latex/<slug>/v<N>/
├── template.tex        # inja template — LaTeX source with {{ }} / {% %} placeholders
├── schema.json          # JSON Schema (draft-07) the input JSON must satisfy
├── expected.txt         # page margin + the static labels this template prints
└── fixtures/
    ├── *.json           # sample inputs, rendered by scripts/render-templates.sh
    └── *.expected.txt   # optional: labels only THIS fixture's data switches on
```

- `<slug>` is the document type, matching `documents.doc_type` /
  `documents.template_slug` (migrations/010_documents.sql).
- `v<N>` is a plain non-negative integer version directory (`v1`, `v2`, ...).
  `TemplateRegistry::latest(slug)` always picks the **highest** `N` — ship a
  new version alongside the old one rather than editing it in place, since a
  document already rendered from `v1` keeps that version in its
  `template_version` snapshot (`Ledger::Document::template_version`).
- `fixtures/*.json` are sample inputs used ONLY by
  `scripts/render-templates.sh` (the `template-render` CI job) to smoke-test
  that the template still compiles under a real XeLaTeX **and that the PDF it
  produced still says everything it was supposed to say** (see "The render
  gate" below) — they are not read by the render job itself.
- `expected.txt` is **required**: it declares the page margin box and the
  static labels the template prints. Without it the gate refuses to run, since
  it cannot tell a lost column from a template that never had one.

`templates/latex/invoice/v1/` is the reference implementation.

## Writing a template

- Use inja's default delimiters: `{{ expr }}` for expressions, `{% if %}` /
  `{% for %}` / `{% endif %}` / `{% endfor %}` for control flow. See
  [pantor/inja](https://github.com/pantor/inja) for the full syntax
  (a Jinja2 subset).
- **Comments are `((# ... #))`, NOT inja's default `{# ... #}`.** Every
  shipped template defines `\newcommand{\field}[1]{\textbf{#1}}` (or a
  similar one-arg macro) — `#1`/`#2`/... macro parameters are routine and
  unavoidable in LaTeX, and `{#1}` starts with the exact two characters
  inja treats as "open comment" by default. With no matching `#}` in the
  file, that silently turns the rest of the template into an unterminated
  comment and the render fails with a parser error at EOF ("expected
  comment close, got '<eof>'"). `Docgen::render_tex` (`src/docgen/Renderer.hpp`)
  remaps the comment markers to `((#` / `#))` via inja's
  `Environment::set_comment(open, close)` — a sequence that cannot occur in
  valid LaTeX — specifically so template authors never have to dodge this.
  Write `((# like this #))` for an inja-only comment that doesn't end up in
  the rendered `.tex`; do not use `{# #}`, it is not special anymore.
  (`{% %}` was checked too: every `template.tex` shipped as of this writing
  was grepped for a literal, non-inja `{%` — LaTeX has no construct that
  produces one on its own, since `%` starts a LaTeX line comment and would
  need a `{` immediately before it to collide, which none of these
  templates do — so `{% %}` was left at its inja default. If a future
  template ever needs a literal `{%` in its LaTeX body, re-run that grep and
  remap `set_statement` the same way.)
- Every **string** value in the input JSON is automatically escaped for
  LaTeX (`Docgen::escape_latex`, `src/docgen/LatexEscape.hpp`) before
  substitution — `\ { } $ & # ^ _ % ~ < >` all become their safe LaTeX
  form. Do **not** escape values yourself in `schema.json`/fixtures; numbers
  and booleans pass through unescaped.
- **Branch on `enum` fields, and never print one.** There is exactly one
  exception to the escaping above: a string leaf whose schema node pins it to
  an `enum`, and whose value is one of those literals, reaches the template
  **raw**. It has to — a value the template COMPARES is not text being
  typeset, and escaping it first is what silently broke ФНО 300.00's closing
  line and two whole `hr_order` kinds (`{% if balance_kind == "to_pay" %}`
  was being evaluated against `to\_pay`). So:
  - write control comparisons **only** against `enum`-constrained fields —
    `{% if kind == "business_trip" %}` works because `hr_order`'s schema
    pins `kind`; the same comparison against a free-text field would be
    tested against the escaped value and is a bug;
  - do **not** `{{ }}` an `enum` field into the document. `to_pay` typeset
    raw is a LaTeX error (bare `_` in text mode), and a control identifier
    is not something a reader should see — print a human label from the
    branch instead. Declare each one `unprinted <path>` in `expected.txt`.
    `ShippedTemplatesTest.NeverPrintAnEnumPinnedField`
    (`tests/unit/test_template_registry.cpp`) fails if a template does.

  Nothing else stops being escaped: every free-text field — a counterparty
  name, a line item, a director's name — still goes through `escape_latex`,
  because none of them is `enum`-pinned. See `src/docgen/Renderer.hpp`'s file
  header for the full argument, including why this adds no injection surface
  (the only bytes that now reach the `.tex` unescaped are byte-exact copies of
  literals this repo wrote in its own `schema.json`; a caller can select among
  them, never contribute to them).
- `schema.json` is the contract: `RenderJob` (and `render-templates.sh`)
  reject any input that doesn't satisfy it — required fields, types, string
  patterns (e.g. a `date` field constrained to `DD.MM.YYYY`) all belong
  there, not in ad-hoc template logic.
- Fonts: templates render under XeLaTeX with `polyglossia` + `fontspec` for
  Cyrillic/Kazakh text (see `docker/Dockerfile`'s worker stage for the
  installed TeX Live + font packages). Stick to `\setmainfont{Noto Sans}` (or
  another font actually installed in the worker image) rather than assuming
  a font is present.

### Tables start their own paragraph

A `\textwidth`-wide `tabularx`/`tabular` is a single unbreakable box. If the
block of text above it has not ended its paragraph, LaTeX typesets that box
**inline**, continuing the last line — and the box then hangs off the right
edge of the page. XeLaTeX reports this only as `Overfull \hbox (...pt too
wide)`, a *warning*: the compile still exits 0 and a PDF is still produced,
just with the right-hand columns pushed off the paper. This shipped in v0.3.0
(ФНО 910.00 printed `10 000` where the amount was `10 000 000,00`; the payslip
lost its entire amounts column *and* the column's static header).

So, above every full-width table:

- Terminate the block with an **unconditional `\par`**, written immediately
  before the table's own `\vspace`. Never put the terminator inside an
  `{% if %}` — an optional field that is absent then takes the paragraph break
  with it, which is exactly how `invoice`/`tax_invoice` acquired the same bug.
- In a block whose **last** line is optional, end *every* line with `\par`
  rather than `\\`. `\par` is idempotent, so the extra one contributed by a
  skipped `{% if %}` line (a skipped block still leaves its newline behind, and
  two newlines are a paragraph break) is a harmless no-op. `\\` is not
  idempotent in either direction: a trailing `\\` immediately before a `\par`
  emits a spurious blank line, and a leading `\\` right after one is the hard
  error `There's no line here to end`.
- Each `\par`-terminated line needs its own `\noindent`, since it is now a
  paragraph rather than a `\\`-continuation.
- Long Kazakh prose has no hyphenation patterns under polyglossia's `russian`
  setup and can overhang by a few points on its own; `\emergencystretch=2em`
  in the preamble (see `labor_contract`) lets TeX loosen the line instead.

`scripts/render-templates.sh` still fails the `template-render` CI job on any
overfull `\hbox` over `OVERFULL_MAX_PT` (default 1.0pt) — but that grep is no
longer the gate, only a LaTeX-only tripwire for overflow in material that
produces no extractable text (a `\hrulefill` signature rule, an `\hline`).
The gate is "The render gate" below, which measures the PDF instead of the
engine's opinion of it and would have caught the v0.3.0 defect even if XeLaTeX
had reported nothing at all.

## Testing a template

- Unit tests (`tests/unit/test_template_registry.cpp`,
  `tests/unit/test_latex_escape.cpp`) cover discovery/validation/escaping
  without invoking XeLaTeX at all.
- `tests/integration/test_render_job.cpp` exercises the full `docgen.render`
  job pipeline with a stubbed `DOCGEN_LATEX_CMD` (no real TeX Live needed).
- The **real** XeLaTeX compile only runs via
  `./scripts/render-templates.sh` against every `fixtures/*.json` under this
  tree — locally on a machine with TeX Live installed, or via the
  `template-render` CI job on the worker image. Add a fixture for every new
  template (and every new required/optional field combination worth
  covering) so a broken template fails there instead of in production.
  That script does far more than check the exit code — see below.

## The render gate

`scripts/check-render.py` runs once per fixture, on the PDF the render just
produced, and knows nothing about which engine produced it (it is written to
survive the Typst migration unchanged — see
`.superpowers/sdd/typst-migration-spike.md`). Five layers:

0. **Oracle.** Every printed amount is **derived from an integer number of
   tiyn**, declared `amount <path> <tiyn>`, using the same algorithm as
   `Money::format_tiyn_ru` (`src/money/MoneyFormat.hpp`). A fixture may not
   hand-write the printed form of an amount; one that looks like an amount
   and carries no directive is refused, and one in the machine form
   (`450000.00` — `Ledger::format_tiyn`, which belongs in the database, the
   API and the ФНО XML) is refused and named. See "Amounts are declared as
   integers" below.
1. **Content.** Every scalar in the fixture, and every static label in
   `expected.txt`, must appear in the PDF's extracted text (`pdftotext
   -layout` and `-raw`; content counts as present if it survives in either).
   Amounts are compared **in the form they are printed** — thousands
   separated by a space, decimal comma — so a `*_tiyn` integer is required as
   `12 345,67`, never as `1234567`, and an amount that only survives split
   across a line break counts as lost. Values the template deliberately never
   prints (an order's `kind`, a period's `half`) must be declared
   `unprinted <path>`, so the default for a new field is "must be printed".
2. **Geometry.** Every word box from `pdftotext -bbox` must lie inside the
   `margin <N>mm` box declared in `expected.txt` — 0.5pt of slack sideways
   (measured: the worst healthy overshoot across all twenty renders is
   0.01pt), 6.0pt vertically, which is the font-ascent artifact of a `\Large`
   title (measured: 3.92–4.15pt) and still far below a real overflow.
3. **Syntax.** No extracted token may look like **template syntax** — a Typst
   `#` sigil (`#let`, `#if`), a content-block bracket, an inja delimiter, a
   LaTeX control sequence, a bare control keyword (`else`, `endif`, …) —
   unless something accounts for it: the token must occur inside a fixture
   value or inside a declared label. That is the whole point. The
   `special-chars` fixtures deliberately ship `#panic("x")`,
   `#read("/etc/passwd")`, `\б` and `{б}` **as data**, and they must keep
   passing; syntax with provenance is fine, syntax with none escaped from the
   template. Catches a control construct that got typeset instead of run.
4. **Ink.** No word may have a rule drawn **through** it. The page is
   rasterised at 200dpi and a word fails when a solid band runs the full
   width of its box and past both sides (so it is a rule, not a glyph) with
   the word's own ink above **and** below it in the same pixel columns. The
   column test is what keeps this quiet: an underline, a `\midrule` under a
   header and a signature line all have ink on one side only. Measured across
   all 22 fixtures and both engines: zero findings.

Layers 3 and 4 exist because layers 0–2 only ever ask whether something is
**missing**. The two defects the labour-contract conversion rebuilt both ADD
ink and both passed with exit 0: `] else [` split across lines printed the
word `else` and its entire branch into the contract body (291 word boxes
instead of 280, all inside the margins), and a `line()` in a grid — which
contributes no height — struck a signature rule through `БСН/БИН` and
`ЖСН/ИИН`. Eight templates are still to convert.

`expected.txt` format, one directive per line:

```
margin 18mm                    # required exactly once
Сомасы, ₸ / Сумма, ₸           # a static label that must reach the PDF
amount balance_tenge 45000000  # a printed amount, as INTEGER TIYN
unprinted kind                 # a fixture value the template never prints
known-defect <path|label>      # printed nowhere because of an open BUG:
                               # excluded, but re-announced on stderr on
                               # every run so it cannot be forgotten. No
                               # template uses one today.
```

Every label must also occur **verbatim in `template.tex`**; a label that does
not is reported as `EXPECTATION ROT`, so the file cannot drift into asserting
something the template stopped printing.

When you add or change a template, add its labels here — and when the gate
fails, read the finding: it names the fixture, and the exact value, label or
word that was lost.

### Amounts are declared as integers

`amount <path> <tiyn>` lives in the **per-fixture** `fixtures/<name>.expected.txt`
(a template-level `expected.txt` has no one fixture to check against). Repeat
the line once per element for an array path — `amount items[].price 350000` —
and the declared amounts are compared with the fixture's values at that path
as multisets; empty cells (`"a_debit": ""`) are ignored.

Why the integer and not the string: v0.4.2 shipped ФНО declarations, payslips
and labour contracts printing `450000.00` where the document must read
`450 000,00`, and the gate stayed green through all of it. Layers 1 and 2
compare the PDF against the fixture — and the fixture hand-wrote the money
string, so fixture and PDF agreed while both were wrong. `amount
balance_tenge 45000000` can only ever mean `450 000,00`; there is no way to
spell the machine form in a directive.

What it does **not** prove is that the SERVER formats with
`Money::format_tiyn_ru` — a fixture is an input to the template, not the
output of a controller. That is pinned separately, on the controllers'
`input_snapshot`, in `tests/integration/test_tax_api.cpp`,
`test_payroll_api.cpp` and `test_hr_api.cpp` (each asserts the human form
**and** asserts the value is not the machine form). And the gate's `money()`
is a Python port of the C++ formatter: `MoneyFormatRu.MatchesEveryAmountDirective`
(`tests/unit/test_money_format.cpp`) re-derives every directive in this repo
through the real `Money::format_tiyn_ru`, so the two cannot drift apart.

Amounts a **client** authors rather than the server (the `reconciliation`
columns, and every `items[]` line of the первичка — see
`Docgen::InputPolicy::derived_amount_for`, which only derives the TOTAL row)
are declared the same way. The gate pins the format of the fixture; it does
not and cannot pin what an API caller sends.

### The self-test

`scripts/check-render-selftest.sh` (a separate step of the same CI job) breaks
`payslip`, `fno_910`, `tax_invoice`, `fno_300` and `labor_contract` on purpose
— amounts column emptied, one amount truncated to `10 000`, a table widened
until its VAT columns fall off the page, every amount of a ФНО 300.00
rewritten into the machine money form, `] else [` broken across two lines so
the branch is typeset as body text, and the signature rules turned back into a
zero-height `line()` that strikes through the party identifiers — renders each
through the same pipeline and fails unless the gate catches all six *and*
names what went wrong. Each case renders the unmutated template first and
requires a PASS, so a case cannot go green for the wrong reason, and a mutator
whose pattern stops matching fails the case loudly ("changed NOTHING") rather
than re-testing the healthy document.

Three cases are regression tests for blind spots the gate actually had. The
fourth breaks the FIXTURE, not the template, and must be caught by layer 0
before a pixel is compared. The fifth and sixth ADD ink rather than lose it —
both were measured passing with exit 0 — and must be caught by layers 3 and 4.
