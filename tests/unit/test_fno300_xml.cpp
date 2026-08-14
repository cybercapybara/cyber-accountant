/**
 * @file test_fno300_xml.cpp
 * @brief Unit tests for Tax::Fno300::build_xml — pure functions, no database
 *        (P2 task-8-brief.md).
 *
 * `pugixml` parses the generated string back in every test — see
 * Fno300.hpp's file header (and Fno910.hpp's, which it mirrors) for why it
 * is used here and NOT in production code: no official XSD exists for ФНО
 * 300.00's content to validate against, only for the unrelated SOAP
 * transport envelope and the SONO-client renderer template.
 */

#include <string>

#include <gtest/gtest.h>

#include <pugixml.hpp>

#include "tax/Fno300.hpp"
#include "tax/FnoXml.hpp"
#include "tax/TaxCalculation.hpp"

using Tax::Calculation;
using Tax::Fno300;
using Tax::OrgInfo;
namespace CalculationKind = Tax::CalculationKind;

namespace {

Calculation make_calculation(long long accrued_tiyn, long long deductible_tiyn) {
    Calculation c;
    c.id = "33333333-3333-3333-3333-333333333333";
    c.org_id = "22222222-2222-2222-2222-222222222222";
    c.kind = CalculationKind::kVat;
    c.period_from = "2026-04-01";
    c.period_to = "2026-06-30";
    c.computed_at = "2026-07-05T00:00:00Z";
    c.input_snapshot = nlohmann::json::object();
    const long long balance_tiyn = accrued_tiyn - deductible_tiyn;
    c.result_snapshot = {
        {"accrued_tiyn", accrued_tiyn},
        {"deductible_tiyn", deductible_tiyn},
        {"balance_tiyn", balance_tiyn},
    };
    c.total_tiyn = balance_tiyn;
    return c;
}

OrgInfo make_org(std::string name = "Test Org") {
    OrgInfo org;
    // Must match make_calculation()'s org_id — build_xml refuses a pair
    // that crosses tenants (multi-tenancy guard, final fix round).
    org.org_id = "22222222-2222-2222-2222-222222222222";
    org.bin = "987654321098";
    org.name = std::move(name);
    org.tax_period_year = "2026";
    org.tax_period_half = "";  // half-year field is not meaningful for a quarterly form
    return org;
}

std::string field_value(const pugi::xml_node& sheet, const char* name) {
    for (auto field : sheet.children("field")) {
        if (std::string(field.attribute("name").value()) == name)
            return field.child_value();
    }
    return "";
}

}  // namespace

TEST(Fno300BuildXml, BuildsWellFormedXml) {
    const auto calc = make_calculation(500'000, 200'000);
    const auto org = make_org();

    const std::string xml = Fno300::build_xml(calc, org);

    pugi::xml_document doc;
    const pugi::xml_parse_result result = doc.load_string(xml.c_str());
    ASSERT_TRUE(result) << "XML failed to parse: " << result.description();
    EXPECT_STREQ(doc.document_element().name(), "fno");
    EXPECT_STREQ(doc.document_element().attribute("code").value(), "300.00");
    EXPECT_STREQ(doc.document_element().attribute("schemaValidated").value(), "false");
    EXPECT_FALSE(Fno300::kSchemaValidated);

    const auto sheet = doc.child("fno").child("form").child("sheetGroup").child("sheet");
    ASSERT_TRUE(sheet);
    EXPECT_EQ(field_value(sheet, "iin"), "987654321098");
    EXPECT_EQ(field_value(sheet, "rnn"), "987654321098");
    EXPECT_EQ(field_value(sheet, "payer_name1"), "Test Org");
    EXPECT_EQ(field_value(sheet, "period_year"), "2026");
    EXPECT_EQ(field_value(sheet, "currency_code"), "KZT");
}

TEST(Fno300BuildXml, ContainsQuarterPeriod) {
    // period_from = 2026-04-01 -> Q2.
    const auto calc = make_calculation(500'000, 200'000);
    const auto org = make_org();

    const std::string xml = Fno300::build_xml(calc, org);

    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(xml.c_str()));
    const auto sheet = doc.child("fno").child("form").child("sheetGroup").child("sheet");
    EXPECT_EQ(field_value(sheet, "period_quarter"), "2");

    const auto summary = doc.child("fno").child("calcSummary");
    ASSERT_TRUE(summary);
    EXPECT_STREQ(summary.child_value("periodFrom"), "2026-04-01");
    EXPECT_STREQ(summary.child_value("periodTo"), "2026-06-30");
}

TEST(Fno300BuildXml, ContainsQuarterPeriodForEachQuarterBoundary) {
    struct Case {
        const char* period_from;
        const char* expected_quarter;
    };
    const Case cases[] = {
        {"2026-01-01", "1"},
        {"2026-04-01", "2"},
        {"2026-07-01", "3"},
        {"2026-10-01", "4"},
    };
    for (const auto& c : cases) {
        auto calc = make_calculation(100'000, 40'000);
        calc.period_from = c.period_from;
        const auto org = make_org();

        const std::string xml = Fno300::build_xml(calc, org);
        pugi::xml_document doc;
        ASSERT_TRUE(doc.load_string(xml.c_str()));
        const auto sheet = doc.child("fno").child("form").child("sheetGroup").child("sheet");
        EXPECT_EQ(field_value(sheet, "period_quarter"), c.expected_quarter) << "period_from=" << c.period_from;
    }
}

