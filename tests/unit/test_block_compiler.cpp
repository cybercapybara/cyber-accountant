/**
 * @file test_block_compiler.cpp
 * @brief Сборка шаблона из блоков. Чистый unit-тест: ни БД, ни Drogon.
 *
 * Главный тест здесь — НЕ «блоки собираются», а то, что текст арендатора не
 * может стать кодом Typst. Это единственное место в системе, где
 * пользовательский текст попадает в позицию исходника: данные документа
 * шаблон читает сам из input.json, а вот статические подписи арендатор пишет
 * в конструкторе, и они оказываются в собранном `.typ`.
 */

#include <string>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "docgen/BlockCompiler.hpp"

namespace {

using json = nlohmann::json;
using Docgen::Blocks::Compiled;

json header(const std::string& title) {
    return json{{"type", "header"}, {"title", title}};
}

Compiled compile_ok(const json& blocks) {
    Compiled out;
    auto err = Docgen::Blocks::compile(blocks, out);
    EXPECT_FALSE(err.has_value()) << (err ? err->code + ": " + err->message : "");
    return out;
}

// --------------------------------------------------------------------------
// Текст арендатора не становится кодом
// --------------------------------------------------------------------------

TEST(BlockCompiler, MarkupCharactersInALabelStayInsideAStringLiteral) {
    // Подпись со всеми управляющими символами разметки Typst. Ни один из них
    // не должен оказаться в позиции разметки: иначе `*20%*` напечаталось бы
    // жирным, а `#panic()` исполнилась бы при компиляции.
    const std::string hostile = "Скидка *20%* _итого_ #panic() `код` @ссылка $x$";
    auto out = compile_ok(json::array({header(hostile)}));

    // Текст присутствует ровно один раз и ровно внутри строкового литерала.
    const std::string expected_literal = "#let s0 = \"" + hostile + "\"";
    EXPECT_NE(out.source.find(expected_literal), std::string::npos) << out.source;
    // И в документе он печатается ПО ИМЕНИ, а не текстом.
    EXPECT_NE(out.source.find("[#s0]"), std::string::npos);
}

TEST(BlockCompiler, QuotesAndBackslashesAreEscapedInsideTheLiteral) {
    // Ровно два символа требуют экранирования внутри строки Typst, и это
    // полное правило, а не список, который забудут дополнить.
    auto out = compile_ok(json::array({header("Путь C:\\счета и \"кавычки\"")}));
    EXPECT_NE(out.source.find("\"Путь C:\\\\счета и \\\"кавычки\\\"\""), std::string::npos) << out.source;
}

TEST(BlockCompiler, ALabelCannotCloseTheLiteralAndInjectCode) {
    // Попытка выйти из строки и дописать код. Закрывающая кавычка обязана
    // оказаться экранированной, а значит остаться данными.
    auto out = compile_ok(json::array({header("x\" ; #panic(\"pwned\") ; \"y")}));
    // Нет ни одной НЕэкранированной кавычки, закрывающей литерал раньше срока:
    // весь текст лежит в одном литерале.
    EXPECT_NE(out.source.find("#let s0 = \"x\\\" ; #panic(\\\"pwned\\\") ; \\\"y\""), std::string::npos) << out.source;
}

// --------------------------------------------------------------------------
// Идиомы, выстраданные при миграции на Typst
// --------------------------------------------------------------------------

TEST(BlockCompiler, ThePreambleCarriesTheHardWonTypstSettings) {
    auto out = compile_ok(json::array({header("Счёт")}));
    // Свешенный за поле дефис — гейт валит рамку слова за границу.
    EXPECT_NE(out.source.find("overhang: false"), std::string::npos);
    // Перенос РАЗРЫВАЕТ слово, и объявленная подпись перестаёт находиться как
    // непрерывный текст: в акте это стоило потери целой фразы.
    EXPECT_NE(out.source.find("hyphenate: false"), std::string::npos);
    // Токен со слешем иначе рвётся по слешу.
    EXPECT_NE(out.source.find("box(it)"), std::string::npos);
    // Данные читает сам шаблон — подстановки в текст не происходит.
    EXPECT_NE(out.source.find("json(\"input.json\")"), std::string::npos);
}

TEST(BlockCompiler, SignatureLinesAreBoxesNotBareLines) {
    // Голая line() нулевой высоты давала линию, проведённую СКВОЗЬ текст —
    // дефект, ради которого в гейте появился отдельный слой.
    auto out = compile_ok(json::array(
        {header("Акт"), json{{"type", "signatures"}, {"parties", json::array({"Исполнитель", "Заказчик"})}}}));
    EXPECT_NE(out.source.find("stroke: (bottom: 0.4pt)"), std::string::npos);
    EXPECT_EQ(out.source.find("line("), std::string::npos) << "голая line() вернулась в сборку";
}

TEST(BlockCompiler, OnlyTheDescriptionColumnGetsTheFlexibleWidth) {
    // 1fr на всех колонках расползает таблицу за поле, что гейт валит как
    // выход за границу.
    auto out = compile_ok(json::array({header("Счёт"),
                                       json{{"type", "table"},
                                            {"columns",
                                             json::array({json{{"title", "Наименование"}, {"key", "name"}},
                                                          json{{"title", "Кол-во"}, {"key", "qty"}},
                                                          json{{"title", "Цена"}, {"key", "price"}}})}}}));
    // Ровно одна гибкая колонка.
    std::size_t count = 0;
    for (std::size_t p = out.source.find("1fr"); p != std::string::npos; p = out.source.find("1fr", p + 1))
        ++count;
    EXPECT_EQ(count, 1u) << out.source;
}

// --------------------------------------------------------------------------
// Схема, форма и подписи выводятся ИЗ блоков
// --------------------------------------------------------------------------

TEST(BlockCompiler, EveryStaticLabelLandsInExpectedSoTheGateCanCheckIt) {
    // Это и есть ответ на главное противоречие фазы: пользовательский шаблон
    // не проходит гейт в CI, поэтому список обязательных подписей обязан
    // порождаться сам. Шаблон без единой подписи собрать нельзя.
    auto out = compile_ok(
        json::array({header("Счёт на оплату"), json{{"type", "text"}, {"text", "Оплата в течение 10 дней."}}}));
    EXPECT_NE(out.expected.find("Счёт на оплату"), std::string::npos);
    EXPECT_NE(out.expected.find("Оплата в течение 10 дней."), std::string::npos);
    // Гейт сверяет поле страницы с настройкой самого шаблона.
    EXPECT_NE(out.expected.find("margin 18mm"), std::string::npos);
}

TEST(BlockCompiler, ATemplateWithNoStaticLabelIsRefused) {
    Compiled out;
    auto err = Docgen::Blocks::compile(json::array({json{{"type", "pagebreak"}}}), out);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, "no_static_labels");
}

