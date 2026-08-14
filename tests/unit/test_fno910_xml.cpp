/**
 * @file test_fno910_xml.cpp
 * @brief Unit tests for Tax::Fno910::build_xml / Tax::FnoXml — pure
 *        functions, no database (P2 task-7-brief.md).
 *
 * `pugixml` parses the generated string back in every test — see
 * Fno910.hpp's file header for why it is used here and NOT in production
 * code (no official XSD exists for ФНО 910.00's content to validate
 * against, only for the unrelated SOAP transport envelope).
 */

#include <string>

#include <gtest/gtest.h>

#include <pugixml.hpp>

#include "tax/Fno910.hpp"
#include "tax/FnoXml.hpp"
#include "tax/TaxCalculation.hpp"

using Tax::Calculation;
namespace CalculationKind = Tax::CalculationKind;
using Tax::Fno910;
using Tax::OrgInfo;
using Tax::FnoXml::escape;
using Tax::FnoXml::round_half_up_to_tenge;

namespace {

Calculation make_calculation(long long income_tiyn, long long rate_bp, long long total_tiyn) {
    Calculation c;
    c.id = "11111111-1111-1111-1111-111111111111";
    c.org_id = "22222222-2222-2222-2222-222222222222";
    c.kind = CalculationKind::kSnrSimplified;
    c.period_from = "2026-01-01";
    c.period_to = "2026-06-30";
    c.computed_at = "2026-07-01T00:00:00Z";
    c.input_snapshot = nlohmann::json::object();
    c.result_snapshot = {
        {"income_tiyn", income_tiyn},
        {"rate_bp", rate_bp},
        {"tax_tiyn", total_tiyn},
    };
    c.total_tiyn = total_tiyn;
    return c;
}

OrgInfo make_org(std::string name = "Test Org") {
    OrgInfo org;
    // Must match make_calculation()'s org_id — build_xml refuses a pair that
    // crosses tenants (the multi-tenancy guard added in the final fix round).
    org.org_id = "22222222-2222-2222-2222-222222222222";
    org.bin = "123456789012";
    org.name = std::move(name);
    org.tax_period_year = "2026";
    org.tax_period_half = "1";
    return org;
}

}  // namespace

// ---- FnoXml helpers, standalone ---------------------------------------

TEST(FnoXmlEscape, EscapesAllFiveSpecialCharacters) {
    EXPECT_EQ(escape("a&b<c>d\"e'f"), "a&amp;b&lt;c&gt;d&quot;e&apos;f");
}

TEST(FnoXmlEscape, PlainTextPassesThroughUnchanged) {
    EXPECT_EQ(escape("Plain ASCII 123"), "Plain ASCII 123");
    EXPECT_EQ(escape("Кириллица"), "Кириллица");
}

TEST(FnoXmlEscape, EmptyStringPassesThrough) {
    EXPECT_EQ(escape(""), "");
}

TEST(FnoXmlEscape, DoesNotDoubleEscapeAlreadyEscapedInput) {
    // A single left-to-right pass must not re-scan its own output.
    EXPECT_EQ(escape("&amp;"), "&amp;amp;");
}

TEST(FnoXmlRounding, RoundsHalfUp) {
    EXPECT_EQ(round_half_up_to_tenge(12350), 124);  // .50 -> up
    EXPECT_EQ(round_half_up_to_tenge(12349), 123);  // .49 -> down
    EXPECT_EQ(round_half_up_to_tenge(50), 1);
    EXPECT_EQ(round_half_up_to_tenge(49), 0);
    EXPECT_EQ(round_half_up_to_tenge(0), 0);
}

TEST(FnoXmlRounding, RoundsNegativeAmountsSymmetrically) {
    EXPECT_EQ(round_half_up_to_tenge(-12350), -124);
    EXPECT_EQ(round_half_up_to_tenge(-12349), -123);
}

// ---- Fno910::build_xml ---------------------------------------------------