TEST(Fno300BuildXml, AmountsInWholeTenge) {
    // accrued 500,000 tiyn (5,000 tenge exactly), deductible with a
    // non-round remainder: 199,950 tiyn = 1999.50 tenge -> half-up 2000.
    const auto calc = make_calculation(500'000, 199'950);
    const auto org = make_org();

    const std::string xml = Fno300::build_xml(calc, org);

    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(xml.c_str()));
    const auto sheet = doc.child("fno").child("form").child("sheetGroup").child("sheet");

    EXPECT_EQ(field_value(sheet, "field_300_00_012"), "5000");
    EXPECT_EQ(field_value(sheet, "field_300_00_023"), "2000");
    // balance = 500,000 - 199,950 = 300,050 tiyn = 3000.50 tenge -> half-up 3001, all payable.
    EXPECT_EQ(field_value(sheet, "field_300_00_030_01"), "3001");
    EXPECT_EQ(field_value(sheet, "field_300_00_030_02"), "0");

    const auto summary = doc.child("fno").child("calcSummary");
    ASSERT_TRUE(summary);
    const std::string accrued_tenge = summary.child_value("accruedTenge");
    const std::string deductible_tenge = summary.child_value("deductibleTenge");
    const std::string balance_tenge = summary.child_value("balanceTenge");
    const std::string to_pay_tenge = summary.child_value("toPayTenge");
    EXPECT_EQ(accrued_tenge, "5000");
    EXPECT_EQ(deductible_tenge, "2000");
    EXPECT_EQ(balance_tenge, "3001");
    EXPECT_EQ(to_pay_tenge, "3001");
    // Whole-tenge amount fields carry no decimal point.
    EXPECT_EQ(accrued_tenge.find('.'), std::string::npos);
    EXPECT_EQ(deductible_tenge.find('.'), std::string::npos);
    EXPECT_EQ(balance_tenge.find('.'), std::string::npos);
}

TEST(Fno300BuildXml, NegativeBalanceGoesToRefundField) {
    // accrued 200,000 tiyn < deductible 500,000 tiyn -> balance -300,000 tiyn
    // (a refund/excess-credit position, per calculate_vat's own doc
    // comment): to-pay must be zero and the full magnitude must appear on
    // the refund side, both in the real KGD field pair and in calcSummary.
    const auto calc = make_calculation(200'000, 500'000);
    const auto org = make_org();

    const std::string xml = Fno300::build_xml(calc, org);

    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(xml.c_str()));
    const auto sheet = doc.child("fno").child("form").child("sheetGroup").child("sheet");

    // field_300_00_030_01 = "к уплате" (payable), field_300_00_030_02 =
    // excess-credit/refund position (see Fno300.hpp file header, finding 2).
    EXPECT_EQ(field_value(sheet, "field_300_00_030_01"), "0");
    EXPECT_EQ(field_value(sheet, "field_300_00_030_02"), "3000");

    const auto summary = doc.child("fno").child("calcSummary");
    ASSERT_TRUE(summary);
    EXPECT_STREQ(summary.child_value("toPayTenge"), "0");
    EXPECT_STREQ(summary.child_value("toRefundTenge"), "3000");
    // balanceTenge keeps the true (negative) sign — it is the raw
    // accrued-minus-deductible figure, not the split payable/refund view.
    EXPECT_STREQ(summary.child_value("balanceTenge"), "-3000");
}

TEST(Fno300BuildXml, ZeroBalanceIsNeitherPayableNorRefundable) {
    const auto calc = make_calculation(300'000, 300'000);
    const auto org = make_org();

    const std::string xml = Fno300::build_xml(calc, org);

    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(xml.c_str()));
    const auto sheet = doc.child("fno").child("form").child("sheetGroup").child("sheet");
    EXPECT_EQ(field_value(sheet, "field_300_00_030_01"), "0");
    EXPECT_EQ(field_value(sheet, "field_300_00_030_02"), "0");
}

TEST(Fno300BuildXml, EscapesSpecialCharsInOrgName) {
    const auto calc = make_calculation(500'000, 200'000);
    const auto org = make_org("ТОО \"Ромашка\" & Ко");

    const std::string xml = Fno300::build_xml(calc, org);

    // Raw special characters must not appear unescaped in the serialized
    // form (would break well-formedness).
    EXPECT_EQ(xml.find("Ромашка\" & Ко"), std::string::npos);

    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_string(xml.c_str())) << "escaped org name must still parse as well-formed XML";

    const auto sheet = doc.child("fno").child("form").child("sheetGroup").child("sheet");
    // Round-trips back to the exact original string once parsed.
    EXPECT_EQ(field_value(sheet, "payer_name1"), "ТОО \"Ромашка\" & Ко");
}

TEST(Fno300BuildXml, ThrowsOnWrongCalculationKind) {
    auto calc = make_calculation(500'000, 200'000);
    calc.kind = CalculationKind::kSnrSimplified;
    const auto org = make_org();

    EXPECT_THROW(Fno300::build_xml(calc, org), std::invalid_argument);
}
