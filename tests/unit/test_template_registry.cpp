/**
 * @file test_template_registry.cpp
 * @brief Unit tests for Docgen::TemplateRegistry — discovery + schema
 *        validation against a fixture tree under a temp directory — and for
 *        Docgen::write_typst_inputs (Renderer.hpp), which operates on the
 *        same TemplateInfo this registry resolves. No Config, no network, no
 *        sidecars, no Typst binary.
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

    /// Write templates/<slug>/<version_dir>/template.typ + schema.json — the
    /// two files a version directory must have for load() to resolve it.
    void write_version(const std::string& slug,
                       const std::string& version_dir,
                       const std::string& typ = "#let d = json(\"input.json\")",
                       const json& schema = json{{"type", "object"},
                                                 {"required", json::array({"name"})},
                                                 {"properties", {{"name", {{"type", "string"}}}}}}) {
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
    EXPECT_EQ(info->source_path, root_ / "invoice" / "v1" / "template.typ");
}

// ── per-template engine discovery ────────────────────────────────────────
// The engine is a property of the template directory, decided once in load()
// and nowhere else. One engine ships today; what these pin is that the
// decision still happens there, so adding a second one later is a change to
// load() rather than to every call site.

TEST_F(TemplateRegistryTest, TypstTemplateReportsTypstEngine) {
    write_version("payslip", "v1");
    Docgen::TemplateRegistry registry(root_);

    auto info = registry.latest("payslip");
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->engine, Docgen::Engine::kTypst);
    EXPECT_EQ(info->source_path, root_ / "payslip" / "v1" / "template.typ");
    EXPECT_STREQ(Docgen::engine_name(info->engine), "typst");
}

// list() resolves through the same load(), so discovery over the whole tree
// fills in the engine per directory too — not just the single-slug path.
TEST_F(TemplateRegistryTest, ListReportsTheEnginePerTemplate) {
    write_version("invoice", "v1");
    write_version("payslip", "v1");
    Docgen::TemplateRegistry registry(root_);

    const auto all = registry.list();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].slug, "invoice");
    EXPECT_EQ(all[0].engine, Docgen::Engine::kTypst);
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

// No source present is a malformed template, not a silent not-found: the
// directory declares a version and a schema, so something is missing from the
// commit and it must fail loudly.
TEST_F(TemplateRegistryTest, MissingTemplateTypThrows) {
    fs::create_directories(root_ / "broken" / "v1");
    std::ofstream(root_ / "broken" / "v1" / "schema.json") << json::object().dump();
    Docgen::TemplateRegistry registry(root_);

    EXPECT_THROW(registry.latest("broken"), std::runtime_error);
}

TEST_F(TemplateRegistryTest, MissingSchemaJsonThrows) {
    fs::create_directories(root_ / "broken" / "v1");
    std::ofstream(root_ / "broken" / "v1" / "template.typ") << "x";
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
// make the template print any of its conditional blocks' labels. The template
// spells the condition `#if d.seller.iik != ""` because that is what matches
// normalize_input's fill — the zero value is a real value, not a marker, so a
// truthiness test would keep the block.
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

    // The fill is what the template's `!= ""` guards compare against,
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

// ── helpers for the shipped-template sweeps below ───────────────────────

/// The canonical hostile value: a counterparty name carrying the constructs
/// TYPST would act on — a code sigil, a call that reads the filesystem, a
/// binding, strong/emphasis markers, math mode, a label and a raw block, plus
/// the LaTeX-special bytes the retired escaper used to rewrite. There is no
/// escaped counterpart because there is no escaping on this path: the value
/// travels to the engine inside `input.json` and must arrive byte for byte,
/// unchanged and unspliced.
const char* const kTypstInjection =
    "ТОО \"Алма & Ко\" #panic(\"pwned\") #read(\"/etc/passwd\") #let x = 1 *bold* _it_ $x^2$ @lbl `code`";

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

/// Every field path a template PRINTS — a Typst `#d.a.b` reference — as
/// opposed to the things it branches on inside an `if`. The inja `{{ a.b }}`
/// half went with the last `template.tex`; the caller guards against this
/// finding nothing (see NeverPrintAnEnumPinnedField's enum_fields tally),
/// because a helper that silently matches nothing is how a shipped test rots
/// into `EXPECT_TRUE(true)`.
std::vector<std::string> printed_expressions(const std::string& source) {
    std::vector<std::string> out;
    static const std::regex kTypst(R"(#d\.([A-Za-z_][A-Za-z0-9_.]*))");
    for (std::sregex_iterator it(source.begin(), source.end(), kTypst), end; it != end; ++it)
        out.push_back((*it)[1].str());
    return out;
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
    write_version("payslip", "v1", "#let d = json(\"input.json\")\n#d.employer.name", schema);
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
    write_version("payslip", "v1", "#let d = json(\"input.json\")", schema);
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
    write_version("payslip", "v1", "#let d = json(\"input.json\")", schema);
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
    write_version("payslip", "v1");
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
        EXPECT_EQ(info.source_path.extension().string(), ".typ") << info.slug << ": unknown template source extension";
        EXPECT_EQ(info.engine, Docgen::Engine::kTypst) << info.slug;
        EXPECT_STREQ(Docgen::engine_name(info.engine), "typst") << info.slug;
    }
}

// The shipped catalogue: ten document types, every one of them resolvable.
// Deliberately says NOTHING about engines — that is asserted against the file
// on disk by the test above. What this pins is the thing the migration must
// never have changed: the set of documents the service can render. A
// conversion changed a template's source file, never its slug, so this list
// only moves when a document type is genuinely added or retired.
TEST(ShippedTemplatesTest, TheSameTenDocumentTypesShip) {
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
// The property that REPLACED escaping is separation, and that is what is
// checked here: `write_typst_inputs` does not escape, because there is nothing
// to escape — the payload travels beside the template in `input.json` and the
// template reads it as data. So `main.typ` must be a byte-exact copy of the
// template (the payload never becomes source) and every value must arrive byte
// for byte (nothing on the way to the engine transformed it into something
// else). The engine is read off the same TemplateInfo the render pipeline uses
// rather than assumed, so this keeps holding if a second engine is ever added.
//
// The remaining claim — that the engine then TYPESETS those bytes rather than
// running them — needs the real binary, which no C++ test bucket has
// (tests/unit takes no services; tests/integration stubs DOCGEN_TYPST_CMD and
// never invokes an engine; typst 0.15.1 exists only on the worker image). It
// runs in the `template-render` CI job instead, which compiles every
// `fixtures/*.json` with the real engine and then gates the PDF: layer 1
// (content) requires every fixture scalar to appear in the extracted text — so
// a `#panic("x")` that Typst EXECUTED fails the compile and one it interpreted
// goes missing from the text — and layer 3 (syntax) requires every
// syntax-looking token in the PDF to be accounted for by the fixture or a
// declared label. Those fixtures are the shipped `special-chars.json`, and the
// test below keeps them hostile so that gate cannot quietly lose its teeth.
TEST(ShippedTemplatesTest, TheInjectionPayloadStaysDataInEveryTemplate) {
    REQUIRE_REPO_TEMPLATE_TREE();

    Docgen::TemplateRegistry registry;  // default root: "templates/latex"
    const auto templates = registry.list();
    ASSERT_FALSE(templates.empty());

    std::size_t checked = 0;
    for (const auto& info : templates) {
        const fs::path fixture_path = info.dir / "fixtures" / "basic.json";
        ASSERT_TRUE(fs::exists(fixture_path)) << info.slug << " has no fixtures/basic.json";
        json fixture;
        std::ifstream(fixture_path) >> fixture;

        ASSERT_EQ(info.engine, Docgen::Engine::kTypst)
            << info.slug << ": this sweep only knows how to check the Typst staging path";
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
        ++checked;
    }

    // Every shipped template was actually swept — the guard cannot silently
    // stop covering one.
    EXPECT_EQ(checked, templates.size());
}

// The other half of the injection guard, and the reason the CI job that runs
// it has anything hostile to render: every template must SHIP a fixture whose
// data carries the weapons of the engine that will compile it. Without this,
// rewriting a `special-chars.json` into something bland would disarm
// `template-render`'s real-engine check silently — the job would stay green
// while proving nothing about injection.
//
// Checked statically here (no engine, no services) because the fixture is a
// file in the repo; the compile that turns it into evidence happens in
// `template-render` (scripts/render-templates.sh + scripts/check-render.py).
// See templates/latex/README.md.
TEST(ShippedTemplatesTest, EveryTemplateShipsAHostileSpecialCharsFixture) {
    REQUIRE_REPO_TEMPLATE_TREE();

    // The bytes the retired LaTeX escaper used to rewrite. They stay on the
    // list after its deletion because the requirement was never "the escaper
    // handles them" but "the engine PRINTS them", and that is the thing the
    // rendered PDF has to show.
    const std::vector<std::string> kEscaperWeapons = {"%", "&", "#", "$", "_", "{", "}", "\\", "^", "~"};
    // ... plus the constructs Typst itself would act on: code sigil calls, a
    // strong marker, a raw block, a label.
    const std::vector<std::string> kTypstWeapons = {"*", "`", "@", "#panic(", "#read("};

    Docgen::TemplateRegistry registry;
    const auto templates = registry.list();
    ASSERT_FALSE(templates.empty());

    std::vector<std::string> required = kEscaperWeapons;
    required.insert(required.end(), kTypstWeapons.begin(), kTypstWeapons.end());

    for (const auto& info : templates) {
        const fs::path fixture_path = info.dir / "fixtures" / "special-chars.json";
        ASSERT_TRUE(fs::exists(fixture_path))
            << info.slug << " ships no fixtures/special-chars.json, so the real engine never sees a hostile value";
        json fixture;
        std::ifstream(fixture_path) >> fixture;
        std::vector<std::pair<std::string, std::string>> leaves;
        collect_string_leaves(fixture, "", leaves);

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

// One rule survives the escaper's deletion, on its own merits: an
// `enum`-pinned field is a control value, so branch on it — never print it. It
// used to be enforced because raw `to_pay` typeset in LaTeX text mode was a
// compile error; it stays because a control identifier is not something a
// reader should ever see on a filed document. Statically checked against every
// shipped template so it cannot be forgotten.
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
                   "values and must only be compared, never typeset";
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

// HrOrderPrintsABodyForEveryKind went the same way, and for the same reason —
// the other production symptom it guarded was that `business_trip` and
// `salary_change`, the two kinds whose literal carries an underscore, printed
// a header, signature lines and nothing in between. It searched the rendered
// LaTeX SOURCE for a phrase only that kind's block emits, so once hr_order
// converted there was no source to search: it was rendering a `template.typ`
// through inja, which left the file essentially unchanged, and every phrase it
// looked for was found in the template's own literal text. It PASSED without
// rendering anything — the vacuous-green failure mode this file's other
// gravestones describe.
//
// Its property is now carried end to end by the render gate: all five kinds
// ship a fixture (`basic` = hire, `special-chars` = dismiss, plus `vacation`,
// `business_trip` and `salary_change`), each with a `.expected.txt` naming the
// labels only that kind's branch prints, and scripts/check-render.py requires
// every one of them in the compiled PDF. `vacation` was added by the
// conversion for exactly this handover — see its fixture's header comment.

}  // namespace