TEST(Fno910BuildXml, BuildsWellFormedXml) {
    const auto calc = make_calculation(1'000'000, 400, 40'000);
    const auto org = make_org();

    const std::string xml = Fno910::build_xml(calc, org);

    pugi::xml_document doc;
    const pugi::xml_parse_result result = doc.load_string(xml.c_str());
    ASSERT_TRUE(result) << "XML failed to parse: " << result.description();
    EXPECT_STREQ(doc.document_element().name(), "fno");
}

TEST(Fno910BuildXml, ContainsBinAndPeriod) {
    const auto calc = make_calculation(1'000'000, 400, 40'000);
    const auto org = make_org();

    const std::string xml = Fno910::build_xml(calc, org);

    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(xml.c_str()));

    EXPECT_STREQ(doc.document_element().attribute("bin").value(), "123456789012");

    const auto sheet = doc.child("fno").child("form").child("sheetGroup").child("sheet");
    ASSERT_TRUE(sheet);

    auto field_value = [&](const char* name) -> std::string {
        for (auto field : sheet.children("field")) {
            if (std::string(field.attribute("name").value()) == name)
                return field.child_value();
        }
        return "";
    };
    EXPECT_EQ(field_value("iin"), "123456789012");
    EXPECT_EQ(field_value("rnn"), "123456789012");
    EXPECT_EQ(field_value("period_year"), "2026");
    EXPECT_EQ(field_value("period_half_year"), "1");

    const auto summary = doc.child("fno").child("calcSummary");
    ASSERT_TRUE(summary);
    EXPECT_STREQ(summary.child_value("periodFrom"), "2026-01-01");
    EXPECT_STREQ(summary.child_value("periodTo"), "2026-06-30");
}

TEST(Fno910BuildXml, AmountsInWholeTenge) {
    // 1,000,000 tiyn income (10,000 tenge exactly) and a total_tiyn with a
    // non-round tiyn remainder (40,050 tiyn = 400.50 tenge -> half-up 401).
    const auto calc = make_calculation(1'000'000, 400, 40'050);
    const auto org = make_org();

    const std::string xml = Fno910::build_xml(calc, org);

    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(xml.c_str()));

    const auto sheet = doc.child("fno").child("form").child("sheetGroup").child("sheet");
    std::string income_field;
    for (auto field : sheet.children("field")) {
        if (std::string(field.attribute("name").value()) == "field_910_00_001")
            income_field = field.child_value();
    }
    EXPECT_EQ(income_field, "10000");
    EXPECT_TRUE(income_field.find('.') == std::string::npos);

    const auto summary = doc.child("fno").child("calcSummary");
    const std::string tax_tenge = summary.child_value("taxTenge");
    const std::string income_tenge = summary.child_value("incomeTenge");
    EXPECT_EQ(tax_tenge, "401");
    EXPECT_EQ(income_tenge, "10000");
    // Whole-tenge amount fields carry no decimal point — checked on the
    // specific amount fields, not the whole document (the form's own
    // "910.00" code attribute legitimately contains a literal '.').
    EXPECT_EQ(tax_tenge.find('.'), std::string::npos);
    EXPECT_EQ(income_tenge.find('.'), std::string::npos);
}

TEST(Fno910BuildXml, EscapesSpecialCharsInOrgName) {
    const auto calc = make_calculation(1'000'000, 400, 40'000);
    const auto org = make_org("ТОО \"Ромашка\" & Ко");

    const std::string xml = Fno910::build_xml(calc, org);

    // Raw special characters must not appear unescaped in the serialized
    // form (would break well-formedness).
    EXPECT_EQ(xml.find("Ромашка\" & Ко"), std::string::npos);

    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(xml.c_str())) << "escaped org name must still parse as well-formed XML";

    const auto sheet = doc.child("fno").child("form").child("sheetGroup").child("sheet");
    std::string payer_name;
    for (auto field : sheet.children("field")) {
        if (std::string(field.attribute("name").value()) == "payer_name1")
            payer_name = field.child_value();
    }
    // Round-trips back to the exact original string once parsed.
    EXPECT_EQ(payer_name, "ТОО \"Ромашка\" & Ко");
}

TEST(Fno910BuildXml, MarksArtifactAsSchemaUnvalidated) {
    // No official XSD exists for ФНО 910.00's content (see Fno910.hpp file
    // header, Step 1) — the artifact must say so both as a named constant
    // and as a machine-readable XML attribute.
    EXPECT_FALSE(Fno910::kSchemaValidated);

    const auto calc = make_calculation(1'000'000, 400, 40'000);
    const auto org = make_org();
    const std::string xml = Fno910::build_xml(calc, org);

    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(xml.c_str()));
    EXPECT_STREQ(doc.document_element().attribute("schemaValidated").value(), "false");
}

TEST(Fno910BuildXml, ThrowsOnWrongCalculationKind) {
    auto calc = make_calculation(1'000'000, 400, 40'000);
    calc.kind = CalculationKind::kVat;
    const auto org = make_org();

    EXPECT_THROW(Fno910::build_xml(calc, org), std::invalid_argument);
}
