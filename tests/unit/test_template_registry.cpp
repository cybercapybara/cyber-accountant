/**
 * @file test_template_registry.cpp
 * @brief Unit tests for Docgen::TemplateRegistry — discovery + schema
 *        validation against a fixture tree under a temp directory — and for
 *        Docgen::render_tex / Docgen::write_typst_inputs (Renderer.hpp),
 *        which operate on the same TemplateInfo this registry resolves. No
 *        Config, no network, no sidecars, no XeLaTeX, no Typst.
 */

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "docgen/Renderer.hpp"
#include "docgen/TemplateRegistry.hpp"
#include "repo_templates.hpp"

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

    /// Write templates/<slug>/<version_dir>/template.typ + schema.json — the
    /// Typst sibling of write_version(), for the engine-discovery tests.
    void write_typst_version(const std::string& slug,
                             const std::string& version_dir,
                             const std::string& typ = "#let d = json(\"input.json\")",
                             const json& schema = json{{"type", "object"}}) {
        const fs::path dir = root_ / slug / version_dir;
        fs::create_directories(dir);
        std::ofstream(dir / "template.typ") << typ;
        std::ofstream(dir / "schema.json") << schema.dump();
    }
};

TEST_F(TemplateRegistryTest, LatestReturnsNulloptForUnknownSlug) {
    Docgen::TemplateRegistry registry(root_);
    EXPECT_FALSE(registry.latest("no_such_slug").has_value());
}

// Security: slug becomes a raw filesystem path component (root_ / slug /
// ...). A traversal/invalid slug must be rejected BEFORE it ever reaches
// std::filesystem, not merely fail to find a template by accident. Also
// covers a template that legitimately exists at the traversal target
// ("invoice") — the allowlist must reject the shape of the slug itself,
// not just "does this resolve to nothing".
TEST_F(TemplateRegistryTest, LatestRejectsPathTraversalSlug) {
    write_version("invoice", "v1");
    Docgen::TemplateRegistry registry(root_);

    EXPECT_FALSE(registry.latest("../evil").has_value());
    EXPECT_FALSE(registry.latest("../../etc/passwd").has_value());
    EXPECT_FALSE(registry.latest("..").has_value());
    // Would resolve to root_/invoice/v1 via a nested path component.
    EXPECT_FALSE(registry.latest("invoice/v1").has_value());
}

TEST_F(TemplateRegistryTest, LatestRejectsSlugWithSlash) {
    Docgen::TemplateRegistry registry(root_);
    EXPECT_FALSE(registry.latest("a/b").has_value());
}

TEST_F(TemplateRegistryTest, LatestRejectsUppercaseSlug) {
    write_version("invoice", "v1");
    Docgen::TemplateRegistry registry(root_);
    // Uppercase isn't a traversal risk on a case-sensitive filesystem, but
    // it's outside the allowlist (^[a-z][a-z0-9_-]*$) by design — one
    // canonical slug shape, no case-variant duplicates.
    EXPECT_FALSE(registry.latest("A").has_value());
    EXPECT_FALSE(registry.latest("Invoice").has_value());
}

TEST_F(TemplateRegistryTest, LatestRejectsEmptySlug) {
    Docgen::TemplateRegistry registry(root_);
    EXPECT_FALSE(registry.latest("").has_value());
}

TEST_F(TemplateRegistryTest, LatestAcceptsUnderscoreAndHyphen) {
    write_version("tax_invoice-v2", "v1");
    Docgen::TemplateRegistry registry(root_);
    EXPECT_TRUE(registry.latest("tax_invoice-v2").has_value());
}

// validate(slug, input) resolves the template through latest() internally —
// a traversal slug must fail the same way (a validation error, not a crash
// or a filesystem escape), not bypass the allowlist via a second entry point.
TEST_F(TemplateRegistryTest, ValidateRejectsPathTraversalSlug) {
    Docgen::TemplateRegistry registry(root_);
    auto err = registry.validate("../evil", json::object());
    ASSERT_TRUE(err.has_value());
}

TEST_F(TemplateRegistryTest, LatestFindsSingleVersion) {
    write_version("invoice", "v1");
    Docgen::TemplateRegistry registry(root_);

    auto info = registry.latest("invoice");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->slug, "invoice");
    EXPECT_EQ(info->version, 1);
    EXPECT_EQ(info->version_str, "v1");
    EXPECT_EQ(info->source_path, root_ / "invoice" / "v1" / "template.tex");
}

// ── per-template engine discovery ────────────────────────────────────────
// The engine is a property of the template directory, decided once here, so
// the migration can convert one template at a time with both engines live.

TEST_F(TemplateRegistryTest, LatexTemplateReportsXelatexEngine) {
    write_version("invoice", "v1");
    Docgen::TemplateRegistry registry(root_);

    auto info = registry.latest("invoice");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->engine, Docgen::Engine::kLatex);
    EXPECT_EQ(info->source_path, root_ / "invoice" / "v1" / "template.tex");
    EXPECT_STREQ(Docgen::engine_name(info->engine), "xelatex");
}

TEST_F(TemplateRegistryTest, TypstTemplateReportsTypstEngine) {
    write_typst_version("payslip", "v1");
    Docgen::TemplateRegistry registry(root_);

    auto info = registry.latest("payslip");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->engine, Docgen::Engine::kTypst);
    EXPECT_EQ(info->source_path, root_ / "payslip" / "v1" / "template.typ");
    EXPECT_STREQ(Docgen::engine_name(info->engine), "typst");
}

// Transient state while a template is being converted: the .typ is
// authoritative. Deliberate, not accidental — the conversion commit adds the
// .typ and deletes the .tex, and if a rebase ever lands only half of that,
// the new source must win rather than the retired one.
TEST_F(TemplateRegistryTest, TypstWinsWhenBothSourcesExist) {
    write_version("payslip", "v1");
    std::ofstream(root_ / "payslip" / "v1" / "template.typ") << "typst body";
    Docgen::TemplateRegistry registry(root_);

    auto info = registry.latest("payslip");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->engine, Docgen::Engine::kTypst);
    EXPECT_EQ(info->source_path, root_ / "payslip" / "v1" / "template.typ");
}

