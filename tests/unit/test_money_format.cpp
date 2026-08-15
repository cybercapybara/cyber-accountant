/**
 * @file test_money_format.cpp
 * @brief Money::format_tiyn_ru — целые тиыны в «12 345,67». Формат обязан
 *        совпадать байт в байт с frontend/src/lib/money.ts::formatTiynRu,
 *        иначе одна и та же сумма выглядит по-разному в форме и в PDF.
 *
 * И с третьей реализацией — `money()` в scripts/check-render.py, гейтом
 * отрендеренного PDF. Последний тест в файле сшивает их: он берёт КАЖДУЮ
 * директиву `amount <path> <tiyn>` из ожиданий шаблонов и проверяет, что
 * настоящий Money::format_tiyn_ru даёт ровно ту строку, которую держит
 * фикстура. Гейт выводит ту же строку из того же целого своим портом; если
 * порт разойдётся с оригиналом, разойдётся и этот тест.
 */

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "money/MoneyFormat.hpp"

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

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

// ── the printed corpus ───────────────────────────────────────────────────

/// Collect every scalar at a dotted path, where a segment may end in `[]` to
/// mean "each element of this array" — the same flattening
/// scripts/check-render.py's `walk()` produces, so a path written in an
/// expectation file addresses the same values in both languages.
void collect_at(const json& node, const std::vector<std::string>& segs, std::size_t idx, std::vector<json>& out) {
    if (idx == segs.size()) {
        out.push_back(node);
        return;
    }
    std::string seg = segs[idx];
    const bool each = seg.size() >= 2 && seg.compare(seg.size() - 2, 2, "[]") == 0;
    if (each)
        seg.resize(seg.size() - 2);
    if (!node.is_object() || !node.contains(seg))
        return;
    const json& child = node.at(seg);
    if (!each) {
        collect_at(child, segs, idx + 1, out);
        return;
    }
    if (!child.is_array())
        return;
    for (const auto& element : child)
        collect_at(element, segs, idx + 1, out);
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> segs;
    std::string current;
    std::istringstream stream(path);
    while (std::getline(stream, current, '.'))
        segs.push_back(current);
    return segs;
}

bool repo_templates_reachable() {
    return fs::exists("templates/latex/fno_300/v1/template.tex");
}

/// THE regression pin for the v0.4.2 defect, and the seam between the C++
/// formatter and the render gate's Python port of it.
///
/// Every printed amount in this repo is declared as an INTEGER in an
/// `amount <path> <tiyn>` directive (scripts/check-render.py); the fixture
/// next to it holds the string the templates typeset. This test runs the
/// REAL Money::format_tiyn_ru over each declared integer and requires it to
/// produce exactly what the fixture holds. Two consequences, both wanted:
///
///  * the gate's `money()` cannot drift from format_tiyn_ru without one of
///    the two failing — they derive the same corpus from the same integers;
///  * if format_tiyn_ru were ever "unified" back into Ledger::format_tiyn's
///    "450000.00", every one of these assertions fails at once. That is the
///    machine form coming back, and the explicit shape check below names it.
TEST(MoneyFormatRu, MatchesEveryAmountDirective) {
    if (!repo_templates_reachable())
        GTEST_SKIP() << "repo templates not reachable from this working directory";

    std::size_t checked = 0;
    for (const auto& entry : fs::recursive_directory_iterator("templates/latex")) {
        const fs::path& expectations = entry.path();
        if (!entry.is_regular_file() || expectations.extension() != ".txt")
            continue;
        const std::string name = expectations.filename().string();
        // Only the PER-FIXTURE files can carry `amount` directives: a
        // template-level expected.txt has no one fixture to check against.
        static const std::string kSuffix = ".expected.txt";
        if (name.size() <= kSuffix.size() || name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
            continue;
        fs::path fixture_path = expectations;
        fixture_path.replace_filename(name.substr(0, name.size() - kSuffix.size()) + ".json");
        ASSERT_TRUE(fs::exists(fixture_path)) << expectations << " has no fixture " << fixture_path;
        json fixture;
        std::ifstream(fixture_path) >> fixture;

        std::ifstream handle(expectations);
        std::string line;
        while (std::getline(handle, line)) {
            if (line.rfind("amount ", 0) != 0)
                continue;
            std::istringstream fields(line);
            std::string directive;
            std::string path;
            long long tiyn = 0;
            ASSERT_TRUE(static_cast<bool>(fields >> directive >> path >> tiyn)) << expectations << ": " << line;

            const std::string printed = Money::format_tiyn_ru(tiyn);
            // The machine form is what shipped in v0.4.2. Named explicitly so
            // a failure here reads as "the machine form is back" rather than
            // as a mysterious string mismatch.
            EXPECT_EQ(printed.find('.'), std::string::npos)
                << path << " in " << expectations << " formatted as \"" << printed
                << "\" — a decimal POINT is Ledger::format_tiyn's machine form, which "
                   "belongs in the database, the API and the ФНО XML, never on a page";
            EXPECT_NE(printed.find(','), std::string::npos) << path << " in " << expectations << ": no decimal comma";

            std::vector<json> values;
            collect_at(fixture, split_path(path), 0, values);
            std::size_t matches = 0;
            for (const auto& value : values)
                if (value.is_string() && value.get<std::string>() == printed)
                    ++matches;
            EXPECT_GT(matches, 0U) << "amount " << path << " " << tiyn << " (" << expectations << ") formats to \""
                                   << printed << "\", which " << fixture_path << " does not carry at that path";
            ++checked;
        }
    }
    // Guard against the whole sweep passing because it found nothing: the
    // shipped corpus has amounts in eight of the ten templates.
    EXPECT_GT(checked, 60U);
}

}  // namespace
