/**
 * @file Renderer.hpp
 * @brief inja-based rendering of a docgen template against input JSON, with
 *        automatic LaTeX escaping of every string leaf.
 *
 * Mirrors `Email::Templates::render` (src/email/Templates.hpp) — same
 * `inja::Environment env; env.render(tpl, ctx);` shape — but the context is
 * preprocessed first: every string value in the input tree is run through
 * `escape_latex` (LatexEscape.hpp) so callers can hand this raw,
 * schema-validated business data (a counterparty name, an invoice line
 * item) without pre-escaping it themselves, while numbers/booleans/null
 * pass through untouched for inja to stringify normally. Templates use
 * inja's default `{{ }}` expression / `{% %}` statement delimiters — no
 * template author needs to know escaping happens at all.
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

/// Recursively escape every string leaf of @p node in place. Objects and
/// arrays are walked; numbers, booleans and null are left as-is (inja
/// stringifies them itself, and there is nothing LaTeX-unsafe about them).
inline void escape_tree(json& node) {
    if (node.is_string()) {
        node = escape_latex(node.get<std::string>());
    } else if (node.is_object()) {
        for (auto& item : node.items())
            escape_tree(item.value());
    } else if (node.is_array()) {
        for (auto& value : node)
            escape_tree(value);
    }
    // number / boolean / null: nothing to do.
}

}  // namespace detail

/**
 * @brief Render @p info's `template.tex` against @p input.
 * @details Deep-copies @p input, LaTeX-escapes every string leaf, then
 *          renders the copy through inja. @p input itself is never mutated.
 * @throws std::runtime_error if the template file can't be read or inja
 *         fails to render (undefined variable, malformed template, ...).
 */
inline std::string render_tex(const TemplateInfo& info, const json& input) {
    json escaped = input;
    detail::escape_tree(escaped);

    std::ifstream tex_file(info.tex_path, std::ios::binary);
    if (!tex_file)
        throw std::runtime_error("render_tex: cannot open " + info.tex_path.string());
    std::ostringstream ss;
    ss << tex_file.rdbuf();
    const std::string tpl_source = ss.str();

    try {
        inja::Environment env;
        return env.render(tpl_source, escaped);
    } catch (const std::exception& e) {
        throw std::runtime_error("render_tex: inja render failed for " + info.slug + "/" + info.version_str + ": " +
                                 e.what());
    }
}

}  // namespace Docgen
