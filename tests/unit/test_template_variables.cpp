/**
 * @file test_template_variables.cpp
 * @brief Каталог переменных шаблона. Чистый unit-тест.
 *
 * Главное свойство — каталог ЗАКРЫТ: неизвестный идентификатор обязан быть
 * отказом, а не молчаливой пустотой в документе.
 */

#include <string>

#include <gtest/gtest.h>

#include "docgen/Variables.hpp"

namespace {

TEST(TemplateVariables, TheCatalogueCoversWhatTheOwnerAskedFor) {
    // Дословно из просьбы владельца: «%НДС, оклад сотрудника, ФИО директора,
    // расчётные счета».
    for (const char* id : {"tax.vat_rate", "employee.salary_tiyn", "org.director_name", "org.iik"})
        EXPECT_TRUE(Docgen::Variables::find(id).has_value()) << id;
}

TEST(TemplateVariables, AnUnknownVariableIsAbsentRatherThanEmpty) {
    EXPECT_FALSE(Docgen::Variables::find("org.nonexistent").has_value());
    EXPECT_FALSE(Docgen::Variables::find("").has_value());
    // Похожее, но не то же имя — тоже отказ: иначе опечатка печатала бы пустоту.
    EXPECT_FALSE(Docgen::Variables::find("org.director").has_value());
}

TEST(TemplateVariables, EveryEntryHasBothLanguages) {
    // Кадровые документы двуязычны, и переменная без казахской подписи
    // означала бы форму с пустым ярлыком.
    for (const auto& d : Docgen::Variables::kCatalogue) {
        EXPECT_FALSE(d.label_ru.empty()) << d.id;
        EXPECT_FALSE(d.label_kk.empty()) << d.id;
    }
}

TEST(TemplateVariables, TheVatRateComesFromTheTaxEngineNotFromText) {
    // Ставка НДС обязана быть данными, а не константой в шаблоне (спека §7.1):
    // записанная числом, она даёт неверный документ в день её изменения.
    auto vat = Docgen::Variables::find("tax.vat_rate");
    ASSERT_TRUE(vat.has_value());
    EXPECT_EQ(vat->source, Docgen::Variables::Source::kTaxRate);
}

TEST(TemplateVariables, SalaryIsMoneyInTiynSoTheServerFormatsIt) {
    // Денежная переменная приходит ЦЕЛЫМ: печатаемую строку и сумму прописью
    // выводит сервер, иначе цифра и текст могут разойтись.
    auto salary = Docgen::Variables::find("employee.salary_tiyn");
    ASSERT_TRUE(salary.has_value());
    EXPECT_EQ(salary->kind, Docgen::Variables::Kind::kMoneyTiyn);
}

TEST(TemplateVariables, TheJsonForTheUiCarriesLabelsButNotTheSource) {
    // Арендатору незачем знать, из какой таблицы берётся значение, а нам
    // незачем обещать это в API.
    const auto j = Docgen::Variables::to_json();
    ASSERT_TRUE(j.is_array());
    ASSERT_FALSE(j.empty());
    for (const auto& e : j) {
        EXPECT_TRUE(e.contains("id"));
        EXPECT_TRUE(e.contains("label_ru"));
        EXPECT_TRUE(e.contains("label_kk"));
        EXPECT_TRUE(e.contains("kind"));
        EXPECT_FALSE(e.contains("source"));
    }
}

TEST(TemplateVariables, IdentifiersAreUnique) {
    // Дубликат сделал бы find() зависимым от порядка объявления.
    for (const auto& a : Docgen::Variables::kCatalogue) {
        int seen = 0;
        for (const auto& b : Docgen::Variables::kCatalogue) {
            if (a.id == b.id)
                ++seen;
        }
        EXPECT_EQ(seen, 1) << a.id;
    }
}

}  // namespace