// list() resolves through the same load(), so discovery over the whole tree
// reports the engine per directory too — not just the single-slug path.
TEST_F(TemplateRegistryTest, ListReportsTheEnginePerTemplate) {
    write_version("invoice", "v1");
    write_typst_version("payslip", "v1");
    Docgen::TemplateRegistry registry(root_);

    const auto all = registry.list();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].slug, "invoice");
    EXPECT_EQ(all[0].engine, Docgen::Engine::kLatex);
    EXPECT_EQ(all[1].slug, "payslip");
    EXPECT_EQ(all[1].engine, Docgen::Engine::kTypst);
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

// Neither source present is a malformed template, not "no such engine" and
// not a silent not-found: the directory declares a version and a schema, so
// something is missing from the commit and it must fail loudly.
TEST_F(TemplateRegistryTest, MissingBothSourcesThrows) {
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

// Regression: every shipped template defines a one-arg LaTeX macro like
// `\newcommand{\field}[1]{\textbf{#1}}` — inja's DEFAULT comment markers are
// "{#"/"#}", so the literal `{#1}` in that macro body used to be parsed as
// an unterminated comment ("expected comment close, got '<eof>'") and the
// render failed outright. render_tex must handle this without throwing, and
// the macro parameter marker must survive into the output unchanged (it's
// plain LaTeX text, not inja syntax under the remapped markers).
TEST_F(TemplateRegistryTest, RenderTexCoexistsWithLatexMacroParameters) {
    write_version("greet", "v1", "\\newcommand{\\field}[1]{\\textbf{#1}}\n{{ name }}");
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("greet");
    ASSERT_TRUE(info.has_value());

    auto out = Docgen::render_tex(*info, json{{"name", "Ada"}});
    EXPECT_EQ(out, "\\newcommand{\\field}[1]{\\textbf{#1}}\nAda");
    EXPECT_NE(out.find("{#1}"), std::string::npos);
}

// render_tex's remapped comment markers — "((#" / "#))" — are still cut from
// the output, same as inja's default "{# #}" would be, just spelled
// differently so it can't collide with LaTeX macro parameters.
TEST_F(TemplateRegistryTest, RenderTexCommentMarkersAreStripped) {
    write_version("greet", "v1", "A((#hidden comment#))B");
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("greet");
    ASSERT_TRUE(info.has_value());

    auto out = Docgen::render_tex(*info, json::object());
    EXPECT_EQ(out, "AB");
    EXPECT_EQ(out.find("hidden"), std::string::npos);
}

// ── normalize_input (schema-driven default-fill) ─────────────────────────
// A caller (correctly) omitting an optional field must not crash inja with
// "variable ... not found" — normalize_input fills every property the
// schema declares but the input lacks: "" for string, [] for array, a
// recursively-filled object for object (resolving one-level "$ref" first,
// the only $ref shape any shipped schema uses).

TEST(NormalizeInputTest, FillsMissingStringArrayAndRefObjectFromSyntheticSchema) {
    // "address" is a $ref to a nested object with its OWN two string
    // properties — exercises $ref resolution AND recursive object fill in
    // one schema, without depending on any real template.
    const json schema = json::parse(R"({
        "type": "object",
        "required": ["name"],
        "properties": {
            "name": {"type": "string"},
            "nickname": {"type": "string"},
            "tags": {"type": "array"},
            "address": {"$ref": "#/definitions/addr"}
        },
        "definitions": {
            "addr": {
                "type": "object",
                "properties": {
                    "city": {"type": "string"},
                    "zip": {"type": "string"}
                }
            }
        }
    })");

    const json input = {{"name", "Ada"}};
    auto out = Docgen::TemplateRegistry::normalize_input(schema, input);

    EXPECT_EQ(out["name"], "Ada");  // present value untouched
    EXPECT_EQ(out["nickname"], "");
    EXPECT_EQ(out["tags"], json::array());
    ASSERT_TRUE(out["address"].is_object());
    EXPECT_EQ(out["address"]["city"], "");
    EXPECT_EQ(out["address"]["zip"], "");
}

// A partially-provided nested object gets its OWN missing fields filled
// too (recursion into an existing value, not just a wholly-absent one).
TEST(NormalizeInputTest, FillsMissingFieldsOfAPartiallyProvidedNestedObject) {
    const json schema = json::parse(R"({
        "type": "object",
        "properties": {
            "address": {"$ref": "#/definitions/addr"}
        },
        "definitions": {
            "addr": {
                "type": "object",
                "properties": {
                    "city": {"type": "string"},
                    "zip": {"type": "string"}
                }
            }
        }
    })");

    const json input = {{"address", {{"city", "Almaty"}}}};
    auto out = Docgen::TemplateRegistry::normalize_input(schema, input);

    EXPECT_EQ(out["address"]["city"], "Almaty");  // present value untouched
    EXPECT_EQ(out["address"]["zip"], "");         // missing sibling filled
}

// Same fill, against the REAL invoice/v1 schema (the one every shipped
// docgen schema shapes its $ref-based "party" definition after).
TEST(NormalizeInputTest, FillsMissingOptionalFieldsOfRealInvoiceSchema) {
    REQUIRE_REPO_TEMPLATE("invoice");
    std::ifstream schema_file(TestTemplates::version_dir("invoice") / "schema.json");
    ASSERT_TRUE(static_cast<bool>(schema_file));
    json schema;
    schema_file >> schema;

    const json input = {
        {"number", "1"},
        {"date", "14.08.2026"},
        {"seller", {{"name", "S"}, {"identifier", "1"}}},  // no address/iik/bank/bik/kbe
        {"buyer", {{"name", "B"}, {"identifier", "2"}}},
        {"items", json::array({json{{"name", "x"}, {"qty", "1"}, {"unit", "шт"}, {"price", "1"}, {"amount", "1"}}})},
        {"total", "1"},
        {"total_words", "one"},
        // contract, vat_rate, vat_amount: omitted top-level optional strings.
    };

    auto out = Docgen::TemplateRegistry::normalize_input(schema, input);

    EXPECT_EQ(out["seller"]["address"], "");
    EXPECT_EQ(out["seller"]["iik"], "");
    EXPECT_EQ(out["seller"]["bank"], "");
    EXPECT_EQ(out["seller"]["bik"], "");
    EXPECT_EQ(out["seller"]["kbe"], "");
    EXPECT_EQ(out["contract"], "");
    EXPECT_EQ(out["vat_rate"], "");
    EXPECT_EQ(out["vat_amount"], "");
    // Provided values must survive untouched.
    EXPECT_EQ(out["seller"]["name"], "S");
    EXPECT_EQ(out["number"], "1");
}

