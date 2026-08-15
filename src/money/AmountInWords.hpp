/**
 * @file AmountInWords.hpp
 * @brief Сумма прописью из целого числа тиын (спека P3 §3.2-3.4).
 * @details Чистый модуль: ни БД, ни Drogon, ни JSON. Вход — НЕОТРИЦАТЕЛЬНОЕ
 *          целое в тиынах; вызывающий обязан передавать модуль (знак несёт
 *          отдельное поле домена — например balance_kind у ФНО 300.00, где
 *          TaxController::build_form_input считает balance_tenge по модулю).
 *          Верхняя граница — kMaxTiyn (10^12 тенге), та же, что у
 *          Payroll::kMaxGrossTiyn; выше — std::out_of_range, на уровне API
 *          это 422.
 *
 *          Деньги в системе — ЦЕЛЫЕ тиыны (100 тиын = 1 тенге). Тенге и тиыны
 *          здесь разделяются целочисленным делением и остатком; double в
 *          денежном пути не появляется ни на шаг.
 *
 *          Точка внедрения зафиксирована спекой: результат подставляется в
 *          JSON `input` документа ДО TemplateRegistry::validate(), шаблон
 *          остаётся с плейсхолдером вида {{ total_words }}. Считать прописи
 *          внутри .tex запрещено — миграция на Typst будет это разбирать
 *          обратно.
 *
 *          Языки. Разложение числа на триады (detail::split_triads) и выбор
 *          словоформы разряда (detail::plural_index) сознательно вынесены из
 *          to_words_ru: казахская пропись добавляется в ЭТОТ же заголовок и
 *          переиспользует их, а своим делает только словарь и сборку триады.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace Money {

/// 10^14 тиын = 10^12 ₸ = один триллион тенге, включительно. Совпадает с
/// Payroll::kMaxGrossTiyn (src/payroll/PayrollCalculator.hpp) намеренно:
/// две границы денежного диапазона в одной системе разъезжаются.
inline constexpr long long kMaxTiyn = 100'000'000'000'000LL;

namespace detail {

/// Число триад, покрывающее kMaxTiyn: единицы, тысячи, миллионы, миллиарды,
/// триллионы.
inline constexpr int kTriadCount = 5;

inline const char* const kRuUnits[20] = {"ноль",       "один",        "два",        "три",          "четыре",
                                         "пять",       "шесть",       "семь",       "восемь",       "девять",
                                         "десять",     "одиннадцать", "двенадцать", "тринадцать",   "четырнадцать",
                                         "пятнадцать", "шестнадцать", "семнадцать", "восемнадцать", "девятнадцать"};

inline const char* const kRuTens[10] = {
    "", "", "двадцать", "тридцать", "сорок", "пятьдесят", "шестьдесят", "семьдесят", "восемьдесят", "девяносто"};

inline const char* const kRuHundreds[10] = {
    "", "сто", "двести", "триста", "четыреста", "пятьсот", "шестьсот", "семьсот", "восемьсот", "девятьсот"};

/// Разряд: три словоформы (1 / 2-4 / 0 и 5-20) плюс род.
struct RuScale {
    const char* forms[3];
    bool feminine;
};

/// Индекс 1 — тысяча (женский род: «одна тысяча», «две тысячи»), дальше —
/// мужской. Нулевой элемент — единицы, у них разрядного слова нет.
inline const RuScale kRuScales[kTriadCount] = {
    {{"", "", ""}, false},
    {{"тысяча", "тысячи", "тысяч"}, true},
    {{"миллион", "миллиона", "миллионов"}, false},
    {{"миллиард", "миллиарда", "миллиардов"}, false},
    {{"триллион", "триллиона", "триллионов"}, false},
};

/// Разложить целое число (тенге) на триады, младшая — индекс 0. Общая для
/// всех языков часть: казахская пропись берёт эту функцию, а не копию цикла.
inline std::array<long long, kTriadCount> split_triads(long long value) {
    std::array<long long, kTriadCount> triads{};
    long long rest = value;
    for (int i = 0; i < kTriadCount; ++i) {
        triads[static_cast<std::size_t>(i)] = rest % 1000;
        rest /= 1000;
    }
    return triads;
}

/// 1 -> ед. ч.; 2-4 -> род. п. ед. ч.; 0 и 5-20 -> род. п. мн. ч.
/// 11-14 ВСЕГДА множественное — это та самая проверка `n % 100`, которую
/// наивные реализации на `n % 10` пропускают («одиннадцать тысяча»).
inline int plural_index(long long n) {
    const long long hundred_rest = n % 100;
    if (hundred_rest >= 11 && hundred_rest <= 14)
        return 2;
    switch (n % 10) {
        case 1:
            return 0;
        case 2:
        case 3:
        case 4:
            return 1;
        default:
            return 2;
    }
}

/// Слова одной триады 0..999. @p feminine переключает 1/2 на «одна»/«две»
/// (нужно только перед «тысяча»). Пустая триада не даёт НИ ОДНОГО слова —
/// именно поэтому 10 000 000 ₸ читается «десять миллионов тенге», а не
/// «десять миллионов ноль тысяч тенге».
inline void append_triad_ru(std::vector<std::string>& out, long long triad, bool feminine) {
    if (triad == 0)
        return;
    const int h = static_cast<int>(triad / 100);
    const int t = static_cast<int>((triad / 10) % 10);
    const int u = static_cast<int>(triad % 10);
    if (h > 0)
        out.emplace_back(kRuHundreds[h]);
    if (t >= 2) {
        out.emplace_back(kRuTens[t]);
        if (u > 0) {
            if (feminine && u == 1)
                out.emplace_back("одна");
            else if (feminine && u == 2)
                out.emplace_back("две");
            else
                out.emplace_back(kRuUnits[u]);
        }
    } else {
        const int rest = t * 10 + u;  // 0..19 — читается одним словом
        if (rest > 0) {
            if (feminine && rest == 1)
                out.emplace_back("одна");
            else if (feminine && rest == 2)
                out.emplace_back("две");
            else
                out.emplace_back(kRuUnits[rest]);
        }
    }
}

/// Пара «строчная -> прописная» для букв ВНЕ русского блока: два байта UTF-8
/// в каждую сторону.
struct CyrillicCasePair {
    unsigned char lower[2];
    unsigned char upper[2];
};

/// Девять казахских букв, которых нет в русском алфавите. Регистровое
/// смещение здесь НЕ равно единому 0x20 русского блока и не сводится к
/// арифметике: в кириллическом дополнении пары идут (чётный = прописная,
/// нечётный = строчная), но уже на U+04C1/U+04C2 порядок переворачивается,
/// поэтому обобщать опаснее, чем перечислить. Из числительных начинаются
/// только на «ү» («үш») и «қ» («қырық»), остальные семь — страховка на
/// будущие словари (валюты, месяцы) и на то, чтобы правило было полным.
/// Пара «і -> І» единственная пересекает границу планов: D1 96 -> D0 86.
inline constexpr CyrillicCasePair kExtraCyrillicCasePairs[] = {
    {{0xD3, 0x99}, {0xD3, 0x98}},  // ә -> Ә
    {{0xD2, 0x93}, {0xD2, 0x92}},  // ғ -> Ғ
    {{0xD2, 0x9B}, {0xD2, 0x9A}},  // қ -> Қ («қырық»)
    {{0xD2, 0xA3}, {0xD2, 0xA2}},  // ң -> Ң
    {{0xD3, 0xA9}, {0xD3, 0xA8}},  // ө -> Ө
    {{0xD2, 0xB1}, {0xD2, 0xB0}},  // ұ -> Ұ
    {{0xD2, 0xAF}, {0xD2, 0xAE}},  // ү -> Ү («үш»)
    {{0xD2, 0xBB}, {0xD2, 0xBA}},  // һ -> Һ
    {{0xD1, 0x96}, {0xD0, 0x86}},  // і -> І
};

/// Поднять первую кириллическую букву @p s в верхний регистр по месту.
/// НЕ std::toupper: он работает побайтно и порвал бы UTF-8. В UTF-8
/// строчные а..п — это D0 B0..D0 BF, строчные р..я — D1 80..D1 8F, а весь
/// прописной блок А..Я непрерывен на D0 90..D0 AF, поэтому оба случая —
/// правка двух байт. Казахские буквы живут вне этого блока и разбираются
/// таблицей kExtraCyrillicCasePairs.
inline void capitalize_cyrillic(std::string& s) {
    if (s.size() < 2)
        return;
    const auto b0 = static_cast<unsigned char>(s[0]);
    const auto b1 = static_cast<unsigned char>(s[1]);
    if (b0 == 0xD0 && b1 >= 0xB0 && b1 <= 0xBF) {
        s[1] = static_cast<char>(b1 - 0x20);
        return;
    }
    if (b0 == 0xD1 && b1 >= 0x80 && b1 <= 0x8F) {
        s[0] = static_cast<char>(0xD0);
        s[1] = static_cast<char>(b1 + 0x20);
        return;
    }
    for (const CyrillicCasePair& pair : kExtraCyrillicCasePairs) {
        if (b0 == pair.lower[0] && b1 == pair.lower[1]) {
            s[0] = static_cast<char>(pair.upper[0]);
            s[1] = static_cast<char>(pair.upper[1]);
            return;
        }
    }
}

/// Склеить слова через один пробел.
inline std::string join_words(const std::vector<std::string>& words) {
    std::string out;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0)
            out.push_back(' ');
        out += words[i];
    }
    return out;
}

/// Хвост «<пробел>NN<пробел>тиын» — тиыны ЧИСЛОМ, всегда две цифры.
/// buf[32], а не тесный buf[8]: GCC -Wformat-truncation считает, что
/// long long даст до 20 знаков, и валит -Werror на плотном буфере.
inline std::string tiyn_tail(long long tiyn_part, const char* unit_word) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), " %02lld ", tiyn_part);
    return std::string(buf) + unit_word;
}

/// Единицы. Нулевой элемент пуст: «нөл» печатается только как всё число
/// целиком и живёт в to_words_kk, а не здесь.
inline const char* const kKkUnits[10] = {"", "бір", "екі", "үш", "төрт", "бес", "алты", "жеті", "сегіз", "тоғыз"};

/// Десятки. В казахском НЕТ слитных «одиннадцать/двенадцать»: 11 — это два
/// слова «он бір», поэтому таблица десятков покрывает и 10..19, а таблицы
/// на 20 элементов, как в русском, не нужно.
inline const char* const kKkTens[10] = {
    "", "он", "жиырма", "отыз", "қырық", "елу", "алпыс", "жетпіс", "сексен", "тоқсан"};

/// Разрядное слово казахского. Индекс 0 — единицы (слова нет). @c drop_one
/// = «единица перед этим разрядом опускается»: у «мың» — да (1965 читается
/// «мың тоғыз жүз алпыс бес», kk.wikipedia «Сан есім»), у «миллион» и выше
/// — нет («бір миллион теңге»), потому что это заимствованные
/// существительные, требующие числительного. Ровно то исключение, которое
/// пропускают реализации, объявляющие казахский «проще русского».
struct KkScale {
    const char* word;
    bool drop_one;
};

inline const KkScale kKkScales[kTriadCount] = {
    {"", false},
    {"мың", true},
    {"миллион", false},
    {"миллиард", false},
    {"триллион", false},
};

/// Слова одной триады 0..999 по-казахски. Ни рода, ни падежных окончаний
/// множественного числа: разряд не изменяется ни от какой цифры.
/// @p drop_leading_one убирает «бір», ТОЛЬКО когда триада равна ровно 1 и
/// разряд это допускает (мың). Сотня обрабатывается здесь же и по тому же
/// правилу: 100 -> «жүз», 200 -> «екі жүз». В 101 единица остаётся —
/// «жүз бір», опускается лишь множитель ПЕРЕД разрядным словом.
inline void append_triad_kk(std::vector<std::string>& out, long long triad, bool drop_leading_one) {
    if (triad == 0)
        return;
    const int h = static_cast<int>(triad / 100);
    const int t = static_cast<int>((triad / 10) % 10);
    const int u = static_cast<int>(triad % 10);
    if (h > 0) {
        if (h > 1)
            out.emplace_back(kKkUnits[h]);
        out.emplace_back("жүз");
    }
    if (t > 0)
        out.emplace_back(kKkTens[t]);
    if (u > 0) {
        const bool alone = (h == 0 && t == 0);
        if (!(alone && u == 1 && drop_leading_one))
            out.emplace_back(kKkUnits[u]);
    }
}

}  // namespace detail

/**
 * @brief Русская сумма прописью: «Двести пятьдесят тысяч пятьсот семьдесят
 *        пять тенге 00 тиын».
 * @details «тенге» не склоняется. Тиыны печатаются числом в две цифры.
 *          Первая буква заглавная. Ноль пишется «Ноль» (не «Нуль») —
 *          принятое решение спеки, зафиксированное golden-вектором.
 * @throws std::invalid_argument если @p tiyn отрицателен (передавайте модуль).
 * @throws std::out_of_range если @p tiyn больше kMaxTiyn.
 */
