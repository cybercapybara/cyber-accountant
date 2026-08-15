/**
 * @file test_input_policy.cpp
 * @brief Docgen::InputPolicy — единая таблица «что клиенту можно прислать»
 *        и «что сервер выводит сам». Чистый модуль, БД не нужна.
 */

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "docgen/InputPolicy.hpp"

namespace {

using json = nlohmann::json;

TEST(InputPolicy, GenerateSlugsAreExactlyThePrimaryDocumentTypes) {
    const auto& slugs = Docgen::InputPolicy::generate_slugs();
    ASSERT_EQ(slugs.size(), 5u);
    for (const auto* s : {"invoice", "avr", "waybill", "tax_invoice", "reconciliation"})
        EXPECT_NE(std::find(slugs.begin(), slugs.end(), std::string(s)), slugs.end()) << s;
    for (const auto* s : {"payslip", "fno_910", "fno_300", "hr_order", "labor_contract"})
        EXPECT_FALSE(Docgen::InputPolicy::input_is_caller_authored(s)) << s;
}

TEST(InputPolicy, DerivedAmountPaths) {
    auto inv = Docgen::InputPolicy::derived_amount_for("invoice");
    ASSERT_TRUE(inv);
    EXPECT_EQ(inv->tiyn_path, "total_tiyn");
    EXPECT_EQ(inv->amount_path, "total");
    EXPECT_EQ(inv->words_path, "total_words");

    EXPECT_TRUE(inv->components.empty());

    auto ti = Docgen::InputPolicy::derived_amount_for("tax_invoice");
    ASSERT_TRUE(ti);
    EXPECT_EQ(ti->tiyn_path, "totals.with_vat_tiyn");
    EXPECT_EQ(ti->amount_path, "totals.with_vat");
    EXPECT_EQ(ti->words_path, "total_words");
    ASSERT_EQ(ti->components.size(), 2u);
    EXPECT_EQ(ti->components[0].first, "totals.amount_tiyn");
    EXPECT_EQ(ti->components[0].second, "totals.amount");
    EXPECT_EQ(ti->components[1].first, "totals.vat_tiyn");
    EXPECT_EQ(ti->components[1].second, "totals.vat");

    EXPECT_FALSE(Docgen::InputPolicy::derived_amount_for("reconciliation").has_value());
    EXPECT_FALSE(Docgen::InputPolicy::derived_amount_for("payslip").has_value());
}

TEST(InputPolicy, EditableFieldsMatchTheServerBuiltForms) {
    EXPECT_EQ(Docgen::InputPolicy::editable_fields("fno_910"), (std::vector<std::string>{"director", "accountant"}));
    EXPECT_EQ(Docgen::InputPolicy::editable_fields("fno_300"), (std::vector<std::string>{"director", "accountant"}));
    EXPECT_TRUE(Docgen::InputPolicy::editable_fields("payslip").empty());
    EXPECT_EQ(Docgen::InputPolicy::editable_fields("hr_order"),
              (std::vector<std::string>{"director", "reason", "details"}));
    EXPECT_EQ(Docgen::InputPolicy::editable_fields("labor_contract"),
              (std::vector<std::string>{
                  "work_schedule", "probation_months", "employer.director", "employer.address", "employee.address"}));
    EXPECT_TRUE(Docgen::InputPolicy::editable_fields("no_such_slug").empty());
}

TEST(InputPolicy, ApplyDerivedAmountFillsBothStrings) {
    json input = {{"number", "1"}, {"total_tiyn", 1234567}};
    std::string field, code, message;
    ASSERT_TRUE(Docgen::InputPolicy::apply_derived_amount("invoice", input, field, code, message)) << message;
    EXPECT_EQ(input["total"].get<std::string>(), "12 345,67");
    EXPECT_EQ(input["total_words"].get<std::string>(), "Двенадцать тысяч триста сорок пять тенге 67 тиын");
}

TEST(InputPolicy, ApplyDerivedAmountFormatsAllThreeTaxInvoiceTotals) {
    json input = {{"totals", {{"amount_tiyn", 9000000}, {"vat_tiyn", 1440000}, {"with_vat_tiyn", 10440000}}}};
    std::string field, code, message;
    ASSERT_TRUE(Docgen::InputPolicy::apply_derived_amount("tax_invoice", input, field, code, message)) << message;
    EXPECT_EQ(input["totals"]["amount"].get<std::string>(), "90 000,00");
    EXPECT_EQ(input["totals"]["vat"].get<std::string>(), "14 400,00");
    EXPECT_EQ(input["totals"]["with_vat"].get<std::string>(), "104 400,00");
    EXPECT_EQ(input["total_words"].get<std::string>(), "Сто четыре тысячи четыреста тенге 00 тиын");
}

TEST(InputPolicy, TaxInvoiceTotalsMustSumExactly) {
    std::string field, code, message;
    // 90 000,00 + 14 400,00 = 104 400,00, а заявлен итог 104 000,00.
    json inconsistent = {{"totals", {{"amount_tiyn", 9000000}, {"vat_tiyn", 1440000}, {"with_vat_tiyn", 10400000}}}};
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("tax_invoice", inconsistent, field, code, message));
    EXPECT_EQ(field, "input.totals.with_vat_tiyn");
    EXPECT_EQ(code, "inconsistent_total");
    EXPECT_NE(message.find("10400000"), std::string::npos);

