/**
 * @file test_amount_in_words.cpp
 * @brief Golden-векторы Money::to_words_ru (спека P3 §3.3). Каждая строка
 *        таблицы — место, где ломаются денежные конвертеры: пустая средняя
 *        триада, женский и мужской род в ОДНОМ числе, 11-14 не в начале
 *        триады, тиыны кроме нуля, границы диапазона.
 */

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "money/AmountInWords.hpp"

namespace {

struct Vector {
    long long tiyn;
    const char* expected;
};

TEST(AmountInWordsRu, GoldenVectors) {
    const Vector kVectors[] = {
        {0, "Ноль тенге 00 тиын"},
        {1, "Ноль тенге 01 тиын"},
        {5, "Ноль тенге 05 тиын"},
        {67, "Ноль тенге 67 тиын"},
        {100, "Один тенге 00 тиын"},
        {20000000, "Двести тысяч тенге 00 тиын"},
        {2100000, "Двадцать одна тысяча тенге 00 тиын"},
        {200000, "Две тысячи тенге 00 тиын"},
        {500000, "Пять тысяч тенге 00 тиын"},
        {1100000, "Одиннадцать тысяч тенге 00 тиын"},
        {11100000, "Сто одиннадцать тысяч тенге 00 тиын"},
        {25057500, "Двести пятьдесят тысяч пятьсот семьдесят пять тенге 00 тиын"},
        {1000000000, "Десять миллионов тенге 00 тиын"},
        {200200200, "Два миллиона две тысячи два тенге 00 тиын"},
        {100000000000, "Один миллиард тенге 00 тиын"},
    };
    for (const auto& v : kVectors)
        EXPECT_EQ(Money::to_words_ru(v.tiyn), std::string(v.expected)) << "tiyn = " << v.tiyn;
}

TEST(AmountInWordsRu, TrillionCeilingIsInclusive) {
    EXPECT_EQ(Money::to_words_ru(Money::kMaxTiyn), "Один триллион тенге 00 тиын");
}

TEST(AmountInWordsRu, AboveCeilingThrowsOutOfRange) {
    EXPECT_THROW(Money::to_words_ru(Money::kMaxTiyn + 1), std::out_of_range);
}

TEST(AmountInWordsRu, NegativeThrowsInvalidArgument) {
    EXPECT_THROW(Money::to_words_ru(-1), std::invalid_argument);
    EXPECT_THROW(Money::to_words_ru(-25057500), std::invalid_argument);
}

TEST(AmountInWordsRu, TiynAlwaysTwoDigits) {
    EXPECT_EQ(Money::to_words_ru(1207), "Двенадцать тенге 07 тиын");
    EXPECT_EQ(Money::to_words_ru(1299), "Двенадцать тенге 99 тиын");
}

TEST(AmountInWordsRu, ElevenToFourteenAlwaysTakePluralForm) {
    EXPECT_EQ(Money::to_words_ru(11200000), "Сто двенадцать тысяч тенге 00 тиын");
    EXPECT_EQ(Money::to_words_ru(11400000), "Сто четырнадцать тысяч тенге 00 тиын");
    EXPECT_EQ(Money::to_words_ru(12100000), "Сто двадцать одна тысяча тенге 00 тиын");
}

}  // namespace
