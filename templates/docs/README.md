# Document templates (docgen)

Every template here is **Typst**. The XeLaTeX path, the inja templating layer
and the LaTeX escaper were deleted once the last `template.tex` was converted.
The directory name `templates/docs/` is historical — it is a path stored in
nothing but this repo's scripts, and it is renamed in its own commit.

Each subdirectory is one **template slug** (a document type: `invoice`,
`avr`, `waybill`, `tax_invoice`, `reconciliation`, ...). `Docgen::TemplateRegistry`
(`src/docgen/TemplateRegistry.hpp`) scans this tree at render time — nothing
here is baked into a database or a compiled registry, so adding a template is
just adding files in the right layout.

## Layout

```
templates/docs/<slug>/v<N>/
├── template.typ         # Typst source, reads its data from input.json
├── schema.json          # JSON Schema (draft-07) the input JSON must satisfy
├── expected.txt         # page margin + the static labels this template prints
└── fixtures/
    ├── *.json           # sample inputs, rendered by scripts/render-templates.sh
    │                    #   (special-chars.json is required — see "Testing a template")
    └── *.expected.txt   # optional: labels only THIS fixture's data switches on
```

**The source file IS the engine declaration.** `TemplateRegistry::load()`
(`src/docgen/TemplateRegistry.hpp`) resolves `template.typ` per version
directory and stamps the engine onto the `TemplateInfo`, which travels all the
way to the compile and to `document_versions.render_engine`. Nothing else — no
list, no config key — records which template uses which engine. There is one
engine today and `Docgen::Engine` has one value, but the decision deliberately
stays in `load()`: adding a second engine must be a change there, not at every
call site. A version directory with no `template.typ` is a hard error, not a
silently skipped template.

- `<slug>` is the document type, matching `documents.doc_type` /
  `documents.template_slug` (migrations/010_documents.sql).
- `v<N>` is a plain non-negative integer version directory (`v1`, `v2`, ...).
  `TemplateRegistry::latest(slug)` always picks the **highest** `N` — ship a
  new version alongside the old one rather than editing it in place, since a
  document already rendered from `v1` keeps that version in its
  `template_version` snapshot (`Ledger::Document::template_version`).