// End-to-end against the REAL shipped invoice template: an input missing
// every optional field must reach the engine WITHOUT throwing, and must not
// make the template print any of its conditional blocks' labels. Both engines
// spell the condition the same way — inja `{% if seller.iik != "" %}` and
// Typst `#if d.seller.iik != ""` — because both are matched to
// normalize_input's fill (see TemplateRegistry::normalize_input's doc comment
// on why "" is truthy in inja), so the property is one property, checked
// through whichever engine the directory currently selects.
//
// It was `RenderTexOfRealInvoiceTemplateOmitsEmptyConditionalBlocks`, guarded
// by a probe for `invoice/v1/template.tex` — so the invoice conversion
// (3c77b1e) both deleted the file it probed and invalidated what it asserted,
// and the test went quiet instead of red. Under Typst there is no rendered
// source to search: the branch is taken inside the engine, and what is
// observable here is the input the engine is handed. The label-absence half
// then lives in the PDF, and in the render gate that reads it
// (scripts/check-render.py: every label declared in a fixture's
// `.expected.txt` must appear, and its syntax layer refuses a `[`/`else`
// typeset as body text by a broken branch).
TEST(NormalizeInputTest, RealInvoiceTemplateGetsEmptyOptionalsForItsConditionalBlocks) {
    REQUIRE_REPO_TEMPLATE("invoice");

    Docgen::TemplateRegistry registry;  // default root: "templates/latex"
    auto info = registry.latest("invoice");
    ASSERT_TRUE(info.has_value());

    const json input = {
        {"number", "1"},
        {"date", "14.08.2026"},
        {"seller", {{"name", "Продавец"}, {"identifier", "1"}}},
        {"buyer", {{"name", "Покупатель"}, {"identifier", "2"}}},
        {"items", json::array({json{{"name", "x"}, {"qty", "1"}, {"unit", "шт"}, {"price", "1"}, {"amount", "1"}}})},
        {"total", "1"},
        {"total_words", "one"},
    };
    const json normalized = Docgen::TemplateRegistry::normalize_input(info->schema, input);

    if (info->engine == Docgen::Engine::kLatex) {
        std::string out;
        EXPECT_NO_THROW(out = Docgen::render_tex(*info, normalized));
        EXPECT_EQ(out.find("ИИК"), std::string::npos);
        EXPECT_EQ(out.find("Основание"), std::string::npos);
        EXPECT_EQ(out.find("НДС"), std::string::npos);
        return;
    }

    // Typst: the fill is what the template's `!= ""` guards compare against,
    // and it has to survive the trip to the engine as the EMPTY STRING — a
    // missing key is a hard engine error, and any other value prints a label
    // with nothing after it.
    const fs::path staging = fs::temp_directory_path() / "docgen_invoice_optionals";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging);
    ASSERT_NO_THROW(Docgen::write_typst_inputs(*info, normalized, staging));

    json written;
    std::ifstream staged_input(staging / "input.json");
    ASSERT_TRUE(static_cast<bool>(staged_input));
    ASSERT_NO_THROW(staged_input >> written);

    ASSERT_TRUE(written.contains("seller"));
    const json& seller = written.at("seller");
    const std::vector<std::string> seller_optionals = {"iik", "bank", "bik", "kbe"};
    for (const auto& key : seller_optionals) {
        ASSERT_TRUE(seller.contains(key)) << "seller." << key << " is absent — Typst hard-errors on a missing key";
        EXPECT_EQ(seller.at(key), "") << "seller." << key;
    }
    const std::vector<std::string> top_level_optionals = {"contract", "vat_rate", "vat_amount"};
    for (const auto& key : top_level_optionals) {
        ASSERT_TRUE(written.contains(key)) << key << " is absent — Typst hard-errors on a missing key";
        EXPECT_EQ(written.at(key), "") << key;
    }
    fs::remove_all(staging, ec);
}

// ── escaping vs. templating: WHEN a string leaf is escaped ───────────────
// Regression bundle for the defect that shipped in v0.4.1: render_tex used
// to escape EVERY string leaf before inja saw the tree, so a template's own
// control flow was evaluated against escaped data. `{% if balance_kind ==
// "to_pay" %}` tested `to\_pay`, never matched, and every ФНО 300.00 went
// out without its closing line and amount in words; hr_order's
// `business_trip` and `salary_change` orders went out with no body at all.
//
// The rule now: a string leaf is escaped UNLESS its schema node pins it to
// an `enum` and the value IS one of those literals. Everything below pins
// both halves of that rule — the branch works, and nothing else stopped
// being escaped.

/// The canonical hostile value: a counterparty name carrying every kind of
/// LaTeX weapon at once. It must reach the PDF as literal text, always.
const char* const kInjection = "ТОО \"Алма & Ко\" \\textbf{x} $x^2$ #read(\"/etc/passwd\")";
/// The same string after escape_latex — what the `.tex` must contain.
const char* const kInjectionEscaped =
    "ТОО \"Алма \\& Ко\" \\textbackslash{}textbf\\{x\\} \\$x\\textasciicircum{}2\\$ \\#read(\"/etc/passwd\")";

/// The Typst-engine sibling of kInjection: the same hostile counterparty
/// name, carrying the constructs TYPST would act on — a code sigil, a call
/// that reads the filesystem, a binding, strong/emphasis markers, math mode,
/// a label and a raw block. There is no escaped counterpart because there is
/// no escaping on that path: the value travels to the engine inside
/// `input.json` and must arrive byte for byte, unchanged and unspliced.
const char* const kTypstInjection =
    "ТОО \"Алма & Ко\" #panic(\"pwned\") #read(\"/etc/passwd\") #let x = 1 *bold* _it_ $x^2$ @lbl `code`";

