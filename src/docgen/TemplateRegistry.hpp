/**
 * @file TemplateRegistry.hpp
 * @brief Discovers docgen document templates on disk, decides which engine
 *        compiles each one, and validates input JSON against their JSON
 *        Schema.
 *
 * Layout convention (design spec, Task 9's `templates/latex/invoice/v1/` is
 * the reference implementation): `templates/latex/<slug>/v<N>/` holding
 * `template.typ` (Typst) **or** `template.tex` (LaTeX, being retired)
 * + `schema.json` (+ one JSON fixture per case under
 * `fixtures/`, e.g. `fixtures/happy_path.json`, used by
 * `scripts/render-templates.sh`, not read by this class). `<N>` is a plain
 * non-negative integer — `latest(slug)` picks the directory with the
 * highest `N`, so shipping `v2` alongside `v1` makes `v2` the default
 * without deleting the old version (documents already rendered from `v1`
 * keep their `template_version` snapshot either way).
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

namespace Docgen {

using json = nlohmann::json;

/// Which document engine renders a template, decided by the source file that
/// is on disk. The migration converts one template at a time
/// (docs/superpowers/plans/2026-08-16-typst-migration.md), so both values are
/// live simultaneously and the worker image carries both engines.
enum class Engine { kLatex, kTypst };

/// The stable, storable name of an engine — this is the string a later task
/// records alongside the document's template version, so it is decided in
/// exactly one place and never re-derived at a call site.
inline const char* engine_name(Engine e) {
    return e == Engine::kTypst ? "typst" : "xelatex";
}

/// One resolved template version: where its source lives on disk, which
/// engine compiles it, and its parsed JSON Schema.
struct TemplateInfo {
    std::string slug;
    int version = 0;                    ///< Numeric part of the `vN` directory name.
    std::string version_str;            ///< The `vN` directory name as it appears on disk.
    std::filesystem::path dir;          ///< `templates/latex/<slug>/<vN>/`
    std::filesystem::path source_path;  ///< `template.typ` (Typst) or `template.tex` (LaTeX)
    Engine engine = Engine::kLatex;
    json schema;
};

/// Scans a `templates/latex/` tree (default root, overridable for tests) for
/// `<slug>/v<N>/` directories.
class TemplateRegistry {
public:
    explicit TemplateRegistry(std::filesystem::path root = "templates/latex") : root_(std::move(root)) {}

    /**
     * @brief Resolve the highest-version template for @p slug.
     * @details `slug` becomes a raw filesystem path component
     *          (`root_ / slug / ...`) below, so it is validated FIRST against
     *          an allowlist before it ever touches `std::filesystem` — a
     *          slug of `"../../etc"` (or containing `/`, `.`, uppercase,
     *          etc.) must never be allowed to walk outside the templates
     *          root. This is the one choke point every slug-taking entry
     *          point in this class (and `RenderJob::render_and_compile`,
     *          which resolves templates exclusively through here) goes
     *          through — do not add another path that builds `root_ / slug`
     *          without this check.
     * @return `std::nullopt` if @p slug fails the allowlist, the slug
     *         directory doesn't exist, or it contains no valid `vN`
     *         subdirectory.
     * @throws std::runtime_error if the highest-version directory has
     *         NEITHER `template.typ` nor `template.tex`, or is missing
     *         `schema.json` (a malformed template ships as a hard error, not
     *         a silent "not found").
     */
    std::optional<TemplateInfo> latest(const std::string& slug) const {
        namespace fs = std::filesystem;
        if (!is_valid_slug(slug))
            return std::nullopt;
        const fs::path slug_dir = root_ / slug;
        std::error_code ec;
        if (!fs::is_directory(slug_dir, ec))
            return std::nullopt;

        int best_version = -1;
        std::string best_version_str;
        for (const auto& entry : fs::directory_iterator(slug_dir, ec)) {
            if (!entry.is_directory())
                continue;
            const std::string name = entry.path().filename().string();
            if (auto v = parse_version(name); v && *v > best_version) {
                best_version = *v;
                best_version_str = name;
            }
        }
        if (best_version < 0)
            return std::nullopt;

        return load(slug, best_version_str, best_version);
    }

    /**
     * @brief Validate @p input against @p slug's latest schema.
     * @return `std::nullopt` on success, else a human-readable validation
     *         error message.
     */
    std::optional<std::string> validate(const std::string& slug, const json& input) const {
        auto info = latest(slug);
        if (!info)
            return "no template found for slug '" + slug + "'";
        return validate(*info, input);
    }

    /**
     * @brief Task 13 addition: every registered template's latest version —
     *        powers `GET /api/v1/doc-templates` (the frontend needs
     *        {slug, version, schema} for each template to build its
     *        generate forms).
     * @details Walks `root_`'s immediate subdirectories exactly the way
     *          `latest()` walks a single slug's `vN` children, running each
     *          directory NAME through the SAME `is_valid_slug()` allowlist
     *          before it is ever used as a `root_ / name` path component —
     *          the one-choke-point invariant documented on `latest()`
     *          extends to this scan too, so a stray non-template directory
     *          on disk can never be walked just because it showed up in a
     *          `directory_iterator`. A name that fails the allowlist, or
     *          that `latest()` itself resolves to `std::nullopt` (no valid
     *          `vN` subdirectory), is silently skipped — this is a
     *          discovery scan over whatever happens to exist on disk, not a
     *          caller asking for one specific slug, so "not a template" is
     *          not an error here the way it is in `validate()`.
     * @return one `TemplateInfo` per discovered slug, sorted by slug so the
     *         API response order is stable across calls.
     */
    std::vector<TemplateInfo> list() const {
        namespace fs = std::filesystem;
        std::vector<TemplateInfo> out;
        std::error_code ec;
        if (!fs::is_directory(root_, ec))
            return out;

        std::vector<std::string> slugs;
        for (const auto& entry : fs::directory_iterator(root_, ec)) {
            if (!entry.is_directory())
                continue;
            const std::string name = entry.path().filename().string();
            if (is_valid_slug(name))
                slugs.push_back(name);
        }
        std::sort(slugs.begin(), slugs.end());

        out.reserve(slugs.size());
        for (const auto& slug : slugs) {
            if (auto info = latest(slug))
                out.push_back(std::move(*info));
        }
        return out;
    }

    /// Validate @p input against an already-resolved @p info's schema —
    /// avoids a redundant disk scan when the caller already has a
    /// TemplateInfo (e.g. RenderJob, after calling latest() once).
    static std::optional<std::string> validate(const TemplateInfo& info, const json& input) {
        try {
            nlohmann::json_schema::json_validator validator;
            validator.set_root_schema(info.schema);
            validator.validate(input);
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
        return std::nullopt;
    }

    /**
     * @brief Schema-driven default-fill: for every property @p schema
     *        declares (recursively, resolving one-level `"$ref":
     *        "#/definitions/..."` — the only `$ref` shape any shipped
     *        schema uses) that is MISSING from @p input, add a
     *        type-appropriate zero value — `string` -> `""`, `array` ->
     *        `[]`, `object` -> itself recursively filled per its own
     *        declared properties. Values already present are never touched
     *        (except to recurse into an already-present nested object, so
     *        its OWN missing optional fields get filled too).
     * @details Why this exists: `{{ }}`/`{% if %}` in a template dot into
     *          paths like `seller.address` that a schema marks optional —
     *          when the caller (correctly) omits an optional field, inja
     *          throws `render_error: variable 'seller.address' not found`
     *          on the bare reference, not a graceful "undefined" the way
     *          Jinja2 treats a missing key. Call this AFTER a successful
     *          `validate()` and BEFORE `render_tex()` — `RenderJob::
     *          render_and_compile` (src/docgen/RenderJob.hpp) is the one
     *          place both the render job and the `--render-template` CLI
     *          mode funnel through, so this is wired in exactly once.
     * @note inja's `truthy()` (vendored `inja.hpp`, confirmed by reading the
     *       pinned v3.4.0 source) falls back to `!value.empty()` for any
     *       type it doesn't special-case, and `nlohmann::json::empty()`
     *       ONLY reports true for `null`/an empty `array`/an empty `object`
     *       — a JSON **string** is never "json-empty" regardless of its
     *       content, so `""` is TRUTHY in inja. Filling a missing optional
     *       string with `""` therefore stops the "not found" crash but does
     *       NOT make `{% if seller.address %}` skip — every shipped
     *       template's `{% if %}` on an optional *string* field was
     *       rewritten to `{% if X != "" %}` to compensate (safe only
     *       because this function guarantees `X` exists by the time the
     *       template runs). An optional *object* filled by this function
     *       (e.g. reconciliation's `opening_balance`) is likewise never
     *       "json-empty" once its declared properties are filled in — such
     *       templates check a representative leaf value instead of the
     *       whole object (see `templates/latex/reconciliation/v1/template.tex`).
     */
    static json normalize_input(const json& schema, json input) {
        if (!input.is_object())
            return input;
        return detail::fill_object(schema, schema, std::move(input));
    }

    /**
     * @brief Resolve a one-level `"$ref": "#/definitions/X"` schema node
     *        against the root schema document @p root.
     * @details Any other node (no `$ref`, or a `$ref` shape this codebase
     *          does not produce) passes through unchanged, so this is safe
     *          to call on every node of a schema walk.
     * @note Public because TWO schema walks need it now: `normalize_input`
     *       below and `Docgen::detail::escape_tree` (src/docgen/Renderer.hpp),
     *       which decides per leaf whether a string is printed text (escape)
     *       or a schema-pinned control literal (leave raw). Both must resolve
     *       `$ref` identically or they would disagree about which node
     *       describes a given value — hence one implementation, not two.
     * @return A reference into @p root (or @p node itself); both must
     *         outlive the returned reference.
     */
    static const json& resolve_ref(const json& root, const json& node) {
        if (!node.is_object() || !node.contains("$ref"))
            return node;
        const std::string ref = node.at("$ref").get<std::string>();
        static const std::string kPrefix = "#/definitions/";
        if (ref.rfind(kPrefix, 0) != 0)
            return node;
        const std::string name = ref.substr(kPrefix.size());
        if (root.contains("definitions") && root.at("definitions").contains(name))
            return root.at("definitions").at(name);
        return node;
    }

private:
    /// Implementation details for normalize_input() — kept out of the
    /// public surface since neither helper makes sense called on its own
    /// (both need the ROOT schema doc to resolve "$ref" against, not just
    /// whatever sub-schema node they're currently filling).
    struct detail {
        /// Type-appropriate zero value for a schema node missing from the
        /// input entirely. `object` recurses so a wholly-absent optional
        /// object still gets every one of ITS declared properties filled
        /// in, at every depth.
        static json default_for(const json& root, const json& node_in) {
            const json& node = TemplateRegistry::resolve_ref(root, node_in);
            const std::string type = node.value("type", "");
            if (type == "object")
                return fill_object(root, node, json::object());
            if (type == "array")
                return json::array();
            if (type == "string")
                return "";
            // number/boolean/other -> JSON null. Since P3 one shipped schema
            // DOES declare an optional integer (invoice/avr's `vat_tiyn`), so
            // this branch is reachable; null is still the right answer and is
            // inert, because no template prints a *_tiyn field — templates
            // only ever print the string the server formatted from it, and
            // that string is absent too when the integer is. Do not "fix"
            // this to 0: an absent optional amount is not an amount of zero.
            return nullptr;
        }

        /// Fill every property @p node_in declares (after $ref resolution)
        /// that @p value is missing; recurse into a property @p value
        /// already has when IT is an object, so partially-provided nested
        /// objects get their own missing optional fields filled too.
        /// @p value must be a JSON object.
        static json fill_object(const json& root, const json& node_in, json value) {
            const json& node = TemplateRegistry::resolve_ref(root, node_in);
            if (!value.is_object() || !node.contains("properties") || !node.at("properties").is_object())
                return value;
            for (const auto& prop : node.at("properties").items()) {
                const std::string& key = prop.key();
                if (!value.contains(key)) {
                    value[key] = default_for(root, prop.value());
                } else if (value.at(key).is_object()) {
                    value[key] = fill_object(root, prop.value(), value.at(key));
                }
                // Existing string/array/number/bool values: left untouched.
            }
            return value;
        }
    };

    /// Allowlist for a template slug: it is used as a raw path component
    /// (`root_ / slug / ...`), so anything outside `^[a-z][a-z0-9_-]*$` is
    /// rejected — no `/`, no `..`, no leading digit/hyphen, no uppercase, no
    /// empty string. This is defense-in-depth path-traversal hardening
    /// (`Storage::key_is_safe`, `Files::sanitize_filename` are the same
    /// posture for object-storage keys / filenames elsewhere in this repo):
    /// a slug ultimately comes from job payloads submitted through the
    /// generic admin job-submission endpoint, not just from code that
    /// already trusts it.
    static bool is_valid_slug(const std::string& slug) {
        if (slug.empty())
            return false;
        if (slug[0] < 'a' || slug[0] > 'z')
            return false;
        for (char c : slug) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!ok)
                return false;
        }
        return true;
    }

    /// "v3" -> 3; anything not matching `v` + one-or-more digits -> nullopt
    /// (so a stray README.md or fixtures-only directory next to `vN` ones is
    /// silently skipped rather than crashing the scan).
    static std::optional<int> parse_version(const std::string& name) {
        if (name.size() < 2 || name[0] != 'v')
            return std::nullopt;
        for (std::size_t i = 1; i < name.size(); ++i) {
            if (std::isdigit(static_cast<unsigned char>(name[i])) == 0)
                return std::nullopt;
        }
        try {
            return std::stoi(name.substr(1));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    TemplateInfo load(const std::string& slug, const std::string& version_str, int version) const {
        namespace fs = std::filesystem;
        TemplateInfo info;
        info.slug = slug;
        info.version = version;
        info.version_str = version_str;
        info.dir = root_ / slug / version_str;
        // template.typ wins: while a template is mid-conversion both files can
        // exist for one commit, and the Typst source is the authoritative one.
        const fs::path typ_path = info.dir / "template.typ";
        const fs::path tex_path = info.dir / "template.tex";
        if (fs::exists(typ_path)) {
            info.source_path = typ_path;
            info.engine = Engine::kTypst;
        } else if (fs::exists(tex_path)) {
            info.source_path = tex_path;
            info.engine = Engine::kLatex;
        } else {
            throw std::runtime_error("template registry: missing template.typ/template.tex for " + slug + "/" +
                                     version_str);
        }

        const fs::path schema_path = info.dir / "schema.json";
        std::ifstream schema_file(schema_path);
        if (!schema_file)
            throw std::runtime_error("template registry: missing schema.json for " + slug + "/" + version_str);
        try {
            schema_file >> info.schema;
        } catch (const std::exception& e) {
            throw std::runtime_error("template registry: malformed schema.json for " + slug + "/" + version_str + ": " +
                                     e.what());
        }
        return info;
    }

    std::filesystem::path root_;
};

}  // namespace Docgen