- `fixtures/*.json` are sample inputs used ONLY by
  `scripts/render-templates.sh` (the `template-render` CI job) to smoke-test
  that the template still compiles under its **real engine** **and that the
  PDF it produced still says everything it was supposed to say** (see "The
  render gate" below) — they are not read by the render job itself.
- `expected.txt` is **required**: it declares the page margin box and the
  static labels the template prints. Without it the gate refuses to run, since
  it cannot tell a lost column from a template that never had one.

Reference implementation: `templates/docs/invoice/v1/`.

## Writing a template

There is no templating layer and nothing to escape. `Docgen::write_typst_inputs`
copies the template to `main.typ` and writes the (normalized) input beside it
as `input.json`; the template reads it with `#let d = json("input.json")`, so a
value is content and never source. `#panic("x")` in a counterparty name is
typeset character for character — which is why the escaper was deleted rather
than ported to Typst syntax: there is nothing for it to do, and an escaper on
this path could only corrupt the data it touched.

The one thing Typst demands in exchange: every key the schema declares must be
present, because it hard-errors on a missing dictionary key where inja merely
printed nothing. `TemplateRegistry::normalize_input` guarantees that before
staging, filling each declared-but-absent optional with a type-appropriate zero
value.

- **Compare against the zero value, don't test truthiness.** A filled `""` is
  indistinguishable from a caller-supplied empty string, so a block that should
  appear only when a field has content is written `#if d.seller.address != ""`.
- **Branch on `enum` fields, and never print one.** A field whose schema pins
  it to an `enum` is a control value: compare it, never typeset it. A control
  identifier (`to_pay`, `business_trip`) is not something a reader of a filed
  document should ever see. Declare each one `unprinted <path>` in
  `expected.txt`; `ShippedTemplatesTest.NeverPrintAnEnumPinnedField`
  (`tests/unit/test_template_registry.cpp`) fails the build if a template
  prints one.

  Historical note worth keeping, because it cost two releases: the retired
  pipeline escaped string leaves *before* inja saw them, so a template's own
  control flow was evaluated against escaped data — `{% if balance_kind ==
  "to_pay" %}` tested `to\_pay`, never matched, and every ФНО 300.00 shipped
  without its closing line while two `hr_order` kinds shipped with no body at
  all. Under Typst the question cannot arise: data never reaches a parser.
- `schema.json` is the contract: `RenderJob` (and `render-templates.sh`)
  reject any input that doesn't satisfy it — required fields, types, string
  patterns (e.g. a `date` field constrained to `DD.MM.YYYY`) all belong
  there, not in ad-hoc template logic.
- Fonts: ask for `#set text(font: "Noto Sans")`, which Typst finds by scanning
  `/usr/share/fonts` (`fonts-noto-core` in the worker image — see
  `docker/Dockerfile`). Typst ships no fonts of its own, so stick to a family
  actually installed there rather than assuming one is present; Noto Sans is
  the family with the Kazakh glyph coverage (ә ғ қ ң ө ұ ү һ і).

### Overflow has no warning — the gate reads the PDF

v0.3.0 shipped ФНО 910.00 printing `10 000` where the amount was
`10 000 000,00`, and a payslip that had lost its entire amounts column *and*
that column's static header. Under XeLaTeX that was an `Overfull \hbox`: a
*warning*, exit 0, a PDF still produced with the right-hand columns pushed off
the paper. The fix at the time was to grep the engine transcript.

Neither half of that survives. Typst does not warn: it CLIPS silently, exits 0
and writes no log at all, so there is no transcript to grep and
`scripts/render-templates.sh` no longer tries. "The render gate" below is the
only check, and that is exactly why it measures the PDF — extracted text,
word-box geometry against the declared margin box, leaked syntax by
provenance, and a raster pass for rules drawn through text. It would have
caught the v0.3.0 defect even if the engine had reported nothing at all, which
is now literally the case.

## Testing a template

- Unit tests (`tests/unit/test_template_registry.cpp`) cover discovery,
  engine selection, validation and Typst staging without invoking an engine at
  all. `ShippedTemplatesTest` there runs a hostile counterparty name through
  **every** shipped template
  (`TheInjectionPayloadStaysDataInEveryTemplate`): it must be carried byte for
  byte into `input.json` beside a `main.typ` that is a verbatim copy of the
  template, so the payload can never become source.
- `tests/integration/test_render_job.cpp` exercises the full `docgen.render`
  job pipeline with a stubbed `DOCGEN_TYPST_CMD` (no real engine needed).
- The **real** engine compile only runs via `./scripts/render-templates.sh`
  against every `fixtures/*.json` under this tree — locally on a machine with
  typst 0.15.1 installed, or via the `template-render` CI job
  on the worker image. Add a fixture for every new template (and every new
  required/optional field combination worth covering) so a broken template
  fails there instead of in production. That script does far more than check
  the exit code — see below.
- **`fixtures/special-chars.json` is required, and must stay hostile.** It is
  the only place the injection property gets tested against a real engine: its
  data carries `#panic(`, `#read(`, `*`, `` ` `` and `@`, plus the
  `% & # $ _ { } \ ^ ~` the deleted escaper used to rewrite — those stay on
  the list because the requirement was always that the engine PRINTS them.
  Layer 1 of the gate then demands each of those values back out of the PDF's
  text and layer 3 demands that nothing which *looks* like syntax lacks that
  provenance — i.e. the engine typeset the payload rather than acting on it;
  `ShippedTemplatesTest.EveryTemplateShipsAHostileSpecialCharsFixture` fails
  the build if you don't, because a bland fixture disarms that gate silently.

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
   `#` sigil (`#let`, `#if`), a content-block bracket, a bare control keyword
   (`else`, `endif`, …), and — still screened, so a stray one cannot creep
   back in unnoticed — an inja delimiter or a LaTeX control sequence —
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
   all 23 fixtures: zero findings.

Layers 3 and 4 exist because layers 0–2 only ever ask whether something is
**missing**. The two defects the labour-contract conversion rebuilt both ADD
ink and both passed with exit 0: `] else [` split across lines printed the
word `else` and its entire branch into the contract body (291 word boxes
instead of 280, all inside the margins), and a `line()` in a grid — which
contributes no height — struck a signature rule through `БСН/БИН` and
`ЖСН/ИИН`. Both are carried as deliberate breakages by
`scripts/check-render-selftest.sh`, because either is one edit away from
coming back.

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

Every label must also occur **verbatim in the template source**
(`check-render.py`'s `template_source()` reads the directory's `template.typ`);
a label that does not is reported as `EXPECTATION ROT`, so the file cannot
drift into asserting something the template stopped printing.

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
