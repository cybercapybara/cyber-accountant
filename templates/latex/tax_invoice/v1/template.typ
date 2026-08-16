// tax_invoice v1 — Typst. Input is the schema-validated, NORMALIZED JSON the
// worker writes next to this file (src/docgen/Renderer.hpp::write_typst_inputs).
// Values are data, never source: nothing here escapes anything.
// Converted from template.tex per .superpowers/sdd/2026-08-16-typst-migration.
#let d = json("input.json")

// LANDSCAPE — `\usepackage[margin=12mm,landscape]{geometry}`. `flipped: true`
// is what swaps A4's sides; without it the gate's geometry layer measures a
// portrait page against a landscape margin box and the NINE-column table has
// 84mm less room than it was laid out for. The 12mm here is cross-checked
// against `margin 12mm` in expected.txt — check-render.py refuses to run if
// the two disagree. 12mm on landscape A4 is the tightest page in the corpus:
// if the geometry layer ever complains, shrink the `inset` or the header text
// size, NEVER the margin and never to a fixed column width.
#set page(paper: "a4", flipped: true, margin: 12mm)
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru")
#set par(justify: true)

// Typst breaks a line after `/` INSIDE a word, and the gate's flow-variant
// reconstruction (check-render.py::flow_variants) rejoins only HYPHENATED
// breaks — so a value split at a slash is simply absent from the extracted
// text and is reported CONTENT LOST, while the rendered page looks perfectly
// normal. Found on hr_order, and it is REACHABLE here, not hypothetical: this
// template prints the static label `БИН/ИИН` twice in justified prose whose
// wrap point moves with the counterparty name, and the hostile fixture's
// `#read("/etc/passwd")` carries two more slashes. A 116-character seller name
// plus a 114-character buyer name splits BOTH occurrences into `БИН/` + `ИИН`
// and the gate reports `CONTENT LOST static label "БИН/ИИН"` — measured, not
// argued. Boxing every slash-bearing token makes it unbreakable and costs
// nothing here: the only such tokens are short (`БИН/ИИН`, `#read("…")`,
// house numbers like `55/21`), so none can overflow its column.
// `hyphenate` is deliberately left ON: no declared label of this template
// hyphenates, and hyphenated breaks are the ONE kind the gate does rejoin.
#show regex("\S+/\S+"): it => box(it)

#align(center)[
  #text(size: 14.4pt, weight: "bold")[Счёт-фактура № #d.number от #d.date]
]
#v(4mm)

// \hphantom{...} equivalent: indent the wrapped address line by the exact
// width of the label above it. `measure` needs a `context` — see
// labor_contract/v1/template.typ, where the same helper is spelled out.
#let phantom(label) = context h(measure(label).width)

// Optional fields: the guard is `!= ""` exactly as in inja, because
// TemplateRegistry::normalize_input fills a declared-but-absent optional
// string with "" — without it Typst hard-errors with `dictionary does not
// contain key "iik"` where inja tolerated the miss. The special-chars fixture
// supplies neither party's optional block, so every one of these guards is
// exercised false on one fixture and true on the other.
// The bank-details line keeps all four of its `#if`s on ONE physical line:
// a newline in markup is a space, and ", #d.seller.bank" has to follow the
// IIK with no space before the comma. The construct that DOES leak is
// `] else [` split across lines (labor_contract, spike defect 2: Typst closes
// the if and typesets `else` and its branch as body text); this template has
// no `else`, and if one is ever added it stays on one physical line.
*Поставщик:* #d.seller.name, БИН/ИИН #d.seller.identifier \
#if d.seller.address != "" [#phantom[*Поставщик:* ]#d.seller.address \ ]
#if d.seller.iik != "" [ИИК #d.seller.iik#if d.seller.bank != "" [, #d.seller.bank]#if d.seller.bik != "" [, БИК #d.seller.bik]#if d.seller.kbe != "" [, КБе #d.seller.kbe] \ ]
#if d.seller.vat_certificate != "" [Свидетельство плательщика НДС: #d.seller.vat_certificate \ ]

