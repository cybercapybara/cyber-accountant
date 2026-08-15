/**
 * @file test_money_format.cpp
 * @brief Money::format_tiyn_ru — целые тиыны в «12 345,67». Формат обязан
 *        совпадать байт в байт с frontend/src/lib/money.ts::formatTiynRu,
 *        иначе одна и та же сумма выглядит по-разному в форме и в PDF.
 */

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "money/MoneyFormat.hpp"

namespace {

TEST(MoneyFormatRu, GroupsThousandsWithPlainSpaceAndCommaDecimal) {
    EXPECT_EQ(Money::format_tiyn_ru(0), "0,00");
    EXPECT_EQ(Money::format_tiyn_ru(7), "0,07");
    EXPECT_EQ(Money::format_tiyn_ru(100), "1,00");
    EXPECT_EQ(Money::format_tiyn_ru(99999), "999,99");
    EXPECT_EQ(Money::format_tiyn_ru(100000), "1 000,00");
    EXPECT_EQ(Money::format_tiyn_ru(1234567), "12 345,67");
    EXPECT_EQ(Money::format_tiyn_ru(10440000), "104 400,00");
    EXPECT_EQ(Money::format_tiyn_ru(100000000), "1 000 000,00");
    EXPECT_EQ(Money::format_tiyn_ru(100000000000), "1 000 000 000,00");
}

TEST(MoneyFormatRu, SeparatorIsAsciiSpaceNotNbsp) {
    const std::string s = Money::format_tiyn_ru(100000);
    ASSERT_EQ(s.size(), 8u);  // "1 000,00" — 8 однобайтовых символов
    EXPECT_EQ(s[1], ' ');
}

TEST(MoneyFormatRu, NegativeThrows) {
    EXPECT_THROW(Money::format_tiyn_ru(-1), std::invalid_argument);
}

}  // namespace
