// hr_order v1 — Typst. Input is the schema-validated, NORMALIZED JSON the
// worker writes next to this file (src/docgen/Renderer.hpp::write_typst_inputs).
// Values are data, never source: nothing here escapes anything.
// Converted from template.tex per .superpowers/sdd/2026-08-16-typst-migration.
//
// This template prints NO money. The only figures on the page arrive inside
// caller free text (`details`, `reason` — "Оклад 450 000,00 тенге"), which is
// why expected.txt declares no `amount` directive: there is nothing here for
// the gate's oracle to derive.
#let d = json("input.json")

#set page(paper: "a4", margin: 18mm)
// Two line-breaking settings, both measured against this template's own
// fixtures, both restoring what the XeLaTeX original did:
//
//  * `hyphenate: false`. Typst hyphenates justified text by default and
//    XeLaTeX had no Kazakh patterns to hyphenate WITH, so the two disagree.
//    On salary_change.json Typst split "өзгер-тілсін", and the label
//    `бастап өзгертілсін.` that fixture declares then existed nowhere in the
//    extracted text — scripts/check-render.py rejoins a hyphenated break only
//    when matching, and it still read as CONTENT LOST at the word level. The
//    prose here is short; unhyphenated justification costs nothing.
//  * `overhang: false`, as on labor_contract: Typst hangs line-final
//    punctuation past the text edge by design, which puts ink outside the
//    18mm box the gate measures at 0.5pt of side tolerance.
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru", overhang: false, hyphenate: false)
#set par(justify: true)

// LaTeX breaks a line only at a space. Typst additionally offers a break
// AFTER a `/` inside a word, and on special-chars.json it took that break:
// the hostile payload's `#read("/etc/passwd")` came out as `#read("/` +
// `etc/passwd")` on the next line, and the gate's content layer lost the
// whole `reason` value (it rejoins a HYPHENATED break, never a bare one).
// Boxing a slash-joined run makes it unbreakable, i.e. exactly one word, the
// way XeLaTeX already treated it. It is deliberately global: it applies to
// every caller value and to "ЖСН/ИИН"-style static labels alike, none of
// which should ever be split. Runs with spaces around the slash — the
// bilingual "Бұйрық № … / Приказ № …" headings — do not match and still wrap.
#show regex("\\S+/\\S+"): it => box(it)

#align(center)[
  #text(size: 14.4pt, weight: "bold")[
    Бұйрық № #d.number / Приказ № #d.number
  ] \
  #d.issued_on ж. / от #d.issued_on
]
#v(4mm)

#d.employer.name, БСН/БИН #d.employer.bin

#v(4mm)

// THE ORDER BODY. Read this before touching anything below.
//
//  1. `kind` is an enum-pinned CONTROL value: it selects a body and is never
//     typeset itself (expected.txt declares it `unprinted`, and
//     ShippedTemplatesTest.NeverPrintAnEnumPinnedField fails the build if a
//     template prints one).
//
//  2. Two of these five bodies — `business_trip` and `salary_change` —
//     shipped EMPTY. Escaping ran BEFORE templating, so `business_trip`
//     reached inja as `business\_trip` and `{% if kind == "business_trip" %}`
//     could never match; the order printed a heading, a signature block and
//     no body at all, with no error anywhere, and neither kind had a fixture
//     to render it. Under Typst there is no escaping and no templating
//     pre-pass at all: this file reads input.json itself, so that cause
//     cannot recur. All five kinds now have a rendered witness: the four
//     fixtures that already existed, plus fixtures/vacation.json, added with
//     this conversion because `vacation` was the one kind nothing had ever
//     rendered. That matters more than it used to —
//     ShippedTemplatesTest.HrOrderPrintsABodyForEveryKind asserted "every
//     kind prints a body" against the rendered LaTeX SOURCE, and a Typst
//     template has no such intermediate to assert on, so the property lives
//     in the render gate's content layer now, one fixture per kind, exactly
//     as Fno300PrintsItsClosingLineForBothBalanceKinds moved to fno_300's
//     per-fixture expectations.
//
//  3. `] else if … [` and `] else [` MUST each stay on ONE physical line.
//     Break between the `]` and the `else` and Typst closes the
//     if-expression, then TYPESETS the word `else` and the whole following
//     branch as literal body text — exit 0, no warning, an order that reads
//     "…на должность Бухгалтер else if d.kind == "dismiss" [Жұмыстан босату
//     туралы бұйрық…]", brackets and all. Spike defect 2, measured on
//     labor_contract and rebuilt by check-render-selftest.sh on every run.
//     scripts/check-render.py's LAYER 3 refuses such a token now, but the
//     gate is the net, not the design.
//
//  4. The chain ENDS in a plain `else`, not in a fifth `if`. Every value the
//     schema admits therefore prints a body: "no body at all" — this
//     template's entire failure history — is not reachable. `kind` is
//     enum-pinned and validated before the render, so nothing outside the
//     five literals can arrive here.
#if d.kind == "hire" [
  *Жұмысқа қабылдау туралы бұйрық / Приказ о приёме на работу* \
  #d.employee.full_name (ЖСН/ИИН #d.employee.iin) #d.effective_from бастап #d.employee.position лауазымына жұмысқа қабылдансын. \
  Принять #d.employee.full_name (ИИН #d.employee.iin) на работу с #d.effective_from на должность #d.employee.position.
] else if d.kind == "dismiss" [
  *Жұмыстан босату туралы бұйрық / Приказ о прекращении трудового договора* \
  #d.employee.full_name (ЖСН/ИИН #d.employee.iin) #d.effective_from бастап #d.employee.position лауазымынан босатылсын. \
  Уволить #d.employee.full_name (ИИН #d.employee.iin) с #d.effective_from с должности #d.employee.position.
] else if d.kind == "vacation" [
  *Демалыс беру туралы бұйрық / Приказ о предоставлении отпуска* \
  #d.employee.full_name қызметкеріне (#d.employee.position) #d.effective_from#if d.effective_to != "" [ бастап #d.effective_to дейін] демалыс берілсін. \
  Предоставить #d.employee.full_name (#d.employee.position) отпуск с #d.effective_from#if d.effective_to != "" [ по #d.effective_to].
] else if d.kind == "business_trip" [
  *Іссапарға жіберу туралы бұйрық / Приказ о направлении в командировку* \
  #d.employee.full_name қызметкері (#d.employee.position) #d.effective_from#if d.effective_to != "" [ бастап #d.effective_to дейін] іссапарға жіберілсін. \
  Направить #d.employee.full_name (#d.employee.position) в командировку с #d.effective_from#if d.effective_to != "" [ по #d.effective_to].
] else [
  *Лауазымдық жалақыны өзгерту туралы бұйрық / Приказ об изменении должностного оклада* \
  #d.employee.full_name қызметкерінің (#d.employee.position) лауазымдық жалақысы #d.effective_from бастап өзгертілсін. \
  Изменить должностной оклад #d.employee.full_name (#d.employee.position) с #d.effective_from.
]