TEST(BlockCompiler, TheFormCarriesRussianAndKazakhLabelsNotFieldNames) {
    // Форма, построенная напрямую по схеме, показала бы пользователю
    // `total_tiyn`: в схемах встроенных шаблонов заголовков полей нет вовсе.
    auto out = compile_ok(json::array(
        {header("Счёт"),
         json{{"type", "fields"},
              {"rows", json::array({json{{"label", "Основание"}, {"label_kk", "Негіздеме"}, {"field", "basis"}}})}}}));
    ASSERT_TRUE(out.form.contains("fields"));
    ASSERT_EQ(out.form["fields"].size(), 1u);
    EXPECT_EQ(out.form["fields"][0]["field"], "basis");
    EXPECT_EQ(out.form["fields"][0]["label_ru"], "Основание");
    EXPECT_EQ(out.form["fields"][0]["label_kk"], "Негіздеме");
}

TEST(BlockCompiler, MoneyAsksForAnIntegerAndPrintsServerWrittenStrings) {
    // P3 §3: пользователь вводит ЦЕЛОЕ в тиынах, а печатаемую строку и сумму
    // прописью пишет сервер — иначе цифра и текст в одном документе могут
    // разойтись. Значит в ФОРМЕ одно числовое поле, а не три.
    auto out = compile_ok(json::array({header("Счёт"), json{{"type", "totals"}}}));

    EXPECT_EQ(out.schema["properties"]["total_tiyn"]["type"], "integer");
    EXPECT_EQ(out.schema["properties"]["total"]["type"], "string");
    EXPECT_EQ(out.schema["properties"]["total_words"]["type"], "string");

    // В форме — только целое. Ни `total`, ни `total_words` пользователь не
    // вводит: их присылка отвергается сервером как not_allowed_override.
    ASSERT_EQ(out.form["fields"].size(), 1u);
    EXPECT_EQ(out.form["fields"][0]["field"], "total_tiyn");
    EXPECT_EQ(out.form["fields"][0]["widget"], "money");
}

TEST(BlockCompiler, AVariableFromTheCatalogueIsAcceptedAndAnUnknownOneIsNot) {
    auto out = compile_ok(json::array(
        {header("Счёт"),
         json{{"type", "fields"},
              {"rows", json::array({json{{"label", "ФИО директора"}, {"variable", "org.director_name"}}})}}}));
    EXPECT_NE(out.source.find("org.director_name"), std::string::npos);

    Compiled bad;
    auto err = Docgen::Blocks::compile(
        json::array({header("Счёт"),
                     json{{"type", "fields"},
                          {"rows", json::array({json{{"label", "Что-то"}, {"variable", "org.nonexistent"}}})}}}),
        bad);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, "unknown_variable");
    // Ошибка называет НОМЕР БЛОКА, а не «где-то в шаблоне».
    EXPECT_EQ(err->block_index, 1u);
}

TEST(BlockCompiler, AFieldNameThatWouldChangeTheExpressionIsRefused) {
    // Имя поля попадает в исходник как `d.<имя>`: точка или дефис превратили
    // бы его в другое выражение.
    for (const char* bad_name : {"my-field", "my.field", "1field", "my field", ""}) {
        Compiled out;
        auto err = Docgen::Blocks::compile(
            json::array({header("Счёт"),
                         json{{"type", "fields"}, {"rows", json::array({json{{"label", "X"}, {"field", bad_name}}})}}}),
            out);
        ASSERT_TRUE(err.has_value()) << bad_name;
        EXPECT_EQ(err->code, "invalid_field_name") << bad_name;
    }
}

TEST(BlockCompiler, AnUnknownBlockTypeIsRefusedRatherThanIgnored) {
    // Молча пропущенный блок — это документ без строки, о которой автор
    // шаблона думает, что она есть.
    Compiled out;
    auto err = Docgen::Blocks::compile(json::array({header("Счёт"), json{{"type", "qr_code"}}}), out);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, "unknown_block_type");
    EXPECT_EQ(err->block_index, 1u);
}

TEST(BlockCompiler, AnEmptyOrOversizedBlockListIsRefused) {
    Compiled out;
    EXPECT_TRUE(Docgen::Blocks::compile(json::array(), out).has_value());
    EXPECT_TRUE(Docgen::Blocks::compile(json::object(), out).has_value());

    json many = json::array();
    for (int i = 0; i < 201; ++i)
        many.push_back(json{{"type", "pagebreak"}});
    auto err = Docgen::Blocks::compile(many, out);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->code, "too_many_blocks");
}

}  // namespace