/// Does @p needle occur in @p haystack NOT preceded by a backslash? The
/// escaped forms of the payload legitimately contain their own raw needle as
/// a substring (`\#read(` contains `#read(`), so a plain find() would report
/// a hit on correctly escaped output. What matters is an occurrence LaTeX
/// would act on: one that is not preceded by the escaping backslash.
/// @note Every needle passed below therefore STARTS with the special
///       character being checked, and is long enough not to collide with the
///       template's own LaTeX (`& Ко"` rather than `& Ко`, which would also
///       match the table header `Показатель & Код строки`).
bool contains_unescaped(const std::string& haystack, const std::string& needle) {
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) {
        if (pos == 0 || haystack[pos - 1] != '\\')
            return true;
    }
    return false;
}

/// Replace EVERY string leaf of @p node with @p value, in place.
void overwrite_strings(json& node, const std::string& value) {
    if (node.is_string()) {
        node = value;
    } else if (node.is_object()) {
        for (auto& item : node.items())
            overwrite_strings(item.value(), value);
    } else if (node.is_array()) {
        for (auto& element : node)
            overwrite_strings(element, value);
    }
}

/// Every string leaf of @p node as (dotted path, value) — the paths make a
/// failure name the field, not just the template.
void collect_string_leaves(const json& node,
                           const std::string& prefix,
                           std::vector<std::pair<std::string, std::string>>& out) {
    if (node.is_string()) {
        out.emplace_back(prefix, node.get<std::string>());
    } else if (node.is_object()) {
        for (const auto& item : node.items())
            collect_string_leaves(item.value(), prefix.empty() ? item.key() : prefix + "." + item.key(), out);
    } else if (node.is_array()) {
        for (std::size_t i = 0; i < node.size(); ++i)
            collect_string_leaves(node[i], prefix + "." + std::to_string(i), out);
    }
}

/// The bytes of @p path (empty if it cannot be read — the callers assert on
/// the content, so an unreadable file fails there rather than throwing here).
std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

/// Every dotted path a schema pins to an `enum` — the control values.
/// Mirrors the schema walk in Renderer.hpp/TemplateRegistry.hpp: resolve
/// `$ref`, recurse through `properties`.
void collect_enum_paths(const json& root,
                        const json& node_in,
                        const std::string& prefix,
                        std::vector<std::string>& out) {
    const json& node = Docgen::TemplateRegistry::resolve_ref(root, node_in);
    if (!node.is_object())
        return;
    if (node.contains("enum") && !prefix.empty()) {
        out.push_back(prefix);
        return;
    }
    if (!node.contains("properties") || !node.at("properties").is_object())
        return;
    for (const auto& prop : node.at("properties").items())
        collect_enum_paths(root, prop.value(), prefix.empty() ? prop.key() : prefix + "." + prop.key(), out);
}

/// Every field path a template PRINTS — inja `{{ a.b }}` expressions and
/// Typst `#d.a.b` references — as opposed to the things it branches on
/// inside `{% %}` / an `if`. Both forms are collected unconditionally: a
/// file only ever contains one of them, and a helper that silently found
/// nothing is how a shipped test rots into `EXPECT_TRUE(true)`.
std::vector<std::string> printed_expressions(const std::string& source) {
    std::vector<std::string> out;
    for (std::size_t pos = source.find("{{"); pos != std::string::npos; pos = source.find("{{", pos + 2)) {
        const std::size_t end = source.find("}}", pos + 2);
        if (end == std::string::npos)
            break;
        std::string expr = source.substr(pos + 2, end - pos - 2);
        const std::size_t first = expr.find_first_not_of(" \t\r\n");
        const std::size_t last = expr.find_last_not_of(" \t\r\n");
        if (first != std::string::npos)
            out.push_back(expr.substr(first, last - first + 1));
    }
    static const std::regex kTypst(R"(#d\.([A-Za-z_][A-Za-z0-9_.]*))");
    for (std::sregex_iterator it(source.begin(), source.end(), kTypst), end; it != end; ++it)
        out.push_back((*it)[1].str());
    return out;
}

// THE defect, in miniature: an enum-pinned control value whose literal
// contains a LaTeX-special character. Escaping it first made the branch
// unreachable; it must now reach the comparison intact.
TEST_F(TemplateRegistryTest, RenderTexLeavesSchemaEnumControlValuesUnescaped) {
    const json schema = json::parse(R"({
        "type": "object",
        "properties": {"balance_kind": {"type": "string", "enum": ["to_pay", "to_refund"]}}
    })");
    write_version("decl",
                  "v1",
                  R"({% if balance_kind == "to_pay" %}PAY{% endif %})"
                  R"({% if balance_kind == "to_refund" %}REFUND{% endif %})",
                  schema);
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("decl");
    ASSERT_TRUE(info.has_value());

    EXPECT_EQ(Docgen::render_tex(*info, json{{"balance_kind", "to_pay"}}), "PAY");
    EXPECT_EQ(Docgen::render_tex(*info, json{{"balance_kind", "to_refund"}}), "REFUND");
}

// The exception is a MEMBERSHIP test on the value, not a property of the
// field: a value that is not one of the schema's literals is escaped exactly
// as before. So a caller who somehow gets a hostile string into an
// enum-pinned field (past the controller allowlist and past
// TemplateRegistry::validate) still cannot place a single raw LaTeX byte —
// and still cannot take the branch.
TEST_F(TemplateRegistryTest, RenderTexEscapesAnEnumFieldValueThatIsNotOneOfTheLiterals) {
    const json schema = json::parse(R"({
        "type": "object",
        "properties": {"kind": {"type": "string", "enum": ["hire", "to_pay"]}}
    })");
    write_version("decl", "v1", R"([{% if kind == "to_pay" %}PAY{% endif %}][{{ kind }}])", schema);
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("decl");
    ASSERT_TRUE(info.has_value());

    // Shaped to close the comparison and open a command if it were ever
    // interpolated raw.
    const std::string hostile = R"(to_pay" %}\input{/etc/passwd}((#)";
    const std::string out = Docgen::render_tex(*info, json{{"kind", hostile}});

    EXPECT_EQ(out.find("PAY"), std::string::npos) << "a non-literal value must not take the branch";
    EXPECT_EQ(out.find("\\input"), std::string::npos) << "raw \\input reached the .tex";
    EXPECT_NE(out.find("\\textbackslash{}input\\{/etc/passwd\\}"), std::string::npos);
    EXPECT_EQ(out, "[][to\\_pay\" \\%\\}\\textbackslash{}input\\{/etc/passwd\\}((\\#]");
}

// The schema walk must not lose escaping anywhere the old blanket walk
// covered: nested objects, arrays of objects, and leaves the schema does not
// describe at all (an unknown key, or an array whose schema declares no
// `items`). Absent schema information means "escape" — the safe direction.
TEST_F(TemplateRegistryTest, RenderTexEscapesNestedArrayAndSchemaLessLeaves) {
    const json schema = json::parse(R"({
        "type": "object",
        "properties": {
            "seller": {"$ref": "#/definitions/party"},
            "items": {"type": "array", "items": {"type": "object",
                      "properties": {"name": {"type": "string"}}}},
            "loose": {"type": "array"}
        },
        "definitions": {
            "party": {"type": "object", "properties": {"name": {"type": "string"}}}
        }
    })");
    write_version("inv",
                  "v1",
                  "[{{ seller.name }}][{{ items.0.name }}][{{ items.0.note }}][{{ loose.0 }}][{{ undeclared }}]",
                  schema);
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("inv");
    ASSERT_TRUE(info.has_value());

    const json input = {{"seller", {{"name", "A & B"}}},
                        // `note` is not declared by items' schema; `loose`
                        // declares no `items`; `undeclared` is not a
                        // property at all. All three must still be escaped.
                        {"items", json::array({json{{"name", "C & D"}, {"note", "E & F"}}})},
                        {"loose", json::array({"G & H"})},
                        {"undeclared", "I & J"}};

    EXPECT_EQ(Docgen::render_tex(*info, input), "[A \\& B][C \\& D][E \\& F][G \\& H][I \\& J]");
}

