/**
 * @file TemplateRegistry.hpp
 * @brief Discovers docgen LaTeX templates on disk and validates input JSON
 *        against their JSON Schema.
 *
 * Layout convention (design spec, Task 9's `templates/latex/invoice/v1/` is
 * the reference implementation): `templates/latex/<slug>/v<N>/` holding
 * `template.tex` + `schema.json` (+ `fixtures/*.json` used by
 * `scripts/render-templates.sh`, not read by this class). `<N>` is a plain
 * non-negative integer — `latest(slug)` picks the directory with the
 * highest `N`, so shipping `v2` alongside `v1` makes `v2` the default
 * without deleting the old version (documents already rendered from `v1`
 * keep their `template_version` snapshot either way).
 */

#pragma once

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

namespace Docgen {

using json = nlohmann::json;

/// One resolved template version: where its `.tex` source lives on disk and
/// its parsed JSON Schema.
struct TemplateInfo {
    std::string slug;
    int version = 0;            ///< Numeric part of the `vN` directory name.
    std::string version_str;    ///< The `vN` directory name as it appears on disk.
    std::filesystem::path dir;  ///< `templates/latex/<slug>/<vN>/`
    std::filesystem::path tex_path;
    json schema;
};

/// Scans a `templates/latex/` tree (default root, overridable for tests) for
/// `<slug>/v<N>/` directories.
class TemplateRegistry {
public:
    explicit TemplateRegistry(std::filesystem::path root = "templates/latex") : root_(std::move(root)) {}

    /**
     * @brief Resolve the highest-version template for @p slug.
     * @return `std::nullopt` if the slug directory doesn't exist or contains
     *         no valid `vN` subdirectory.
     * @throws std::runtime_error if the highest-version directory is missing
     *         `template.tex` or `schema.json` (a malformed template ships as
     *         a hard error, not a silent "not found").
     */
    std::optional<TemplateInfo> latest(const std::string& slug) const {
        namespace fs = std::filesystem;
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

private:
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
        info.tex_path = info.dir / "template.tex";
        if (!fs::exists(info.tex_path))
            throw std::runtime_error("template registry: missing template.tex for " + slug + "/" + version_str);

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
