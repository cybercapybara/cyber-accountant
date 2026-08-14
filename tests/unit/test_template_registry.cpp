/**
 * @file test_template_registry.cpp
 * @brief Unit tests for Docgen::TemplateRegistry — discovery + schema
 *        validation against a fixture tree under a temp directory — and for
 *        Docgen::render_tex (Renderer.hpp), which operates on the same
 *        TemplateInfo this registry resolves. No Config, no network, no
 *        sidecars, no XeLaTeX.
 */

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "docgen/Renderer.hpp"
#include "docgen/TemplateRegistry.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

class TemplateRegistryTest : public ::testing::Test {
protected:
    fs::path root_;

    void SetUp() override {
        root_ = fs::temp_directory_path() / "docgen_registry_test";
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    /// Write templates/<slug>/<version_dir>/template.tex + schema.json.
    void write_version(const std::string& slug,
                       const std::string& version_dir,
                       const std::string& tex = "Hello {{ name }}",
                       const json& schema = json{{"type", "object"},
                                                 {"required", json::array({"name"})},
                                                 {"properties", {{"name", {{"type", "string"}}}}}}) {
        const fs::path dir = root_ / slug / version_dir;
        fs::create_directories(dir);
        std::ofstream(dir / "template.tex") << tex;
        std::ofstream(dir / "schema.json") << schema.dump();
    }
};

TEST_F(TemplateRegistryTest, LatestReturnsNulloptForUnknownSlug) {
    Docgen::TemplateRegistry registry(root_);
    EXPECT_FALSE(registry.latest("no_such_slug").has_value());
}

TEST_F(TemplateRegistryTest, LatestFindsSingleVersion) {
    write_version("invoice", "v1");
    Docgen::TemplateRegistry registry(root_);

    auto info = registry.latest("invoice");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->slug, "invoice");
    EXPECT_EQ(info->version, 1);
    EXPECT_EQ(info->version_str, "v1");
    EXPECT_EQ(info->tex_path, root_ / "invoice" / "v1" / "template.tex");
}

// v10 sorts BEFORE v2 lexicographically ("v10" < "v2") but must win
// numerically — this is exactly the bug a naive string-max would hit.
TEST_F(TemplateRegistryTest, LatestPicksMaxVersionNumerically) {
    write_version("invoice", "v1");
    write_version("invoice", "v2");
    write_version("invoice", "v10");
    Docgen::TemplateRegistry registry(root_);

    auto info = registry.latest("invoice");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->version, 10);
    EXPECT_EQ(info->version_str, "v10");
}

TEST_F(TemplateRegistryTest, IgnoresNonVersionDirectories) {
    write_version("invoice", "v1");
    // A stray directory that doesn't match "v<digits>" must not be picked as
    // a version, and must not crash the scan.
    fs::create_directories(root_ / "invoice" / "notes");
    fs::create_directories(root_ / "invoice" / "vNext");
    Docgen::TemplateRegistry registry(root_);

    auto info = registry.latest("invoice");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->version_str, "v1");
}

TEST_F(TemplateRegistryTest, MissingTemplateTexThrows) {
    fs::create_directories(root_ / "broken" / "v1");
    std::ofstream(root_ / "broken" / "v1" / "schema.json") << json::object().dump();
    Docgen::TemplateRegistry registry(root_);

    EXPECT_THROW(registry.latest("broken"), std::runtime_error);
}

TEST_F(TemplateRegistryTest, MissingSchemaJsonThrows) {
    fs::create_directories(root_ / "broken" / "v1");
    std::ofstream(root_ / "broken" / "v1" / "template.tex") << "x";
    Docgen::TemplateRegistry registry(root_);

    EXPECT_THROW(registry.latest("broken"), std::runtime_error);
}

TEST_F(TemplateRegistryTest, ValidateSucceedsForValidInput) {
    write_version("invoice", "v1");
    Docgen::TemplateRegistry registry(root_);

    auto err = registry.validate("invoice", json{{"name", "Ada"}});
    EXPECT_FALSE(err.has_value());
}

TEST_F(TemplateRegistryTest, ValidateCatchesMissingRequiredField) {
    write_version("invoice", "v1");
    Docgen::TemplateRegistry registry(root_);

    auto err = registry.validate("invoice", json::object());
    ASSERT_TRUE(err.has_value());
    EXPECT_FALSE(err->empty());
}

TEST_F(TemplateRegistryTest, ValidateOnUnknownSlugReturnsError) {
    Docgen::TemplateRegistry registry(root_);
    auto err = registry.validate("nope", json::object());
    ASSERT_TRUE(err.has_value());
}

// The static TemplateInfo-based overload — the one RenderJob uses so it
// doesn't re-scan the filesystem after already resolving latest().
TEST_F(TemplateRegistryTest, ValidateWithResolvedTemplateInfo) {
    write_version("invoice", "v1");
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("invoice");
    ASSERT_TRUE(info.has_value());

    EXPECT_FALSE(Docgen::TemplateRegistry::validate(*info, json{{"name", "Ada"}}).has_value());
    EXPECT_TRUE(Docgen::TemplateRegistry::validate(*info, json::object()).has_value());
}

// ── render_tex (Renderer.hpp) ────────────────────────────────────────────
// Exercises inja substitution plus the automatic LaTeX-escaping pass over
// every string leaf of the input — operates on the TemplateInfo this
// registry resolves, so these tests live alongside it.

TEST_F(TemplateRegistryTest, RenderTexSubstitutesVariables) {
    write_version("greet", "v1", "Hello {{ name }}, total {{ total }}.");
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("greet");
    ASSERT_TRUE(info.has_value());

    auto out = Docgen::render_tex(*info, json{{"name", "Ada"}, {"total", "100"}});
    EXPECT_EQ(out, "Hello Ada, total 100.");
}

// The brief's canonical special-character case: a string containing several
// LaTeX-special characters must come out auto-escaped, substituted in place.
TEST_F(TemplateRegistryTest, RenderTexAutoEscapesSpecialCharactersInBody) {
    write_version("greet", "v1", "Line: {{ line }}");
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("greet");
    ASSERT_TRUE(info.has_value());

    auto out = Docgen::render_tex(*info, json{{"line", "50% скидка & \"спец\" _x_"}});
    EXPECT_EQ(out, "Line: 50\\% скидка \\& \"спец\" \\_x\\_");
}

// Numbers/booleans are substituted as-is (inja stringifies them), never run
// through escape_latex — there is nothing to escape in a bare number, and
// e.g. a JSON `true` must not become the (mangled) string "true" escaped.
TEST_F(TemplateRegistryTest, RenderTexLeavesNumbersAndBooleansUnescaped) {
    write_version("greet", "v1", "{{ count }}/{{ ok }}");
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("greet");
    ASSERT_TRUE(info.has_value());

    auto out = Docgen::render_tex(*info, json{{"count", 5}, {"ok", true}});
    EXPECT_EQ(out, "5/true");
}

// render_tex must not mutate the caller's input.
TEST_F(TemplateRegistryTest, RenderTexDoesNotMutateInput) {
    write_version("greet", "v1", "{{ line }}");
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("greet");
    ASSERT_TRUE(info.has_value());

    json input = {{"line", "a & b"}};
    const json snapshot = input;
    Docgen::render_tex(*info, input);
    EXPECT_EQ(input, snapshot);
}

}  // namespace
