/**
 * @file LatexEscape.hpp
 * @brief Escape an arbitrary UTF-8 string for safe interpolation into LaTeX
 *        source.
 *
 * `Docgen::render_tex` (Renderer.hpp) runs every string leaf of a template's
 * input tree through this before handing the tree to inja, so a counterparty
 * name containing `&`, a contract number containing `#`, or a line item
 * containing `\` never breaks — or, worse, injects — the compiled `.tex`.
 * Only the fixed ASCII set of LaTeX-special characters is rewritten; every
 * other byte (including every multi-byte UTF-8 sequence — Cyrillic, Kazakh
 * letters, etc.) is copied through unchanged, since none of those bytes can
 * collide with the ASCII values switched on below.
 */

#pragma once

#include <string>

namespace Docgen {

/**
 * @brief Escape LaTeX-special characters in @p input.
 * @details Single left-to-right pass over the input bytes; each recognized
 *          character is replaced by its escaped form exactly once, so the
 *          output is never re-scanned and can't be double-escaped. Handles:
 *          `\` `{` `}` `$` `&` `#` `^` `_` `%` `~` `<` `>`.
 * @param input Raw (untrusted) UTF-8 string.
 * @return LaTeX-safe string, safe to place inside `.tex` source.
 */
inline std::string escape_latex(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '\\':
                out += "\\textbackslash{}";
                break;
            case '{':
                out += "\\{";
                break;
            case '}':
                out += "\\}";
                break;
            case '$':
                out += "\\$";
                break;
            case '&':
                out += "\\&";
                break;
            case '#':
                out += "\\#";
                break;
            case '^':
                out += "\\textasciicircum{}";
                break;
            case '_':
                out += "\\_";
                break;
            case '%':
                out += "\\%";
                break;
            case '~':
                out += "\\textasciitilde{}";
                break;
            case '<':
                out += "\\textless{}";
                break;
            case '>':
                out += "\\textgreater{}";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

}  // namespace Docgen
