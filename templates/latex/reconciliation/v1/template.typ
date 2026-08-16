// reconciliation v1 — Typst. Input is the schema-validated, NORMALIZED JSON
// the worker writes next to this file (src/docgen/Renderer.hpp::
// write_typst_inputs). Values are data, never source: nothing here escapes
// anything.
// Converted from template.tex per .superpowers/sdd/2026-08-16-typst-migration.
//
// NOTE ON THE MONEY, so nobody "fixes" it in passing: unlike every other
// template in this tree, this one's figures — opening_balance.*, rows[].*,
// closing.* — are caller-authored FREE TEXT with no integer tiyn behind them
// anywhere on the server. That is a known, recorded gap, not an oversight of
// this conversion: it is why both .expected.txt files still declare `amount`
// directives the gate checks as multisets against strings the caller wrote.
// Do not add `amount` declarations here and do not reroute the values; the
// conversion changes the RENDERING only.
#let d = json("input.json")

#set page(paper: "a4", margin: 16mm)
// `overhang: false`, as on labor_contract/fno_910/hr_order: Typst HANGS a
// line-final hyphen and line-final punctuation PAST the text edge — visually
// pleasing, but it puts ink outside the margin box and the gate's geometry
// layer fails at its 0.5pt side tolerance. Neither shipped fixture trips it,
// but every long value on this document is caller-authored free text (a party
// name, a document description, the two closing sentences), so a customer's
// input, not a fixture, is what would surface it in production. Measured with
// a 108-character party name: with the overhang on, six words cross the right
// margin by up to 1.97pt; with it off, none does. Turn the overhang off, never
// the margin.
// `hyphenate: false` for the same reason as hr_order/labor_contract: Typst
// hyphenates justified text by default and XeLaTeX did not hyphenate the same
// words, so the two disagree — and check-render.py rejoins a hyphenated break
// over ONE line only. Measured here: a long party name squeezed the Документ
// column until Typst hyphenated the header across three lines (`До-`/`ку-`/
// `мент`) and the gate reported the declared label `Документ` LOST. Off, the
// word overflows visibly instead of disappearing from the text layer.
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru", overhang: false, hyphenate: false)
#set par(justify: true)

// TYPST BREAKS A LINE AFTER `/` INSIDE A WORD, and the gate rejoins only
// HYPHENATED breaks — so a slash-bearing token split at a line end vanishes
// from the extracted text while the page still looks correct. Found on
// hr_order; it is live on this document in TWO places, both of them positioned
// by caller data and neither safe by construction:
//
//   * the declared label `БИН/ИИН`, which follows a free-text party name on
//     both party lines, so the name's length decides where it lands;
//   * the special-chars payload's `#read("/etc/passwd")`, inside the `1fr`
//     Документ column.
//
// The shipped fixtures only dodge it by luck: sweeping a filler prefix through
// the payload cell, 20 of 40 lengths split it into `#read("/etc/` + `passwd")`
// and the gate reported `CONTENT LOST` for the whole `rows[].doc` value. Boxing
// the token makes it unbreakable, and after this rule the same 40-length sweep
// breaks at zero lengths. Do not remove this because "the current fixture is
// fine" — that is exactly the state this rule was added from.
#show regex("\\S+/\\S+"): it => box(it)

#align(center)[
  #text(size: 14.4pt, weight: "bold")[Акт сверки взаиморасчётов за период с #d.period_from по #d.period_to]
]
#v(4mm)

// \hphantom{\field{Сторона А:} } equivalent: indent the wrapped address line
// by the exact width of the label above it. `measure` needs a `context` —
// without it Typst hard-errors with "can only be used when context is known".
#let phantom(label) = context h(measure(label).width)

// Optional fields: the guard is `!= ""` exactly as in inja, because
// TemplateRegistry::normalize_input fills a declared-but-absent optional
// string with "" — without it Typst hard-errors with `dictionary does not
// contain key "address"` where inja tolerated the miss. The special-chars
// fixture supplies neither party's address, so both guards are exercised
// false on one fixture and true on the other.
// `\par`/`\noindent` have NO counterpart here and are simply gone: a Typst
// table is block-level and cannot be typeset inline, so the whole "tables
// start their own paragraph" discipline (templates/latex/README.md) that the
// LaTeX original needed two comment blocks to explain does not exist.
*Сторона А:* #d.party_a.name, БИН/ИИН #d.party_a.identifier \
#if d.party_a.address != "" [#phantom[*Сторона А:* ]#d.party_a.address \ ]

#v(2mm)
*Сторона Б:* #d.party_b.name, БИН/ИИН #d.party_b.identifier \
#if d.party_b.address != "" [#phantom[*Сторона Б:* ]#d.party_b.address \ ]

#v(4mm)

