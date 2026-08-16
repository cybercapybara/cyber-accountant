// labor_contract v1 — Typst. Input is the schema-validated, NORMALIZED JSON the
// worker writes next to this file (src/docgen/Renderer.hpp::write_typst_inputs).
// Values are data, never source: nothing here escapes anything.
// Converted from template.tex per .superpowers/sdd/2026-08-16-typst-migration.
#let d = json("input.json")

#set page(paper: "a4", margin: 18mm)
// `overhang: false` is load-bearing, not taste. Typst HANGS a line-final
// hyphen past the text edge by design; with it left at the default this
// template put "Бух-" and "Респуб-" 1.63pt outside the 18mm margin box, which
// scripts/check-render.py fails at its measured 0.5pt side tolerance (it
// deliberately did NOT carry over the spike prototype's 2.5pt hyphen
// exception). Justified Russian prose long enough to hyphenate is what makes
// this the first template to hit it — the payslip's cells never wrap that far.
// Turning the overhang off, not the hyphenation, keeps LaTeX's line breaks.
#set text(font: "Noto Sans", size: 9.2pt, lang: "ru", overhang: false)
#set par(justify: true)
// No \emergencystretch counterpart: the LaTeX source needed 2em of it because
// polyglossia's russian setup has no Kazakh hyphenation patterns and a long
// Kazakh line overhung the right margin by 6-7pt. Typst loosens the line
// itself.

#align(center)[
  #text(size: 14.4pt, weight: "bold")[
    Еңбек шарты № #d.number / Трудовой договор № #d.number
  ] \
  #d.signed_on ж. / от #d.signed_on
]
#v(4mm)

// Optional fields: the guard is `!= ""` exactly as in inja, because
// TemplateRegistry::normalize_input fills a declared-but-absent optional
// string with "" — without it Typst hard-errors with `dictionary does not
// contain key "address"` where inja tolerated the miss. Typst also rejects a
// non-boolean condition outright, so inja's "empty string is truthy" trap
// cannot be written here even by accident.
// \hphantom{...} equivalent: indent the wrapped address line by the exact
// width of the label above it. `measure` needs a `context` — that is the one
// real gotcha in this file.
#let phantom(label) = context h(measure(label).width)

*Жұмыс беруші / Работодатель:* #d.employer.name, БСН/БИН #d.employer.bin \
#if d.employer.address != "" [#phantom[*Жұмыс беруші / Работодатель:* ]#d.employer.address \ ]
атынан әрекет ететін / в лице #d.employer.director

#v(2mm)
*Қызметкер / Работник:* #d.employee.full_name, ЖСН/ИИН #d.employee.iin \
#if d.employee.address != "" [#phantom[*Қызметкер / Работник:* ]#d.employee.address \ ]
*Лауазымы / Должность:* #d.employee.position

#v(4mm)
*1. Шарттың мәні / Предмет договора* \
Осы еңбек шарты бойынша Қызметкер #d.employee.position лауазымында жұмыс істеуге, ал
Жұмыс беруші оған еңбек жағдайларын қамтамасыз етіп, айлық жалақыны уақтылы төлеуге
міндеттенеді. \
По настоящему трудовому договору Работник обязуется выполнять трудовые обязанности по
должности #d.employee.position, а Работодатель обязуется обеспечить условия труда и
своевременно выплачивать заработную плату.

#v(2mm)
*2. Мерзімі / Срок действия* \
// WARNING: `] else [` MUST stay on ONE physical line. Break the line between
// the `]` and the `else` and Typst closes the if-expression, then typesets the
// word "else" and the whole second branch as literal body text — exit 0, no
// error, no warning, a labour contract that reads "…и действует до 17.08.2027
// else [и заключён на неопределённый срок]" — brackets and all. Spike defect 2.
// Reproduced against this file: typst exits 0 and scripts/check-render.py
// reports PASS on the broken PDF (every declared label and value is still
// there; the leak only ADDS eleven words). The gate cannot catch this. The
// raster review is what catches it.
Шарт #d.starts_on жылдан бастап күшіне енеді #if d.ends_on != "" [және #d.ends_on дейін қолданылады] else [және белгісіз мерзімге жасалады]. \
Договор вступает в силу с #d.starts_on #if d.ends_on != "" [и действует до #d.ends_on] else [и заключён на неопределённый срок].
#if d.probation_months != "" [
  \ Сынақ мерзімі — #d.probation_months ай / Испытательный срок — #d.probation_months мес.
]

#v(2mm)
*3. Жұмыс режимі / Режим работы* \
#d.work_schedule

#v(2mm)
*4. Еңбекақы / Оплата труда* \
Айлық жалақы — #d.salary_tenge теңге (#d.salary_words_kk) / Оклад — #d.salary_tenge тенге
(#d.salary_words).

#v(2mm)
*5. Тараптардың құқықтары мен міндеттері / Права и обязанности сторон* \
Тараптар Қазақстан Республикасының Еңбек кодексіне сәйкес құқықтарды пайдаланады және
міндеттерді орындайды. / Стороны пользуются правами и несут обязанности в соответствии с
Трудовым кодексом Республики Казахстан.

#v(6mm)
*6. Тараптардың деректемелері мен қолдары / Реквизиты и подписи сторон*

#v(2mm)
// \hrulefill equivalent. A bare `line(length: 100%)` here looked right in the
// source and rendered ON TOP of the two rows above it: `line` contributes no
// height, so the grid row collapsed to zero and the rule struck through the
// party names. A stroked box with an EXPLICIT height does carry its row.
// Spike defect 3. Reproduced against this file at 200dpi: with `line` the rule
// lands at y=551.9pt, dead through the ink of "БСН/БИН 104332181962" and
// "ЖСН/ИИН 900112350487"; with this box it lands at y=574.9pt, in white space,
// 1.1pt above the director line. check-render.py reports PASS on BOTH — same
// 280 word boxes, all inside the margin. Invisible to pdftotext by
// construction; only a raster shows it.
#let sigrule = box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))
#grid(
  columns: (45%, 45%),
  column-gutter: 1fr,
  row-gutter: (2mm, 0.6mm, 0mm, 0.6mm),
  [Жұмыс беруші / Работодатель:], [Қызметкер / Работник:],
  [#d.employer.name], [#d.employee.full_name],
  [БСН/БИН #d.employer.bin], [ЖСН/ИИН #d.employee.iin],
  sigrule, sigrule,
  [#d.employer.director], [#d.employee.full_name],
)
