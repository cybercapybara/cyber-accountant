// fno_910 v1 — Typst. A readable printed form of the simplified declaration,
// not a facsimile of the КГД blank. Input is the schema-validated, NORMALIZED
// JSON the worker writes next to this file. Values are data, never source.
// The XML actually FILED with the tax authority is produced elsewhere
// (FnoXml, whole tenge) and has nothing to do with this template.
//
// Every figure on this page is load-bearing: v0.3.0 printed and filed a 910.00
// whose income line read `10 000` where the input said `10 000 000,00 ₸`,
// because an overfull box is only a warning in LaTeX. Typst is worse — it
// clips with exit 0 and no transcript at all — so nothing here may constrain
// the width of an amount. See the `columns:` note on the table below.
#let d = json("input.json")

#set page(paper: "a4", margin: 18mm)
// `overhang: false`: Typst hangs a line-final hyphen PAST the text edge by
// design, which puts ink outside the margin box that scripts/check-render.py
// fails at its 0.5pt side tolerance. Turning the overhang off, not the
// hyphenation, keeps the line breaks. Carried over from labor_contract v1.
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru", overhang: false)
#set par(justify: true)

#align(center)[
  #text(size: 14.4pt, weight: "bold")[
    Упрощённая декларация для субъектов малого бизнеса \
    (форма 910.00)
  ] \
  // `period.half` is an enum-pinned control value: it selects a roman numeral
  // and is never itself printed (`unprinted period.half` in expected.txt,
  // ShippedTemplatesTest.NeverPrintAnEnumPinnedField). Both branches are
  // written on ONE line each — a `]` left dangling at a line end closes the
  // if-expression and Typst then TYPESETS the rest as body text, exit 0.
  #if d.period.half == "1" [I] #if d.period.half == "2" [II] полугодие #d.period.year года
]

#v(4mm)

*Наименование:* #d.org.name \
*БИН:* #d.org.bin

#v(4mm)

// columns: (1fr, auto, auto) is load-bearing — the label column is the only
// flexible one, so the row code and the SUM are sized to their content and can
// never be clipped. A fixed mm width on the amount column is exactly the
// v0.3.0 defect, and Typst would commit it silently.
#table(
  columns: (1fr, auto, auto),
  align: (left, center, right),
  stroke: none,
  inset: (x: 0pt, y: 3pt),
  column-gutter: 3mm,
  table.hline(stroke: 1pt),
  table.header([Показатель], [Код строки], [Сумма]),
  table.hline(stroke: 0.6pt),
  [Доход за налоговый период], [910.00.001], [#d.income_tenge ₸],
  [Ставка налога], [910.00.002], [#d.rate_percent%],
  [Сумма налога, подлежащая уплате], [910.00.003], [#d.tax_tenge ₸],
  table.hline(stroke: 1pt),
)

#v(3mm)

Сумма налога к уплате: #d.tax_tenge ₸ \
(#d.tax_words)

#v(12mm)

// \hrulefill. A bare `line()` contributes NO height: its row collapses and the
// rule is drawn THROUGH the text above it — invisible to pdftotext, caught
// only by the gate's ink layer. A stroked box with an explicit height carries
// its own height and sits on the baseline, where \hrulefill put it.
#let sigrule = box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))

Директор #sigrule #h(2em) #d.director

#v(6mm)

Бухгалтер #sigrule #h(2em) #d.accountant

#v(6mm)

Дата подписания: #d.signed_on
