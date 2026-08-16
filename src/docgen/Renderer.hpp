/**
 * @file Renderer.hpp
 * @brief Stage a Typst render: the template plus the normalized input JSON,
 *        written side by side into a scratch directory.
 *
 * There is no templating layer and nothing to escape. Typst opens the data
 * itself (`#let d = json("input.json")`), so a tenant-supplied value is
 * content and never source: the literal string
 * `#panic("x") *bold* #read("/etc/passwd")` is typeset character for
 * character (verified in .superpowers/sdd/typst-migration-spike.md §3 and
 * pinned by templates/docs/<slug>/v1/fixtures/special-chars.json, whose
 * values carry exactly that payload and must appear verbatim in the rendered
 * PDF). The slug is spelled out rather than globbed on purpose: a literal
 * `*` before the `/` would close this comment block.
 *
 * This replaced an inja + escape_latex pipeline whose two failure modes are
 * worth remembering, because the constructor phase must not reintroduce
 * them: escaping BEFORE templating made a template's own control flow run
 * against escaped data (`{% if balance_kind == "to_pay" %}` tested
 * `to\_pay`, and every ФНО 300.00 silently lost its closing line), and
 * escaping AFTER it mangled the template's own markup. Under Typst the
 * question does not arise — data never reaches a parser.
 *
 * `TemplateRegistry::normalize_input` is MORE load-bearing than it was:
 * Typst raises a hard error on a declared-but-absent optional key where inja
 * merely printed nothing.
 */

#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "docgen/TemplateRegistry.hpp"

namespace Docgen {

using json = nlohmann::json;

/**
 * @brief Stage a Typst render in @p out_dir: `main.typ` (a copy of the
 *        template) + `input.json` (the normalized input).
 * @details There is no templating layer here and nothing to escape. Typst
 *          reads the JSON itself (`#let d = json("input.json")`), so a value
 *          is content and never source: the literal string
 *          `#panic("x") *bold* #read("/etc/passwd")` is typeset character for
 *          character. That is why `escape_latex` was deleted rather than
 *          ported when the last LaTeX template went — there is nothing for a
 *          Typst equivalent to do, and an escaper on this path could only
 *          corrupt the data it touched.
 * @param normalized_input MUST be the output of
 *        `TemplateRegistry::normalize_input` — Typst raises a hard error
 *        ("dictionary does not contain key") on a declared-but-absent
 *        optional key, where inja merely printed nothing. Verified against
 *        typst 0.15.1.
 * @throws std::runtime_error if either file cannot be written (a missing
 *         @p out_dir included — the copy is where that surfaces, named,
 *         rather than as a puzzling "file not found" out of the engine).
 */
inline void write_typst_inputs(const TemplateInfo& info,
                               const json& normalized_input,
                               const std::filesystem::path& out_dir) {
    std::error_code ec;
    std::filesystem::copy_file(
        info.source_path, out_dir / "main.typ", std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        throw std::runtime_error("write_typst_inputs: cannot copy " + info.source_path.string() + " to " +
                                 (out_dir / "main.typ").string() + ": " + ec.message());

    std::ofstream data(out_dir / "input.json", std::ios::binary | std::ios::trunc);
    if (!data)
        throw std::runtime_error("write_typst_inputs: cannot write input.json to " + out_dir.string());
    data << normalized_input.dump();
    data.close();
    if (!data)
        throw std::runtime_error("write_typst_inputs: failed writing input.json to " + out_dir.string());
}

}  // namespace Docgen
