// invoice v1 — Typst. Input is the schema-validated, NORMALIZED JSON the
// worker writes next to this file (src/docgen/Renderer.hpp::write_typst_inputs).
// Values are data, never source: nothing here escapes anything.
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

#align(center)[
  #text(size: 14.4pt, weight: "bold")[Счёт на оплату № #d.number от #d.date]
]
#v(6mm)

// \hphantom{...} equivalent: indent the wrapped address line by the exact
// width of the label above it. `measure` needs a `context` — see
// labor_contract/v1/template.typ, where the same helper is spelled out.
#let phantom(label) = context h(measure(label).width)

// Optional fields: the guard is `!= ""` exactly as in inja, because
// TemplateRegistry::normalize_input fills a declared-but-absent optional
// string with "" — without it Typst hard-errors with `dictionary does not
// contain key "iik"` where inja tolerated the miss.
// The bank-details line keeps all four of its `#if`s on ONE physical line:
// a newline in markup is a space, and ", #d.seller.bank" has to follow the
// IIK with no space before the comma. Measured, not assumed — breaking the
// line adds a space and nothing else, it does not leak. The construct that
// DOES leak is `] else [` split across lines (labor_contract, spike defect 2:
// Typst closes the if and typesets `else` and its branch as body text), so a
// template that grows an `else` must keep that on one line too.
*Поставщик:* #d.seller.name, БИН/ИИН #d.seller.identifier \
#if d.seller.address != "" [#phantom[*Поставщик:* ]#d.seller.address \ ]
#if d.seller.iik != "" [ИИК #d.seller.iik#if d.seller.bank != "" [, #d.seller.bank]#if d.seller.bik != "" [, БИК #d.seller.bik]#if d.seller.kbe != "" [, КБе #d.seller.kbe] \ ]

#v(2mm)
*Покупатель:* #d.buyer.name, БИН/ИИН #d.buyer.identifier \
#if d.buyer.address != "" [#phantom[*Покупатель:* ]#d.buyer.address \ ]
#if d.contract != "" [*Основание:* #d.contract \ ]

#v(4mm)

// THE ITEM LOOP. `{% for item in items %}` becomes one spread of cells: each
// item maps to the tuple of its columns, `.flatten()` concatenates the tuples
// into the flat cell sequence `#table` wants, and `..` splices it in. The
// row's cells therefore live on ONE line, in column order, right under the
// header they belong to — there is no per-row markup to drift out of sync.
// `enumerate(start: 1)` is inja's `loop.index1`; `str(i)` because a table cell
// takes content and an integer is not content.
//
// The column widths are the load-bearing part. `1fr` for the description and
// `auto` for every figure means the description column is the only one that
// can shrink, so no amount is ever clipped — Typst clips SILENTLY, exit 0 and
// no log (migration spike, §5), which is exactly the v0.3.0 lost-amounts-column
// bug with no warning attached. Never give the name column or a money column a
// fixed width.
#table(
  columns: (auto, 1fr, auto, auto, auto, auto),
  align: (center, left, right, right, right, right),
  stroke: none,
  inset: (x: 0pt, y: 3pt),
  column-gutter: 3mm,
  table.hline(stroke: 1pt),
  table.header([№], [Наименование], [Кол-во], [Ед.], [Цена, ₸], [Сумма, ₸]),
  table.hline(stroke: 0.6pt),
  ..d.items.enumerate(start: 1).map(((i, it)) => (str(i), it.name, it.qty, it.unit, it.price, it.amount)).flatten(),
  table.hline(stroke: 1pt),
)

#v(3mm)
#if d.vat_amount != "" [#align(right)[НДС (#d.vat_rate): #d.vat_amount ₸]]
#align(right)[*Итого к оплате: #d.total ₸*]

Всего наименований #d.items.len(), на сумму #d.total ₸ \
(#d.total_words)

#v(12mm)
// \hrulefill equivalent. A `line()` contributes NO height, so inside a grid
// the row collapses and the rule is drawn THROUGH the text above it — spike
// defect 3, measured on labor_contract, invisible to pdftotext and caught only
// by the gate's raster layer. This grid has a single row, so `line()` here
// happens to land on the baseline instead of through a name; that is luck, not
// safety, and it evaporates the moment a row is added above. A stroked box
// with an EXPLICIT height carries its row in every layout. Do not "simplify"
// this back to a bare `line()`.
#let sigrule = box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))
#grid(
  columns: (1fr, 1fr),
  gutter: 8mm,
  [Руководитель #sigrule],
  [Гл. бухгалтер #sigrule],
)
