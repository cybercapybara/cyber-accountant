/**
 * @file MoneyFormat.hpp
 * @brief Целые тиыны -> «12 345,67»: тот денежный формат, который ждут
 *        docgen-шаблоны (см. schema.json и фикстуры под templates/latex/).
 * @details Это НЕ Ledger::format_tiyn (src/ledger/JournalService.hpp): та
 *          даёт машинную «1234.56» для journal_lines.amount и API, эта —
 *          человеческую строку для печати. Разделитель тысяч — обычный
 *          пробел U+0020, ровно как у frontend/src/lib/money.ts::
 *          formatTiynRu (там сознательно не toLocaleString: ru-RU в ICU
 *          даёт NBSP U+00A0, и строки перестали бы совпадать).
 *
 *          Обратной операции здесь нет и быть не должно: разбор
 *          форматированной строки обратно в деньги запрещён инвариантом
 *          фазы.
 */

#pragma once

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace Money {

/**
 * @brief Отформатировать @p tiyn как «12 345,67».
 * @throws std::invalid_argument если @p tiyn отрицателен — знак несёт
 *         отдельное поле домена, сюда передают модуль.
 */
inline std::string format_tiyn_ru(long long tiyn) {
    if (tiyn < 0)
        throw std::invalid_argument("format_tiyn_ru: amount must be non-negative, got " + std::to_string(tiyn));
    const long long whole = tiyn / 100;
    const long long frac = tiyn % 100;

    const std::string digits = std::to_string(whole);
    // Группируем справа налево, потом разворачиваем: проход слева требует
    // предвычисленной длины первой группы и легко даёт «12 3 45».
    std::string grouped;
    grouped.reserve(digits.size() + digits.size() / 3 + 1);
    int taken = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (taken > 0 && taken % 3 == 0)
            grouped.push_back(' ');
        grouped.push_back(*it);
        ++taken;
    }
    std::reverse(grouped.begin(), grouped.end());

    // buf[32], не тесный buf[8]: GCC -Wformat-truncation считает, что
    // long long даст до 20 знаков, и валит -Werror на плотном буфере.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld", frac);
    return grouped + "," + buf;
}

}  // namespace Money
