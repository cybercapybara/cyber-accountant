// fno_300 v1 — Typst. A readable printed form of the VAT declaration, not a
// facsimile of the КГД blank. Input is the schema-validated, NORMALIZED JSON
// the worker writes next to this file. Values are data, never source. The XML
// actually FILED with the tax authority is produced elsewhere (FnoXml, whole
// tenge) and has nothing to do with this template.
// Converted from template.tex per .superpowers/sdd/2026-08-16-typst-migration.
#let d = json("input.json")

#set page(paper: "a4", margin: 18mm)
// `overhang: false` is load-bearing, not taste. Typst HANGS a line-final
// hyphen past the right edge and the gate fails the word box against the
// margin. `avr` proved a local render CANNOT rule this out: it did not
// hyphenate under a Homebrew Noto build and did hyphenate in the worker
// image, losing a declared label. Set uniformly: it is inert where nothing
// hangs, and it suppresses the overhang without moving any line break.
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru", overhang: false)
#set par(justify: true)

// The quarter is an enum-pinned CONTROL value: it selects a roman numeral and
// is never typeset itself (expected.txt declares it `unprinted`, and
// ShippedTemplatesTest.NeverPrintAnEnumPinnedField fails the build if a
// template prints one). Four independent `#if`s, exactly as the inja source
// had them — no `else` fallback, so an unexpected value prints nothing rather
// than silently claiming the fourth quarter. They share one physical line
// because a newline in markup is a space and "II квартал" must not gain one.
#align(center)[
  #text(size: 14.4pt, weight: "bold")[
    Декларация по налогу на добавленную стоимость \
    (форма 300.00)
  ] \
  #if d.period.quarter == "1" [I]#if d.period.quarter == "2" [II]#if d.period.quarter == "3" [III]#if d.period.quarter == "4" [IV] квартал #d.period.year года
]
#v(4mm)

*Наименование:* #d.org.name \
*БИН:* #d.org.bin

#v(4mm)

// `1fr` on the indicator column and `auto` on the other two is load-bearing:
// only the prose column may shrink, so neither the line code nor the amount
// can ever be clipped. Typst clips SILENTLY — exit 0, no transcript (migration
// spike, §5) — and a lost amounts column on a filed 300.00 is exactly the
// v0.3.0 defect. Never give the code or money column a fixed width.
#table(
  columns: (1fr, auto, auto),
  align: (left, center, right),
  stroke: none,
  inset: (x: 0pt, y: 3pt),
  column-gutter: 4mm,
  table.hline(stroke: 1pt),
  table.header([Показатель], [Код строки], [Сумма]),
  table.hline(stroke: 0.6pt),
  [Оборот по реализации, облагаемый НДС], [300.00.001], [#d.sales_tenge ₸],
  [Сумма НДС, начисленная за налоговый период], [300.00.002], [#d.vat_charged_tenge ₸],
  [Сумма НДС, относимая в зачёт], [300.00.003], [#d.vat_credited_tenge ₸],
  [Сумма НДС к уплате (возврату)], [300.00.004], [#d.balance_tenge ₸],
  table.hline(stroke: 1pt),
)

#v(3mm)
// THE CLOSING LINE. Two things about it, both paid for in production:
//
//  1. It vanished from every filed ФНО 300.00 in v0.4.2. Escaping ran BEFORE
//     templating, so `{% if balance_kind == "to_pay" %}` compared against an
//     already-escaped `to\_pay` and neither branch ever matched — the
//     declaration printed no closing sentence and no amount in words, with no
//     error anywhere. Under Typst there is no escaping and no templating
//     pre-pass at all: this file reads input.json itself, so that cause cannot
//     recur. `balance_kind` is compared, never printed.
//  2. `] else [` MUST stay on ONE physical line. Break between the `]` and the
//     `else` and Typst closes the if-expression, then TYPESETS the word `else`
//     and the whole second branch as body text — exit 0, no warning, a filed
//     declaration reading "…уплате в бюджет: 450 000,00 ₸ else [Сумма НДС,
//     подлежащая возврату из бюджета: …]", brackets and all. Spike defect 2,
//     measured on labor_contract. scripts/check-render.py's LAYER 3 now
//     refuses such a token and check-render-selftest.sh rebuilds the break on
//     every run, but the gate is the net, not the design.
//
// Both sentences are verbatim from template.tex and each is asserted by the
// fixture that selects it: basic.json is "to_pay", special-chars.json is
// "to_refund", and each names its own label in its .expected.txt.
#if d.balance_kind == "to_pay" [Сумма НДС, подлежащая уплате в бюджет: #d.balance_tenge ₸] else [Сумма НДС, подлежащая возврату из бюджета: #d.balance_tenge ₸] \
(#d.balance_words)

#v(12mm)
// \hrulefill equivalent. A bare `line()` contributes NO height, so its grid
// row collapses and the rule is drawn THROUGH the text above it — spike defect
// 3, invisible to pdftotext and caught only by the gate's raster layer. A
// stroked box with an EXPLICIT height carries its row in every layout. Do not
// "simplify" this back to a `line()`.
// The name column is capped at 40% and the RULE gets the `1fr`, never the
// other way round. With `auto` there the rule vanished outright on the
// special-chars fixture: an over-long name took the whole line and collapsed
// the 1fr column to zero width, so a signature block rendered with no line to
// sign on — silently, and invisibly to every layer of the gate, which checks
// that no rule crosses text but never that a rule exists. (The LaTeX original
// degraded worse on the same input: \hrulefill shrank to a stub and both
// signature lines plus the date ran together into one paragraph.) A capped
// column wraps the name instead.
#let sigrule = box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))
#grid(
  columns: (auto, 1fr, 40%),
  align: bottom,
  column-gutter: 4mm,
  row-gutter: 6mm,
  [Директор], sigrule, [#d.director],
  [Бухгалтер], sigrule, [#d.accountant],
)

#v(6mm)
Дата подписания: #d.signed_on
