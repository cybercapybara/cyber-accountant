// waybill v1 — Typst. Input is the schema-validated, NORMALIZED JSON the
// worker writes next to this file (src/docgen/Renderer.hpp::write_typst_inputs).
// Values are data, never source: nothing here escapes anything.
// Converted from template.tex per .superpowers/sdd/2026-08-16-typst-migration.
#let d = json("input.json")

// LANDSCAPE — `\usepackage[margin=14mm,landscape]{geometry}`. `flipped: true`
// is what swaps A4's sides; without it the gate's geometry layer measures a
// portrait page against a landscape margin box and the six-column table has
// 84mm less room than it was laid out for. The 14mm here is cross-checked
// against `margin 14mm` in expected.txt — check-render.py refuses to run if
// the two disagree.
#set page(paper: "a4", flipped: true, margin: 14mm)
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru")
#set par(justify: true)

#align(center)[
  #text(size: 14.4pt, weight: "bold")[Накладная на отпуск запасов на сторону № #d.number от #d.date]
]
#v(4mm)

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
// IIK with no space before the comma. The construct that DOES leak is
// `] else [` split across lines (labor_contract, spike defect 2), and this
// template has no `else` at all — but if one is ever added, it stays on one
// physical line.
*Поставщик (склад):* #d.seller.name, БИН/ИИН #d.seller.identifier \
#if d.seller.address != "" [#phantom[*Поставщик (склад):* ]#d.seller.address \ ]
#if d.seller.iik != "" [ИИК #d.seller.iik#if d.seller.bank != "" [, #d.seller.bank]#if d.seller.bik != "" [, БИК #d.seller.bik]#if d.seller.kbe != "" [, КБе #d.seller.kbe] \ ]

#v(2mm)
*Получатель:* #d.buyer.name, БИН/ИИН #d.buyer.identifier \
#if d.buyer.address != "" [#phantom[*Получатель:* ]#d.buyer.address \ ]
// `basis` is REQUIRED by schema.json, so unlike the invoice's `contract`
// this line carries no `#if` — it is unconditional in template.tex too.
*Основание:* #d.basis

#v(4mm)

// THE ITEM LOOP, identical in shape to invoice/v1. `{% for item in items %}`
// becomes one spread of cells: each item maps to the tuple of its columns,
// `.flatten()` concatenates the tuples into the flat cell sequence `#table`
// wants, and `..` splices it in. The row's cells therefore live on ONE line,
// in column order, right under the header they belong to — there is no
// per-row markup to drift out of sync. `enumerate(start: 1)` is inja's
// `loop.index1`; `str(i)` because a table cell takes content and an integer
// is not content.
//
// The COLUMN ORDER is this template's own and differs from the invoice's:
// the накладная prints Ед. before Кол-во, so the tuple is
// (i, name, unit, qty, price, amount) — the header above and the tuple below
// must be read together, which is exactly why they sit adjacent.
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
  table.header([№], [Наименование], [Ед.], [Кол-во], [Цена, ₸], [Сумма, ₸]),
  table.hline(stroke: 0.6pt),
  ..d.items.enumerate(start: 1).map(((i, it)) => (str(i), it.name, it.unit, it.qty, it.price, it.amount)).flatten(),
  table.hline(stroke: 1pt),
)

#v(3mm)
#align(right)[*Итого: #d.total ₸*]

Всего наименований #d.items.len(), на сумму #d.total ₸ \
(#d.total_words)

#v(12mm)
// \hrulefill equivalent. A `line()` contributes NO height, so inside a grid
// the row collapses and the rule is drawn THROUGH the text — spike defect 3,
// measured on labor_contract, invisible to pdftotext and caught only by the
// gate's raster layer. Here the risk is not hypothetical: the names row sits
// directly BELOW the rules row, so a zero-height rule row would drop the rule
// onto "Отпустил:"/"Получил:" themselves. A stroked box with an EXPLICIT
// height carries its row in every layout. Do not "simplify" this back to a
// bare `line()`.
#let sigrule = box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))
#grid(
  columns: (45%, 45%),
  column-gutter: 1fr,
  row-gutter: 2mm,
  [Отпустил: #sigrule], [Получил: #sigrule],
  [#d.released_by], [#d.received_by],
)
