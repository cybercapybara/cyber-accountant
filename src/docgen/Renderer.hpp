/**
 * @file Renderer.hpp
 * @brief inja-based rendering of a docgen template against input JSON, with
 *        automatic LaTeX escaping of the string leaves that are TEXT.
 *
 * Mirrors `Email::Templates::render` (src/email/Templates.hpp) — same
 * `inja::Environment env; env.render(tpl, ctx);` shape — but the context is
 * preprocessed first: string values in the input tree are run through
 * `escape_latex` (LatexEscape.hpp) so callers can hand this raw,
 * schema-validated business data (a counterparty name, an invoice line
 * item) without pre-escaping it themselves, while numbers/booleans/null
 * pass through untouched for inja to stringify normally. Templates use
 * inja's default `{{ }}` expression / `{% %}` statement delimiters — no
 * template author needs to know escaping happens at all.
 *
 * WHEN escaping happens, and why it is not simply "all of it, up front".
 * ---------------------------------------------------------------------
 * Escaping and templating cannot both own the whole tree:
 *
 *   * escape everything BEFORE inja and the template's own control flow is
 *     evaluated against escaped data — `{% if balance_kind == "to_pay" %}`
 *     tests `to\_pay`, never matches, and the block silently vanishes. That
 *     shipped: every ФНО 300.00 rendered without its closing line and
 *     `balance_words`, and hr_order's `business_trip`/`salary_change` orders
 *     rendered with no body at all. No error, no warning — the PDF was
 *     produced, stored and handed to the user;
 *   * escape everything AFTER inja and the template's OWN LaTeX (`\hrulefill`,
 *     `\begin{tabularx}`, `#1` macro parameters) gets escaped into visible
 *     garbage. Strictly worse.
 *
 * So the decision is made per LEAF, and the thing that knows which leaves are
 * printed text and which are control values is the template's JSON Schema —
 * already the API contract, already walked by
 * `TemplateRegistry::normalize_input`. A string leaf whose schema node pins
 * it to an `enum` of literals, and whose value IS one of those literals, is a
 * control value: it is what the template branches on, and it is a byte-exact
 * copy of a string this repo wrote in its own `schema.json`. It is passed
 * through raw. Every other string leaf — all free text, all caller-authored
 * data — is escaped exactly as before. `detail::escape_tree` below is that
 * walk; `detail::is_control_literal` is that test.
 *
 * The corollary a template author has to know (and the only one): an
 * `enum`-pinned field is a control value, so branch on it — do not PRINT it.
 * `to_pay` typeset raw would be a LaTeX error (a bare `_` in text mode), and
 * a control identifier is never the right thing to show a reader anyway. Each
 * shipped one is already declared `unprinted` in its template's
 * `expected.txt`, and `ShippedTemplatesTest.NeverPrintAnEnumPinnedField`
 * (tests/unit/test_template_registry.cpp) fails the build if a template
 * starts printing one.
 *
 * Comment delimiters are NOT the inja default, though: inja's `{# ... #}`
 * collides head-on with plain LaTeX. Every shipped template defines
 * `\newcommand{\field}[1]{\textbf{#1}}` (or similar), and `{#1}` in that
 * macro body starts with the two characters `{#` — inja's default
 * comment-open token — with no matching `#}` anywhere in the file, so the
 * parser runs off the end looking for a close and the whole render fails
 * with a parser_error at EOF. `#1`-style TeX macro parameters are routine
 * and unavoidable in LaTeX-first templates, so the comment markers were
 * moved instead: `((#` / `#))` (see `set_comment` below) — a sequence that
 * cannot occur in valid LaTeX. `{% %}`/`{{ }}` stay untouched (see the
 * exhaustive grep over every shipped `template.tex` in
 * `templates/latex/README.md`: no literal, non-inja `{%` exists today).
 */

#pragma once

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <inja/inja.hpp>
#include <nlohmann/json.hpp>

#include "docgen/LatexEscape.hpp"
#include "docgen/TemplateRegistry.hpp"