    // Нулевой НДС — законная разбивка, а не краевой случай.
    json no_vat = {{"totals", {{"amount_tiyn", 9000000}, {"vat_tiyn", 0}, {"with_vat_tiyn", 9000000}}}};
    EXPECT_TRUE(Docgen::InputPolicy::apply_derived_amount("tax_invoice", no_vat, field, code, message)) << message;
    EXPECT_EQ(no_vat["totals"]["vat"].get<std::string>(), "0,00");
}

TEST(InputPolicy, TaxInvoiceRejectsClientSuppliedComponentStrings) {
    std::string field, code, message;
    json with_amount = {
        {"totals", {{"amount_tiyn", 9000000}, {"vat_tiyn", 1440000}, {"with_vat_tiyn", 10440000}, {"vat", "0,00"}}}};
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("tax_invoice", with_amount, field, code, message));
    EXPECT_EQ(field, "input.totals.vat");
    EXPECT_EQ(code, "not_allowed_override");
}

TEST(InputPolicy, TaxInvoiceRequiresEveryComponentInteger) {
    std::string field, code, message;
    json missing_vat = {{"totals", {{"amount_tiyn", 9000000}, {"with_vat_tiyn", 10440000}}}};
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("tax_invoice", missing_vat, field, code, message));
    EXPECT_EQ(field, "input.totals.vat_tiyn");
    EXPECT_EQ(code, "missing");
}

TEST(InputPolicy, ApplyDerivedAmountRejectsClientSuppliedAmountAndWords) {
    std::string field, code, message;
    json with_words = {{"total_tiyn", 100}, {"total_words", "Один тенге 00 тиын"}};
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("invoice", with_words, field, code, message));
    EXPECT_EQ(field, "input.total_words");
    EXPECT_EQ(code, "not_allowed_override");

    json with_total = {{"total_tiyn", 100}, {"total", "1,00"}};
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("invoice", with_total, field, code, message));
    EXPECT_EQ(field, "input.total");
    EXPECT_EQ(code, "not_allowed_override");
}

TEST(InputPolicy, ApplyDerivedAmountRejectsMissingBadAndOutOfRangeTiyn) {
    std::string field, code, message;
    json missing = json::object();
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("invoice", missing, field, code, message));
    EXPECT_EQ(field, "input.total_tiyn");
    EXPECT_EQ(code, "missing");

    json not_int = {{"total_tiyn", "1234567"}};
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("invoice", not_int, field, code, message));
    EXPECT_EQ(code, "not_integer");

    json negative = {{"total_tiyn", -1}};
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("invoice", negative, field, code, message));
    EXPECT_EQ(code, "out_of_range");

    json huge = {{"total_tiyn", Money::kMaxTiyn + 1}};
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("invoice", huge, field, code, message));
    EXPECT_EQ(code, "out_of_range");
}

}  // namespace
