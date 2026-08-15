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

// The defect that survived the first pass of P3: `vat_amount` was neither
// derived nor rejected on invoice/avr, while both templates print it on the
// line directly ABOVE the server-derived total (invoice template.tex:30-31,
// avr:32-33). The exploit below issued a PDF reading «НДС (12%): 999 999,00 ₸»
// over «Итого к оплате: 1 120,00 ₸» — one document contradicting itself.
TEST(InputPolicy, InvoiceRejectsAClientSuppliedVatAmount) {
    std::string field, code, message;
    json exploit = {{"total_tiyn", 112000}, {"vat_rate", "12%"}, {"vat_amount", "999 999,00"}};
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("invoice", exploit, field, code, message));
    EXPECT_EQ(field, "input.vat_amount");
    EXPECT_EQ(code, "not_allowed_override");

    // avr prints the same two lines and gets the same treatment.
    json avr_exploit = {{"total_tiyn", 112000}, {"vat_rate", "12%"}, {"vat_amount", "999 999,00"}};
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("avr", avr_exploit, field, code, message));
    EXPECT_EQ(field, "input.vat_amount");
    EXPECT_EQ(code, "not_allowed_override");
}

TEST(InputPolicy, InvoiceVatMayNotExceedTheTotal) {
    std::string field, code, message;
    // The same forgery expressed honestly as an integer is still refused: the
    // template prints no net line, so "VAT is a part of the amount due" is the
    // strongest rule its actual output supports — and it is enough to make the
    // two printed lines impossible to contradict.
    json too_much = {{"total_tiyn", 112000}, {"vat_rate", "12%"}, {"vat_tiyn", 99999900}};
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("invoice", too_much, field, code, message));
    EXPECT_EQ(field, "input.vat_tiyn");
    EXPECT_EQ(code, "exceeds_total");
    EXPECT_NE(message.find("99999900"), std::string::npos);

    // Equal is legal: a document may be entirely VAT.
    json all_vat = {{"total_tiyn", 112000}, {"vat_rate", "12%"}, {"vat_tiyn", 112000}};
    EXPECT_TRUE(Docgen::InputPolicy::apply_derived_amount("invoice", all_vat, field, code, message)) << message;
}

TEST(InputPolicy, InvoiceFormatsVatFromTheIntegerAndOmitsItWhenAbsent) {
    std::string field, code, message;
    json with_vat = {{"total_tiyn", 112000}, {"vat_rate", "12%"}, {"vat_tiyn", 12000}};
    ASSERT_TRUE(Docgen::InputPolicy::apply_derived_amount("invoice", with_vat, field, code, message)) << message;
    EXPECT_EQ(with_vat["vat_amount"].get<std::string>(), "120,00");
    EXPECT_EQ(with_vat["total"].get<std::string>(), "1 120,00");
    EXPECT_EQ(with_vat["vat_rate"].get<std::string>(), "12%");  // a RATE, left as the caller's free text

    // No VAT at all: no integer in, no string out — the template's
    // `{% if vat_amount != "" %}` then prints no VAT line.
    json no_vat = {{"total_tiyn", 112000}};
    ASSERT_TRUE(Docgen::InputPolicy::apply_derived_amount("invoice", no_vat, field, code, message)) << message;
    EXPECT_FALSE(no_vat.contains("vat_amount"));
    EXPECT_EQ(no_vat["total"].get<std::string>(), "1 120,00");
}

TEST(InputPolicy, VatIntegerGoesThroughTheSameRangeChecksAsTheTotal) {
    std::string field, code, message;
    json not_int = {{"total_tiyn", 112000}, {"vat_tiyn", "1200"}};
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("invoice", not_int, field, code, message));
    EXPECT_EQ(field, "input.vat_tiyn");
    EXPECT_EQ(code, "not_integer");

    json negative = {{"total_tiyn", 112000}, {"vat_tiyn", -1}};
    EXPECT_FALSE(Docgen::InputPolicy::apply_derived_amount("invoice", negative, field, code, message));
    EXPECT_EQ(code, "out_of_range");
}

TEST(InputPolicy, WaybillDeclaresNoVatPartBecauseItPrintsNoVatLine) {
    auto waybill = Docgen::InputPolicy::derived_amount_for("waybill");
    ASSERT_TRUE(waybill);
    EXPECT_TRUE(waybill->optional_parts.empty());

    auto invoice = Docgen::InputPolicy::derived_amount_for("invoice");
    ASSERT_TRUE(invoice);
    ASSERT_EQ(invoice->optional_parts.size(), 1u);
    EXPECT_EQ(invoice->optional_parts[0].first, "vat_tiyn");
    EXPECT_EQ(invoice->optional_parts[0].second, "vat_amount");
    // The exact-sum breakdown stays a tax_invoice-only rule.
    EXPECT_TRUE(invoice->components.empty());
    EXPECT_TRUE(Docgen::InputPolicy::derived_amount_for("tax_invoice")->optional_parts.empty());
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