namespace Docgen {

using json = nlohmann::json;

namespace detail {

/// The sub-schema to use when the walk has no schema node for a value: an
/// empty object declares nothing, so `is_control_literal` says "not a
/// control value" and the leaf is escaped. Absent schema information must
/// always fall back to escaping — that is the safe direction.
inline const json k_no_schema = json::object();

/**
 * @brief Is @p value one of the string literals @p node_in pins via `enum`?
 * @details This is the whole of the "printed text vs. control value"
 *          decision, and it is deliberately a membership test on the VALUE,
 *          not a property of the field. A field whose schema says
 *          `"enum": ["to_pay", "to_refund"]` only yields a control literal
 *          when the value actually IS `to_pay` or `to_refund`; anything else
 *          — a value from a caller that somehow got past
 *          `TemplateRegistry::validate`, a schema that grew a non-string
 *          enum member — is escaped exactly as before. So the set of bytes
 *          that reach the `.tex` unescaped is a subset of the literals
 *          written in the repo's own `schema.json`; a request can only
 *          SELECT among them, never contribute a byte to them.
 */
inline bool is_control_literal(const json& root_schema, const json& node_in, const std::string& value) {
    const json& node = TemplateRegistry::resolve_ref(root_schema, node_in);
    if (!node.is_object() || !node.contains("enum"))
        return false;
    const json& literals = node.at("enum");
    if (!literals.is_array())
        return false;
    for (const auto& literal : literals) {
        if (literal.is_string() && literal.get<std::string>() == value)
            return true;
    }
    return false;
}

/**
 * @brief Recursively escape the string leaves of @p node in place, walking
 *        @p schema_node (a node of @p root_schema) alongside it.
 * @details Objects recurse through `properties`, arrays through `items`,
 *          both after `$ref` resolution — the same walk
 *          `TemplateRegistry::normalize_input` does, so the two agree on
 *          which schema node describes which value. Numbers, booleans and
 *          null are left as-is (inja stringifies them itself, and there is
 *          nothing LaTeX-unsafe about them).
 *
 *          A string leaf is escaped UNLESS it is a control literal (see
 *          `is_control_literal`). That exception is the fix for the defect
 *          this walk was written for: escaping used to run over the whole
 *          input tree indiscriminately, so `{% if balance_kind == "to_pay" %}`
 *          was evaluated against `to\_pay` and never matched — every ФНО
 *          300.00 silently lost its closing line, and hr_order's
 *          `business_trip` / `salary_change` orders lost their entire body.
 *          A value a template COMPARES is not text being typeset, and
 *          escaping it is not a safety measure but a corruption of it.
 * @note Nothing else in the pipeline stops escaping. Every free-text field
 *       — a counterparty name, a line item, a director's name — is still
 *       escaped, because none of them is pinned by an `enum`.
 */
inline void escape_tree(const json& root_schema, const json& schema_node, json& node) {
    const json& schema = TemplateRegistry::resolve_ref(root_schema, schema_node);

    if (node.is_string()) {
        const std::string value = node.get<std::string>();
        if (!is_control_literal(root_schema, schema, value))
            node = escape_latex(value);
        return;
    }
    if (node.is_object()) {
        const bool has_props =
            schema.is_object() && schema.contains("properties") && schema.at("properties").is_object();
        for (auto& item : node.items()) {
            const bool described = has_props && schema.at("properties").contains(item.key());
            escape_tree(root_schema, described ? schema.at("properties").at(item.key()) : k_no_schema, item.value());
        }
        return;
    }
    if (node.is_array()) {
        const bool has_items = schema.is_object() && schema.contains("items");
        for (auto& value : node)
            escape_tree(root_schema, has_items ? schema.at("items") : k_no_schema, value);
    }
    // number / boolean / null: nothing to do.
}

}  // namespace detail

/**
 * @brief Render @p info's `template.tex` against @p input.
 * @details Deep-copies @p input, LaTeX-escapes its string leaves — every one
 *          of them except the control literals @p info's schema pins with an
 *          `enum` — then renders the copy through inja. @p input itself is
 *          never mutated.
 * @throws std::runtime_error if the template file can't be read or inja
 *         fails to render (undefined variable, malformed template, ...).
 */
inline std::string render_tex(const TemplateInfo& info, const json& input) {
    json escaped = input;
    detail::escape_tree(info.schema, info.schema, escaped);

    std::ifstream tex_file(info.tex_path, std::ios::binary);
    if (!tex_file)
        throw std::runtime_error("render_tex: cannot open " + info.tex_path.string());
    std::ostringstream ss;
    ss << tex_file.rdbuf();
    const std::string tpl_source = ss.str();

    try {
        inja::Environment env;
        // "((#" / "#))" — see the file header: inja's default "{#"/"#}"
        // collides with `#1`-style LaTeX macro parameters (`{#1}`).
        env.set_comment("((#", "#))");
        return env.render(tpl_source, escaped);
    } catch (const std::exception& e) {
        throw std::runtime_error("render_tex: inja render failed for " + info.slug + "/" + info.version_str + ": " +
                                 e.what());
    }
}

}  // namespace Docgen