// THE SIX-COLUMN TABLE — the widest in the tree, and the only one with a
// spanning header. Two LaTeX constructs land here that no other converted
// template had:
//
//   * `\multicolumn{2}{c}{По данным ...}` becomes
//     `table.cell(colspan: 2, align: center)[...]`. The two leading `[], []`
//     are the empty Дата/Документ cells of that row — a colspan cell consumes
//     its own slots, so the row is still exactly six columns wide, and Typst
//     errors on a short row rather than silently shifting the next one.
//   * `\cmidrule(lr){3-4}` / `\cmidrule(lr){5-6}` become
//     `table.hline(start: 2, end: 4)` / `table.hline(start: 4, end: 6)`.
//     THE INDICES ARE 0-BASED AND end IS EXCLUSIVE, where booktabs' are
//     1-based and inclusive: LaTeX columns 3-4 are Typst [2,4). Getting this
//     wrong shifts a rule one column sideways, which no gate layer catches —
//     it is checked by eye on the raster and by the two column pairs lining
//     up under their headings.
//     `(lr)` trims the rule inside the column gutter; here `column-gutter:
//     3mm` with a zero x-inset gives the same effect, because the rule spans
//     the two columns plus the 3mm between them and stops at their edges.
//
// Both header rows and both cmidrules sit inside `table.header`, so a
// reconciliation long enough to break across pages repeats its whole heading.
//
// The column widths are the load-bearing part. `1fr` for Документ and `auto`
// for every figure means the description column is the only one that can
// shrink, so no amount is ever clipped — Typst clips SILENTLY, exit 0 and no
// log (migration spike, §5), which is exactly the v0.3.0 lost-amounts-column
// bug with no warning attached. Never give Документ or a money column a fixed
// width.
//
// Empty cells: `a_debit`/`b_debit`/... are legitimately `""` on most rows.
// A Typst string is content, so `""` typesets as nothing — the cell is empty,
// not the word `none`. Do not wrap these in `[#...]` or in any `if`: the
// gate SKIPS an empty fixture string, so nothing would catch the word `none`
// appearing in a money column except a human on the raster.
#table(
  columns: (auto, 1fr, auto, auto, auto, auto),
  align: (left, left, right, right, right, right),
  stroke: none,
  inset: (x: 0pt, y: 3pt),
  column-gutter: 3mm,
  table.hline(stroke: 1pt),                                    // \toprule
  table.header(
    [], [],
    table.cell(colspan: 2, align: center)[По данным #d.party_a.name],
    table.cell(colspan: 2, align: center)[По данным #d.party_b.name],
    table.hline(start: 2, end: 4, stroke: 0.4pt),              // \cmidrule(lr){3-4}
    table.hline(start: 4, end: 6, stroke: 0.4pt),              // \cmidrule(lr){5-6}
    [Дата], [Документ], [Дебет, ₸], [Кредит, ₸], [Дебет, ₸], [Кредит, ₸],
  ),
  table.hline(stroke: 0.6pt),                                  // \midrule
  // The optional opening-balance row. inja's `{% if %}` becomes a spliced
  // array that is either the row's six cells or `()` — an empty spread adds
  // nothing at all, so the fixture without an opening balance gets no row
  // rather than a blank-labelled one. `opening_balance` is an optional OBJECT
  // and normalize_input fills it with "" leaves, so the guard tests a
  // representative leaf, never the object itself (which is never "empty").
  ..(if d.opening_balance.a_debit != "" or d.opening_balance.a_credit != "" {
      (table.cell(colspan: 2)[Сальдо на начало периода],
       d.opening_balance.a_debit, d.opening_balance.a_credit, [], [])
    } else { () }),
  // THE ROW LOOP. `{% for row in rows %}` becomes one spread of cells: each
  // row maps to the tuple of its columns, `.flatten()` concatenates the
  // tuples into the flat cell sequence `#table` wants, and `..` splices it
  // in — so the row's cells live on ONE line, in column order, immediately
  // under the header they must agree with, with no per-row markup to drift
  // out of sync. There is no `№` column on this document, so unlike
  // invoice/avr/waybill there is no `enumerate(start: 1)`/`str(i)`.
  ..d.rows.map(r => (r.date, r.doc, r.a_debit, r.a_credit, r.b_debit, r.b_credit)).flatten(),
  table.hline(stroke: 1pt),                                    // \bottomrule
)

#v(4mm)
#d.closing.a_says \
#d.closing.b_says

#v(12mm)
// \hrulefill equivalent. A bare `line()` contributes NO height, so its grid
// row collapses and the rule is drawn THROUGH the text above it — spike
// defect 3, measured on labor_contract, invisible to pdftotext and caught
// only by the gate's raster layer. A stroked box with an EXPLICIT height
// carries its row in every layout. Do not "simplify" this back to a `line()`.
//
// The other way a rule dies is by having no width left: on fno_300 an
// over-long director name took the whole line, collapsed the rule's column to
// zero width and BOTH rules vanished outright — silently, and invisibly to
// every layer of the gate, which checks that no rule crosses text but never
// that a rule EXISTS. Unlike avr's and invoice's signature blocks, the label
// here is not static: it interpolates a party name, so a fixture value can
// starve the rule. Hence the name gets its own grid ROW and the rule gets a
// row of its own at the full cell width — `width: 100%` of a `1fr` column,
// which no length of name can shrink. Measured with a 120-character party
// name: both rules still drawn, full width, below their text. Do not fold the
// rule back onto the name's line.
#let sigrule = box(width: 100%, height: 8mm, stroke: (bottom: 0.4pt))
#grid(
  columns: (1fr, 1fr),
  column-gutter: 8mm,
  row-gutter: 2mm,
  [От #d.party_a.name:], [От #d.party_b.name:],
  sigrule, sigrule,
)
