// payslip v1 — Typst. Input is the schema-validated, NORMALIZED JSON the
// worker writes next to this file (src/docgen/Renderer.hpp::write_typst_inputs).
// Values are data, never source: nothing here escapes anything.
#let d = json("input.json")

#set page(paper: "a4", margin: 18mm)
// `overhang: false` is load-bearing, not taste. Typst HANGS a line-final
// hyphen past the right edge and the gate fails the word box against the
// margin. `avr` proved a local render CANNOT rule this out: it did not
// hyphenate under a Homebrew Noto build and did hyphenate in the worker
// image, losing a declared label. Set uniformly: it is inert where nothing
// hangs, and it suppresses the overhang without moving any line break.
// `hyphenate: false`: `overhang: false` alone is NOT enough. CI run
// 31958855105 proved it on avr — with the overhang off the margin
// excursion went away but Typst still SPLIT `каче-`/`ству`, and a split
// word is not findable as contiguous text, so the whole declared
// acceptance sentence was reported LOST. That is a real defect, not a
// gate artifact: a broken text layer also breaks Ctrl+F for a reader.
// Applied to every template that declares long justified prose labels
// (this one does). This SUPERSEDES the earlier "turn the overhang off,
// not the hyphenation" reasoning above, which predates that evidence.
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru", overhang: false, hyphenate: false)
#set par(justify: true)

#align(center)[
  #text(size: 14.4pt, weight: "bold")[Есеп-төлем парағы / Расчётный листок] \
  #d.period_label
]
#v(4mm)

*Жұмыс беруші / Работодатель:* #d.employer.name, БСН/БИН #d.employer.bin \
*Қызметкер / Работник:* #d.employee.full_name, ЖСН/ИИН #d.employee.iin \
*Лауазымы / Должность:* #d.employee.position

#v(4mm)

// columns: (1fr, auto) is load-bearing. A fixed width clips silently (typst
// exits 0 and writes no log); with 1fr the label column shrinks and the amount
// is never lost. Measured in the migration spike, §5.
#let money(v) = align(right, v)
#table(
  columns: (1fr, auto),
  align: (left, right),
  stroke: none,
  inset: (x: 0pt, y: 3pt),
  table.hline(stroke: 1pt),
  table.header([Көрсеткіш / Показатель], [Сомасы, ₸ / Сумма, ₸]),
  table.hline(stroke: 0.6pt),
  [Есептелген жалақы (жалпы сома) / Начислено (гросс)], money(d.gross_tenge),
  table.hline(stroke: 0.6pt),
  [Жеке табыс салығы (ИПН) / Индивидуальный подоходный налог (ИПН)], money(d.ipn),
  [Міндетті зейнетақы жарналары (ОПВ) / Обязательные пенсионные взносы (ОПВ)], money(d.opv),
  [Жұмыскердің ӘМСС жарнасы (ВОСМС) / Взнос работника на ОСМС (ВОСМС)], money(d.vosms),
  table.hline(stroke: 0.6pt),
  strong[Қолға берілетін сома / К выплате (нетто)], money(strong(d.net)),
  table.hline(stroke: 0.6pt),
  [Жұмыс беруші есебінен зейнетақы жарналары (ОПВР) / Взносы работодателя в ЕНПФ (ОПВР)], money(d.opvr),
  [Әлеуметтік аударым (СО) / Социальные отчисления (СО)], money(d.so),
  [Жұмыс берушінің ӘМСС жарнасы (ОСМС) / Взнос работодателя на ОСМС (ОСМС)], money(d.osms),
  [Әлеуметтік салық / Социальный налог], money(d.social_tax),
  table.hline(stroke: 1pt),
)

#v(3mm)
Қолға берілетін сома: #d.net ₸ \
(#d.net_words)

#v(12mm)
// A `line()` inside a grid contributes NO height: the row collapses and the
// rule is drawn through the text. A stroked box with an explicit height does
// not. Spike defect 3 — invisible to pdftotext, visible only in a raster.
#grid(
  columns: (1fr, 1fr),
  gutter: 8mm,
  [Бас бухгалтер / Гл. бухгалтер #box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))],
  [Қызметкер / Работник #box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))],
)