// A non-string enum (a schema pinning an integer) must not make the leaf
// skip escaping by accident, and must not disturb the numbers-pass-through
// rule either.
TEST_F(TemplateRegistryTest, RenderTexEscapesAStringLeafAgainstANonStringEnum) {
    const json schema = json::parse(R"({
        "type": "object",
        "properties": {"n": {"enum": [1, 2]}, "s": {"enum": [1, 2]}}
    })");
    write_version("mix", "v1", "[{{ n }}][{{ s }}]", schema);
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("mix");
    ASSERT_TRUE(info.has_value());

    EXPECT_EQ(Docgen::render_tex(*info, json{{"n", 2}, {"s", "a_b"}}), "[2][a\\_b]");
}

// ── write_typst_inputs (Renderer.hpp) ────────────────────────────────────
// The Typst staging step: copy the template to main.typ, write the
// normalized input to input.json beside it. No templating layer, no
// escaping — Typst reads the JSON itself, so a value is content and never
// source. These need no engine binary; the compile itself is exercised on
// the worker image by the `template-render` CI job.

TEST_F(TemplateRegistryTest, WriteTypstInputsCopiesTemplateAndWritesNormalizedJson) {
    const json schema = json::parse(R"({
        "type": "object",
        "properties": {
            "employer": {
                "type": "object",
                "properties": {"name": {"type": "string"}, "address": {"type": "string"}}
            }
        }
    })");
    write_typst_version("payslip", "v1", "#let d = json(\"input.json\")\n#d.employer.name", schema);
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("payslip");
    ASSERT_TRUE(info.has_value());

    const json input = {{"employer", {{"name", "ТОО \"Ромашка\""}}}};
    const json normalized = Docgen::TemplateRegistry::normalize_input(info->schema, input);

    const auto out = root_ / "out";
    fs::create_directories(out);
    Docgen::write_typst_inputs(*info, normalized, out);

    std::ifstream typ(out / "main.typ", std::ios::binary);
    ASSERT_TRUE(typ.good());
    const std::string typ_body((std::istreambuf_iterator<char>(typ)), std::istreambuf_iterator<char>());
    EXPECT_EQ(typ_body, "#let d = json(\"input.json\")\n#d.employer.name");

    std::ifstream data(out / "input.json", std::ios::binary);
    ASSERT_TRUE(data.good());
    json written;
    data >> written;
    EXPECT_EQ(written.at("employer").at("name"), "ТОО \"Ромашка\"");
    // normalize_input filled the declared-but-absent optional; Typst hard-errors
    // on a missing key ("dictionary does not contain key"), where inja merely
    // printed nothing — so this is load-bearing, not cosmetic.
    EXPECT_EQ(written.at("employer").at("address"), "");
}

// The whole security argument in one test: a value that looks like Typst code
// reaches the engine as DATA, byte for byte, with no escaping applied.
TEST_F(TemplateRegistryTest, WriteTypstInputsNeverTransformsValues) {
    const json schema = json::parse(R"({"type": "object", "properties": {"note": {"type": "string"}}})");
    write_typst_version("payslip", "v1", "#let d = json(\"input.json\")", schema);
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("payslip");
    ASSERT_TRUE(info.has_value());

    const std::string payload = R"(#panic("pwned") *bold* $x^2$ #read("/etc/passwd") @l _it_)";
    const auto out = root_ / "out2";
    fs::create_directories(out);
    Docgen::write_typst_inputs(*info, json{{"note", payload}}, out);

    std::ifstream data(out / "input.json", std::ios::binary);
    ASSERT_TRUE(data.good());
    json written;
    data >> written;
    EXPECT_EQ(written.at("note").get<std::string>(), payload);
}

// Overwrites rather than appends: a ScopedTempDir is fresh per render, but a
// stale main.typ/input.json pair must never survive into a second staging in
// the same directory (the `--render-template` CLI mode reuses one out_dir).
TEST_F(TemplateRegistryTest, WriteTypstInputsOverwritesAPreviousStaging) {
    const json schema = json::parse(R"({"type": "object", "properties": {"note": {"type": "string"}}})");
    write_typst_version("payslip", "v1", "#let d = json(\"input.json\")", schema);
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("payslip");
    ASSERT_TRUE(info.has_value());

    const auto out = root_ / "out3";
    fs::create_directories(out);
    std::ofstream(out / "main.typ") << "stale template that is much longer than the real one";
    std::ofstream(out / "input.json") << R"({"note":"stale","extra":"gone"})";

    Docgen::write_typst_inputs(*info, json{{"note", "fresh"}}, out);

    std::ifstream typ(out / "main.typ", std::ios::binary);
    const std::string typ_body((std::istreambuf_iterator<char>(typ)), std::istreambuf_iterator<char>());
    EXPECT_EQ(typ_body, "#let d = json(\"input.json\")");

    std::ifstream data(out / "input.json", std::ios::binary);
    json written;
    data >> written;
    EXPECT_EQ(written.at("note").get<std::string>(), "fresh");
    EXPECT_FALSE(written.contains("extra"));
}