// Optional free text. The guard is `!= ""` exactly as in inja, because
// TemplateRegistry::normalize_input fills a declared-but-absent optional
// string with "" — without it Typst hard-errors with `dictionary does not
// contain key "details"` where inja tolerated the miss. Typst also rejects a
// non-boolean condition outright, so inja's "empty string is truthy" trap
// cannot be written here even by accident.
#if d.details != "" [
  #v(2mm)
  *Мазмұны / Содержание:* #d.details
]
#if d.reason != "" [
  #v(2mm)
  *Негіздеме / Основание:* #d.reason
]

#v(12mm)
// \hrulefill equivalent. A bare `line()` contributes NO height, so its grid
// row collapses and the rule is drawn THROUGH the text above it — spike
// defect 3, invisible to pdftotext and caught only by the gate's raster
// layer. A stroked box with an EXPLICIT height carries its row in every
// layout. Do not "simplify" this back to a `line()`.
// The name column is capped at 40% and the RULE gets the `1fr`, never the
// other way round: on fno_300 an `auto` name column let an over-long name
// take the whole line, collapse the 1fr to zero width and make the rule
// VANISH — silently, and invisibly to every layer of the gate, which checks
// that no rule crosses text but never that a rule exists. Measured here at
// 200dpi against a 68-character employee name and a 60-character director:
// both rules still drawn, at the same widths and the same x as on
// basic.json, with the names wrapped inside their capped column instead.
// ONE GRID PER SIGNATURE LINE, not one grid with two rows: `\hrulefill`
// filled each line independently, and a shared `auto` label column would
// shrink both rules to what the LONGER of the two labels leaves over.
#let sigrule = box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))
#let sigline(label, name) = grid(
  columns: (auto, 1fr, 40%),
  align: bottom,
  column-gutter: 4mm,
  label, sigrule, name,
)
// What the XeLaTeX original actually did here, for the record: lines 49-51 of
// template.tex put `\vspace{6mm}` between two `\noindent` lines with no
// paragraph break between them, so both signature lines and both names ran
// together into ONE justified paragraph — "…Ахметов Ерлан Серикович
// Бұйрықпен таныстым / С приказом ознакомлен(а): ⎯⎯ Серикбаева Айгерим
// Кайратовна" — with each `\hrulefill` squeezed to a ~30pt stub. It rendered
// that way on the healthy basic.json fixture, not only on a hostile one, and
// no layer of the gate objects: every label and value is present, inside the
// margins, and no rule crosses any word. The conversion fixes it.
#sigline([Басшы / Директор], [#d.director])
#v(6mm)
#sigline([Бұйрықпен таныстым / С приказом ознакомлен(а):], [#d.employee.full_name])