inline std::string to_words_ru(long long tiyn) {
    if (tiyn < 0)
        throw std::invalid_argument("to_words_ru: amount must be non-negative, got " + std::to_string(tiyn));
    if (tiyn > kMaxTiyn)
        throw std::out_of_range("to_words_ru: amount " + std::to_string(tiyn) + " tiyn exceeds the supported maximum " +
                                std::to_string(kMaxTiyn));

    const long long tenge = tiyn / 100;
    const long long tiyn_part = tiyn % 100;

    const std::array<long long, detail::kTriadCount> triads = detail::split_triads(tenge);

    std::vector<std::string> words;
    for (int i = detail::kTriadCount - 1; i >= 0; --i) {
        const long long triad = triads[static_cast<std::size_t>(i)];
        if (triad == 0)
            continue;
        const detail::RuScale& scale = detail::kRuScales[i];
        detail::append_triad_ru(words, triad, scale.feminine);
        if (i > 0)
            words.emplace_back(scale.forms[detail::plural_index(triad)]);
    }
    if (words.empty())
        words.emplace_back("ноль");  // единственный случай, когда «ноль» печатается
    words.emplace_back("тенге");

    std::string out = detail::join_words(words);
    detail::capitalize_cyrillic(out);
    return out + detail::tiyn_tail(tiyn_part, "тиын");
}