// A staging directory that does not exist is a hard, named error — not a
// silently skipped copy that would leave compile_typst to fail with a
// confusing "main.typ not found" from the engine instead.
TEST_F(TemplateRegistryTest, WriteTypstInputsThrowsWhenTheOutputDirIsMissing) {
    write_typst_version("payslip", "v1");
    Docgen::TemplateRegistry registry(root_);
    auto info = registry.latest("payslip");
    ASSERT_TRUE(info.has_value());

    EXPECT_THROW(Docgen::write_typst_inputs(*info, json::object(), root_ / "nope" / "missing"), std::runtime_error);
}

// ── the shipped templates ────────────────────────────────────────────────

// The alarm for every skip below, and for the ones in tests/integration:
// each of them steps aside when the repo's template tree is not reachable
// from the working directory, and that tolerance is exactly how three
// regression pins ran nothing for months while reporting success (see
// tests/repo_templates.hpp for the post-mortem). This test does NOT step
// aside. Every CI run of this binary has the repo root as its working
// directory — /app in docker/Dockerfile's test-runner stage, and the `cd
// /app` in the sanitizer and TSan jobs — so an unreachable template tree
// there is a broken assumption about the environment, not an environment to
// accommodate. One loud red test that names the cause beats a screenful of
// quiet green skips.
//
// Running the binary from somewhere else (`ctest` from the build directory,
// say) fails HERE and only here; the message says what to do about it.
TEST(ShippedTemplatesTest, TheRepoTemplateTreeIsReachableFromTheWorkingDirectory) {
    ASSERT_TRUE(TestTemplates::tree_reachable())
        << "'" << TestTemplates::kRoot << "' is not reachable from the working directory "
        << fs::current_path().string()
        << ". Run the test binary from the repo root (CI runs it from /app). Every template-backed test in this "
           "binary just skipped, and skips are not failures — this one is.";

    // ... and it is a template tree, not merely a directory with that name.
    // A probe that is happy with the directory alone would survive a checkout
    // whose templates never landed.
    std::size_t with_schema = 0;
    for (const auto& entry : fs::directory_iterator(TestTemplates::kRoot)) {
        if (!entry.is_directory())
            continue;
        const std::string slug = entry.path().filename().string();
        EXPECT_TRUE(TestTemplates::has_template(slug)) << TestTemplates::missing_template_reason(slug);
        if (TestTemplates::has_template(slug))
            ++with_schema;
    }
    EXPECT_GE(with_schema, 10U) << "the shipped catalogue is ten document types; only " << with_schema
                                << " resolved a v1/schema.json";
}

// The engine a shipped template reports must be the one its source file on
// disk implies, with that file actually present. Self-maintaining: it keeps
// holding as templates convert one at a time.
TEST(ShippedTemplatesTest, EveryShippedTemplateReportsTheEngineOfItsSourceFile) {
    REQUIRE_REPO_TEMPLATE_TREE();

    Docgen::TemplateRegistry registry;
    const auto templates = registry.list();
    ASSERT_FALSE(templates.empty());

    for (const auto& info : templates) {
        EXPECT_TRUE(fs::exists(info.source_path)) << info.slug << ": " << info.source_path.string() << " is missing";
        const std::string extension = info.source_path.extension().string();
        if (extension == ".typ") {
            EXPECT_EQ(info.engine, Docgen::Engine::kTypst) << info.slug;
            EXPECT_STREQ(Docgen::engine_name(info.engine), "typst") << info.slug;
        } else {
            EXPECT_EQ(extension, ".tex") << info.slug << ": unknown template source extension";
            EXPECT_EQ(info.engine, Docgen::Engine::kLatex) << info.slug;
            EXPECT_STREQ(Docgen::engine_name(info.engine), "xelatex") << info.slug;
        }
    }
}

// The shipped catalogue: ten document types, and every one of them claimed by
// an engine. Deliberately says NOTHING about which engine any given slug uses
// — that is what the Typst migration moves, one commit at a time, and it is
// asserted against the file on disk by the test above instead of against a
// list here that would go stale on every conversion. What this pins is the
// thing a conversion must never change: the set of documents the service can
// render. A conversion changes a template's source file, never its slug, so
// this list only moves when a document type is genuinely added or retired.
TEST(ShippedTemplatesTest, TheSameTenDocumentTypesShipUnderEitherEngine) {
    REQUIRE_REPO_TEMPLATE_TREE();

    Docgen::TemplateRegistry registry;
    const auto templates = registry.list();

    std::vector<std::string> slugs;
    for (const auto& info : templates)
        slugs.push_back(info.slug);

    // Every slug directory on disk must have produced a TemplateInfo. A
    // directory that lost its `vN` subdirectory (a botched rebase, a
    // conversion committed one level too high) is dropped by list() in
    // silence, and would otherwise show up only as a 404 in production.
    std::size_t slug_dirs = 0;
    for (const auto& entry : fs::directory_iterator("templates/latex")) {
        if (entry.is_directory())
            ++slug_dirs;
    }
    EXPECT_EQ(templates.size(), slug_dirs) << "a template directory on disk resolved to no template";

    const std::vector<std::string> kShippedSlugs = {"avr",
                                                    "fno_300",
                                                    "fno_910",
                                                    "hr_order",
                                                    "invoice",
                                                    "labor_contract",
                                                    "payslip",
                                                    "reconciliation",
                                                    "tax_invoice",
                                                    "waybill"};
    EXPECT_EQ(slugs, kShippedSlugs);
}

