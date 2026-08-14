/**
 * @file test_latex_escape.cpp
 * @brief Unit tests for Docgen::escape_latex — pure function, no fixtures.
 */

#include <string>

#include <gtest/gtest.h>

#include "docgen/LatexEscape.hpp"

using Docgen::escape_latex;

TEST(LatexEscapeTest, EmptyStringPassesThrough) {
    EXPECT_EQ(escape_latex(""), "");
}

TEST(LatexEscapeTest, PlainAsciiPassesThroughUnchanged) {
    EXPECT_EQ(escape_latex("Hello World 123"), "Hello World 123");
}

TEST(LatexEscapeTest, Backslash) {
    EXPECT_EQ(escape_latex("\\"), "\\textbackslash{}");
}

TEST(LatexEscapeTest, CurlyBraces) {
    EXPECT_EQ(escape_latex("{"), "\\{");
    EXPECT_EQ(escape_latex("}"), "\\}");
}

TEST(LatexEscapeTest, DollarSign) {
    EXPECT_EQ(escape_latex("$"), "\\$");
}

TEST(LatexEscapeTest, Ampersand) {
    EXPECT_EQ(escape_latex("&"), "\\&");
}

TEST(LatexEscapeTest, Hash) {
    EXPECT_EQ(escape_latex("#"), "\\#");
}

TEST(LatexEscapeTest, Caret) {
    EXPECT_EQ(escape_latex("^"), "\\textasciicircum{}");
}

TEST(LatexEscapeTest, Underscore) {
    EXPECT_EQ(escape_latex("_"), "\\_");
}

TEST(LatexEscapeTest, Percent) {
    EXPECT_EQ(escape_latex("%"), "\\%");
}

TEST(LatexEscapeTest, Tilde) {
    EXPECT_EQ(escape_latex("~"), "\\textasciitilde{}");
}

TEST(LatexEscapeTest, LessThan) {
    EXPECT_EQ(escape_latex("<"), "\\textless{}");
}

TEST(LatexEscapeTest, GreaterThan) {
    EXPECT_EQ(escape_latex(">"), "\\textgreater{}");
}

// All 12 special characters together, in one pass — confirms none of the
// replacement forms (which themselves contain backslashes and braces) get
// re-scanned and double-escaped.
TEST(LatexEscapeTest, AllSpecialCharactersInOnePass) {
    const std::string input = "\\{}$&#^_%~<>";
    const std::string expected =
        "\\textbackslash{}"
        "\\{"
        "\\}"
        "\\$"
        "\\&"
        "\\#"
        "\\textasciicircum{}"
        "\\_"
        "\\%"
        "\\textasciitilde{}"
        "\\textless{}"
        "\\textgreater{}";
    EXPECT_EQ(escape_latex(input), expected);
}

// Cyrillic passes through untouched — none of its UTF-8 bytes collide with
// the ASCII special-character set.
TEST(LatexEscapeTest, CyrillicPassesThroughUnchanged) {
    const std::string input = "Счёт на оплату № 145 от Покупателя";
    EXPECT_EQ(escape_latex(input), input);
}

// Kazakh-specific letters (ә ғ қ ң ө ұ ү һ і) — the Cyrillic-script letters
// not present in Russian — must pass through unchanged too.
TEST(LatexEscapeTest, KazakhLettersPassThroughUnchanged) {
    const std::string input = "ӘәҒғҚқҢңӨөҰұҮүҺһІі";
    EXPECT_EQ(escape_latex(input), input);
}

// A realistic mixed string: Cyrillic/Kazakh text interleaved with several
// special characters — the exact shape a counterparty name or line-item
// description could take.
TEST(LatexEscapeTest, MixedCyrillicAndSpecialCharacters) {
    const std::string input = "50% скидка & \"кавычки\" #1 _test_ {брейсы} \\бэкслеш ғарыш";
    const std::string expected =
        "50\\% скидка \\& \"кавычки\" \\#1 \\_test\\_ \\{брейсы\\} \\textbackslash{}бэкслеш ғарыш";
    EXPECT_EQ(escape_latex(input), expected);
}