/**
 * @brief Казахская сумма прописью: «Екі жүз елу мың бес жүз жетпіс бес
 *        теңге 00 тиын».
 * @details Ни рода, ни падежных окончаний множественного числа — разряд не
 *          изменяется, зат есім после числительного остаётся в исходной
 *          форме. Единица опускается перед «жүз» и «мың» (1965 -> «мың
 *          тоғыз жүз алпыс бес», 1150 -> «мың жүз елу»), но НЕ перед
 *          «миллион»/«миллиард»/«триллион» -> «бір миллион». Ноль — «нөл».
 *          Тиыны, как и в русской прописи, печатаются числом в две цифры.
 *          Источники правил и golden-векторов перечислены в шапке
 *          tests/unit/test_amount_in_words.cpp.
 * @throws std::invalid_argument если @p tiyn отрицателен (передавайте модуль).
 * @throws std::out_of_range если @p tiyn больше kMaxTiyn.
 */
inline std::string to_words_kk(long long tiyn) {
    if (tiyn < 0)
        throw std::invalid_argument("to_words_kk: amount must be non-negative, got " + std::to_string(tiyn));
    if (tiyn > kMaxTiyn)
        throw std::out_of_range("to_words_kk: amount " + std::to_string(tiyn) + " tiyn exceeds the supported maximum " +
                                std::to_string(kMaxTiyn));

    const long long tenge = tiyn / 100;
    const long long tiyn_part = tiyn % 100;

    const std::array<long long, detail::kTriadCount> triads = detail::split_triads(tenge);

    std::vector<std::string> words;
    for (int i = detail::kTriadCount - 1; i >= 0; --i) {
        const long long triad = triads[static_cast<std::size_t>(i)];
        if (triad == 0)
            continue;
        const detail::KkScale& scale = detail::kKkScales[i];
        detail::append_triad_kk(words, triad, scale.drop_one);
        if (i > 0)
            words.emplace_back(scale.word);
    }
    if (words.empty())
        words.emplace_back("нөл");  // единственный случай, когда «нөл» печатается
    words.emplace_back("теңге");

    std::string out = detail::join_words(words);
    detail::capitalize_cyrillic(out);
    return out + detail::tiyn_tail(tiyn_part, "тиын");
}

}  // namespace Money
