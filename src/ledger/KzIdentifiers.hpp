#pragma once
#include <array>
#include <cctype>
#include <string>

namespace Ledger {

/// Контрольный разряд БИН/ИИН РК (12 цифр, две системы весов, 10 → невалидно).
inline bool is_valid_bin_iin(const std::string& id) {
    if (id.size() != 12)
        return false;
    std::array<int, 12> d{};
    for (int i = 0; i < 12; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(id[i])))
            return false;
        d[i] = id[i] - '0';
    }
    static constexpr std::array<int, 11> w1{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    static constexpr std::array<int, 11> w2{3, 4, 5, 6, 7, 8, 9, 10, 11, 1, 2};
    auto weighted = [&](const std::array<int, 11>& w) {
        int s = 0;
        for (int i = 0; i < 11; ++i)
            s += d[i] * w[i];
        return s % 11;
    };
    int s = weighted(w1);
    if (s == 10) {
        s = weighted(w2);
        if (s == 10)
            return false;
    }
    return d[11] == s;
}

}  // namespace Ledger