// The blast-radius test the fix owes: a hostile counterparty name,
// substituted into EVERY string leaf of EVERY shipped template's fixture,
// must reach the document as literal text — never as something the engine
// executes. (Every leaf includes the enum-pinned ones — which is the point:
// the payload is not one of the schema's literals, so it is treated like any
// other string and the control branches simply do not fire.)
//
// Each template is exercised through ITS OWN engine, chosen from the same
// TemplateInfo the render pipeline uses, so this keeps holding as templates
// convert one at a time — no list of slugs to update:
//
//   * XeLaTeX — `render_tex`, whose whole job is the escaping. The property
//     is checked on the `.tex` it produces: the payload present in its
//     escaped form, and none of its raw forms present at all.
//   * Typst — `write_typst_inputs`, which does NOT escape, because there is
//     nothing to escape: the payload travels beside the template in
//     `input.json` and the template reads it as data. The property that
//     replaces escaping is separation, and that is what is checked here —
//     `main.typ` is a byte-exact copy of the template (the payload never
//     becomes source) and every value arrives byte for byte (nothing on the
//     way to the engine transformed it into something else).
//
// The Typst half's remaining claim — that the engine then TYPESETS those
// bytes rather than running them — needs the real binary, which no C++ test
// bucket has (tests/unit takes no services; tests/integration stubs
// DOCGEN_LATEX_CMD and never invokes an engine; typst 0.15.1 exists only on
// the worker image). It runs in the `template-render` CI job instead, which
// compiles every `fixtures/*.json` with the real engine and then gates the
// PDF: layer 1 (content) requires every fixture scalar to appear in the
// extracted text — so a `#panic("x")` that Typst EXECUTED fails the compile
// and one it interpreted goes missing from the text — and layer 3 (syntax)
// requires every syntax-looking token in the PDF to be accounted for by the
// fixture or a declared label. Those fixtures are the shipped
// `special-chars.json`, and the test below keeps them hostile so that gate
// cannot quietly lose its teeth.
TEST(ShippedTemplatesTest, TheInjectionPayloadStaysDataInEveryTemplateThroughItsOwnEngine) {
    REQUIRE_REPO_TEMPLATE_TREE();

    Docgen::TemplateRegistry registry;  // default root: "templates/latex"
    const auto templates = registry.list();
    ASSERT_FALSE(templates.empty());

    std::size_t latex_checked = 0;
    std::size_t typst_checked = 0;
    for (const auto& info : templates) {
        const fs::path fixture_path = info.dir / "fixtures" / "basic.json";
        ASSERT_TRUE(fs::exists(fixture_path)) << info.slug << " has no fixtures/basic.json";
        json fixture;
        std::ifstream(fixture_path) >> fixture;

        if (info.engine == Docgen::Engine::kLatex) {
            overwrite_strings(fixture, kInjection);
            const json normalized = Docgen::TemplateRegistry::normalize_input(info.schema, fixture);

            std::string out;
            ASSERT_NO_THROW(out = Docgen::render_tex(info, normalized)) << info.slug;

            EXPECT_NE(out.find(kInjectionEscaped), std::string::npos)
                << info.slug << ": the payload is not present in its escaped form";
            // None of the raw forms may appear. `\textbf{#1}` occurs in these
            // templates as a macro DEFINITION, so the needles below are the
            // payload's own byte sequences, which no template source contains.
            EXPECT_FALSE(contains_unescaped(out, "\\textbf{x}")) << info.slug << ": raw \\textbf{x} reached the .tex";
            EXPECT_FALSE(contains_unescaped(out, "$x^2$")) << info.slug << ": raw math mode reached the .tex";
            EXPECT_FALSE(contains_unescaped(out, "#read(")) << info.slug << ": raw macro parameter reached the .tex";
            EXPECT_FALSE(contains_unescaped(out, "& Ко\"")) << info.slug << ": raw & reached the .tex";
            EXPECT_FALSE(contains_unescaped(out, "^2")) << info.slug << ": raw superscript reached the .tex";
            ++latex_checked;
            continue;
        }

        overwrite_strings(fixture, kTypstInjection);
        const json normalized = Docgen::TemplateRegistry::normalize_input(info.schema, fixture);

        const fs::path staging = fs::temp_directory_path() / ("docgen_injection_" + info.slug);
        std::error_code ec;
        fs::remove_all(staging, ec);
        fs::create_directories(staging);
        ASSERT_NO_THROW(Docgen::write_typst_inputs(info, normalized, staging)) << info.slug;

        // The template is copied, not templated: byte for byte the file on
        // disk, with none of the payload spliced into it. This is the whole
        // reason the Typst path needs no escaping.
        const std::string staged = read_file(staging / "main.typ");
        EXPECT_FALSE(staged.empty()) << info.slug << ": main.typ was not staged";
        EXPECT_EQ(staged, read_file(info.source_path)) << info.slug << ": main.typ is not a copy of the template";
        EXPECT_EQ(staged.find(kTypstInjection), std::string::npos)
            << info.slug << ": the payload was spliced into the Typst source";

        // ... and every value reaches the engine unchanged. The only string
        // leaves that may differ are the empty ones normalize_input added for
        // declared-but-absent optionals (Typst hard-errors on a missing key).
        json written;
        ASSERT_NO_THROW(written = json::parse(read_file(staging / "input.json"))) << info.slug;
        std::vector<std::pair<std::string, std::string>> leaves;
        collect_string_leaves(written, "", leaves);
        std::size_t carried = 0;
        for (const auto& leaf : leaves) {
            if (leaf.second.empty())
                continue;
            EXPECT_EQ(leaf.second, kTypstInjection)
                << info.slug << ": " << leaf.first << " was transformed on the way to the engine";
            if (leaf.second == kTypstInjection)
                ++carried;
        }
        EXPECT_GT(carried, 0u) << info.slug << ": the payload reached input.json nowhere";
        fs::remove_all(staging, ec);
        ++typst_checked;
    }

    // Every shipped template went through exactly one of the two branches —
    // the guard cannot silently stop covering a template because its engine
    // changed under it.
    EXPECT_EQ(latex_checked + typst_checked, templates.size());
}

