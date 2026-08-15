# LaTeX document templates (docgen)

Each subdirectory is one **template slug** (a document type: `invoice`,
`avr`, `waybill`, `tax_invoice`, `reconciliation`, ...). `Docgen::TemplateRegistry`
(`src/docgen/TemplateRegistry.hpp`) scans this tree at render time — nothing
here is baked into a database or a compiled registry, so adding a template is
just adding files in the right layout.

## Layout

```
templates/latex/<slug>/v<N>/
├── template.tex        # inja template — LaTeX source with {{ }} / {% %} placeholders
├── schema.json          # JSON Schema (draft-07) the input JSON must satisfy
└── fixtures/
    └── *.json           # sample inputs, rendered by scripts/render-templates.sh
```

- `<slug>` is the document type, matching `documents.doc_type` /
  `documents.template_slug` (migrations/010_documents.sql).
- `v<N>` is a plain non-negative integer version directory (`v1`, `v2`, ...).
  `TemplateRegistry::latest(slug)` always picks the **highest** `N` — ship a
  new version alongside the old one rather than editing it in place, since a
  document already rendered from `v1` keeps that version in its
  `template_version` snapshot (`Ledger::Document::template_version`).
- `fixtures/*.json` are sample inputs used ONLY by
  `scripts/render-templates.sh` (the `template-render` CI job) to smoke-test
  that the template still compiles under a real XeLaTeX **and that nothing
  overhangs the page** (see "Tables start their own paragraph") — they are not
  read by the render job itself.

`templates/latex/invoice/v1/` is the reference implementation.

## Writing a template

- Use inja's default delimiters: `{{ expr }}` for expressions, `{% if %}` /
  `{% for %}` / `{% endif %}` / `{% endfor %}` for control flow. See
  [pantor/inja](https://github.com/pantor/inja) for the full syntax
  (a Jinja2 subset).
- **Comments are `((# ... #))`, NOT inja's default `{# ... #}`.** Every
  shipped template defines `\newcommand{\field}[1]{\textbf{#1}}` (or a
  similar one-arg macro) — `#1`/`#2`/... macro parameters are routine and
  unavoidable in LaTeX, and `{#1}` starts with the exact two characters
  inja treats as "open comment" by default. With no matching `#}` in the
  file, that silently turns the rest of the template into an unterminated
  comment and the render fails with a parser error at EOF ("expected
  comment close, got '<eof>'"). `Docgen::render_tex` (`src/docgen/Renderer.hpp`)
  remaps the comment markers to `((#` / `#))` via inja's
  `Environment::set_comment(open, close)` — a sequence that cannot occur in
  valid LaTeX — specifically so template authors never have to dodge this.
  Write `((# like this #))` for an inja-only comment that doesn't end up in
  the rendered `.tex`; do not use `{# #}`, it is not special anymore.
  (`{% %}` was checked too: every `template.tex` shipped as of this writing
  was grepped for a literal, non-inja `{%` — LaTeX has no construct that
  produces one on its own, since `%` starts a LaTeX line comment and would
  need a `{` immediately before it to collide, which none of these
  templates do — so `{% %}` was left at its inja default. If a future
  template ever needs a literal `{%` in its LaTeX body, re-run that grep and
  remap `set_statement` the same way.)
- Every **string** value in the input JSON is automatically escaped for
  LaTeX (`Docgen::escape_latex`, `src/docgen/LatexEscape.hpp`) before
  substitution — `\ { } $ & # ^ _ % ~ < >` all become their safe LaTeX
  form. Do **not** escape values yourself in `schema.json`/fixtures; numbers
  and booleans pass through unescaped.
- `schema.json` is the contract: `RenderJob` (and `render-templates.sh`)
  reject any input that doesn't satisfy it — required fields, types, string
  patterns (e.g. a `date` field constrained to `DD.MM.YYYY`) all belong
  there, not in ad-hoc template logic.
- Fonts: templates render under XeLaTeX with `polyglossia` + `fontspec` for
  Cyrillic/Kazakh text (see `docker/Dockerfile`'s worker stage for the
  installed TeX Live + font packages). Stick to `\setmainfont{Noto Sans}` (or
  another font actually installed in the worker image) rather than assuming
  a font is present.

### Tables start their own paragraph

A `\textwidth`-wide `tabularx`/`tabular` is a single unbreakable box. If the
block of text above it has not ended its paragraph, LaTeX typesets that box
**inline**, continuing the last line — and the box then hangs off the right
edge of the page. XeLaTeX reports this only as `Overfull \hbox (...pt too
wide)`, a *warning*: the compile still exits 0 and a PDF is still produced,
just with the right-hand columns pushed off the paper. This shipped in v0.3.0
(ФНО 910.00 printed `10 000` where the amount was `10 000 000,00`; the payslip
lost its entire amounts column *and* the column's static header).

So, above every full-width table:

- Terminate the block with an **unconditional `\par`**, written immediately
  before the table's own `\vspace`. Never put the terminator inside an
  `{% if %}` — an optional field that is absent then takes the paragraph break
  with it, which is exactly how `invoice`/`tax_invoice` acquired the same bug.
- In a block whose **last** line is optional, end *every* line with `\par`
  rather than `\\`. `\par` is idempotent, so the extra one contributed by a
  skipped `{% if %}` line (a skipped block still leaves its newline behind, and
  two newlines are a paragraph break) is a harmless no-op. `\\` is not
  idempotent in either direction: a trailing `\\` immediately before a `\par`
  emits a spurious blank line, and a leading `\\` right after one is the hard
  error `There's no line here to end`.
- Each `\par`-terminated line needs its own `\noindent`, since it is now a
  paragraph rather than a `\\`-continuation.
- Long Kazakh prose has no hyphenation patterns under polyglossia's `russian`
  setup and can overhang by a few points on its own; `\emergencystretch=2em`
  in the preamble (see `labor_contract`) lets TeX loosen the line instead.

`scripts/render-templates.sh` fails the `template-render` CI job on any
overfull `\hbox` over `OVERFULL_MAX_PT` (default 1.0pt), so a regression here
is caught before release rather than on a printed declaration.

## Testing a template

- Unit tests (`tests/unit/test_template_registry.cpp`,
  `tests/unit/test_latex_escape.cpp`) cover discovery/validation/escaping
  without invoking XeLaTeX at all.
- `tests/integration/test_render_job.cpp` exercises the full `docgen.render`
  job pipeline with a stubbed `DOCGEN_LATEX_CMD` (no real TeX Live needed).
- The **real** XeLaTeX compile only runs via
  `./scripts/render-templates.sh` against every `fixtures/*.json` under this
  tree — locally on a machine with TeX Live installed, or via the
  `template-render` CI job on the worker image. Add a fixture for every new
  template (and every new required/optional field combination worth
  covering) so a broken template fails there instead of in production.
  That script gates on the XeLaTeX *transcript*, not just the exit code: any
  `Overfull \hbox` over `OVERFULL_MAX_PT` (default 1.0pt) fails the job,
  because a document that overhangs the page still compiles happily.