#v(2mm)
*Покупатель:* #d.buyer.name, БИН/ИИН #d.buyer.identifier \
#if d.buyer.address != "" [#phantom[*Покупатель:* ]#d.buyer.address \ ]
#if d.buyer.vat_certificate != "" [Свидетельство плательщика НДС: #d.buyer.vat_certificate \ ]

#v(4mm)

// THE ITEM LOOP, the same shape as invoice/v1 and waybill/v1 — nine columns
// instead of six. `{% for item in items %}` becomes one spread of cells: each
// item maps to the tuple of its columns, `.flatten()` concatenates the tuples
// into the flat cell sequence `#table` wants, and `..` splices it in. The
// row's cells therefore live on ONE line, in column order, IMMEDIATELY under
// the header they must agree with — there is no per-row markup to drift out of
// sync, and with nine columns that adjacency is the only thing keeping the
// header and the row readable together. `enumerate(start: 1)` is inja's
// `loop.index1`; `str(i)` because a table cell takes content and an integer is
// not content.
//
// The column widths are the load-bearing part. `1fr` for the description and
// `auto` for every figure means the description column is the only one that
// can shrink, so no figure — and on this document that includes the two VAT
// columns — is ever clipped. Typst clips SILENTLY, exit 0 and no log
// (migration spike, §5), which is exactly the v0.3.0 lost-amounts-column bug
// with no warning attached, and it is what the self-test's `over-wide-table`
// case reproduces. Never give the name column or a money column a fixed width.
//
// `vat_rate` is a PERCENTAGE ("16%"), not an amount: it is a plain string, it
// carries no `amount` directive in either .expected.txt, and it must not be
// formatted or derived like money.
#table(
  columns: (auto, 1fr, auto, auto, auto, auto, auto, auto, auto),
  align: (center, left, right, right, right, right, right, right, right),
  stroke: none,
  inset: (x: 0pt, y: 3pt),
  column-gutter: 3mm,
  table.hline(stroke: 1pt),
  table.header(
    [№], [Наименование], [Ед.], [Кол-во], [Цена, ₸], [Стоимость без НДС, ₸],
    [Ставка НДС], [Сумма НДС, ₸], [Всего с НДС, ₸],
  ),
  table.hline(stroke: 0.6pt),
  ..d.items.enumerate(start: 1).map(((i, it)) => (str(i), it.name, it.unit, it.qty, it.price, it.amount, it.vat_rate, it.vat_amount, it.total_with_vat)).flatten(),
  table.hline(stroke: 1pt),
)

#v(3mm)
#align(right)[Стоимость без НДС: #d.totals.amount ₸]
#align(right)[Сумма НДС: #d.totals.vat ₸]
#align(right)[*Всего с НДС: #d.totals.with_vat ₸*]

Всего наименований #d.items.len(), на сумму #d.totals.with_vat ₸ \
(#d.total_words)

#v(12mm)
// \hrulefill equivalent. A `line()` contributes NO height, so inside a grid
// the row collapses and the rule is drawn THROUGH the text above it — spike
// defect 3, measured on labor_contract, invisible to pdftotext and caught only
// by the gate's raster layer. A stroked box with an EXPLICIT height carries
// its row in every layout. Do not "simplify" this back to a bare `line()`.
//
// The other way a rule dies is by having no width left: on fno_300 a long
// director name collapsed its column and BOTH rules vanished, and no gate
// layer catches a missing rule. Here the two grid columns are a fixed 1fr/1fr
// split and the only text in each cell is a static label, so no fixture value
// can starve either rule — which is why this grid needs no percentage cap.
#let sigrule = box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))
#grid(
  columns: (1fr, 1fr),
  gutter: 8mm,
  [Руководитель #sigrule],
  [Гл. бухгалтер #sigrule],
)