// The other half of the injection guard, and the reason the CI job that runs
// it has anything hostile to render: every template must SHIP a fixture whose
// data carries the weapons of the engine that will compile it. Without this,
// converting a template and rewriting its `special-chars.json` into something
// bland would disarm `template-render`'s real-engine check silently — the job
// would stay green while proving nothing about injection.
//
// Checked statically here (no engine, no services) because the fixture is a
// file in the repo; the compile that turns it into evidence happens in
// `template-render` (scripts/render-templates.sh + scripts/check-render.py).
// Converting a template therefore means extending its special-chars fixture
// with the Typst constructs as well — see templates/latex/README.md.
TEST(ShippedTemplatesTest, EveryTemplateShipsAHostileSpecialCharsFixture) {
    REQUIRE_REPO_TEMPLATE_TREE();

    // Weapons of the LaTeX escaper (LatexEscape.hpp) — every one of them must
    // arrive in the PDF as a printed character.
    const std::vector<std::string> kAnyEngine = {"%", "&", "#", "$", "_", "{", "}", "\\", "^", "~"};
    // ... plus the constructs Typst itself would act on: code sigil calls, a
    // strong marker, a raw block, a label.
    const std::vector<std::string> kTypstOnly = {"*", "`", "@", "#panic(", "#read("};

    Docgen::TemplateRegistry registry;
    const auto templates = registry.list();
    ASSERT_FALSE(templates.empty());

    for (const auto& info : templates) {
        const fs::path fixture_path = info.dir / "fixtures" / "special-chars.json";
        ASSERT_TRUE(fs::exists(fixture_path))
            << info.slug << " ships no fixtures/special-chars.json, so the real engine never sees a hostile value";
        json fixture;
        std::ifstream(fixture_path) >> fixture;
        std::vector<std::pair<std::string, std::string>> leaves;
        collect_string_leaves(fixture, "", leaves);

        std::vector<std::string> required = kAnyEngine;
        if (info.engine == Docgen::Engine::kTypst)
            required.insert(required.end(), kTypstOnly.begin(), kTypstOnly.end());

        for (const auto& token : required) {
            bool carried = false;
            for (const auto& leaf : leaves) {
                if (leaf.second.find(token) != std::string::npos) {
                    carried = true;
                    break;
                }
            }
            EXPECT_TRUE(carried) << info.slug << " (" << Docgen::engine_name(info.engine)
                                 << "): no special-chars fixture value carries '" << token
                                 << "', so nothing proves the engine typesets it instead of acting on it";
        }
    }
}

// The one rule the escaping exception costs template authors: an
// `enum`-pinned field is a control value, so branch on it — never print it.
// Raw `to_pay` typeset in text mode is a LaTeX error, and a control
// identifier is not something a reader should ever see. Statically checked
// against every shipped template so it cannot be forgotten.
TEST(ShippedTemplatesTest, NeverPrintAnEnumPinnedField) {
    REQUIRE_REPO_TEMPLATE_TREE();

    Docgen::TemplateRegistry registry;
    const auto templates = registry.list();
    ASSERT_FALSE(templates.empty());

    std::size_t enum_fields = 0;
    for (const auto& info : templates) {
        std::vector<std::string> control_paths;
        collect_enum_paths(info.schema, info.schema, "", control_paths);
        enum_fields += control_paths.size();

        std::ifstream source(info.source_path, std::ios::binary);
        std::ostringstream buf;
        buf << source.rdbuf();
        const auto printed = printed_expressions(buf.str());

        for (const auto& path : control_paths) {
            EXPECT_EQ(std::find(printed.begin(), printed.end(), path), printed.end())
                << info.slug << "/" << info.version_str << " (" << Docgen::engine_name(info.engine) << ") prints '"
                << path
                << "', but its schema pins that field to an enum — enum fields are control "
                   "values, reach the template unescaped, and must only be compared";
        }
    }
    // Guard against the check passing because it found nothing to check.
    EXPECT_GT(enum_fields, 0u);
}

// Fno300PrintsItsClosingLineForBothBalanceKinds was deleted with the ФНО
// 300.00 LaTeX source: under Typst the branch is taken inside the engine, so
// there is no intermediate source to assert on. The property did NOT go
// unguarded — it moved to scripts/check-render.py, which renders
// fno_300/v1/fixtures/basic.json (balance_kind "to_pay") and
// special-chars.json ("to_refund") and requires each fixture's own
// .expected.txt closing label to be present in the PDF.
//
// Two of that gate's layers hold the two halves. CONTENT: the closing line
// and its balance_words must be found in the PDF's extracted text, per
// fixture, so a branch that prints nothing (the v0.4.1 symptom — every ФНО
// 300.00 filed without its closing line) fails. LEAKED TEMPLATE SYNTAX: the
// gate screens every extracted token that looks like template syntax by
// PROVENANCE — a `#` sigil, a content-block bracket, a bare `else` that no
// fixture value and no declared label accounts for — so a branch typeset as
// body text instead of taken fails too. That second one is the failure Typst
// makes possible and LaTeX did not, and the gate's own self-test
// (scripts/check-render-selftest.sh) carries it as a deliberate breakage: a
// `] else [` split across lines.

// The other production symptom: an hr_order of every kind must print a body.
// `business_trip` and `salary_change` — the two kinds whose literal carries
// an underscore — printed a header, signature lines and nothing in between.
TEST(ShippedTemplatesTest, HrOrderPrintsABodyForEveryKind) {
    REQUIRE_REPO_TEMPLATE_TREE();

    Docgen::TemplateRegistry registry;
    auto info = registry.latest("hr_order");
    ASSERT_TRUE(info.has_value());

    // kind -> a phrase only that kind's block prints.
    const std::vector<std::pair<std::string, std::string>> kinds = {{"hire", "на работу с"},
                                                                    {"dismiss", "с должности"},
                                                                    {"vacation", "отпуск с"},
                                                                    {"business_trip", "в командировку с"},
                                                                    {"salary_change", "Изменить должностной оклад"}};

    for (const auto& [kind, phrase] : kinds) {
        const json input = {
            {"kind", kind},
            {"number", "42"},
            {"issued_on", "14.08.2026"},
            {"employer", {{"name", "Cyber Capybara ТОО"}, {"bin", "104332181962"}}},
            {"employee",
             {{"full_name", "Серикбаева Айгерим Кайратовна"}, {"iin", "900112350487"}, {"position", "Бухгалтер"}}},
            {"effective_from", "18.08.2026"},
            {"director", "Ахметов Ерлан Серикович"}};
        ASSERT_FALSE(Docgen::TemplateRegistry::validate(*info, input).has_value()) << kind;
        const std::string out =
            Docgen::render_tex(*info, Docgen::TemplateRegistry::normalize_input(info->schema, input));
        EXPECT_NE(out.find(phrase), std::string::npos) << "hr_order kind '" << kind << "' rendered no body";
    }
}

}  // namespace
