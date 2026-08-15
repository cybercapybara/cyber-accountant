# P3: суммы прописью, жизненный цикл документов, роль кадровика, навигация — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Сумма прописью считается сервером из целого числа тиын (русский и казахский), документы редактируются с версиями и разделяют удаление и аннулирование, появляется роль кадровика поверх настоящей матрицы прав, верхнее меню сворачивается из десяти плоских пунктов в четыре раздела.

**Architecture:** Четыре независимых слоя поверх готовой архитектуры P2. (1) Чистый модуль `src/money/AmountInWords.hpp` — целые тиыны на входе, строка на выходе, без БД и Drogon; серверные строители `input` подставляют результат в JSON, шаблон остаётся с плейсхолдером. (2) Новая таблица `document_versions` забирает у `documents` файловые метаданные и `input_snapshot`, `documents` хранит указатель на текущую версию; правка = новая версия через тот же allowlist, что и создание. (3) `src/tenancy/OrgPermissions.hpp` превращает денилист «viewer» в матрицу «роль × ресурс × действие» с запретом по умолчанию, макрос `API_REQUIRE_ORG_PERM` применяет её ко всем мутациям И ко всем чтениям. (4) Манифест маршрутов SPA получает `navGroup`/`navRoles`, org-роль доходит до SPA через `/auth/me`.

**Tech Stack:** C++20, Drogon, PostgreSQL 15, pqxx, nlohmann/json, GoogleTest; React 18 + TypeScript, TanStack Query, zod, react-hook-form, vitest; XeLaTeX-шаблоны через существующий docgen.

**Spec:** `docs/superpowers/specs/2026-08-15-p3-documents-roles-words-design.md` (ревизия 2 + правки по итогам релиза v0.3.1). Критика, из которой выросла ревизия 2, с точными file:line — `.superpowers/sdd/p3-spec-critique.md`.

## Global Constraints

Раздел применяется к КАЖДОЙ задаче — исполнитель задачи N обязан соблюдать всё нижеперечисленное, даже если в тексте задачи это не повторено.

- **Ветка:** `feature/p3-documents-roles-words` режется от **`main`**, не от `feature/p2-tax-payroll-hr`. Та ветка отстала от `main` на 8 коммитов и пинит тег образа `0.2.1` в Helm-values — сборка поверх неё предложит откатить прод на два релиза назад. Имплементеры коммитят, но **не пушат** (пуш — за контроллером); `git add` — только своими путями, по явному перечислению файлов.
- **Деньги — целые тиыны** (`long long`). Никакой плавающей точки; никакого обратного парсинга форматированных строк («12 345,67») в деньги. Формат в БД — `NUMERIC(18,2)`, в API/домене — строка «1234.56» (`Ledger::parse_tiyn`/`Ledger::format_tiyn`, `src/ledger/JournalService.hpp`).
- **Ставки и константы** берутся только из `tax_rates`/`tax_constants` по дате. Ни одной налоговой константы в коде.
- **`org_id` — только из JWT-клейма** через `API_REQUIRE_ORG`; каждый запрос ограничен организацией. Репозитории тенантных таблиц наследуют `Tenancy::OrgCrudBase` (`src/tenancy/OrgScoped.hpp`) — методов «выбрать без org» не существует.
- **Журнал insert-only**, правится только сторно.
- **Семантика ошибок:** 400 — кривая форма запроса; 422 — семантически неверное значение; 409 — конфликт состояния. Форма ответа одна: `{error, status, message, ...}` через `ErrorResponse::*` / `Api::Validation::*`, никакого самодельного JSON ошибки.
- **Тройная синхронизация маршрутов:** каждый `ADD_METHOD_TO` в контроллере обязан появиться в `Api::get_endpoints()` (`src/api/Endpoints.hpp`) **и** в `docs/openapi.yaml`. Гейты `./scripts/check-openapi-drift.sh` и `./scripts/check-routes-registered.sh` валят CI на расхождении.
- **`src/` header-only (ADR 0003):** реализация живёт в `.hpp`; новых `.cpp` не добавлять (кроме существующих точек входа `src/main.cpp`, `src/worker_main.cpp`).
- **Тестовые корзины по каталогам** (`./scripts/check-test-buckets.sh`): `tests/unit` — без сервисов; `tests/integration` — настоящие Postgres/Redis; `tests/api` — контроллер через `TestHelpers::make_request`; `tests/e2e` — отдельный бинарь с живым HTTP-сервером. CMake подхватывает файлы глобом (`tests/unit/*.cpp`, `tests/integration/*.cpp` + `tests/api/*.cpp`) — новый файл в CMakeLists прописывать не надо.
- **Миграции** — `migrations/NNN_slug.sql`, сквозная нумерация, **без `BEGIN`/`COMMIT`** (раннер сам оборачивает файл в одну транзакцию под advisory-локом). Безымянные `CHECK` снимаются по автоимени (`org_members_role_check`, `documents_status_check`).
- **Весь пользовательский текст интерфейса — русский.** Единственное принятое заимствование — `Email`. Даты рендерятся только через `frontend/src/lib/dateFormat.ts` (фиксированный UTC+5), деньги показываются только через `<Money tiyn={...} />`.
- **Сборки и тесты идут ТОЛЬКО в GitHub Actions.** Локально скомпилировать нельзя. Локально доступны: git, `clang-format` 17.0.6, `npx tsc --noEmit`, eslint, vitest, кодоген и шелл-гейты `./scripts/check-*.sh`. Итерация через CI ~40 минут, поэтому код вычитывается на компилируемость глазами ДО пуша.
- **Коммиты — conventional, без AI-атрибуции.** Никаких `Co-Authored-By: Claude`, `Generated with Claude Code` и подобных трейлеров.

### Ошибки компиляции, которые ломали именно этот репозиторий

Их нельзя поймать локально — компилятора нет. Перечитывайте свой диф на каждую из восьми:

1. **Лямбда в `execute_write` без `return 0;` в конце.** `Database::get().execute_write([&](auto& txn) { txn.exec(...); return 0; })` — если тело заканчивается `txn.exec(...)` без `return`, вывод типа даёт `void` и шаблон не инстанцируется. Всегда возвращайте значение (`return 0;`, `return !r.empty();`, `return Entity::from_row(r[0]);`).
2. **Пропущенный `.template as<T>()` на зависимом выражении.** Внутри шаблонной лямбды `r.at(0).at(0).as<long>()` не компилируется — нужно `r.at(0).at(0).template as<long>()`.
3. **`const std::string&` в range-for по списку C-строк.** `for (const std::string& s : {"a", "b"})` создаёт временные — привязывайте `const auto*` / `const char*`: `for (const auto* kind : {"a", "b"})`.
4. **`*/` внутри блочного комментария.** Doxygen-шапки в этом репозитории длинные; последовательность `*/` в тексте (например в пути `templates/latex/*/v1/`) закрывает комментарий досрочно. Пишите `templates/latex/<slug>/v1/`.
5. **Неоднозначный `json` → `std::optional<json>`.** Именованный `json`-lvalue, передаваемый в параметр `std::optional<nlohmann::json>`, под GCC неоднозначен (жадный конструктор-шаблон nlohmann). Оборачивайте явно: `std::optional<nlohmann::json>{input}`, а при возврате — `return std::optional<json>(std::move(input));`, никогда `return input;`.
6. **Brace-init прямо в `Response::accepted` / `Response::ok`.** Двойная вложенная brace-инициализация в параметр `const json&` разрешается через initializer_list-конструктор неоднозначно. Стройте именованный объект: `const json payload = {{"a", 1}}; callback(Response::ok({{"data", payload}}));`.
7. **`using X;`, где `X` — пространство имён, а не тип.** `using Foo::Bar;` для namespace — ошибка; нужно `namespace bar = Foo::Bar;` или `using namespace Foo::Bar;`.
8. **Плотно рассчитанный `char buf[]` под GCC `-Wformat-truncation`.** `char buf[8]; std::snprintf(buf, sizeof(buf), " %02lld", v);` — GCC считает, что `long long` даст до 20 знаков, и валит `-Werror`. Берите `char buf[32]`.

---

### Task 1: `Money::to_words_ru` — сумма прописью по-русски

**Files:**
- Create: `src/money/AmountInWords.hpp`
- Test: `tests/unit/test_amount_in_words.cpp`

**Interfaces:**
- Consumes: ничего (модуль чистый — ни БД, ни Drogon, ни `nlohmann/json`).
- Produces (на это опираются задачи 2, 4, 9):
  - `inline constexpr long long Money::kMaxTiyn = 100'000'000'000'000LL;` — 10^14 тиын = 10^12 ₸ = один триллион тенге, та же граница, что `Payroll::kMaxGrossTiyn` (`src/payroll/PayrollCalculator.hpp:112`).
  - `std::string Money::to_words_ru(long long tiyn)` — бросает `std::invalid_argument` при `tiyn < 0`, `std::out_of_range` при `tiyn > Money::kMaxTiyn`.
  - `void Money::detail::capitalize_cyrillic(std::string& s)` — поднимает первую кириллическую букву в верхний регистр по месту; задача 2 расширит её казахскими буквами.
  - `void Money::detail::append_triad_ru(std::vector<std::string>& out, long long triad, bool feminine)` и `int Money::detail::plural_index(long long n)` — внутренние, но задача 2 читает их как образец.

- [ ] **Step 1: Написать падающий тест с golden-векторами**

`tests/unit/test_amount_in_words.cpp`:

```cpp
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
```

- [ ] **Step 2: Убедиться, что тест падает**

Локально собрать нельзя (Global Constraints). Проверка — глазами: файла `src/money/AmountInWords.hpp` не существует, значит `#include` не разрешится. Гейт на этом шаге: `./scripts/check-test-buckets.sh` должен пройти (файл лежит в `tests/unit`, сервисов не трогает).

- [ ] **Step 3: Реализация**

`src/money/AmountInWords.hpp` — целиком:

```cpp
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
 *          Точка внедрения зафиксирована спекой: результат подставляется в
 *          JSON `input` документа ДО TemplateRegistry::validate(), шаблон
 *          остаётся с плейсхолдером вида {{ total_words }}. Считать прописи
 *          внутри .tex запрещено — миграция на Typst будет это разбирать
 *          обратно.
 */

#pragma once

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

inline const char* const kRuUnits[20] = {"ноль",         "один",         "два",          "три",
                                         "четыре",       "пять",         "шесть",        "семь",
                                         "восемь",       "девять",       "десять",       "одиннадцать",
                                         "двенадцать",   "тринадцать",   "четырнадцать", "пятнадцать",
                                         "шестнадцать",  "семнадцать",   "восемнадцать", "девятнадцать"};

inline const char* const kRuTens[10] = {"",           "",          "двадцать",   "тридцать", "сорок",
                                        "пятьдесят",  "шестьдесят", "семьдесят", "восемьдесят", "девяносто"};

inline const char* const kRuHundreds[10] = {"",         "сто",      "двести",    "триста",  "четыреста",
                                            "пятьсот",  "шестьсот", "семьсот",   "восемьсот", "девятьсот"};

/// Разряд: три словоформы (1 / 2-4 / 0 и 5-20) плюс род.
struct RuScale {
    const char* forms[3];
    bool feminine;
};

/// Индекс 0 — тысяча (женский род: «одна тысяча», «две тысячи»), дальше —
/// мужской. Нулевой элемент — единицы, у них разрядного слова нет.
inline const RuScale kRuScales[5] = {
    {{"", "", ""}, false},
    {{"тысяча", "тысячи", "тысяч"}, true},
    {{"миллион", "миллиона", "миллионов"}, false},
    {{"миллиард", "миллиарда", "миллиардов"}, false},
    {{"триллион", "триллиона", "триллионов"}, false},
};

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

/// Поднять первую кириллическую букву @p s в верхний регистр по месту.
/// НЕ std::toupper: он работает побайтно и порвал бы UTF-8. В UTF-8
/// строчные а..п — это D0 B0..D0 BF, строчные р..я — D1 80..D1 8F, а весь
/// прописной блок А..Я непрерывен на D0 90..D0 AF, поэтому оба случая —
/// правка двух байт.
inline void capitalize_cyrillic(std::string& s) {
    if (s.size() < 2)
        return;
    const auto b0 = static_cast<unsigned char>(s[0]);
    const auto b1 = static_cast<unsigned char>(s[1]);
    if (b0 == 0xD0 && b1 >= 0xB0 && b1 <= 0xBF) {
        s[1] = static_cast<char>(b1 - 0x20);
    } else if (b0 == 0xD1 && b1 >= 0x80 && b1 <= 0x8F) {
        s[0] = static_cast<char>(0xD0);
        s[1] = static_cast<char>(b1 + 0x20);
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

/// Хвост «<пробел>NN тиын»/«<пробел>NN тиын» — тиыны ЧИСЛОМ, всегда две
/// цифры. buf[32], а не тесный buf[8]: GCC -Wformat-truncation считает,
/// что long long даст до 20 знаков, и валит -Werror на плотном буфере.
inline std::string tiyn_tail(long long tiyn_part, const char* unit_word) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), " %02lld ", tiyn_part);
    return std::string(buf) + unit_word;
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

    long long triads[5] = {0, 0, 0, 0, 0};
    long long rest = tenge;
    for (int i = 0; i < 5; ++i) {
        triads[i] = rest % 1000;
        rest /= 1000;
    }

    std::vector<std::string> words;
    for (int i = 4; i >= 0; --i) {
        if (triads[i] == 0)
            continue;
        detail::append_triad_ru(words, triads[i], detail::kRuScales[i].feminine);
        if (i > 0)
            words.emplace_back(detail::kRuScales[i].forms[detail::plural_index(triads[i])]);
    }
    if (words.empty())
        words.emplace_back("ноль");  // единственный случай, когда «ноль» печатается
    words.emplace_back("тенге");

    std::string out = detail::join_words(words);
    detail::capitalize_cyrillic(out);
    return out + detail::tiyn_tail(tiyn_part, "тиын");
}

}  // namespace Money
```

- [ ] **Step 4: Прогнать тест в CI**

Запуск: GitHub Actions на пуше ветки (локальной сборки нет). Ожидание: сьют `AmountInWordsRu.*` — 6 тестов, все PASS. Если какой-то вектор разошёлся — чинить реализацию, а НЕ вектор: таблица §3.3 согласована владельцем и сверена с реальными данными (`templates/latex/payslip/v1/fixtures/basic.json:21` содержит ровно строку для 250 575,00 ₸).

- [ ] **Step 5: Форматирование и коммит**

```bash
clang-format-17 -i src/money/AmountInWords.hpp tests/unit/test_amount_in_words.cpp
./scripts/check-test-buckets.sh
git add src/money/AmountInWords.hpp tests/unit/test_amount_in_words.cpp
git commit -m "feat(money): Russian amount-in-words converter over integer tiyn"
```

---

### Task 2: `Money::to_words_kk` — сумма прописью по-казахски

**Files:**
- Modify: `src/money/AmountInWords.hpp` (добавить казахскую половину; расширить `detail::capitalize_cyrillic`)
- Modify: `tests/unit/test_amount_in_words.cpp` (добавить казахский сьют)
- Modify: `templates/latex/labor_contract/v1/schema.json`, `templates/latex/labor_contract/v1/template.tex`, `templates/latex/labor_contract/v1/fixtures/basic.json`, `templates/latex/labor_contract/v1/fixtures/special-chars.json`

**Interfaces:**
- Consumes (задача 1): `Money::kMaxTiyn`, `Money::detail::capitalize_cyrillic(std::string&)`, `Money::detail::join_words(const std::vector<std::string>&)`, `Money::detail::tiyn_tail(long long, const char*)`.
- Produces (на это опирается задача 4): `std::string Money::to_words_kk(long long tiyn)` — те же исключения, что у `to_words_ru`: `std::invalid_argument` на отрицательном, `std::out_of_range` выше `kMaxTiyn`. Плюс поле `salary_words_kk` в схеме, фикстурах и шаблоне `labor_contract` — задача 4 обязана его заполнять.

- [ ] **Step 1: Сверка по цитируемым источникам (ОБЯЗАТЕЛЬНА до кода)**

Гейт качества спеки §3.4: казахские векторы должны быть **выведены из источников**, а не сочинены — иначе реализация становится собственным оракулом. Через WebSearch/WebFetch подтвердить каждое числительное минимум по трём источникам, из них хотя бы один — казахстанский нормативный/академический:

1. `tilalemi.kz` или `emle.kz` — «Қазақ тілінің емле ережелері», раздел о сан есім (числительное);
2. `kk.wikipedia.org` — статья «Сан есім» и/или «Қазақ тіліндегі сандар»;
3. `omniglot.com/language/numbers/kazakh.htm` — независимая сводная таблица;
4. дополнительно, для проверки формата денежной строки — любой двуязычный официальный бланк с сайта `kgd.gov.kz` или `egov.kz`, где сумма напечатана прописью.

Записать в шапку казахского сьюта комментарием: URL каждого источника, дату обращения и то, что именно им подтверждено. Три правила, которые надо проверить особенно тщательно (первая редакция спеки описывала их неверно):

- **единица опускается перед `жүз` и `мың`**: 100 → `жүз`, 1000 → `мың` (НЕ `бір жүз`, НЕ `бір мың`), но 200 → `екі жүз`, 2000 → `екі мың`;
- **перед `миллион`/`миллиард`/`триллион` единица НЕ опускается**: `бір миллион`;
- согласования по родам нет, падежных окончаний во множественном числе нет — разряд не изменяется.

Если источники расходятся с векторами Step 2 — **править вектор и записать расхождение в отчёт**, а не подгонять код. До проверки носителем казахский вывод считается предварительным; русская первичка и ФНО от этого не зависят и разблокированы независимо.

- [ ] **Step 2: Написать падающий казахский сьют**

Дописать в `tests/unit/test_amount_in_words.cpp` (в тот же анонимный namespace):

```cpp
// Источники казахских числительных — см. Step 1 задачи 2; вписать сюда
// URL и дату обращения перед коммитом.
TEST(AmountInWordsKk, GoldenVectors) {
    const Vector kVectors[] = {
        {0, "Нөл теңге 00 тиын"},
        {1, "Нөл теңге 01 тиын"},
        {5, "Нөл теңге 05 тиын"},
        {67, "Нөл теңге 67 тиын"},
        {100, "Бір теңге 00 тиын"},
        {10000, "Жүз теңге 00 тиын"},            // единица ОПУЩЕНА перед жүз
        {100000, "Мың теңге 00 тиын"},           // единица ОПУЩЕНА перед мың
        {200000, "Екі мың теңге 00 тиын"},       // но двойка — нет
        {500000, "Бес мың теңге 00 тиын"},
        {1100000, "Он бір мың теңге 00 тиын"},
        {2100000, "Жиырма бір мың теңге 00 тиын"},
        {20000000, "Екі жүз мың теңге 00 тиын"},
        {11100000, "Жүз он бір мың теңге 00 тиын"},
        {25057500, "Екі жүз елу мың бес жүз жетпіс бес теңге 00 тиын"},
        {100000000, "Бір миллион теңге 00 тиын"},  // перед миллион единица НЕ опускается
        {1000000000, "Он миллион теңге 00 тиын"},
        {200200200, "Екі миллион екі мың екі теңге 00 тиын"},
        {100000000000, "Бір миллиард теңге 00 тиын"},
    };
    for (const auto& v : kVectors)
        EXPECT_EQ(Money::to_words_kk(v.tiyn), std::string(v.expected)) << "tiyn = " << v.tiyn;
}

TEST(AmountInWordsKk, CeilingAndSignMatchRussian) {
    EXPECT_EQ(Money::to_words_kk(Money::kMaxTiyn), "Бір триллион теңге 00 тиын");
    EXPECT_THROW(Money::to_words_kk(Money::kMaxTiyn + 1), std::out_of_range);
    EXPECT_THROW(Money::to_words_kk(-1), std::invalid_argument);
}

TEST(AmountInWordsKk, CapitalizesKazakhSpecificLetters) {
    EXPECT_EQ(Money::to_words_kk(300), "Үш теңге 00 тиын");       // ү -> Ү
    EXPECT_EQ(Money::to_words_kk(4000), "Қырық теңге 00 тиын");   // қ -> Қ
}
```

- [ ] **Step 3: Расширить `capitalize_cyrillic` двумя казахскими буквами**

В `src/money/AmountInWords.hpp` заменить тело `detail::capitalize_cyrillic` на:

```cpp
inline void capitalize_cyrillic(std::string& s) {
    if (s.size() < 2)
        return;
    const auto b0 = static_cast<unsigned char>(s[0]);
    const auto b1 = static_cast<unsigned char>(s[1]);
    if (b0 == 0xD0 && b1 >= 0xB0 && b1 <= 0xBF) {
        s[1] = static_cast<char>(b1 - 0x20);
    } else if (b0 == 0xD1 && b1 >= 0x80 && b1 <= 0x8F) {
        s[0] = static_cast<char>(0xD0);
        s[1] = static_cast<char>(b1 + 0x20);
    } else if (b0 == 0xD2 && b1 == 0x9B) {
        s[1] = static_cast<char>(0x9A);  // қ -> Қ («қырық»)
    } else if (b0 == 0xD2 && b1 == 0xAF) {
        s[1] = static_cast<char>(0xAE);  // ү -> Ү («үш»)
    }
}
```

Явные ветки, а не арифметика: казахские буквы живут в планах D2/D3, где смещение регистра НЕ равно единому 0x20 русского блока, и обобщать здесь опаснее, чем перечислить те две буквы, с которых вообще может начинаться числительное.

- [ ] **Step 4: Реализовать `to_words_kk`**

Дописать в `src/money/AmountInWords.hpp` — в `namespace detail` перед закрывающей скобкой:

```cpp
inline const char* const kKkUnits[10] = {"", "бір", "екі", "үш", "төрт", "бес", "алты", "жеті", "сегіз", "тоғыз"};

inline const char* const kKkTens[10] = {"",      "он",     "жиырма", "отыз",     "қырық",
                                        "елу",   "алпыс",  "жетпіс", "сексен",   "тоқсан"};

/// Разрядные слова казахского. Индекс 0 — единицы (слова нет). `drop_one`
/// = «единица перед этим разрядом опускается»: жүз/мың — да, миллион и
/// выше — нет. Это то самое исключение, которое пропускают все реализации,
/// объявляющие казахский «существенно проще русского».
struct KkScale {
    const char* word;
    bool drop_one;
};

inline const KkScale kKkScales[5] = {
    {"", false},
    {"мың", true},
    {"миллион", false},
    {"миллиард", false},
    {"триллион", false},
};

/// Слова одной триады 0..999 по-казахски. @p drop_leading_one убирает «бір»
/// ТОЛЬКО когда триада равна ровно 1 и разряд это допускает (мың);
/// «жүз» обрабатывается внутри — 100 даёт «жүз», 200 даёт «екі жүз».
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
        // «бір» опускается только если это ВСЯ триада (100..999 c единицей
        // в конце читается «жүз бір», единица остаётся).
        const bool alone = (h == 0 && t == 0);
        if (!(alone && u == 1 && drop_leading_one))
            out.emplace_back(kKkUnits[u]);
    }
}
```

И собственно функция, рядом с `to_words_ru`:

```cpp
/**
 * @brief Казахская сумма прописью: «Екі жүз елу мың бес жүз жетпіс бес
 *        теңге 00 тиын».
 * @details Родов и падежных окончаний множественного числа нет, разряд
 *          не изменяется. Единица опускается перед «жүз» и «мың», но НЕ
 *          перед «миллион»/«миллиард»/«триллион». Ноль — «нөл».
 * @throws std::invalid_argument если @p tiyn отрицателен.
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

    long long triads[5] = {0, 0, 0, 0, 0};
    long long rest = tenge;
    for (int i = 0; i < 5; ++i) {
        triads[i] = rest % 1000;
        rest /= 1000;
    }

    std::vector<std::string> words;
    for (int i = 4; i >= 0; --i) {
        if (triads[i] == 0)
            continue;
        detail::append_triad_kk(words, triads[i], detail::kKkScales[i].drop_one);
        if (i > 0)
            words.emplace_back(detail::kKkScales[i].word);
    }
    if (words.empty())
        words.emplace_back("нөл");
    words.emplace_back("теңге");

    std::string out = detail::join_words(words);
    detail::capitalize_cyrillic(out);
    return out + detail::tiyn_tail(tiyn_part, "тиын");
}
```

- [ ] **Step 5: Поле `salary_words_kk` в двуязычном трудовом договоре**

Сегодня `templates/latex/labor_contract/v1/template.tex:40` печатает РУССКУЮ пропись в казахской половине фразы:

```
Айлық жалақы — {{ salary_tenge }} теңге ({{ salary_words }}) / Оклад — {{ salary_tenge }} тенге ({{ salary_words }}).
```

Заменить строку 40 на:

```
Айлық жалақы — {{ salary_tenge }} теңге ({{ salary_words_kk }}) / Оклад — {{ salary_tenge }} тенге ({{ salary_words }}).
```

В `templates/latex/labor_contract/v1/schema.json` добавить `"salary_words_kk"` в массив `required` (сразу после `"salary_words"`) и в `properties`:

```json
    "salary_words_kk": {"type": "string"},
```

В фикстурах дописать поле рядом с существующим `salary_words`:

- `fixtures/basic.json` — `salary_words` там `"Триста тысяч тенге 00 тиын"` (300 000 ₸ = 30000000 тиын), значит `"salary_words_kk": "Үш жүз мың теңге 00 тиын"`;
- `fixtures/special-chars.json` — `salary_words` там `"Одна тысяча тенге 00 тиын"` (1 000 ₸ = 100000 тиын), значит `"salary_words_kk": "Мың теңге 00 тиын"`.

Оба значения обязаны совпадать с тем, что выдаёт `to_words_kk` на соответствующем числе тиын — это проверяется гейтом `template-render` только на компилируемость PDF, поэтому сверьте их вручную против векторов Step 2 (`{100000, "Мың теңге 00 тиын"}` уже в наборе; 30000000 тиын = 300 000 ₸ = `үш жүз мың` — «үш жүз» потому что 3 > 1).

- [ ] **Step 6: Прогнать CI**

Ожидание: `AmountInWordsKk.*` — 3 теста PASS; гейт `template-render` (он триггерится на изменения под `templates/**`) рендерит `labor_contract` из обеих фикстур без overfull \hbox > 1.0pt и с нулевым кодом XeLaTeX.

- [ ] **Step 7: Форматирование и коммит**

```bash
clang-format-17 -i src/money/AmountInWords.hpp tests/unit/test_amount_in_words.cpp
git add src/money/AmountInWords.hpp tests/unit/test_amount_in_words.cpp templates/latex/labor_contract
git commit -m "feat(money): Kazakh amount-in-words and salary_words_kk in the labor contract"
```

В отчёт вынести: список источников из Step 1, какие векторы ими подтверждены, какие расхождения найдены, и явную пометку «казахский вывод предварителен до проверки носителем».

---

### Task 3: Целочисленные суммы в четырёх схемах шаблонов первички

**Files:**
- Modify: `templates/latex/invoice/v1/schema.json`
- Modify: `templates/latex/avr/v1/schema.json`
- Modify: `templates/latex/waybill/v1/schema.json`
- Modify: `templates/latex/tax_invoice/v1/schema.json`
- Modify: `templates/latex/invoice/v1/fixtures/basic.json`, `templates/latex/invoice/v1/fixtures/special-chars.json`
- Modify: `templates/latex/avr/v1/fixtures/basic.json`, `templates/latex/avr/v1/fixtures/special-chars.json`
- Modify: `templates/latex/waybill/v1/fixtures/basic.json`, `templates/latex/waybill/v1/fixtures/special-chars.json`
- Modify: `templates/latex/tax_invoice/v1/fixtures/basic.json`, `templates/latex/tax_invoice/v1/fixtures/special-chars.json`

**Interfaces:**
- Consumes: ничего (только JSON-файлы; C++ не трогается).
- Produces (на это опирается задача 4, критично — имена и пути должны совпасть буква в букву):
  - `invoice`, `avr`, `waybill`: новое **required** поле верхнего уровня `total_tiyn`, тип `integer`, `minimum: 0`;
  - `tax_invoice`: новое **required** поле `with_vat_tiyn` ВНУТРИ объекта `totals`, тип `integer`, `minimum: 0`;
  - `total` / `total_words` (для `tax_invoice` — `totals.with_vat` / `total_words`) **остаются** в схемах и остаются `required`: их теперь заполняет сервер перед валидацией, а не клиент. Из схемы их убирать нельзя — шаблоны на них ссылаются плейсхолдерами.

**Почему это отдельная задача.** Спека §2 первой редакции обещала «шаблоны не трогаем» — неверно. `total_words` объявлен обязательным в этих четырёх схемах, а сумма там хранится **строкой** (`"total": {"type": "string"}`), то есть целого значения на сервере не существует ни в запросе, ни в БД. Вывести пропись из строки «12 345,67» означало бы распарсить форматированную строку обратно в деньги — ровно то, что запрещает инвариант «деньги — целые тиыны». Поэтому целое поле вводится в схему, и только потом (задача 4) сервер начинает считать из него и `total`, и `total_words`.

- [ ] **Step 1: `invoice` — схема**

В `templates/latex/invoice/v1/schema.json` строку `required` заменить на:

```json
  "required": ["number", "date", "seller", "buyer", "items", "total_tiyn", "total", "total_words"],
```

и в `properties` заменить строку с `total`/`total_words` на:

```json
    "total_tiyn": {"type": "integer", "minimum": 0},
    "total": {"type": "string"}, "total_words": {"type": "string"}
```

- [ ] **Step 2: `avr` — схема**

`templates/latex/avr/v1/schema.json`, `required`:

```json
  "required": ["number", "date", "act_period", "seller", "buyer", "items", "total_tiyn", "total", "total_words"],
```

и в `properties`, перед строкой `"total": {"type": "string"},`, добавить:

```json
    "total_tiyn": {"type": "integer", "minimum": 0},
```

- [ ] **Step 3: `waybill` — схема**

`templates/latex/waybill/v1/schema.json`, `required`:

```json
  "required": ["number", "date", "seller", "buyer", "basis", "items", "total_tiyn", "total", "total_words", "released_by", "received_by"],
```

и в `properties`, перед строкой `"total": {"type": "string"},`, добавить:

```json
    "total_tiyn": {"type": "integer", "minimum": 0},
```

- [ ] **Step 4: `tax_invoice` — схема (поле вложено в `totals`)**

`templates/latex/tax_invoice/v1/schema.json`: блок `totals` заменить целиком на

```json
    "totals": {
      "type": "object",
      "required": ["with_vat_tiyn", "amount", "vat", "with_vat"],
      "properties": {
        "with_vat_tiyn": {"type": "integer", "minimum": 0},
        "amount": {"type": "string"}, "vat": {"type": "string"}, "with_vat": {"type": "string"}
      }},
```

Массив `required` верхнего уровня не меняется (`totals` в нём уже есть).

Осознанно: `totals.amount` и `totals.vat` остаются клиентскими строками. Спека §3.5 выводит на сервере ровно `with_vat` и `total_words` — «сервер считает и `total`, и `total_words`» из одного целого поля. Расширять деривацию на `amount`/`vat` — отдельное решение, в эту фазу не входит.

- [ ] **Step 5: Восемь фикстур**

В каждую дописать целое поле, арифметически равное существующей строковой сумме (тиыны = тенге × 100). Значения — точные, сверены с текущим содержимым файлов:

| файл | существующая сумма | добавляемое поле |
|---|---|---|
| `templates/latex/invoice/v1/fixtures/basic.json` | `"total": "12 345,67"` | `"total_tiyn": 1234567,` |
| `templates/latex/invoice/v1/fixtures/special-chars.json` | `"total": "1 000,00"` | `"total_tiyn": 100000,` |
| `templates/latex/avr/v1/fixtures/basic.json` | `"total": "110 200,00"` | `"total_tiyn": 11020000,` |
| `templates/latex/avr/v1/fixtures/special-chars.json` | `"total": "1 000,00"` | `"total_tiyn": 100000,` |
| `templates/latex/waybill/v1/fixtures/basic.json` | `"total": "105 000,00"` | `"total_tiyn": 10500000,` |
| `templates/latex/waybill/v1/fixtures/special-chars.json` | `"total": "1 000,00"` | `"total_tiyn": 100000,` |
| `templates/latex/tax_invoice/v1/fixtures/basic.json` | `"with_vat": "104 400,00"` | `"with_vat_tiyn": 10440000,` (внутрь `totals`) |
| `templates/latex/tax_invoice/v1/fixtures/special-chars.json` | `"with_vat": "1 160,00"` | `"with_vat_tiyn": 116000,` (внутрь `totals`) |

Для `invoice`/`avr`/`waybill` поле кладётся рядом с `"total"` на верхнем уровне. Для `tax_invoice` — первым ключом объекта `totals`, например:

```json
  "totals": {
    "with_vat_tiyn": 10440000,
    "amount": "90 000,00",
    "vat": "14 400,00",
    "with_vat": "104 400,00"
  },
```

Проверьте согласованность прописи в фикстурах с новым целым: `invoice/basic` 1234567 тиын → `Money::to_words_ru` даёт «Двенадцать тысяч триста сорок пять тенге 67 тиын» — ровно то, что уже лежит в `total_words`. То же самое сверьте для остальных семи; если строка не совпадает с тем, что даст конвертер задачи 1, поправьте **прописную строку фикстуры**, а не целое.

- [ ] **Step 6: Проверить JSON и прогнать `template-render`**

Локально:

```bash
for f in templates/latex/{invoice,avr,waybill,tax_invoice}/v1/schema.json \
         templates/latex/{invoice,avr,waybill,tax_invoice}/v1/fixtures/*.json; do
  python3 -m json.tool "$f" > /dev/null || echo "BROKEN: $f"
done
```

В CI: гейт `template-render` (триггерится изменениями под `templates/**`) рендерит каждый шаблон из обеих фикстур на worker-образе. Ожидание: четыре шаблона × две фикстуры компилируются, ни одного overfull `\hbox` больше 1.0pt, нулевой код XeLaTeX. Новое поле в шаблон не выводится, поэтому вёрстка меняться не должна — любой overfull здесь означает, что что-то поехало в JSON, а не в LaTeX.

- [ ] **Step 7: Коммит**

```bash
git add templates/latex/invoice templates/latex/avr templates/latex/waybill templates/latex/tax_invoice
git commit -m "feat(templates): integer tiyn totals in invoice, avr, waybill and tax_invoice schemas"
```

---

### Task 4: Сервер выводит прописи; allowlist'ы чистятся; `POST /documents/generate` перестаёт быть дырой и перестаёт отдавать 500

**Files:**
- Create: `src/money/MoneyFormat.hpp`
- Create: `src/docgen/InputPolicy.hpp`
- Create: `tests/unit/test_money_format.cpp`
- Create: `tests/unit/test_input_policy.cpp`
- Modify: `src/api/TaxController.hpp` (allowlist'ы на строках 239-259, `build_form_input` на 952+)
- Modify: `src/api/PayrollController.hpp` (allowlist на 167-169, построение `input` на 445-466)
- Modify: `src/api/HrController.hpp` (allowlist'ы на 655-673, построение `input` трудового договора)
- Modify: `src/api/DocgenController.hpp` (`generate`, строки 140-266)
- Modify: `docs/openapi.yaml` (`GenerateDocumentCreate`, ~строка 586)
- Test: `tests/integration/test_tax_api.cpp`, `tests/integration/test_payroll_api.cpp`, `tests/integration/test_hr_api.cpp`, `tests/integration/test_docgen_api.cpp`

**Interfaces:**
- Consumes (задачи 1-3): `Money::to_words_ru(long long)`, `Money::to_words_kk(long long)`, `Money::kMaxTiyn`; поля схем `total_tiyn` и `totals.with_vat_tiyn`; поле `salary_words_kk` в схеме `labor_contract`.
- Produces (на это опираются задачи 9 и 13):
  - `std::string Money::format_tiyn_ru(long long tiyn)` — «12 345,67» (пробел-разделитель тысяч U+0020, запятая-разделитель дробной части), бросает `std::invalid_argument` на отрицательном;
  - `const std::vector<std::string>& Docgen::InputPolicy::generate_slugs()`;
  - `bool Docgen::InputPolicy::input_is_caller_authored(const std::string& slug)`;
  - `struct Docgen::InputPolicy::DerivedAmount { std::string tiyn_path, amount_path, words_path; };`
  - `std::optional<Docgen::InputPolicy::DerivedAmount> Docgen::InputPolicy::derived_amount_for(const std::string& slug)`;
  - `const std::vector<std::string>& Docgen::InputPolicy::editable_fields(const std::string& slug)`;
  - `const nlohmann::json* Docgen::InputPolicy::at_path(const nlohmann::json& obj, const std::string& path)`;
  - `void Docgen::InputPolicy::set_path(nlohmann::json& obj, const std::string& path, nlohmann::json value)`;
  - `bool Docgen::InputPolicy::apply_derived_amount(const std::string& slug, nlohmann::json& input, std::string& error_field, std::string& error_code, std::string& error_message)`.

**Что именно здесь чинится.** Три разных механизма:

1. **Серверные строители форм** (`fno_910`, `fno_300`, `payslip`, трудовой договор) — у них allowlist уже есть, надо убрать из него `*_words` и начать выводить значение. Клиентская присылка `*_words` после этого автоматически становится 422 `not_allowed_override` существующим механизмом `Api::Validation::merge_allowed_extra`.
2. **`POST /documents/generate`** — allowlist'а нет вообще, весь `input` приходит от клиента. Здесь вводится деривация: сервер требует целое поле, сам считает сумму строкой и пропись, а клиентские `total`/`total_words` отвергает.
3. **Дефект релиза v0.3.1:** тот же `POST /documents/generate` отдаёт **500** для слагов `payslip`, `fno_910`, `fno_300`, `hr_order`, `labor_contract`. OpenAPI ограничивает `template_slug` пятью слагами первички, но код это не проверяет — слаг проходит насквозь как `doc_type` (`DocgenController.hpp`: «doc_type == slug — see file header») и валится на `documents_doc_type_check` из `migrations/010_documents.sql`. Транзакция откатывается, строк-сирот не остаётся, но клиент получает 500 вместо внятного 422.

- [ ] **Step 1: Падающий unit-тест форматтера**

`tests/unit/test_money_format.cpp`:

```cpp
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
```

- [ ] **Step 2: Реализовать форматтер**

`src/money/MoneyFormat.hpp`:

```cpp
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
```

- [ ] **Step 3: Падающий unit-тест политики входа**

`tests/unit/test_input_policy.cpp`:

```cpp
/**
 * @file test_input_policy.cpp
 * @brief Docgen::InputPolicy — единая таблица «что клиенту можно прислать»
 *        и «что сервер выводит сам». Чистый модуль, БД не нужна.
 */

#include <optional>
#include <string>

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

    auto ti = Docgen::InputPolicy::derived_amount_for("tax_invoice");
    ASSERT_TRUE(ti);
    EXPECT_EQ(ti->tiyn_path, "totals.with_vat_tiyn");
    EXPECT_EQ(ti->amount_path, "totals.with_vat");
    EXPECT_EQ(ti->words_path, "total_words");

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
              (std::vector<std::string>{"work_schedule",
                                        "probation_months",
                                        "employer.director",
                                        "employer.address",
                                        "employee.address"}));
    EXPECT_TRUE(Docgen::InputPolicy::editable_fields("no_such_slug").empty());
}

TEST(InputPolicy, ApplyDerivedAmountFillsBothStrings) {
    json input = {{"number", "1"}, {"total_tiyn", 1234567}};
    std::string field, code, message;
    ASSERT_TRUE(Docgen::InputPolicy::apply_derived_amount("invoice", input, field, code, message)) << message;
    EXPECT_EQ(input["total"].get<std::string>(), "12 345,67");
    EXPECT_EQ(input["total_words"].get<std::string>(), "Двенадцать тысяч триста сорок пять тенге 67 тиын");
}

TEST(InputPolicy, ApplyDerivedAmountFillsNestedPathForTaxInvoice) {
    json input = {{"totals", {{"with_vat_tiyn", 10440000}, {"amount", "90 000,00"}, {"vat", "14 400,00"}}}};
    std::string field, code, message;
    ASSERT_TRUE(Docgen::InputPolicy::apply_derived_amount("tax_invoice", input, field, code, message)) << message;
    EXPECT_EQ(input["totals"]["with_vat"].get<std::string>(), "104 400,00");
    EXPECT_EQ(input["total_words"].get<std::string>(), "Сто четыре тысячи четыреста тенге 00 тиын");
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
```

Тест использует `std::find` — не забудьте `#include <algorithm>` и `#include <vector>`.

- [ ] **Step 4: Реализовать `src/docgen/InputPolicy.hpp`**

```cpp
/**
 * @file InputPolicy.hpp
 * @brief Одна таблица на всю систему: что клиент вправе прислать в `input`
 *        документа и какие денежные поля сервер выводит сам.
 * @details До P3 эти правила были размазаны по четырём контроллерам как
 *          приватные функции *_allowed_extra_fields(), а у
 *          POST /documents/generate их не было вовсе. Собраны сюда, потому
 *          что правку документа (задача 9) обязан пропускать ТОТ ЖЕ
 *          allowlist, что и создание: input_snapshot — ровно то, что
 *          рендерит джоба, и приём его целиком заново открывает дыру
 *          подделки, закрытую в P2 (PDF декларации с балансом 1 ₸ при
 *          правдивом XML той же записи).
 *
 *          Точка внедрения прописей: сервер кладёт их в JSON `input` ДО
 *          TemplateRegistry::validate(); шаблон остаётся с плейсхолдером
 *          {{ total_words }}. Считать пропись внутри .tex или переносить
 *          поле из схемы в шаблон запрещено — миграция на Typst будет это
 *          разбирать обратно.
 */

#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "money/AmountInWords.hpp"
#include "money/MoneyFormat.hpp"

namespace Docgen::InputPolicy {

using json = nlohmann::json;

/// Слаги, которые POST /api/v1/documents/generate имеет право создавать.
/// Ровно те значения, которые migrations/010_documents.sql перечисляет в
/// CHECK на doc_type для первички — слаг идёт в doc_type дословно, поэтому
/// любой другой слаг нарушает documents_doc_type_check и до P3 давал 500.
inline const std::vector<std::string>& generate_slugs() {
    static const std::vector<std::string> kSlugs = {"invoice", "avr", "waybill", "tax_invoice", "reconciliation"};
    return kSlugs;
}

/// true — весь `input` авторский (первичка: за ней в БД ничего не стоит).
/// false — форму строит контроллер из авторитетных строк, и editable_fields()
/// исчерпывающе перечисляет, что клиенту дозволено добавить.
inline bool input_is_caller_authored(const std::string& slug) {
    const auto& s = generate_slugs();
    return std::find(s.begin(), s.end(), slug) != s.end();
}

/// Денежное поле, которое сервер выводит из целого числа тиын. Пути
/// точечные и не глубже одного уровня вложенности.
struct DerivedAmount {
    std::string tiyn_path;    ///< целое, которое ОБЯЗАН прислать клиент
    std::string amount_path;  ///< сюда сервер пишет Money::format_tiyn_ru
    std::string words_path;   ///< сюда сервер пишет Money::to_words_ru
};

inline std::optional<DerivedAmount> derived_amount_for(const std::string& slug) {
    if (slug == "invoice" || slug == "avr" || slug == "waybill")
        return DerivedAmount{"total_tiyn", "total", "total_words"};
    if (slug == "tax_invoice")
        return DerivedAmount{"totals.with_vat_tiyn", "totals.with_vat", "total_words"};
    return std::nullopt;  // reconciliation и все серверные формы
}

/// Единственные ключи, которые каллер вправе прислать для серверно
/// строящейся формы. Пустой вектор = «ни одного». Точечный путь адресует
/// один лист ("employer.director" разрешает только его, но не соседние
/// authoritative employer.name/employer.bin).
inline const std::vector<std::string>& editable_fields(const std::string& slug) {
    static const std::vector<std::string> kNone = {};
    static const std::vector<std::string> kFnoSignatories = {"director", "accountant"};
    static const std::vector<std::string> kHrOrder = {"director", "reason", "details"};
    static const std::vector<std::string> kLaborContract = {
        "work_schedule", "probation_months", "employer.director", "employer.address", "employee.address"};
    if (slug == "fno_910" || slug == "fno_300")
        return kFnoSignatories;
    if (slug == "hr_order")
        return kHrOrder;
    if (slug == "labor_contract")
        return kLaborContract;
    return kNone;  // payslip, вся первичка и любой неизвестный слаг
}

/// Прочитать точечный путь. nullptr, если любой сегмент отсутствует или
/// промежуточный узел не объект.
inline const json* at_path(const json& obj, const std::string& path) {
    const json* node = &obj;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!node->is_object() || !node->contains(key))
            return nullptr;
        node = &(*node)[key];
        if (dot == std::string::npos)
            return node;
        start = dot + 1;
    }
    return nullptr;
}

/// Записать по точечному пути, создавая промежуточные объекты.
inline void set_path(json& obj, const std::string& path, json value) {
    const std::size_t dot = path.find('.');
    if (dot == std::string::npos) {
        obj[path] = std::move(value);
        return;
    }
    const std::string head = path.substr(0, dot);
    if (!obj.contains(head) || !obj[head].is_object())
        obj[head] = json::object();
    set_path(obj[head], path.substr(dot + 1), std::move(value));
}

/**
 * @brief Проверить целое поле суммы и записать в @p input строковую сумму
 *        и пропись. Для слага без деривации — no-op с результатом true.
 * @return false + заполненные @p error_field / @p error_code /
 *         @p error_message, если каллер прислал серверно-выводимое поле
 *         либо целое отсутствует/не целое/вне диапазона. Каллер обязан
 *         превратить это в 422.
 */
inline bool apply_derived_amount(const std::string& slug,
                                 json& input,
                                 std::string& error_field,
                                 std::string& error_code,
                                 std::string& error_message) {
    auto derived = derived_amount_for(slug);
    if (!derived)
        return true;

    for (const auto* path : {derived->amount_path.c_str(), derived->words_path.c_str()}) {
        if (at_path(input, path) != nullptr) {
            error_field = std::string("input.") + path;
            error_code = "not_allowed_override";
            error_message = std::string("'") + path + "' is derived by the server from '" + derived->tiyn_path +
                            "' and may not be supplied by the client";
            return false;
        }
    }

    const json* tiyn_node = at_path(input, derived->tiyn_path);
    if (tiyn_node == nullptr) {
        error_field = "input." + derived->tiyn_path;
        error_code = "missing";
        error_message = "'" + derived->tiyn_path + "' is required — the total is carried as an integer number of tiyn";
        return false;
    }
    if (!tiyn_node->is_number_integer()) {
        error_field = "input." + derived->tiyn_path;
        error_code = "not_integer";
        error_message = "'" + derived->tiyn_path + "' must be an integer number of tiyn";
        return false;
    }
    const long long tiyn = tiyn_node->get<long long>();
    if (tiyn < 0 || tiyn > Money::kMaxTiyn) {
        error_field = "input." + derived->tiyn_path;
        error_code = "out_of_range";
        error_message = "'" + derived->tiyn_path + "' must be between 0 and " + std::to_string(Money::kMaxTiyn) +
                        " tiyn inclusive";
        return false;
    }

    set_path(input, derived->amount_path, json(Money::format_tiyn_ru(tiyn)));
    set_path(input, derived->words_path, json(Money::to_words_ru(tiyn)));
    return true;
}

}  // namespace Docgen::InputPolicy
```

Осторожно с двумя ловушками: (а) `for (const auto* path : {a.c_str(), b.c_str()})` — именно `const auto*`, привязка `const std::string&` к списку C-строк создаёт временные; (б) `json(Money::format_tiyn_ru(tiyn))` завёрнуто явно, а не передано как `std::string` — чтобы не ловить неоднозначность конверсий nlohmann.

- [ ] **Step 5: `DocgenController::generate` — слаг-гейт и деривация**

В `src/api/DocgenController.hpp`:

1. К списку `#include` добавить `#include "docgen/InputPolicy.hpp"`.
2. В `generate()` заменить блок разрешения шаблона (сейчас: `registry.latest(template_slug)` → `if (!info) 422 unknown_template` → `TemplateRegistry::validate`) на:

```cpp
        // Слаг обязан быть из списка первички ДО обращения к реестру: он
        // идёт в documents.doc_type дословно (см. шапку файла), а
        // migrations/010_documents.sql разрешает там только эти пять
        // значений. Без этой проверки существующий на диске, но не
        // первичный шаблон (payslip, fno_910, fno_300, hr_order,
        // labor_contract) проходил дальше и валился на
        // documents_doc_type_check уже внутри INSERT — клиент получал 500
        // вместо внятного 422 (дефект, найденный при релизе v0.3.1).
        if (!Docgen::InputPolicy::input_is_caller_authored(template_slug)) {
            callback(Validation::response_422("template_slug",
                                              "unsupported_template",
                                              "template '" + template_slug +
                                                  "' is not generated through this endpoint — use the endpoint that "
                                                  "owns it (tax filings, payroll payslips or HR documents)"));
            return;
        }

        Docgen::TemplateRegistry registry;
        auto info = registry.latest(template_slug);
        if (!info) {
            callback(Validation::response_422(
                "template_slug", "unknown_template", "no template found for slug '" + template_slug + "'"));
            return;
        }

        // Прописи и строковая сумма выводятся сервером из целого числа
        // тиын и подставляются в `input` ДО schema-валидации: у этих
        // шаблонов allowlist'а нет, весь объект приходит от клиента, и без
        // деривации цифра и текст в одном документе могли разойтись.
        json input = client_input;
        {
            std::string bad_field, bad_code, bad_message;
            if (!Docgen::InputPolicy::apply_derived_amount(
                    template_slug, input, bad_field, bad_code, bad_message)) {
                callback(Validation::response_422(bad_field, bad_code, bad_message));
                return;
            }
        }

        if (auto err = Docgen::TemplateRegistry::validate(*info, input)) {
            callback(Validation::response_422("input", "schema_validation_failed", *err));
            return;
        }
```

3. Выше по функции переименовать существующее `const json input = body.value("input", json::object());` в `const json client_input = body.value("input", json::object());` — теперь `input` не const и мутируется деривацией.
4. Ниже по функции `documents.create(...)` и полезная нагрузка джобы уже используют `input` — они получат обогащённый объект, как и должно быть: `input_snapshot` и то, что рендерит джоба, обязаны совпадать.

- [ ] **Step 6: `TaxController` — allowlist'ы и выведение `tax_words` / `balance_words`**

В `src/api/TaxController.hpp`:

1. `#include "money/AmountInWords.hpp"` к списку include.
2. `fno_910_allowed_extra_fields()` (строка ~239) — тело заменить на

```cpp
    static const std::vector<std::string>& fno_910_allowed_extra_fields() {
        // `tax_words` убран (P3 §3.5): сумма прописью однозначно выводится
        // из calc.total_tiyn, поэтому она серверная, а присланная клиентом
        // теперь получает 422 not_allowed_override — тем же механизмом,
        // которым здесь уже защищены income_tenge/rate_percent/tax_tenge.
        static const std::vector<std::string> kAllowed = {"director", "accountant"};
        return kAllowed;
    }
```

3. `fno_300_allowed_extra_fields()` (строка ~257) — так же, `{"director", "accountant"}` (убрать `balance_words`).
4. В `build_form_input()`, в ветке `kFno910`, после `input["tax_tenge"] = Ledger::format_tiyn(calc.total_tiyn);` добавить:

```cpp
            input["tax_words"] = Money::to_words_ru(calc.total_tiyn);
```

5. В ветке ФНО 300, после `input["balance_kind"] = ...`, добавить:

```cpp
        // Модуль, как и balance_tenge строкой выше: знак несёт balance_kind,
        // а to_words_ru принимает только неотрицательное. Без этого первая
        // же декларация с НДС к возврату уронила бы рендер-джобу
        // необработанным std::invalid_argument.
        input["balance_words"] = Money::to_words_ru(balance_tiyn < 0 ? -balance_tiyn : balance_tiyn);
```

`calc.total_tiyn` и `balance_tiyn` — `long long` и уже не превышают `Money::kMaxTiyn` по построению расчёта; если `to_words_ru` всё же бросит `std::out_of_range`, это поймает существующий `with_repo_errors`/`try` вокруг `build_form_xml` — нет, не поймает: `build_form_input` вызывается раньше. Оберните оба вызова в общий `try { ... } catch (const std::exception& e) { missing_key = "amount_out_of_range"; return std::nullopt; }` внутри `build_form_input`, чтобы каллер отдал существующий 422 `incomplete_calculation` вместо 500.

- [ ] **Step 7: `PayrollController` — `net_words` выводится**

В `src/api/PayrollController.hpp`:

1. `#include "money/AmountInWords.hpp"`.
2. `payslip_allowed_extra_fields()` (строка ~167) заменить на

```cpp
    /// Пустой: после P3 у расчётного листка не осталось ни одного поля,
    /// которое клиент вправе прислать — `net_words` выводится сервером из
    /// payslip.net. Любой ключ в теле запроса теперь 422
    /// not_allowed_override (Api::Validation::merge_allowed_extra).
    static const std::vector<std::string>& payslip_allowed_extra_fields() {
        static const std::vector<std::string> kAllowed = {};
        return kAllowed;
    }
```

3. В построении `input` (строка ~445) после `{"social_tax", Ledger::format_tiyn(payslip->social_tax)},` добавить внутрь инициализатора:

```cpp
            {"net_words", Money::to_words_ru(payslip->net)},
```

Обновить комментарий выше `merge_allowed_extra` (сейчас он говорит «`net_words` … is the only key the caller may supply») на «после P3 каллер не вправе прислать ни одного ключа».

- [ ] **Step 8: `HrController` — `salary_words` и `salary_words_kk` выводятся**

В `src/api/HrController.hpp`:

1. `#include "money/AmountInWords.hpp"`.
2. `labor_contract_allowed_extra_fields()` (строка ~665) — убрать `"salary_words"`, оставить:

```cpp
        static const std::vector<std::string> kAllowed = {"work_schedule",
                                                          "probation_months",
                                                          "employer.director",
                                                          "employer.address",
                                                          "employee.address"};
```

3. В построении `input` трудового договора (там, где формируется `salary_tenge` из `employee.salary_tiyn`) добавить два поля:

```cpp
        input["salary_words"] = Money::to_words_ru(employee->salary_tiyn);
        input["salary_words_kk"] = Money::to_words_kk(employee->salary_tiyn);
```

`salary_words_kk` — новое обязательное поле схемы `labor_contract` (задача 2). Без него `TemplateRegistry::validate` отдаст 422 на каждом трудовом договоре.

4. `hr_order_allowed_extra_fields()` не трогать — прописей в приказе нет.

- [ ] **Step 9: OpenAPI — `template_slug` и новые 422**

В `docs/openapi.yaml`, схема `GenerateDocumentCreate` (~строка 586): enum уже перечисляет пять слагов первички — оставить как есть, но в `description` дописать, что несоответствие теперь 422 `unsupported_template`, а не 500. В `input` дописать:

```yaml
        input:            { type: object, additionalProperties: true, description: 'Validated against the template''s JSON Schema. The total is supplied as an INTEGER number of tiyn (total_tiyn; totals.with_vat_tiyn for tax_invoice) — the server derives the formatted amount and the amount in words from it, so total/total_words/totals.with_vat in the request are a 422 not_allowed_override' }
```

В блоке `/api/v1/documents/generate` в `responses` убедиться, что `'422'` описан и добавить в его `description`: `unsupported_template / not_allowed_override / missing / not_integer / out_of_range on the derived total`.

- [ ] **Step 10: Падающие интеграционные тесты**

`tests/integration/test_docgen_api.cpp` — дописать (фикстура и хелперы `member(...)`, `authed_json(...)`, `body_of(...)` уже есть в файле):

```cpp
TEST_F(DocgenApiTest, GenerateDerivesTotalAndWordsFromTiyn) {
    auto p = member("derive@example.com", org_.id, "accountant");
    json input = {{"number", "1"},
                  {"date", "14.08.2026"},
                  {"seller", {{"name", "ТОО Тест"}, {"identifier", "104332181962"}}},
                  {"buyer", {{"name", "ИП Тест"}, {"identifier", "001338908381"}}},
                  {"items", json::array({{{"name", "Услуга"}, {"qty", "1"}, {"unit", "усл"}, {"price", "12 345,67"},
                                          {"amount", "12 345,67"}}})},
                  {"total_tiyn", 1234567}};
    const json body = {{"template_slug", "invoice"}, {"input", input}};
    auto resp = call(ctrl_generate, authed_json(Post, body, p));
    ASSERT_EQ(resp->getStatusCode(), k202Accepted);
    const std::string doc_id = body_of(resp)["document_id"].get<std::string>();

    Ledger::DocumentRepository docs;
    auto doc = docs.find_in_org(doc_id, org_.id);
    ASSERT_TRUE(doc);
    ASSERT_TRUE(doc->input_snapshot);
    EXPECT_EQ((*doc->input_snapshot)["total"].get<std::string>(), "12 345,67");
    EXPECT_EQ((*doc->input_snapshot)["total_words"].get<std::string>(),
              "Двенадцать тысяч триста сорок пять тенге 67 тиын");
}

TEST_F(DocgenApiTest, GenerateRejectsClientSuppliedTotalWords) {
    auto p = member("words@example.com", org_.id, "accountant");
    const json body = {{"template_slug", "invoice"},
                       {"input", {{"total_tiyn", 1234567}, {"total_words", "Один тенге 00 тиын"}}}};
    auto resp = call(ctrl_generate, authed_json(Post, body, p));
    EXPECT_EQ(resp->getStatusCode(), k422UnprocessableEntity);
    EXPECT_EQ(body_of(resp)["errors"][0]["code"].get<std::string>(), "not_allowed_override");
}

TEST_F(DocgenApiTest, GenerateRejectsNonPrimarySlugWith422NotFiveHundred) {
    auto p = member("slug@example.com", org_.id, "accountant");
    for (const auto* slug : {"payslip", "fno_910", "fno_300", "hr_order", "labor_contract"}) {
        const json body = {{"template_slug", slug}, {"input", json::object()}};
        auto resp = call(ctrl_generate, authed_json(Post, body, p));
        EXPECT_EQ(resp->getStatusCode(), k422UnprocessableEntity) << slug;
        EXPECT_EQ(body_of(resp)["errors"][0]["code"].get<std::string>(), "unsupported_template") << slug;
    }
}
```

Имена хелперов (`call`, `ctrl_generate`, `authed_json`, `body_of`, `org_`) подогнать под то, что реально объявлено в этом файле — их формы разные в разных сьютах; смотрите на соседние тесты в том же файле и повторяйте их дословно.

В `tests/integration/test_tax_api.cpp` добавить: попытка прислать `{"document_input": {"tax_words": "..."}}` при создании ФНО 910 → 422 `not_allowed_override`; успешный филинг → в `documents.input_snapshot` поле `tax_words` равно `Money::to_words_ru(calc.total_tiyn)`. То же для 300 с `balance_words`, отдельно — случай отрицательного сальдо (НДС к возврату): `balance_kind == "to_refund"` и `balance_words` — пропись **модуля**, без минуса и без исключения.

В `tests/integration/test_payroll_api.cpp`: тело с `{"net_words": "..."}` → 422 `not_allowed_override`; пустое тело → 202 и `input_snapshot.net_words` совпадает с `Money::to_words_ru(payslip.net)`.

В `tests/integration/test_hr_api.cpp`: тело с `{"salary_words": "..."}` → 422; успешная генерация → `input_snapshot.salary_words` и `input_snapshot.salary_words_kk` заполнены и различаются.

- [ ] **Step 11: Гейты и коммит**

```bash
clang-format-17 -i src/money/MoneyFormat.hpp src/docgen/InputPolicy.hpp \
  src/api/DocgenController.hpp src/api/TaxController.hpp src/api/PayrollController.hpp src/api/HrController.hpp \
  tests/unit/test_money_format.cpp tests/unit/test_input_policy.cpp
./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh && ./scripts/check-test-buckets.sh
make lint-openapi
git add src/money/MoneyFormat.hpp src/docgen/InputPolicy.hpp src/api/DocgenController.hpp \
  src/api/TaxController.hpp src/api/PayrollController.hpp src/api/HrController.hpp docs/openapi.yaml \
  tests/unit/test_money_format.cpp tests/unit/test_input_policy.cpp tests/integration/test_docgen_api.cpp \
  tests/integration/test_tax_api.cpp tests/integration/test_payroll_api.cpp tests/integration/test_hr_api.cpp
git commit -m "feat(docgen): derive amounts in words server-side and gate template slugs"
```

**Известное промежуточное состояние:** после этой задачи SPA ещё шлёт `total_words`/`net_words`/`tax_words` и не шлёт `total_tiyn` — то есть генерация документов из интерфейса сломана до задачи 5. Это нормально внутри ветки: релиз происходит только в задаче 15.

---

### Task 5: SPA — убрать все поля ввода прописи и ставшие ложными подсказки

**Files:**
- Modify: `frontend/src/pages/GenerateDocument.tsx` (строки 371, 406, 582-585, 628, 812-815, 839, 869, 1025-1028, 1092, 1120, 1305-1308)
- Modify: `frontend/src/lib/schemas/documents.ts` (строки 100, 116, 129)
- Modify: `frontend/src/pages/Taxes.tsx` (строки 830, 834, 883-887, 916-920)
- Modify: `frontend/src/lib/schemas/tax.ts` (строки 37-38, 272-281, 291, 299)
- Modify: `frontend/src/lib/schemas/tax.test.ts` (строки 152-230)
- Modify: `frontend/src/pages/Payroll.tsx` (строки 519, 638, 659, 666-673)
- Modify: `frontend/src/lib/schemas/payroll.ts` (строки 29-30, 171-182)
- Modify: `frontend/src/lib/schemas/payroll.test.ts` (строки 163-166)
- Modify: `frontend/src/pages/HrOrders.tsx` (строки 100, 949, 972-976)
- Modify: `frontend/src/lib/schemas/hr.ts` (строки 30, 351-378)
- Modify: `frontend/src/lib/schemas/hr.test.ts` (строки 225, 235)
- Modify: `frontend/src/lib/api/types.ts` (строка 59)
- Modify: `frontend/src/lib/api/schema.gen.ts` (регенерация)

**Interfaces:**
- Consumes (задачи 3-4): в `input` для `invoice`/`avr`/`waybill` уходит целое `total_tiyn`, для `tax_invoice` — `totals.with_vat_tiyn`; поля `total`, `total_words`, `totals.with_vat` присылать НЕЛЬЗЯ (422 `not_allowed_override`); тела `generate-document` для ФНО отдают только `{director, accountant}`, для расчётного листка — пустой объект, для трудового договора — без `salary_words`.
- Produces: ничего для C++; следующие фронтовые задачи (13, 14) работают поверх этого состояния.

**Список мест полон.** Критика спеки называла пять файлов; на деле их тринадцать — она пропустила `Payroll.tsx` (форма `net_words`), `lib/schemas/documents.ts` (три `total_words`), `lib/schemas/tax.ts` + `tax.test.ts`, `lib/schemas/hr.ts` + `hr.test.ts` и `lib/api/types.ts`. Пропуск любого = 422 в проде на первом же документе.

- [ ] **Step 1: `documents.ts` — убрать `total_words` из трёх zod-схем**

В `frontend/src/lib/schemas/documents.ts` удалить строку

```ts
  total_words: z.string().trim().min(1, 'Обязательное поле'),
```

из `invoiceFormSchema` (строка 100), `waybillFormSchema` (116) и `taxInvoiceFormSchema` (129). `avrFormSchema` наследуется от `invoiceFormSchema` через `.extend`, отдельной правки не требует. Ничего взамен не добавлять: `total_tiyn` не поле формы — оно вычисляется из строк позиций при отправке.

- [ ] **Step 2: `GenerateDocument.tsx` — четыре формы**

В каждом из четырёх строителей input:

- `buildInvoiceInput` (строка ~358): заменить `total_words: values.total_words.trim(),` на `total_tiyn: subtotalTiyn + vatTiyn,` — целое уже посчитано двумя строками выше и уже используется для `total`. Саму строку `total: formatTiynRu(subtotalTiyn + vatTiyn),` **удалить**: её теперь считает сервер, а присланная даст 422.
- `buildAvrInput` (строка ~839): то же самое.
- `buildWaybillInput` (строка ~1092): то же самое.
- `buildTaxInvoiceInput` (строка ~1092+): в объекте `totals` заменить `with_vat: ...` на `with_vat_tiyn: <целое с НДС>`; `amount` и `vat` оставить строками (сервер их не выводит); `total_words` удалить.

В `defaultValues` четырёх форм (строки 406, 628, 869, 1120) удалить `total_words: '',`.

Удалить четыре блока поля ввода целиком — `Field` с `id="inv-total-words"` (582-585), `id="avr-total-words"` (812-815), `id="wb-total-words"` (1025-1028), `id="ti-total-words"` (1305-1308), вместе с их `label="Сумма прописью"`. Если рядом есть поясняющий текст вида «укажите сумму прописью» — он тоже удаляется: подсказка стала ложной.

- [ ] **Step 3: `tax.ts` + `Taxes.tsx` + `tax.test.ts`**

`frontend/src/lib/schemas/tax.ts`:

```ts
/** The 910.00 allowlist: two signatories. The tax amount in words is derived
 *  server-side from the calculation's integer total (P3 §3.5). */
export const fno910DocumentSchema = z.object({
  director: z.string().trim().min(1, 'Укажите руководителя'),
  accountant: z.string().trim().min(1, 'Укажите бухгалтера'),
});

/** The 300.00 allowlist: the same shape. */
export const fno300DocumentSchema = z.object({
  director: z.string().trim().min(1, 'Укажите руководителя'),
  accountant: z.string().trim().min(1, 'Укажите бухгалтера'),
});
```

(точные имена полей и текст сообщений подгоните под уже существующие в файле — меняется только удаление `tax_words`/`balance_words`). В строителях (строки 291, 299) удалить `tax_words: values.tax_words.trim(),` и `balance_words: values.balance_words.trim(),`. Шапку файла (строки 37-38) переписать: «Those TWO per form are the whole allowlist — the amount in words is server-derived».

`frontend/src/pages/Taxes.tsx`: в `defaultValues` (830, 834) убрать `tax_words: ''` и `balance_words: ''`; удалить оба блока `Field` — `id="fno910-tax-words"` (883-887) и `id="fno300-balance-words"` (916-920).

`frontend/src/lib/schemas/tax.test.ts`: убрать `tax_words`/`balance_words` из входов и ожиданий (152-177, 190), заменить `expect(Object.keys(input).sort()).toEqual(['accountant', 'director', 'tax_words'])` на `.toEqual(['accountant', 'director'])` (то же для 300.00), а тест «require the signatories and the amount in words» (200) переименовать в «require both signatories» и убрать из него два кейса про `*_words`; список ключей на 226/230 сократить.

- [ ] **Step 4: `payroll.ts` + `Payroll.tsx` + `payroll.test.ts` + `types.ts`**

`frontend/src/lib/schemas/payroll.ts`: удалить `payslipDocumentSchema` вместе с полем `net_words` (171-182) и функцию `buildPayslipDocumentExtra`, либо — если она вызывается из страницы — оставить её возвращающей пустой объект:

```ts
/**
 * После P3 у расчётного листка не осталось ни одного каллер-поля: сумма к
 * выплате прописью выводится сервером из payslip.net. Тело запроса —
 * пустой объект; любой присланный ключ backend отвергает 422
 * not_allowed_override.
 */
export function buildPayslipDocumentExtra(): Record<string, never> {
  return {};
}
```

`frontend/src/pages/Payroll.tsx`: удалить всю форму `net_words` (659-673) и её компонент-обёртку; кнопка «Расчётный листок» теперь сразу шлёт мутацию с пустым телом. Удалить подсказку на 666 («… Укажите только сумму к выплате прописью: <Money …>») — она стала ложной; если карточке нужен поясняющий текст, оставьте одну строку вида «Все суммы берутся из расчёта, вводить ничего не нужно.». Комментарий на 519 про «422 here is the template's own schema check over `net_words`» переписать под новую причину. Комментарий на 638 удалить.

`frontend/src/lib/schemas/payroll.test.ts` (163-166): тест заменить на

```ts
  it('sends an empty body — every payslip field is derived server-side', () => {
    expect(buildPayslipDocumentExtra()).toEqual({});
  });
```

`frontend/src/lib/api/types.ts` (59): комментарий `Body of …/payslips/{employee_id}/generate-document — net_words and nothing else.` и сам тип заменить на пустой объект-тип с комментарием «тело пустое, все поля выводятся сервером».

- [ ] **Step 5: `hr.ts` + `HrOrders.tsx` + `hr.test.ts`**

`frontend/src/lib/schemas/hr.ts`: из схемы трудового договора (351-378) удалить `salary_words: z.string()...` (360) и `salary_words: values.salary_words.trim(),` (378). Шапку (строка 30) поправить: перечисление каллер-полей теперь `director, work_schedule, addresses` без `salary_words`.

`frontend/src/pages/HrOrders.tsx`: в `defaultValues` (949) убрать `salary_words: ''`; удалить блок `Field` `id="contract-doc-salary-words"` (972-976) с меткой «Оклад прописью». Комментарий на 100 («оклад прописью, режим работы, адреса») сократить до «режим работы, адреса».

`frontend/src/lib/schemas/hr.test.ts`: убрать `salary_words: 'триста тысяч тенге',` (225) из входа и поправить ожидание ключей (235) на `['employer', 'work_schedule']`.

- [ ] **Step 6: Регенерировать клиент и прогнать фронтовые гейты**

```bash
cd frontend
npm run codegen         # перечитывает docs/openapi.yaml в src/lib/api/schema.gen.ts
npx tsc --noEmit
npm run lint
npm test                # vitest: schemas/*.test.ts
```

Ожидание: `tsc` чист (любая оставшаяся ссылка на `total_words`/`net_words`/`tax_words`/`balance_words`/`salary_words` вылезет здесь как ошибка типа), eslint чист, vitest зелёный.

Финальная проверка полноты — этот grep обязан не находить НИЧЕГО, кроме `break-words` в `components/ui/toaster.tsx`:

```bash
grep -rn '_words\|прописью' frontend/src/ | grep -v schema.gen.ts
```

- [ ] **Step 7: Коммит**

```bash
git add frontend/src/pages/GenerateDocument.tsx frontend/src/pages/Taxes.tsx frontend/src/pages/Payroll.tsx \
  frontend/src/pages/HrOrders.tsx frontend/src/lib/schemas frontend/src/lib/api/types.ts \
  frontend/src/lib/api/schema.gen.ts
git commit -m "feat(frontend): drop amount-in-words inputs — the server derives them"
```

---

### Task 6: `OrgPermissions` — матрица прав вместо денилиста «viewer»

**Files:**
- Create: `src/tenancy/OrgPermissions.hpp`
- Create: `tests/unit/test_org_permissions.cpp`
- Modify: `src/api/Guards.hpp`
- Modify: `src/api/PayrollController.hpp` (строки 209, 282, 313, 389), `src/api/CounterpartiesController.hpp` (85, 134), `src/api/JournalController.hpp` (142, 232, 266), `src/api/EmployeesController.hpp` (111, 160, 197), `src/api/AccountsController.hpp` (86), `src/api/DocgenController.hpp` (142), `src/api/LedgerDocumentsController.hpp` (265, 355), `src/api/TaxController.hpp` (356, 471), `src/api/HrController.hpp` (186, 276, 364, 443, 526)
- Modify: `tests/integration/test_payroll_api.cpp` (строка 207), `tests/integration/test_tax_api.cpp` (строка 314)

**Interfaces:**
- Consumes: `Tenancy::OrgContext` (`src/tenancy/OrgContext.hpp:24`) — несёт `org_id`, `role`, `user_id`.
- Produces (на это опираются задачи 7, 9, 11):
  - `bool Tenancy::OrgPerm::allows(const std::string& role, const std::string& resource, const std::string& action)` — **запрет по умолчанию**;
  - строковые константы ресурсов: `Tenancy::OrgPerm::Resource::kEmployees` (`"employees"`), `kHrDocs` (`"hr_docs"`), `kPayroll` (`"payroll"`), `kPayrollPosting` (`"payroll_posting"`), `kJournal` (`"journal"`), `kCounterparties` (`"counterparties"`), `kDocuments` (`"documents"`), `kTax` (`"tax"`), `kMembers` (`"members"`);
  - действия: `Tenancy::OrgPerm::Action::kRead` (`"read"`), `kWrite` (`"write"`);
  - макрос `API_REQUIRE_ORG_PERM(callback, ctx, RES, ACT)` в `src/api/Guards.hpp` — отвечает 403 с кодом ошибки `org_role_denied`.

**Почему без этого роль `hr` откроется fail-open.** Контроль прав в системе сегодня — это **запрет одного значения**, `if (ctx.role == "viewer")`, в 23 местах девяти контроллеров (критика спеки называла 22; пересчёт по её же собственному перечню даёт 23 — перечень ниже полный). Это денилист из одного элемента: любая новая роль проходит все проверки. В момент, когда миграция задачи 7 расширит `CHECK` до `('owner','accountant','hr','viewer')`, кадровик получил бы **полный CRUD** на журнал проводок, налоговые расчёты, ФНО и проведение зарплаты в учёт. Поэтому матрица вводится ПЕРЕД ролью, отдельной задачей, с отдельным ревью.

**Матрица (спека §5.3).** «—» означает **невидимо**, а не «только чтение»:

| Ресурс | owner | accountant | hr | viewer |
|---|---|---|---|---|
| Сотрудники (вкл. оклад) — `employees` | CRUD | CRUD | CRUD | чтение |
| Трудовые договоры, приказы, отпуска, кадровые документы — `hr_docs` | CRUD | CRUD | CRUD | чтение |
| Расчётные листки, прогоны зарплаты — `payroll` | CRUD | CRUD | — | чтение |
| Проведение зарплаты в учёт — `payroll_posting` | да | да | — | — |
| Журнал проводок и план счетов — `journal` | CRUD | CRUD | — | чтение |
| Контрагенты — `counterparties` | CRUD | CRUD | — | чтение |
| Первичные документы — `documents` | CRUD | CRUD | — | чтение |
| Налоги, расчёты, ФНО — `tax` | CRUD | CRUD | — | чтение |
| Участники организации — `members` | CRUD | — | — | — |

- [ ] **Step 1: Падающий unit-тест матрицы**

`tests/unit/test_org_permissions.cpp`:

```cpp
/**
 * @file test_org_permissions.cpp
 * @brief Таблица прав §5.3 как код. Чистый unit-тест: ни БД, ни Drogon.
 *        Главный тест здесь — не «owner может всё», а deny-by-default:
 *        неизвестная роль, неизвестный ресурс и неизвестное действие
 *        обязаны давать false, иначе следующая роль повторит ошибку,
 *        из-за которой эта таблица и появилась.
 */

#include <string>

#include <gtest/gtest.h>

#include "tenancy/OrgPermissions.hpp"

namespace {

using namespace Tenancy::OrgPerm;

TEST(OrgPermissions, OwnerAndAccountantHaveFullLedgerAccess) {
    for (const auto* role : {"owner", "accountant"}) {
        for (const auto* res : {Resource::kEmployees,
                                Resource::kHrDocs,
                                Resource::kPayroll,
                                Resource::kPayrollPosting,
                                Resource::kJournal,
                                Resource::kCounterparties,
                                Resource::kDocuments,
                                Resource::kTax}) {
            EXPECT_TRUE(allows(role, res, Action::kWrite)) << role << " / " << res;
        }
    }
}

TEST(OrgPermissions, HrSeesOnlyPeopleResources) {
    EXPECT_TRUE(allows("hr", Resource::kEmployees, Action::kWrite));
    EXPECT_TRUE(allows("hr", Resource::kEmployees, Action::kRead));
    EXPECT_TRUE(allows("hr", Resource::kHrDocs, Action::kWrite));
    EXPECT_TRUE(allows("hr", Resource::kHrDocs, Action::kRead));
    // «—» = невидимо: ни записи, НИ ЧТЕНИЯ.
    for (const auto* res : {Resource::kPayroll,
                            Resource::kPayrollPosting,
                            Resource::kJournal,
                            Resource::kCounterparties,
                            Resource::kDocuments,
                            Resource::kTax,
                            Resource::kMembers}) {
        EXPECT_FALSE(allows("hr", res, Action::kRead)) << res;
        EXPECT_FALSE(allows("hr", res, Action::kWrite)) << res;
    }
}

TEST(OrgPermissions, ViewerReadsButNeverWrites) {
    for (const auto* res : {Resource::kEmployees,
                            Resource::kHrDocs,
                            Resource::kPayroll,
                            Resource::kJournal,
                            Resource::kCounterparties,
                            Resource::kDocuments,
                            Resource::kTax}) {
        EXPECT_TRUE(allows("viewer", res, Action::kRead)) << res;
        EXPECT_FALSE(allows("viewer", res, Action::kWrite)) << res;
    }
    // Проведение зарплаты в учёт viewer не видит вовсе.
    EXPECT_FALSE(allows("viewer", Resource::kPayrollPosting, Action::kRead));
    EXPECT_FALSE(allows("viewer", Resource::kPayrollPosting, Action::kWrite));
}

TEST(OrgPermissions, MembersAreOwnerOnly) {
    EXPECT_TRUE(allows("owner", Resource::kMembers, Action::kRead));
    EXPECT_TRUE(allows("owner", Resource::kMembers, Action::kWrite));
    for (const auto* role : {"accountant", "hr", "viewer"}) {
        EXPECT_FALSE(allows(role, Resource::kMembers, Action::kRead)) << role;
        EXPECT_FALSE(allows(role, Resource::kMembers, Action::kWrite)) << role;
    }
}

TEST(OrgPermissions, DenyByDefault) {
    EXPECT_FALSE(allows("superuser", Resource::kJournal, Action::kRead));
    EXPECT_FALSE(allows("", Resource::kJournal, Action::kRead));
    EXPECT_FALSE(allows("owner", "no_such_resource", Action::kRead));
    EXPECT_FALSE(allows("owner", Resource::kJournal, "delete"));
    EXPECT_FALSE(allows("owner", Resource::kJournal, ""));
}

}  // namespace
```

- [ ] **Step 2: Реализовать `src/tenancy/OrgPermissions.hpp`**

```cpp
/**
 * @file OrgPermissions.hpp
 * @brief Матрица прав тенантной роли (спека P3 §5.3) как чистая функция.
 * @details До P3 контроль прав был денилистом из одного значения —
 *          `if (ctx.role == "viewer")` в 23 местах — и любая новая роль
 *          проходила его насквозь, получая полный CRUD. Здесь таблица
 *          §5.3 записана явно, с ЗАПРЕТОМ ПО УМОЛЧАНИЮ: неизвестная роль,
 *          неизвестный ресурс и неизвестное действие дают false.
 *
 *          «—» в таблице спеки означает НЕВИДИМО, а не «только чтение»:
 *          поэтому у кадровика нет и read на payroll/journal/tax, и гейт
 *          обязан стоять не только на мутациях, но и на каждом GET
 *          (задача 7 плана).
 *
 *          Чистый модуль: ни БД, ни Drogon — тестируется в tests/unit.
 *          Потребители используют макрос API_REQUIRE_ORG_PERM
 *          (src/api/Guards.hpp), а не вызывают allows() руками.
 */

#pragma once

#include <cstring>
#include <string>

namespace Tenancy::OrgPerm {

namespace Resource {
inline constexpr const char* kEmployees = "employees";
inline constexpr const char* kHrDocs = "hr_docs";
inline constexpr const char* kPayroll = "payroll";
inline constexpr const char* kPayrollPosting = "payroll_posting";
inline constexpr const char* kJournal = "journal";
inline constexpr const char* kCounterparties = "counterparties";
inline constexpr const char* kDocuments = "documents";
inline constexpr const char* kTax = "tax";
inline constexpr const char* kMembers = "members";
}  // namespace Resource

namespace Action {
inline constexpr const char* kRead = "read";
inline constexpr const char* kWrite = "write";
}  // namespace Action

namespace detail {

/// Одна строка таблицы §5.3. Значение гранта: "rw" — чтение и запись,
/// "r" — только чтение, "" — невидимо.
struct MatrixRow {
    const char* resource;
    const char* owner;
    const char* accountant;
    const char* hr;
    const char* viewer;
};

inline constexpr MatrixRow kMatrix[] = {
    {Resource::kEmployees, "rw", "rw", "rw", "r"},
    {Resource::kHrDocs, "rw", "rw", "rw", "r"},
    {Resource::kPayroll, "rw", "rw", "", "r"},
    {Resource::kPayrollPosting, "rw", "rw", "", ""},
    {Resource::kJournal, "rw", "rw", "", "r"},
    {Resource::kCounterparties, "rw", "rw", "", "r"},
    {Resource::kDocuments, "rw", "rw", "", "r"},
    {Resource::kTax, "rw", "rw", "", "r"},
    {Resource::kMembers, "rw", "", "", ""},
};

}  // namespace detail

/**
 * @brief Разрешено ли @p role выполнить @p action над @p resource.
 * @details Запрет по умолчанию во всех трёх измерениях: неизвестный
 *          ресурс, неизвестная роль и неизвестное действие дают false.
 *          Любой грант подразумевает чтение ("rw" и "r" читают, "" — нет);
 *          запись требует буквы 'w'.
 */
inline bool allows(const std::string& role, const std::string& resource, const std::string& action) {
    const char* grants = nullptr;
    for (const auto& row : detail::kMatrix) {
        if (resource != row.resource)
            continue;
        if (role == "owner")
            grants = row.owner;
        else if (role == "accountant")
            grants = row.accountant;
        else if (role == "hr")
            grants = row.hr;
        else if (role == "viewer")
            grants = row.viewer;
        break;
    }
    if (grants == nullptr)  // неизвестный ресурс ИЛИ неизвестная роль
        return false;
    if (action == Action::kRead)
        return grants[0] != '\0';
    if (action == Action::kWrite)
        return std::strchr(grants, 'w') != nullptr;
    return false;  // неизвестное действие
}

}  // namespace Tenancy::OrgPerm
```

- [ ] **Step 3: Макрос `API_REQUIRE_ORG_PERM` в `Guards.hpp`**

В `src/api/Guards.hpp` добавить `#include "tenancy/OrgPermissions.hpp"` и в конец файла, сразу после `API_REQUIRE_ORG`:

```cpp
/// Reject with 403 unless @p ctx's TENANT role is granted (RES, ACT) by the
/// §5.3 matrix (Tenancy::OrgPerm::allows). Deny-by-default: an unknown role,
/// an unknown resource and an unknown action all produce a 403.
///
/// Takes no `req`: unlike the auth-side guards, everything this decision
/// needs already sits in the OrgContext API_REQUIRE_ORG bound — same shape
/// as API_REQUIRE_JOBS_READY, which also takes only the callback. Use it
/// AFTER API_REQUIRE_ORG in every org-scoped handler, on reads as well as
/// on writes: "—" in the matrix means invisible, not read-only.
#define API_REQUIRE_ORG_PERM(callback, ctx, RES, ACT)                                                    \
    do {                                                                                                 \
        if (!Tenancy::OrgPerm::allows((ctx).role, (RES), (ACT))) {                                        \
            callback(ErrorResponse::forbidden(                                                           \
                "org_role_denied",                                                                       \
                "Your role in this organization is not allowed to " + std::string(ACT) + " " + std::string(RES))); \
            return;                                                                                      \
        }                                                                                                \
    } while (0)
```

- [ ] **Step 4: Заменить все 23 проверки — полный перечень**

Каждая замена одинаковой формы: удалить блок

```cpp
        if (ctx.role == "viewer") {
            callback(ErrorResponse::forbidden("viewer_read_only", "<текст>"));
            return;
        }
```

и поставить на его место одну строку. Полный список — обходить сверху вниз, каждый файл целиком:

| Файл : строка | Хендлер | Замена |
|---|---|---|
| `PayrollController.hpp:209` | `calculate` | `API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kPayroll, Tenancy::OrgPerm::Action::kWrite);` |
| `PayrollController.hpp:282` | `approve` | то же, `kPayroll` / `kWrite` |
| `PayrollController.hpp:313` | `postToJournal` | `kPayrollPosting` / `kWrite` |
| `PayrollController.hpp:389` | `generatePayslip` | `kPayroll` / `kWrite` |
| `CounterpartiesController.hpp:85` | `create` | `kCounterparties` / `kWrite` |
| `CounterpartiesController.hpp:134` | `patch` | `kCounterparties` / `kWrite` |
| `JournalController.hpp:142` | `create` | `kJournal` / `kWrite` |
| `JournalController.hpp:232` | `post` | `kJournal` / `kWrite` |
| `JournalController.hpp:266` | `reverse` | `kJournal` / `kWrite` |
| `EmployeesController.hpp:111` | `create` | `kEmployees` / `kWrite` |
| `EmployeesController.hpp:160` | `patch` | `kEmployees` / `kWrite` |
| `EmployeesController.hpp:197` | `dismiss` | `kEmployees` / `kWrite` |
| `AccountsController.hpp:86` | `create` | `kJournal` / `kWrite` (план счетов — часть учётного контура, отдельной строки в матрице §5.3 у него нет) |
| `DocgenController.hpp:142` | `generate` | `kDocuments` / `kWrite` |
| `LedgerDocumentsController.hpp:265` | `startUpload` | `kDocuments` / `kWrite` |
| `LedgerDocumentsController.hpp:355` | `confirmUpload` | `kDocuments` / `kWrite` |
| `TaxController.hpp:356` | `createCalculation` | `kTax` / `kWrite` |
| `TaxController.hpp:471` | `createFiling` | `kTax` / `kWrite` |
| `HrController.hpp:186` | `createOrder` | `kHrDocs` / `kWrite` |
| `HrController.hpp:276` | `generateOrderDocument` | `kHrDocs` / `kWrite` |
| `HrController.hpp:364` | `createContract` | `kHrDocs` / `kWrite` |
| `HrController.hpp:443` | `generateContractDocument` | `kHrDocs` / `kWrite` |
| `HrController.hpp:526` | `createVacation` | `kHrDocs` / `kWrite` |

Итого 23 замены. В каждый затронутый файл добавить `#include "tenancy/OrgPermissions.hpp"` (макрос разворачивает `Tenancy::OrgPerm::` в точке использования — та же посадка, что у `API_REQUIRE_JOBS_READY`, которому включатель обязан подтянуть `jobs/Jobs.hpp`).

Заодно поправить шапки-Doxygen, которые сейчас утверждают «every mutating route rejects `ctx.role == "viewer"`»: `PayrollController.hpp:20`, `JournalController.hpp:25`, `CounterpartiesController.hpp:14`, `EmployeesController.hpp:16`, `TaxController.hpp:31`, `HrController.hpp:29`. Новая формулировка: «каждый маршрут проходит через API_REQUIRE_ORG_PERM по матрице §5.3; запрет по умолчанию».

После правок этот grep обязан не находить НИЧЕГО:

```bash
grep -rn 'role == "viewer"' src/api/
```

(остаётся ровно одно вхождение `role == "viewer"` во всём `src/` — в `Tenancy::is_valid_role`, `src/tenancy/Organization.hpp:65`, его трогает задача 7.)

- [ ] **Step 5: Поправить два теста, зашивших старый код ошибки**

`tests/integration/test_payroll_api.cpp:207` и `tests/integration/test_tax_api.cpp:314` проверяют `EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "viewer_read_only");`. Заменить строковый литерал на `"org_role_denied"`. Статус 403 не меняется.

- [ ] **Step 6: Прогнать CI и закоммитить**

Ожидание: `OrgPermissions.*` — 5 тестов PASS; все существующие сьюты остаются зелёными (поведение для owner/accountant/viewer не изменилось ни на одном маршруте — изменился только код ошибки в двух проверенных выше местах).

```bash
clang-format-17 -i src/tenancy/OrgPermissions.hpp src/api/Guards.hpp src/api/*Controller.hpp \
  tests/unit/test_org_permissions.cpp
./scripts/check-test-buckets.sh
git add src/tenancy/OrgPermissions.hpp src/api/Guards.hpp src/api/PayrollController.hpp \
  src/api/CounterpartiesController.hpp src/api/JournalController.hpp src/api/EmployeesController.hpp \
  src/api/AccountsController.hpp src/api/DocgenController.hpp src/api/LedgerDocumentsController.hpp \
  src/api/TaxController.hpp src/api/HrController.hpp tests/unit/test_org_permissions.cpp \
  tests/integration/test_payroll_api.cpp tests/integration/test_tax_api.cpp
git commit -m "feat(tenancy): replace the viewer denylist with a deny-by-default org permission matrix"
```

---

### Task 7: Роль `hr` и read-гейты на все org-scoped GET-маршруты

**Files:**
- Create: `migrations/017_hr_role.sql`
- Create: `tests/integration/test_org_read_gates.cpp`
- Modify: `src/tenancy/Organization.hpp` (строка 65)
- Modify: `src/api/OrganizationsController.hpp` (строки 335-337, 405-407, плюс гейты на четыре member-маршрута)
- Modify: `src/api/PayrollController.hpp` (183, 352), `src/api/CounterpartiesController.hpp` (65, 107), `src/api/JournalController.hpp` (93, 205), `src/api/EmployeesController.hpp` (91, 133), `src/api/AccountsController.hpp` (65), `src/api/DocgenController.hpp` (125), `src/api/LedgerDocumentsController.hpp` (160, 205, 227), `src/api/TaxController.hpp` (270, 297, 323, 423, 686, 714, 745), `src/api/HrController.hpp` (162, 336, 502)
- Modify: `docs/openapi.yaml` (enum роли участника — три места), `frontend/src/lib/api/schema.gen.ts` (регенерация)
- Modify: `tests/integration/test_organizations_api.cpp`

**Interfaces:**
- Consumes (задача 6): `Tenancy::OrgPerm::allows`, константы `Resource::*` / `Action::*`, макрос `API_REQUIRE_ORG_PERM(callback, ctx, RES, ACT)`.
- Produces (на это опираются задачи 13, 14): значение роли `"hr"` принимается `Tenancy::is_valid_role`, миграцией и `OrganizationsController`; каждый org-scoped GET отвечает **403 `org_role_denied`** роли, у которой нет read на его ресурс.

**Вторая половина проблемы RBAC.** В восьми доменных контроллерах 46 org-scoped маршрутов, и **21 GET-хендлер не имеет проверки роли вообще** (критика спеки говорила «20» — пересчёт по хендлерам даёт 21, полный перечень ниже), плюс два POST-маршрута выдачи presigned-ссылок, которые «read-only, но пишут URL». Значит правило «`—` = невидимо» сегодня невыразимо: чтобы кадровик не видел зарплатную ведомость, гейт нужно поставить туда, где его никогда не было.

- [ ] **Step 1: Миграция роли**

`migrations/017_hr_role.sql`:

```sql
-- Роль кадровика (спека P3 §5). CHECK в migrations/006_organizations.sql
-- безымянный, поэтому снимается по автоимени, которое Postgres ему дал:
-- <таблица>_<колонка>_check = org_members_role_check.
--
-- Расширение множества ролей БЕЗ матрицы прав (src/tenancy/OrgPermissions.hpp,
-- задача 6 плана) — это fail-open: старый контроль был денилистом
-- `role == 'viewer'`, и новая роль прошла бы его насквозь, получив полный
-- CRUD на журнал, налоги и зарплату. Матрица обязана существовать ДО этой
-- миграции.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

ALTER TABLE org_members DROP CONSTRAINT IF EXISTS org_members_role_check;
ALTER TABLE org_members ADD CONSTRAINT org_members_role_check
    CHECK (role IN ('owner', 'accountant', 'hr', 'viewer'));
```

- [ ] **Step 2: `is_valid_role` и тексты ошибок**

`src/tenancy/Organization.hpp:63-66` — тело и комментарий:

```cpp
/// The four tenancy roles org_members.role is CHECK-constrained to
/// (migrations/006_organizations.sql + migrations/017_hr_role.sql). Kept
/// here so callers can validate a role value before it ever reaches the
/// database. What each role may actually DO is Tenancy::OrgPerm::allows
/// (src/tenancy/OrgPermissions.hpp), not this function.
inline bool is_valid_role(const std::string& role) {
    return role == "owner" || role == "accountant" || role == "hr" || role == "viewer";
}
```

`src/api/OrganizationsController.hpp:337` и `:407` — текст сообщения в обоих местах:

```cpp
            role_errs.add("role", "not_allowed", "must be one of: 'owner' 'accountant' 'hr' 'viewer'");
```

- [ ] **Step 3: Гейты на управление участниками**

Четыре member-маршрута `OrganizationsController` (`listMembers`, `addMember`, `updateMemberRole`, `removeMember`) сейчас защищены `require_admin_or_org_owner(req, id)` — то есть уже owner-only, и матрица `members` их не ослабляет. Ничего не менять, но добавить в шапку контроллера строку: «`members` в матрице §5.3 — owner-only; это уже обеспечено `require_admin_or_org_owner`, отдельный `API_REQUIRE_ORG_PERM` был бы дублированием и вторым источником истины».

Отдельно зафиксировать тестом, что появление `hr` не ломает защиту последнего владельца: `updateMemberRole` считает владельцев по `role == "owner"`, и `hr` за владельца не считается.

- [ ] **Step 4: Read-гейты — полный перечень 21 GET-хендлера**

В каждый вписать `API_REQUIRE_ORG_PERM(callback, ctx, <RES>, Tenancy::OrgPerm::Action::kRead);` СРАЗУ после `API_REQUIRE_ORG(req, callback, ctx);`. Там, где сейчас стоит `(void)ctx;` («no per-org data here»), эту строку удалить — `ctx` теперь используется.

| Файл : строка | Хендлер | Ресурс |
|---|---|---|
| `PayrollController.hpp:183` | `list` (`GET /payroll-runs`) | `kPayroll` |
| `PayrollController.hpp:352` | `listPayslips` | `kPayroll` |
| `CounterpartiesController.hpp:65` | `list` | `kCounterparties` |
| `CounterpartiesController.hpp:107` | `get` | `kCounterparties` |
| `JournalController.hpp:93` | `list` | `kJournal` |
| `JournalController.hpp:205` | `get` | `kJournal` |
| `EmployeesController.hpp:91` | `list` | `kEmployees` |
| `EmployeesController.hpp:133` | `get` | `kEmployees` |
| `AccountsController.hpp:65` | `list` | `kJournal` |
| `DocgenController.hpp:125` | `listTemplates` | `kDocuments` |
| `LedgerDocumentsController.hpp:160` | `list` | `kDocuments` (см. оговорку ниже) |
| `LedgerDocumentsController.hpp:205` | `get` | по типу документа (см. ниже) |
| `LedgerDocumentsController.hpp:227` | `downloadUrl` (POST, но чтение) | по типу документа (см. ниже) |
| `TaxController.hpp:270` | `listRates` | `kTax` |
| `TaxController.hpp:297` | `listDeadlines` | `kTax` |
| `TaxController.hpp:323` | `listAlerts` | `kTax` |
| `TaxController.hpp:423` | `listCalculations` | `kTax` |
| `TaxController.hpp:686` | `listFilings` | `kTax` |
| `TaxController.hpp:714` | `getFiling` | `kTax` |
| `TaxController.hpp:745` | `filingDownloadUrl` (POST, но чтение) | `kTax` |
| `HrController.hpp:162` | `listOrders` | `kHrDocs` |
| `HrController.hpp:336` | `listContracts` | `kHrDocs` |
| `HrController.hpp:502` | `listVacations` | `kHrDocs` |

Итого 21 GET + 2 POST-маршрута выдачи presigned-ссылки, которые семантически являются чтением и обязаны гейтиться как чтение (`LedgerDocumentsController::downloadUrl`, `TaxController::filingDownloadUrl`).

**Оговорка про `/documents`.** Кадровые документы (`doc_type = 'hr'`) лежат в той же таблице `documents`, что и первичка, и в матрице §5.3 они относятся к `hr_docs` (кадровику доступны), а первичка — к `documents` (кадровику невидима). Поэтому в `LedgerDocumentsController`:

- `get` и `downloadUrl` (и, после задачи 9, версионные маршруты) выбирают ресурс по загруженной строке:

```cpp
            // Кадровые документы в матрице §5.3 — ресурс hr_docs (кадровик
            // их видит), вся остальная первичка — documents (не видит).
            // Обе ветки идут ПОСЛЕ find_in_org: до чтения строки тип
            // документа неизвестен, а «не в этой организации» обязано
            // оставаться 404, не 403.
            const char* resource = found->doc_type == "hr" ? Tenancy::OrgPerm::Resource::kHrDocs
                                                           : Tenancy::OrgPerm::Resource::kDocuments;
            if (!Tenancy::OrgPerm::allows(ctx.role, resource, Tenancy::OrgPerm::Action::kRead)) {
                callback(ErrorResponse::forbidden("org_role_denied",
                                                  "Your role in this organization is not allowed to read this document"));
                return;
            }
```

- `list` пропускает вызывающего, если у него есть read хотя бы на один из двух ресурсов, и **принудительно сужает выборку до `doc_type='hr'`**, когда есть только `hr_docs`:

```cpp
        const bool may_read_all = Tenancy::OrgPerm::allows(ctx.role, Tenancy::OrgPerm::Resource::kDocuments,
                                                           Tenancy::OrgPerm::Action::kRead);
        const bool may_read_hr = Tenancy::OrgPerm::allows(ctx.role, Tenancy::OrgPerm::Resource::kHrDocs,
                                                          Tenancy::OrgPerm::Action::kRead);
        if (!may_read_all && !may_read_hr) {
            callback(ErrorResponse::forbidden("org_role_denied",
                                              "Your role in this organization is not allowed to read documents"));
            return;
        }
        // Кадровик видит реестр, но только свою часть: фильтр ?type
        // ПЕРЕЗАПИСЫВАЕТСЯ, а не проверяется — иначе ?type=invoice дал бы
        // ему первичку в обход матрицы.
        std::optional<std::string> doc_type_filter = /* существующий разбор ?type */;
        if (!may_read_all)
            doc_type_filter = std::string("hr");
```

- [ ] **Step 5: OpenAPI и регенерация клиента**

В `docs/openapi.yaml` найти три места, где перечислены роли участника (тело `POST /orgs/{id}/members`, тело `PATCH /orgs/{id}/members/{user_id}`, схема ответа членства), и в каждом расширить enum до `[owner, accountant, hr, viewer]`. Найти их так:

```bash
grep -n "owner" docs/openapi.yaml | grep -i "accountant"
```

Дописать в описания затронутых GET-маршрутов ответ `'403': { description: Your organization role is not allowed to read this resource (org_role_denied) }` там, где его ещё нет.

Затем:

```bash
cd frontend && npm run codegen && npx tsc --noEmit
```

`frontend/src/lib/api/schema.gen.ts` перегенерируется — enum роли зашит там в пяти местах (строки ~1801, 1806, 1931, 5219, 5241, 5254 до регенерации); руками файл НЕ править.

- [ ] **Step 6: Падающий интеграционный тест read-гейтов**

`tests/integration/test_org_read_gates.cpp` — по одному 403 на каждый закрытый для кадровика ресурс, включая GET. Фикстуру и хелперы (`seed_user`, `seed_org`, `member`, `authed`, `call`) копировать из `tests/integration/test_documents_api.cpp:104-200` — там канонический вид для этого репозитория; MinIO здесь не нужен, S3-часть фикстуры не переносить.

```cpp
/**
 * @file test_org_read_gates.cpp
 * @brief «—» в матрице §5.3 означает НЕВИДИМО. Каждый закрытый для
 *        кадровика ресурс обязан отвечать 403 и на ЧТЕНИЕ, иначе тест на
 *        403 для POST /payroll-runs пройдёт, а GET /payroll-runs/{id}/
 *        payslips останется открытым — ровно тот класс ошибки, который дал
 *        утечку в P2.
 */

TEST_F(OrgReadGatesTest, HrIsDeniedReadOnPayrollJournalTaxAndDocuments) {
    auto hr = member("hr@example.com", org_.id, "hr");
    struct Case {
        const char* label;
        std::function<HttpResponsePtr(const Security::Auth::AuthPrincipal&)> invoke;
    };
    const Case kCases[] = {
        {"GET /payroll-runs", [&](auto p) { return call_list(payroll_, p); }},
        {"GET /payroll-runs/{id}/payslips", [&](auto p) { return call_payslips(payroll_, p, run_id_); }},
        {"GET /journal-entries", [&](auto p) { return call_list(journal_, p); }},
        {"GET /journal-entries/{id}", [&](auto p) { return call_get(journal_, p, entry_id_); }},
        {"GET /accounts", [&](auto p) { return call_list(accounts_, p); }},
        {"GET /counterparties", [&](auto p) { return call_list(counterparties_, p); }},
        {"GET /counterparties/{id}", [&](auto p) { return call_get(counterparties_, p, counterparty_id_); }},
        {"GET /tax/rates", [&](auto p) { return call_rates(tax_, p); }},
        {"GET /tax/deadlines", [&](auto p) { return call_deadlines(tax_, p); }},
        {"GET /tax/alerts", [&](auto p) { return call_alerts(tax_, p); }},
        {"GET /tax/calculations", [&](auto p) { return call_calculations(tax_, p); }},
        {"GET /tax/filings", [&](auto p) { return call_filings(tax_, p); }},
        {"GET /doc-templates", [&](auto p) { return call_templates(docgen_, p); }},
    };
    for (const auto& c : kCases) {
        auto resp = c.invoke(hr);
        EXPECT_EQ(resp->getStatusCode(), k403Forbidden) << c.label;
        EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "org_role_denied") << c.label;
    }
}

TEST_F(OrgReadGatesTest, HrMayReadEmployeesAndHrDocuments) {
    auto hr = member("hr2@example.com", org_.id, "hr");
    EXPECT_EQ(call_list(employees_, hr)->getStatusCode(), k200OK);
    EXPECT_EQ(call_list(hr_, hr)->getStatusCode(), k200OK);       // GET /hr-orders
    EXPECT_EQ(call_contracts(hr_, hr)->getStatusCode(), k200OK);  // GET /labor-contracts
    EXPECT_EQ(call_vacations(hr_, hr)->getStatusCode(), k200OK);  // GET /vacations
}

TEST_F(OrgReadGatesTest, HrCannotWriteToTheLedgerTaxesOrPayroll) {
    auto hr = member("hr3@example.com", org_.id, "hr");
    EXPECT_EQ(call_create_entry(journal_, hr)->getStatusCode(), k403Forbidden);
    EXPECT_EQ(call_create_counterparty(counterparties_, hr)->getStatusCode(), k403Forbidden);
    EXPECT_EQ(call_calculate_payroll(payroll_, hr)->getStatusCode(), k403Forbidden);
    EXPECT_EQ(call_post_to_journal(payroll_, hr, run_id_)->getStatusCode(), k403Forbidden);
    EXPECT_EQ(call_create_calculation(tax_, hr)->getStatusCode(), k403Forbidden);
    EXPECT_EQ(call_generate_document(docgen_, hr)->getStatusCode(), k403Forbidden);
}

TEST_F(OrgReadGatesTest, DocumentsListIsNarrowedToHrDocumentsForTheHrRole) {
    // Кадровик видит реестр, но только кадровые документы — и ?type=invoice
    // не расширяет выборку.
    auto hr = member("hr4@example.com", org_.id, "hr");
    auto resp = call_documents_list(documents_, hr, /*type=*/"invoice");
    ASSERT_EQ(resp->getStatusCode(), k200OK);
    for (const auto& d : body_of(resp)["data"])
        EXPECT_EQ(d["doc_type"].get<std::string>(), "hr");
}

TEST_F(OrgReadGatesTest, HrDoesNotCountAsAnOwnerForLastOwnerProtection) {
    // Организация с единственным owner и одним hr: попытка разжаловать
    // owner-а обязана остаться 409, hr за владельца не считается.
    auto owner_membership = /* владелец org_ */;
    auto resp = call_update_member_role(orgs_, admin_, org_.id, owner_membership.user_id, "hr");
    EXPECT_EQ(resp->getStatusCode(), k409Conflict);
}
```

Имена хелперов подгоните под свой файл: важен не их вид, а покрытие — **на каждый из 21 read-гейта и каждый закрытый write должен быть кейс**.

- [ ] **Step 7: Гейты, CI, коммит**

```bash
clang-format-17 -i src/tenancy/Organization.hpp src/api/*Controller.hpp tests/integration/test_org_read_gates.cpp
./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh && ./scripts/check-test-buckets.sh
make lint-openapi
(cd frontend && npx tsc --noEmit && npm run lint)
git add migrations/017_hr_role.sql src/tenancy/Organization.hpp src/api/OrganizationsController.hpp \
  src/api/PayrollController.hpp src/api/CounterpartiesController.hpp src/api/JournalController.hpp \
  src/api/EmployeesController.hpp src/api/AccountsController.hpp src/api/DocgenController.hpp \
  src/api/LedgerDocumentsController.hpp src/api/TaxController.hpp src/api/HrController.hpp \
  docs/openapi.yaml frontend/src/lib/api/schema.gen.ts \
  tests/integration/test_org_read_gates.cpp tests/integration/test_organizations_api.cpp
git commit -m "feat(tenancy): add the hr role and gate every org-scoped read"
```

---

### Task 8: Таблица `document_versions` — миграция, домен, репозиторий

**Files:**
- Create: `migrations/018_document_versions.sql`
- Create: `src/ledger/DocumentVersion.hpp`
- Modify: `src/ledger/Document.hpp` (поля и `to_json`)
- Modify: `src/ledger/DocumentRepository.hpp` (константы базы, `create`, `set_pending_upload`, `set_file`, `list_filtered`, `count_filtered`, `list_for_entry`)
- Modify: `tests/test_helpers.hpp` (`wipe_org_data`)
- Modify: `docs/openapi.yaml` (схема `Document`)
- Test: `tests/integration/test_documents.cpp`

**Interfaces:**
- Consumes: ничего из предыдущих задач.
- Produces (на это опираются задачи 9, 10, 11, 13):
  - `struct Ledger::DocumentVersion { std::string id, org_id, document_id; int version_no; std::optional<std::string> s3_key, checksum_sha256, mime; std::optional<long long> size_bytes; std::optional<std::string> template_version; std::optional<nlohmann::json> input_snapshot; std::optional<std::string> created_by_user_id; std::string created_at, updated_at; static DocumentVersion from_row(const pqxx::row&); };` + ADL `void to_json(nlohmann::json&, const DocumentVersion&)`;
  - `Ledger::Document` дополнительно несёт `std::optional<std::string> current_version_id;` и `int latest_version_no;`, а его файловые поля (`s3_key`, `checksum_sha256`, `mime`, `size_bytes`, `template_version`, `input_snapshot`) теперь читаются из ТЕКУЩЕЙ версии, а не из своей строки;
  - `DocumentVersion DocumentRepository::add_version(const std::string& org_id, const std::string& document_id, std::optional<nlohmann::json> input_snapshot, std::optional<std::string> template_version, std::optional<std::string> created_by_user_id)`;
  - `std::vector<DocumentVersion> DocumentRepository::list_versions(const std::string& org_id, const std::string& document_id)` — по возрастанию `version_no`;
  - `std::optional<DocumentVersion> DocumentRepository::find_version(const std::string& org_id, const std::string& document_id, int version_no)`;
  - `std::optional<DocumentVersion> DocumentRepository::latest_version(const std::string& org_id, const std::string& document_id)`;
  - `bool DocumentRepository::set_version_file(const std::string& org_id, const std::string& version_id, const std::string& s3_key, const std::string& checksum_sha256, const std::string& mime, long long size_bytes)`;
  - `bool DocumentRepository::set_current_version(const std::string& org_id, const std::string& document_id, const std::string& version_id)` — на этом шаге простой `UPDATE documents SET current_version_id = $3 WHERE id = $1 AND org_id = $2 RETURNING id`; задача 10 ужесточит его условием «версия всё ещё самая новая», не меняя сигнатуру;
  - `bool DocumentRepository::set_pending_upload(...)` — сигнатура прежняя, пишет теперь в версию 1;
  - **`DocumentRepository::set_file(org_id, document_id, ...)` удаляется** — задача 10 адресуется по `version_id`.

- [ ] **Step 1: Миграция**

`migrations/018_document_versions.sql`:

```sql
-- Версии документов (спека P3 §4.1). Правка документа порождает НОВУЮ
-- версию с новым рендером; предыдущий PDF остаётся. Бухгалтерский документ
-- — свидетельство: тихая перезапись файла, на который могли сослаться,
-- рвёт аудиторский след.
--
-- Файловые метаданные и input_snapshot переезжают с `documents` сюда;
-- `documents` хранит указатель на ТЕКУЩУЮ версию. Версии по отдельности не
-- удаляются никогда — удаление и аннулирование действуют на документ
-- целиком (§4.2).
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

CREATE TABLE IF NOT EXISTS document_versions (
    id                 UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id             UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    document_id        UUID NOT NULL,
    version_no         INTEGER NOT NULL CHECK (version_no >= 1),
    s3_key             TEXT,
    checksum_sha256    CHAR(64),
    mime               TEXT,
    size_bytes         BIGINT,
    template_version   TEXT,
    input_snapshot     JSONB,
    created_by_user_id UUID REFERENCES users(id) ON DELETE SET NULL,
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (document_id, version_no),
    -- Составная цель для указателя documents.current_version_id ниже —
    -- та же идиома, что UNIQUE(id, org_id) на documents/journal_entries:
    -- указатель пиннится сразу по обоим столбцам, поэтому текущая версия
    -- документа провабельно принадлежит тому же тенанту.
    UNIQUE (id, org_id),
    FOREIGN KEY (document_id, org_id) REFERENCES documents (id, org_id) ON DELETE CASCADE
);
DROP TRIGGER IF EXISTS trg_document_versions_touch ON document_versions;
CREATE TRIGGER trg_document_versions_touch BEFORE UPDATE ON document_versions
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
CREATE INDEX IF NOT EXISTS idx_document_versions_document ON document_versions (document_id, version_no DESC);

-- Указатель на текущую (опубликованную) версию. NULL — легальное
-- состояние: «версия создана, рендер ещё не закончился» (§4.2 п.3), и
-- ровно так же выглядит только что созданный документ до успеха джобы.
ALTER TABLE documents ADD COLUMN IF NOT EXISTS current_version_id UUID;

-- Бэкфилл: каждому существующему документу — версия 1 с его нынешними
-- метаданными, указатель ставится только если файл реально есть (иначе
-- документ и был без файла).
INSERT INTO document_versions (org_id, document_id, version_no, s3_key, checksum_sha256, mime, size_bytes,
                               template_version, input_snapshot, created_at, updated_at)
SELECT org_id, id, 1, s3_key, checksum_sha256, mime, size_bytes, template_version, input_snapshot,
       created_at, updated_at
  FROM documents
 WHERE NOT EXISTS (SELECT 1 FROM document_versions v WHERE v.document_id = documents.id);

UPDATE documents d
   SET current_version_id = v.id
  FROM document_versions v
 WHERE v.document_id = d.id AND v.version_no = 1 AND v.s3_key IS NOT NULL
   AND d.current_version_id IS NULL;

-- Указатель добавляется ПОСЛЕ бэкфилла: иначе NOT VALID-состояние пришлось
-- бы разруливать отдельно. DEFERRABLE — потому что связь циклическая
-- (documents -> document_versions -> documents), и вставка документа с его
-- первой версией происходит в одной транзакции.
ALTER TABLE documents DROP CONSTRAINT IF EXISTS documents_current_version_fk;
ALTER TABLE documents ADD CONSTRAINT documents_current_version_fk
    FOREIGN KEY (current_version_id, org_id) REFERENCES document_versions (id, org_id)
    DEFERRABLE INITIALLY DEFERRED;

-- Старые колонки уходят: два источника истины на один и тот же файл —
-- это гарантированное расхождение. Всё, что их читало, переведено на
-- LEFT JOIN к текущей версии (src/ledger/DocumentRepository.hpp).
ALTER TABLE documents DROP COLUMN IF EXISTS s3_key;
ALTER TABLE documents DROP COLUMN IF EXISTS checksum_sha256;
ALTER TABLE documents DROP COLUMN IF EXISTS mime;
ALTER TABLE documents DROP COLUMN IF EXISTS size_bytes;
ALTER TABLE documents DROP COLUMN IF EXISTS template_version;
ALTER TABLE documents DROP COLUMN IF EXISTS input_snapshot;
```

- [ ] **Step 2: Домен версии**

`src/ledger/DocumentVersion.hpp` — по образцу `src/ledger/Document.hpp` (тот же стиль `from_row` с `.template as<>()` там, где выражение зависимое, и ADL-`to_json`):

```cpp
/**
 * @file DocumentVersion.hpp
 * @brief Одна версия документа: файл в S3 + снапшот входа, из которого он
 *        отрендерен (спека P3 §4.1).
 * @details Версии не удаляются по отдельности НИКОГДА. Удаление и
 *          аннулирование — операции над документом целиком; строка версии
 *          исчезает только вместе с ним (FK ON DELETE CASCADE).
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>

#include <nlohmann/json.hpp>

namespace Ledger {

struct DocumentVersion {
    std::string id;
    std::string org_id;
    std::string document_id;
    int version_no = 0;
    std::optional<std::string> s3_key;
    std::optional<std::string> checksum_sha256;
    std::optional<std::string> mime;
    std::optional<long long> size_bytes;
    std::optional<std::string> template_version;
    std::optional<nlohmann::json> input_snapshot;
    std::optional<std::string> created_by_user_id;
    std::string created_at;
    std::string updated_at;

    static DocumentVersion from_row(const pqxx::row& r) {
        DocumentVersion v;
        v.id = r["id"].as<std::string>();
        v.org_id = r["org_id"].as<std::string>();
        v.document_id = r["document_id"].as<std::string>();
        v.version_no = r["version_no"].as<int>();
        if (!r["s3_key"].is_null())
            v.s3_key = r["s3_key"].as<std::string>();
        if (!r["checksum_sha256"].is_null())
            v.checksum_sha256 = r["checksum_sha256"].as<std::string>();
        if (!r["mime"].is_null())
            v.mime = r["mime"].as<std::string>();
        if (!r["size_bytes"].is_null())
            v.size_bytes = r["size_bytes"].as<long long>();
        if (!r["template_version"].is_null())
            v.template_version = r["template_version"].as<std::string>();
        if (!r["input_snapshot"].is_null())
            v.input_snapshot = nlohmann::json::parse(r["input_snapshot"].as<std::string>());
        if (!r["created_by_user_id"].is_null())
            v.created_by_user_id = r["created_by_user_id"].as<std::string>();
        v.created_at = r["created_at"].as<std::string>();
        v.updated_at = r["updated_at"].as<std::string>();
        return v;
    }
};

inline void to_json(nlohmann::json& j, const DocumentVersion& v) {
    j = nlohmann::json{
        {"id", v.id},
        {"document_id", v.document_id},
        {"version_no", v.version_no},
        {"s3_key", v.s3_key ? nlohmann::json(*v.s3_key) : nlohmann::json(nullptr)},
        {"checksum_sha256", v.checksum_sha256 ? nlohmann::json(*v.checksum_sha256) : nlohmann::json(nullptr)},
        {"mime", v.mime ? nlohmann::json(*v.mime) : nlohmann::json(nullptr)},
        {"size_bytes", v.size_bytes ? nlohmann::json(*v.size_bytes) : nlohmann::json(nullptr)},
        {"template_version", v.template_version ? nlohmann::json(*v.template_version) : nlohmann::json(nullptr)},
        {"created_by_user_id", v.created_by_user_id ? nlohmann::json(*v.created_by_user_id) : nlohmann::json(nullptr)},
        {"created_at", v.created_at},
        {"updated_at", v.updated_at},
    };
    // input_snapshot наружу НЕ отдаётся списком версий: это полный вход
    // рендера, включая суммы и подписантов, и его место — в детальном
    // ответе, а не в перечне. Задача 9 решает, отдавать ли его вообще.
}

}  // namespace Ledger
```

- [ ] **Step 3: `Document` — два новых поля, файловые читаются из текущей версии**

В `src/ledger/Document.hpp` добавить в структуру:

```cpp
    /// Указатель на текущую (опубликованную) версию; NULL, пока рендер
    /// первой версии не завершился успехом.
    std::optional<std::string> current_version_id;
    /// Номер САМОЙ НОВОЙ версии (не обязательно текущей): позволяет UI
    /// показать «версия 3 из 4, рендер идёт», не делая второй запрос.
    int latest_version_no = 0;
```

и в `from_row` — их чтение (`current_version_id` через `is_null()`, `latest_version_no` через `.as<int>()`), в `to_json` — их вывод. Остальные поля структуры не меняются: они по-прежнему называются `s3_key`, `checksum_sha256`, `mime`, `size_bytes`, `template_version`, `input_snapshot`, просто источник у них теперь другой. Это сознательно: API-контракт `GET /documents/{id}` и SPA остаются прежними, меняется только то, откуда берутся значения.

- [ ] **Step 4: Репозиторий — JOIN вместо собственных колонок**

В `src/ledger/DocumentRepository.hpp` заменить константы базы на:

```cpp
    // OrgCrudBase подставляет kTable в FROM дословно, поэтому джойн живёт
    // здесь, а kIdColumn/kOrgColumn/kOrderBy квалифицированы алиасом.
    // Файловые метаданные приходят из ТЕКУЩЕЙ версии (v), а не из строки
    // документа — своих таких колонок у `documents` больше нет
    // (migrations/018_document_versions.sql).
    static constexpr const char* kTable =
        "documents d LEFT JOIN document_versions v ON v.id = d.current_version_id";
    static constexpr const char* kColumns =
        "d.id, d.org_id, d.doc_type, d.source, d.status, d.counterparty_id, "
        "v.s3_key, v.checksum_sha256, v.mime, v.size_bytes, "
        "d.template_slug, v.template_version, v.input_snapshot, "
        "d.current_version_id, "
        "COALESCE((SELECT MAX(vv.version_no) FROM document_versions vv WHERE vv.document_id = d.id), 0) "
        "AS latest_version_no, "
        "d.created_at, d.updated_at";
    static constexpr const char* kIdColumn = "d.id";
    static constexpr const char* kOrderBy = "d.created_at DESC";
    static constexpr const char* kOrgColumn = "d.org_id";
```

`kColumnsAliased` больше не нужна — `kColumns` уже квалифицирована; `list_for_entry` переписать на `FROM documents d LEFT JOIN document_versions v ON v.id = d.current_version_id JOIN document_entries de ON de.document_id = d.id`. `list_filtered` / `count_filtered` — заменить `FROM documents` на тот же джойн (для `count_filtered` джойн не нужен, только квалифицировать `d.` или убрать алиас: там `SELECT COUNT(*) FROM documents WHERE org_id = $1 ...` работает как есть, если не менять текст запроса — оставьте его, но проверьте, что он не ссылается на удалённые колонки).

`create()` — сохранить сигнатуру, но вставить и документ, и версию 1 в одной транзакции:

```cpp
    Document create(const std::string& org_id,
                    const std::string& doc_type,
                    const std::string& source,
                    const std::string& status,
                    std::optional<std::string> counterparty_id = std::nullopt,
                    std::optional<std::string> template_slug = std::nullopt,
                    std::optional<std::string> template_version = std::nullopt,
                    std::optional<nlohmann::json> input_snapshot = std::nullopt,
                    std::optional<std::string> created_by_user_id = std::nullopt) {
        std::optional<std::string> snapshot_text;
        if (input_snapshot)
            snapshot_text = input_snapshot->dump();

        return Database::get().execute_write([&](auto& txn) {
            auto d = txn.exec_params(
                "INSERT INTO documents (org_id, doc_type, source, status, counterparty_id, template_slug) "
                "VALUES ($1, $2, $3, $4, $5, $6) RETURNING id",
                org_id,
                doc_type,
                source,
                status,
                counterparty_id,
                template_slug);
            const std::string document_id = d[0][0].template as<std::string>();
            txn.exec_params(
                "INSERT INTO document_versions (org_id, document_id, version_no, template_version, input_snapshot, "
                "created_by_user_id) VALUES ($1, $2, 1, $3, $4::jsonb, $5)",
                org_id,
                document_id,
                template_version,
                snapshot_text,
                created_by_user_id);
            // current_version_id намеренно НЕ ставится: указатель встаёт
            // только когда рендер положил файл (задача 10). До этого
            // download-url честно отвечает 409 no_file.
            auto r = txn.exec_params(std::string("SELECT ") + kColumns + " FROM " + kTable + " WHERE d.id = $1",
                                     document_id);
            return Document::from_row(r[0]);
        });
    }
```

Добавить пять новых методов (`add_version`, `list_versions`, `find_version`, `latest_version`, `set_version_file`, `set_current_version`) с точными сигнатурами из блока Interfaces. Константа списка колонок версии:

```cpp
    static constexpr const char* kVersionColumns =
        "id, org_id, document_id, version_no, s3_key, checksum_sha256, mime, size_bytes, template_version, "
        "input_snapshot, created_by_user_id, created_at, updated_at";
```

`add_version` вычисляет номер прямо в INSERT, без гонки на чтении:

```cpp
    DocumentVersion add_version(const std::string& org_id,
                                const std::string& document_id,
                                std::optional<nlohmann::json> input_snapshot,
                                std::optional<std::string> template_version,
                                std::optional<std::string> created_by_user_id) {
        std::optional<std::string> snapshot_text;
        if (input_snapshot)
            snapshot_text = input_snapshot->dump();
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "INSERT INTO document_versions (org_id, document_id, version_no, template_version, input_snapshot, "
                "created_by_user_id) "
                "SELECT $1, $2, COALESCE(MAX(version_no), 0) + 1, $3, $4::jsonb, $5 "
                "  FROM document_versions WHERE document_id = $2 "
                "RETURNING " +
                    std::string(kVersionColumns),
                org_id,
                document_id,
                template_version,
                snapshot_text,
                created_by_user_id);
            return DocumentVersion::from_row(r[0]);
        });
    }
```

`set_pending_upload` переписать на версию 1 документа (проверка `status = 'draft'` остаётся — она про документ):

```cpp
    bool set_pending_upload(const std::string& org_id,
                            const std::string& id,
                            const std::string& s3_key,
                            const std::string& mime) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE document_versions v SET s3_key = $3, mime = $4 "
                "  FROM documents d "
                " WHERE v.document_id = d.id AND d.id = $1 AND d.org_id = $2 AND d.status = 'draft' "
                "   AND v.version_no = (SELECT MAX(vv.version_no) FROM document_versions vv WHERE vv.document_id = d.id) "
                "RETURNING v.id",
                id,
                org_id,
                s3_key,
                mime);
            return !r.empty();
        });
    }
```

**Удалить** старый `set_file(org_id, id, ...)`. Единственный его вызывающий — `Docgen::RenderJob::process_job` — переводится на `set_version_file` в задаче 10; чтобы ветка компилировалась уже сейчас, в этой задаче поменяйте `RenderJob` минимально: получить версию через `latest_version(org_id, document_id)` и вызвать `set_version_file(org_id, v->id, ...)`, оставив `set_status(..., "final")` как есть. Полную защиту джобы добавит задача 10.

- [ ] **Step 5: `wipe_org_data` и OpenAPI**

`tests/test_helpers.hpp:358-364` — в TRUNCATE добавить новую таблицу явно (CASCADE подхватил бы её и сам, но явный список — документация того, что чистится):

```cpp
        txn.exec("TRUNCATE TABLE journal_lines, journal_entries, document_entries, document_versions, documents "
                 "CASCADE");
```

`docs/openapi.yaml`, схема `Document` (~строка 442): добавить свойства

```yaml
        current_version_id: { type: ['string', 'null'], format: uuid, description: 'Current (published) version; null while the first render has not finished' }
        latest_version_no:  { type: integer, description: 'Highest existing version number; may exceed the current version while a render is in flight' }
```

и в описании `s3_key`/`checksum_sha256`/`mime`/`size_bytes`/`template_version`/`input_snapshot` дописать «read from the CURRENT version (document_versions), not from the document row».

- [ ] **Step 6: Падающие интеграционные тесты**

Дописать в `tests/integration/test_documents.cpp`:

```cpp
TEST_F(DocumentsTest, CreateMakesVersionOneAndLeavesTheCurrentPointerNull) {
    Ledger::DocumentRepository repo;
    auto doc = repo.create(org_.id, "invoice", "generated", "draft", std::nullopt, "invoice", "v1",
                           std::optional<nlohmann::json>{nlohmann::json{{"number", "1"}}});
    EXPECT_EQ(doc.latest_version_no, 1);
    EXPECT_FALSE(doc.current_version_id.has_value());
    auto versions = repo.list_versions(org_.id, doc.id);
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_EQ(versions[0].version_no, 1);
    ASSERT_TRUE(versions[0].input_snapshot);
    EXPECT_EQ((*versions[0].input_snapshot)["number"].get<std::string>(), "1");
}

TEST_F(DocumentsTest, AddVersionIncrementsAndKeepsOlderRows) {
    Ledger::DocumentRepository repo;
    auto doc = repo.create(org_.id, "invoice", "generated", "draft");
    auto v2 = repo.add_version(org_.id, doc.id, std::optional<nlohmann::json>{nlohmann::json{{"number", "2"}}},
                               std::string("v1"), std::nullopt);
    EXPECT_EQ(v2.version_no, 2);
    EXPECT_EQ(repo.list_versions(org_.id, doc.id).size(), 2u);
    auto v1 = repo.find_version(org_.id, doc.id, 1);
    ASSERT_TRUE(v1);
    EXPECT_EQ(v1->version_no, 1);
}

TEST_F(DocumentsTest, DocumentReadsFileMetadataFromTheCurrentVersion) {
    Ledger::DocumentRepository repo;
    auto doc = repo.create(org_.id, "invoice", "generated", "draft");
    auto v1 = repo.latest_version(org_.id, doc.id);
    ASSERT_TRUE(v1);
    ASSERT_TRUE(repo.set_version_file(org_.id, v1->id, "org/x/generated/a.pdf", std::string(64, 'a'),
                                      "application/pdf", 1234));
    // Пока указатель не переставлен — файла у документа «нет».
    auto before = repo.find_in_org(doc.id, org_.id);
    ASSERT_TRUE(before);
    EXPECT_FALSE(before->s3_key.has_value());
    ASSERT_TRUE(repo.set_current_version(org_.id, doc.id, v1->id));
    auto after = repo.find_in_org(doc.id, org_.id);
    ASSERT_TRUE(after);
    ASSERT_TRUE(after->s3_key);
    EXPECT_EQ(*after->s3_key, "org/x/generated/a.pdf");
    EXPECT_EQ(after->size_bytes.value_or(0), 1234);
}

TEST_F(DocumentsTest, VersionsAreOrgIsolated) {
    Ledger::DocumentRepository repo;
    auto doc = repo.create(org_.id, "invoice", "generated", "draft");
    EXPECT_TRUE(repo.list_versions(other_org_.id, doc.id).empty());
    EXPECT_FALSE(repo.find_version(other_org_.id, doc.id, 1).has_value());
    EXPECT_FALSE(repo.set_current_version(other_org_.id, doc.id, "00000000-0000-0000-0000-000000000000"));
}

TEST_F(DocumentsTest, MigrationBackfilledExistingDocuments) {
    // Каждый документ обязан иметь хотя бы одну версию — иначе бэкфилл
    // 018 пропустил строки, и старые PDF стали недостижимы.
    auto orphans = Database::get().execute_read([](auto& txn) {
        auto r = txn.exec("SELECT COUNT(*) FROM documents d "
                          " WHERE NOT EXISTS (SELECT 1 FROM document_versions v WHERE v.document_id = d.id)");
        return r.at(0).at(0).template as<long>();
    });
    EXPECT_EQ(orphans, 0);
}
```

- [ ] **Step 7: Гейты и коммит**

```bash
clang-format-17 -i src/ledger/DocumentVersion.hpp src/ledger/Document.hpp src/ledger/DocumentRepository.hpp \
  src/docgen/RenderJob.hpp tests/test_helpers.hpp tests/integration/test_documents.cpp
./scripts/check-openapi-drift.sh && ./scripts/check-test-buckets.sh && make lint-openapi
git add migrations/018_document_versions.sql src/ledger/DocumentVersion.hpp src/ledger/Document.hpp \
  src/ledger/DocumentRepository.hpp src/docgen/RenderJob.hpp docs/openapi.yaml tests/test_helpers.hpp \
  tests/integration/test_documents.cpp
git commit -m "feat(documents): move file metadata and input snapshots into document_versions"
```

---

### Task 9: Правка документа через тот же allowlist + история версий

**Files:**
- Modify: `src/api/LedgerDocumentsController.hpp` (три новых маршрута + `downloadUrl`)
- Modify: `src/api/Endpoints.hpp`
- Modify: `docs/openapi.yaml`
- Modify: `frontend/src/lib/api/schema.gen.ts` (регенерация)
- Test: `tests/integration/test_documents_api.cpp`

**Interfaces:**
- Consumes: `Docgen::InputPolicy::input_is_caller_authored / editable_fields / apply_derived_amount` (задача 4); `DocumentRepository::add_version / list_versions / find_version / latest_version` (задача 8); `Api::Validation::merge_allowed_extra` (`src/api/Validation.hpp:392`); `Tenancy::OrgPerm::Resource::kDocuments|kHrDocs` (задача 6).
- Produces (на это опирается задача 13, а `version_id` в полезной нагрузке — задача 10):
  - `POST /api/v1/documents/{id}/versions` — тело `{input: {...}}`, ответ **202** `{document_id, version_id, version_no, render_queued}`;
  - `GET /api/v1/documents/{id}/versions` — ответ `{data: [DocumentVersion...]}`, по возрастанию `version_no`;
  - `POST /api/v1/documents/{id}/versions/{version_no}/download-url` — ответ `{url}`, TTL 300 с;
  - полезная нагрузка джобы `docgen.render` дополняется ключом `version_id` (строка UUID).

**Правка обязана соблюдать тот же allowlist, что и создание.** Это прямое следствие §3.1: `input_snapshot` — ровно то, что рендерит джоба, поэтому приём его целиком заново открывает дыру подделки, которую P2 закрыл (PDF декларации с балансом 1 ₸ при правдивом XML той же записи). Тело правки — **не снапшот**.

Правило по слагу:

- слаг из `Docgen::InputPolicy::generate_slugs()` (первичка) — за ним в БД ничего не стоит, поэтому весь `input` авторский; принимается тот же объект, что и в `POST /documents/generate`, и проходит ту же деривацию (`total_tiyn` → `total` + `total_words`, клиентские `total`/`total_words` → 422);
- любой другой слаг (`fno_910`, `fno_300`, `payslip`, `hr_order`, `labor_contract`) — принимаются ТОЛЬКО ключи из `Docgen::InputPolicy::editable_fields(slug)`, и они мерджатся поверх снапшота предыдущей версии: каждая серверно выведенная величина переносится из версии, которая была отрендерена из авторитетных строк, неизменной;
- документ с `source` = `uploaded` или `email` не редактируется вовсе: у него `input_snapshot` пуст по построению (`migrations/010_documents.sql`), редактировать нечего → **409**;
- документ без `template_slug` — тоже 409 (тот же случай).

- [ ] **Step 1: Падающие тесты**

Дописать в `tests/integration/test_documents_api.cpp`:

```cpp
TEST_F(LedgerDocumentsApiTest, EditCreatesANewVersionAndKeepsTheOldPdf) {
    auto p = member("edit@example.com", org_.id, "accountant");
    const std::string doc_id = seed_rendered_invoice(/*total_tiyn=*/1234567);  // хелпер фикстуры
    auto before = /* GET /documents/{doc_id} */;
    const std::string old_key = body_of(before)["data"]["s3_key"].get<std::string>();

    const json edit = {{"input", make_invoice_input(/*total_tiyn=*/200000)}};
    auto resp = call_create_version(ctrl, authed_json(Post, edit, p), doc_id);
    ASSERT_EQ(resp->getStatusCode(), k202Accepted);
    EXPECT_EQ(body_of(resp)["version_no"].get<int>(), 2);

    auto versions = call_list_versions(ctrl, authed(Get, p), doc_id);
    ASSERT_EQ(body_of(versions)["data"].size(), 2u);
    // Старый файл на месте и скачивается по своему номеру версии.
    auto url1 = call_version_download_url(ctrl, authed(Post, p), doc_id, 1);
    EXPECT_EQ(url1->getStatusCode(), k200OK);
    EXPECT_FALSE(body_of(url1)["url"].get<std::string>().empty());
    // Указатель ещё на версии 1: рендер второй не закончился.
    Ledger::DocumentRepository repo;
    auto doc = repo.find_in_org(doc_id, org_.id);
    ASSERT_TRUE(doc);
    EXPECT_EQ(*doc->s3_key, old_key);
    EXPECT_EQ(doc->latest_version_no, 2);
}

TEST_F(LedgerDocumentsApiTest, EditRejectsFieldsOutsideTheCreationAllowlist) {
    auto p = member("edit2@example.com", org_.id, "accountant");
    const std::string filing_doc = seed_fno910_document();  // слаг fno_910
    const json edit = {{"input", {{"director", "С.С."}, {"accountant", "И.И."}, {"tax_tenge", "1,00"}}}};
    auto resp = call_create_version(ctrl, authed_json(Post, edit, p), filing_doc);
    EXPECT_EQ(resp->getStatusCode(), k422UnprocessableEntity);
    EXPECT_EQ(body_of(resp)["errors"][0]["code"].get<std::string>(), "not_allowed_override");
}

TEST_F(LedgerDocumentsApiTest, EditOfAServerBuiltFormCarriesDerivedFiguresForward) {
    auto p = member("edit3@example.com", org_.id, "accountant");
    const std::string filing_doc = seed_fno910_document();
    Ledger::DocumentRepository repo;
    const auto v1 = repo.find_version(org_.id, filing_doc, 1);
    ASSERT_TRUE(v1);
    const std::string tax_words = (*v1->input_snapshot)["tax_words"].get<std::string>();

    const json edit = {{"input", {{"director", "Новый директор"}}}};
    auto resp = call_create_version(ctrl, authed_json(Post, edit, p), filing_doc);
    ASSERT_EQ(resp->getStatusCode(), k202Accepted);
    const auto v2 = repo.find_version(org_.id, filing_doc, 2);
    ASSERT_TRUE(v2);
    EXPECT_EQ((*v2->input_snapshot)["director"].get<std::string>(), "Новый директор");
    EXPECT_EQ((*v2->input_snapshot)["tax_words"].get<std::string>(), tax_words);
    EXPECT_EQ((*v2->input_snapshot)["tax_tenge"], (*v1->input_snapshot)["tax_tenge"]);
}

TEST_F(LedgerDocumentsApiTest, UploadedAndEmailDocumentsCannotBeEdited) {
    auto p = member("edit4@example.com", org_.id, "accountant");
    Ledger::DocumentRepository repo;
    for (const auto* source : {"uploaded", "email"}) {
        auto doc = repo.create(org_.id, "incoming", source, "inbox");
        auto resp = call_create_version(ctrl, authed_json(Post, json{{"input", json::object()}}, p), doc.id);
        EXPECT_EQ(resp->getStatusCode(), k409Conflict) << source;
        EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "not_editable") << source;
    }
}

TEST_F(LedgerDocumentsApiTest, VersionDownloadUrlIsFourOhFourForAnUnknownVersionNumber) {
    auto p = member("edit5@example.com", org_.id, "accountant");
    const std::string doc_id = seed_rendered_invoice(1234567);
    auto resp = call_version_download_url(ctrl, authed(Post, p), doc_id, 99);
    EXPECT_EQ(resp->getStatusCode(), k404NotFound);
}

TEST_F(LedgerDocumentsApiTest, ViewerCannotEditButCanListVersions) {
    auto v = member("viewer@example.com", org_.id, "viewer");
    const std::string doc_id = seed_rendered_invoice(1234567);
    EXPECT_EQ(call_create_version(ctrl, authed_json(Post, json{{"input", json::object()}}, v), doc_id)
                  ->getStatusCode(),
              k403Forbidden);
    EXPECT_EQ(call_list_versions(ctrl, authed(Get, v), doc_id)->getStatusCode(), k200OK);
}
```

- [ ] **Step 2: Три маршрута в контроллере**

В `src/api/LedgerDocumentsController.hpp` — в `METHOD_LIST_BEGIN/END` добавить строки (по одной на строку: `scripts/check-routes-registered.sh` сканирует `ADD_METHOD_TO` построчно, перенос строки его сломает):

```cpp
    ADD_METHOD_TO(LedgerDocumentsController::listVersions, "/api/v1/documents/{1}/versions", Get);
    ADD_METHOD_TO(LedgerDocumentsController::createVersion, "/api/v1/documents/{1}/versions", Post);
    ADD_METHOD_TO(LedgerDocumentsController::versionDownloadUrl, "/api/v1/documents/{1}/versions/{2}/download-url", Post);
```

`listVersions` — read-гейт по типу документа (как `get` в задаче 7), затем `repo.list_versions(ctx.org_id, id)` → `Response::list(data)`.

`versionDownloadUrl` — read-гейт, `find_version(ctx.org_id, id, version_no)`; отсутствует → 404; `s3_key` пуст → 409 `no_file`; иначе `s3->presign(*v->s3_key, "GET", kDownloadTtlSec)`. `version_no` парсится из строкового сегмента: не число или ≤ 0 → 400 `invalid_version`.

`createVersion` — полное тело:

```cpp
    // -------------------------------------------------------------------
    // POST /api/v1/documents/{id}/versions — правка документа: новая
    // версия + новый рендер, предыдущий PDF остаётся. Тело — {input},
    // и это НЕ снапшот: принимается ровно тот же набор полей, что и при
    // создании документа этого шаблона (см. Docgen::InputPolicy), всё
    // остальное переносится из предыдущей версии. Приём снапшота целиком
    // воспроизвёл бы дыру подделки, закрытую в P2.
    // -------------------------------------------------------------------
    void createVersion(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed document id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        if (body.contains("input") && !body["input"].is_object()) {
            Validation::Errors errs;
            errs.add("input", "not_object", "must be a JSON object");
            callback(Validation::response_400(errs));
            return;
        }
        const json client_input = body.value("input", json::object());

        with_repo_errors(callback, "documents createVersion", [&] {
            Ledger::DocumentRepository repo;
            auto found = repo.find_in_org(id, ctx.org_id);
            if (!found) {
                callback(ErrorResponse::not_found("document"));
                return;
            }
            const char* resource = found->doc_type == "hr" ? Tenancy::OrgPerm::Resource::kHrDocs
                                                           : Tenancy::OrgPerm::Resource::kDocuments;
            if (!Tenancy::OrgPerm::allows(ctx.role, resource, Tenancy::OrgPerm::Action::kWrite)) {
                callback(ErrorResponse::forbidden("org_role_denied",
                                                  "Your role in this organization is not allowed to edit this document"));
                return;
            }
            // Загруженные и пришедшие почтой документы не редактируются: у
            // них input_snapshot пуст по построению, редактировать нечего.
            if (found->source != "generated" || !found->template_slug || found->template_slug->empty()) {
                callback(ErrorResponse::conflict("not_editable",
                                                 "Only documents generated from a template can be edited"));
                return;
            }
            if (found->voided_at) {  // колонка появляется в задаче 11; до неё условие не пишется
                callback(ErrorResponse::conflict("document_voided", "A voided document cannot be edited"));
                return;
            }
            const std::string slug = *found->template_slug;

            auto previous = repo.latest_version(ctx.org_id, id);
            if (!previous) {
                spdlog::error("documents createVersion: document {} has no versions", id);
                callback(ErrorResponse::internal_error());
                return;
            }

            json input;
            if (Docgen::InputPolicy::input_is_caller_authored(slug)) {
                // Первичка: весь объект авторский, ровно как в
                // POST /documents/generate — и та же деривация суммы.
                input = client_input;
                std::string bad_field, bad_code, bad_message;
                if (!Docgen::InputPolicy::apply_derived_amount(slug, input, bad_field, bad_code, bad_message)) {
                    callback(Validation::response_422(bad_field, bad_code, bad_message));
                    return;
                }
            } else {
                // Серверная форма: база — снапшот предыдущей версии,
                // сверху ложатся ТОЛЬКО allowlisted-ключи. Любой другой —
                // 422 not_allowed_override, тем же механизмом, что при
                // создании.
                input = previous->input_snapshot ? *previous->input_snapshot : json::object();
                if (!Validation::merge_allowed_extra(
                        input, client_input, Docgen::InputPolicy::editable_fields(slug), callback))
                    return;
            }

            Docgen::TemplateRegistry registry;
            auto info = registry.latest(slug);
            if (!info) {
                spdlog::error("documents createVersion: template '{}' not found on disk", slug);
                callback(ErrorResponse::internal_error());
                return;
            }
            if (auto err = Docgen::TemplateRegistry::validate(*info, input)) {
                callback(Validation::response_422("input", "schema_validation_failed", *err));
                return;
            }
            API_REQUIRE_JOBS_READY(callback);

            auto version = repo.add_version(ctx.org_id,
                                            id,
                                            std::optional<nlohmann::json>{input},
                                            std::optional<std::string>{info->version_str},
                                            std::optional<std::string>{ctx.user_id});

            // Best-effort enqueue — та же посадка, что у
            // DocgenController::generate: версия уже существует, и сбой
            // Redis не должен превращать её в 500 без номера, по которому
            // клиент мог бы опрашивать состояние.
            json payload = {{"org_id", ctx.org_id},
                            {"document_id", id},
                            {"version_id", version.id},
                            {"slug", slug},
                            {"input", input}};
            bool render_queued = false;
            try {
                auto job = Jobs::get().submit(kRenderJobType, payload);
                spdlog::debug("documents createVersion: version {} enqueued as job {}", version.id, job.id);
                render_queued = true;
            } catch (const std::exception& e) {
                spdlog::error("documents createVersion: enqueue docgen.render for version {} failed: {}",
                              version.id,
                              e.what());
            }
            const json response_body = {{"document_id", id},
                                        {"version_id", version.id},
                                        {"version_no", version.version_no},
                                        {"render_queued", render_queued}};
            callback(Response::accepted(response_body));
        });
    }
```

`kRenderJobType` в этом контроллере ещё не объявлен — добавить рядом с `kDownloadTtlSec`:

```cpp
    /// Тот же тип джобы, который ставит DocgenController::generate.
    static constexpr const char* kRenderJobType = "docgen.render";
```

и `#include "jobs/Jobs.hpp"`, `#include "docgen/TemplateRegistry.hpp"`, `#include "docgen/InputPolicy.hpp"`.

Замечание про порядок задач: условие `found->voided_at` относится к колонке, которую вводит задача 11. Если задача 9 выполняется раньше — строку не писать, а задача 11 обязана её добавить (это явно записано в её шагах).

- [ ] **Step 3: `downloadUrl` документа — оговорка про пресайн**

В Doxygen `downloadUrl` дописать: «Пресайн, выданный до аннулирования или до вытеснения версии, живёт до истечения TTL (300 с) — это принятая экспозиция, а не дефект: отзыв уже выданной ссылки требовал бы прокси-скачивания, которого в системе нет».

- [ ] **Step 4: Тройная синхронизация**

`src/api/Endpoints.hpp` — три строки рядом с существующими `documents`:

```cpp
        {"GET", "/api/v1/documents/{id}/versions", "List a document's versions (oldest first)"},
        {"POST", "/api/v1/documents/{id}/versions", "Edit a document: create a new version and queue its render"},
        {"POST", "/api/v1/documents/{id}/versions/{version_no}/download-url", "Mint a presigned download URL for one version (GET, TTL 300s)"},
```

`docs/openapi.yaml` — три блока путей рядом с `/api/v1/documents/{id}/download-url`, плюс схемы компонентов `DocumentVersion`, `DocumentVersionListResponse`, `CreateDocumentVersionRequest` (`{input: object}`), `CreateDocumentVersionResponse` (`{document_id, version_id, version_no, render_queued}`). Ответы: 200/202, `400` (кривой id или номер версии), `403` (`org_role_denied`), `404` (документ или версия), `409` (`not_editable`, `no_file`, `document_voided`), `422` (`schema_validation_failed`, `not_allowed_override`), `503` (`jobs_disabled`, `presign_unsupported`).

- [ ] **Step 5: Гейты, регенерация клиента, коммит**

```bash
clang-format-17 -i src/api/LedgerDocumentsController.hpp src/api/Endpoints.hpp tests/integration/test_documents_api.cpp
./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh && ./scripts/check-test-buckets.sh
make lint-openapi
(cd frontend && npm run codegen && npx tsc --noEmit)
git add src/api/LedgerDocumentsController.hpp src/api/Endpoints.hpp docs/openapi.yaml \
  frontend/src/lib/api/schema.gen.ts tests/integration/test_documents_api.cpp
git commit -m "feat(documents): edit through the creation allowlist, with version history endpoints"
```

---

### Task 10: `RenderJob` — адресация по версии, no-op на аннулированном и вытесненном

**Files:**
- Modify: `src/docgen/RenderJob.hpp` (`process_job`, строки ~195-235)
- Modify: `src/api/DocgenController.hpp` (полезная нагрузка джобы), `src/api/TaxController.hpp`, `src/api/PayrollController.hpp`, `src/api/HrController.hpp` (их `finish_generate_document` / эквиваленты)
- Modify: `src/ledger/DocumentRepository.hpp` (`version_render_state`)
- Test: `tests/integration/test_render_job.cpp`

**Interfaces:**
- Consumes (задача 8): `DocumentRepository::latest_version`, `find_version`, `set_version_file`, `set_current_version`; `Ledger::DocumentVersion`.
- Produces (на это опирается задача 11 — она добавляет в состояние ветку `voided`):
  - полезная нагрузка `docgen.render` обязана нести ключ `version_id` (строка UUID) наряду с `org_id`, `document_id`, `slug`, `input`;
  - `enum class Ledger::VersionRenderState { kMissing, kRenderable, kSuperseded, kVoided };`
  - `Ledger::VersionRenderState DocumentRepository::version_render_state(const std::string& org_id, const std::string& document_id, const std::string& version_id)`.

**Что чинится.** `RenderJob::process_job` сегодня делает `set_file(...)` + `set_status(org, doc, "final")` без единой проверки состояния. Отсюда три поломки:

1. `set_status(..., "final")` на аннулированном документе **снимет аннулирование** (а с отдельной колонкой `voided_at` — оставит противоречивое `final` + `voided_at`);
2. `set_file()` не проверял статус вообще (в отличие от `set_pending_upload`, где есть `AND status = 'draft'`) — повторный запуск джобы перезаписывал файл текущей версии;
3. состояние «версия N+1 создана, рендер не закончился» не было определено: указатель должен оставаться на N, пока рендер не удался, иначе скачивание отдаст 500 или старый файл под новым номером.

- [ ] **Step 1: Состояние версии в репозитории**

В `src/ledger/DocumentRepository.hpp`:

```cpp
/// Можно ли класть результат рендера в эту версию.
enum class VersionRenderState {
    kMissing,      ///< версии нет в этой организации (или документ удалён)
    kRenderable,   ///< самая новая версия живого документа — результат принимается
    kSuperseded,   ///< поверх неё уже создана более новая версия — джоба no-op
    kVoided,       ///< документ аннулирован — джоба no-op
};
```

```cpp
    /**
     * @brief Решение «принимать ли результат рендера для этой версии».
     * @details Один запрос вместо трёх чтений: гонка «версия вытеснена
     *          между проверкой и записью» здесь всё равно возможна, и
     *          защищает от неё not-superseded-условие в самом UPDATE
     *          set_current_version(), а эта функция даёт джобе внятную
     *          причину не делать ничего и не шуметь ошибкой.
     */
    VersionRenderState version_render_state(const std::string& org_id,
                                            const std::string& document_id,
                                            const std::string& version_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT (d.voided_at IS NOT NULL) AS voided, "
                "       (v.version_no = (SELECT MAX(vv.version_no) FROM document_versions vv "
                "                         WHERE vv.document_id = d.id)) AS newest "
                "  FROM document_versions v JOIN documents d ON d.id = v.document_id "
                " WHERE v.id = $1 AND v.document_id = $2 AND v.org_id = $3",
                version_id,
                document_id,
                org_id);
            if (r.empty())
                return VersionRenderState::kMissing;
            if (r[0]["voided"].template as<bool>())
                return VersionRenderState::kVoided;
            if (!r[0]["newest"].template as<bool>())
                return VersionRenderState::kSuperseded;
            return VersionRenderState::kRenderable;
        });
    }
```

Колонка `documents.voided_at` появляется в задаче 11. Если задача 10 идёт раньше — временно заменить `(d.voided_at IS NOT NULL) AS voided` на `FALSE AS voided`, а задача 11 обязана вернуть настоящее условие (это записано в её шагах).

`set_current_version` (задача 8) обязан нести защиту от гонки прямо в SQL:

```cpp
    bool set_current_version(const std::string& org_id, const std::string& document_id, const std::string& version_id) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE documents d SET current_version_id = $3 "
                " WHERE d.id = $1 AND d.org_id = $2 "
                "   AND EXISTS (SELECT 1 FROM document_versions v "
                "                WHERE v.id = $3 AND v.document_id = d.id AND v.org_id = d.org_id "
                "                  AND v.version_no = (SELECT MAX(vv.version_no) FROM document_versions vv "
                "                                       WHERE vv.document_id = d.id)) "
                "RETURNING d.id",
                document_id,
                org_id,
                version_id);
            return !r.empty();
        });
    }
```

- [ ] **Step 2: `process_job` — новая концовка**

В `src/docgen/RenderJob.hpp` заменить хвост `process_job` (от `Ledger::DocumentRepository documents;` до `return json{...}`) на:

```cpp
    Ledger::DocumentRepository documents;

    // Версия, в которую кладём результат. Ключ обязателен: адресация по
    // document_id перезаписывала бы файл текущей версии при каждом
    // повторе джобы (историю правок это молча стирает).
    const std::string version_id = payload.at("version_id").get<std::string>();

    // Состояние проверяется ДО загрузки в S3: аннулированный или
    // вытесненный документ не должен получать ни объекта в хранилище, ни
    // записи в БД. Возврат — не исключение: джоба отработала штатно,
    // просто её результат больше никому не нужен, и повтор её не спасёт.
    const auto state = documents.version_render_state(org_id, document_id, version_id);
    if (state != Ledger::VersionRenderState::kRenderable) {
        const char* reason = state == Ledger::VersionRenderState::kMissing     ? "missing"
                             : state == Ledger::VersionRenderState::kVoided    ? "voided"
                                                                               : "superseded";
        spdlog::info("docgen: skipping render of version {} of document {}: {}", version_id, document_id, reason);
        return json{{"document_id", document_id}, {"version_id", version_id}, {"skipped", reason}};
    }

    const std::string checksum = Utils::Crypto::sha256_hex(pdf_bytes);
    const std::string key = Files::org_key(org_id, "generated", slug + ".pdf");
    Storage::get().put(key, pdf_bytes, "application/pdf");

    if (!documents.set_version_file(
            org_id, version_id, key, checksum, "application/pdf", static_cast<long long>(pdf_bytes.size())))
        throw std::runtime_error("docgen: set_version_file found no version " + version_id + " in org " + org_id);

    // Указатель переставляется ПОСЛЕ того, как файл лёг в версию, и только
    // если версия всё ещё самая новая: «версия N+1 создана, рендер не
    // закончился» = указатель остаётся на N. Проигранная гонка (пока мы
    // рендерили, создали N+2) — не ошибка: следующая джоба поставит
    // указатель на свою версию.
    if (!documents.set_current_version(org_id, document_id, version_id)) {
        spdlog::info("docgen: version {} of document {} was superseded before publication", version_id, document_id);
        return json{{"document_id", document_id}, {"version_id", version_id}, {"skipped", "superseded"}};
    }
    // Статус трогаем только у сгенерированных черновиков: 'sent' и
    // входящий жизненный цикл (inbox/recognized/linked/archived) джоба
    // откатывать не имеет права.
    documents.set_status_if(org_id, document_id, "draft", "final");

    return json{{"document_id", document_id},
                {"version_id", version_id},
                {"slug", slug},
                {"key", key},
                {"checksum_sha256", checksum},
                {"size_bytes", pdf_bytes.size()}};
```

Обратите внимание: `pdf_bytes` вычисляется ВЫШЕ по функции — блок проверки состояния поставьте после чтения PDF, но до `Storage::get().put`. Заодно `render_and_compile` можно было бы вызывать после проверки; не переносите его — он дороже, но именно он и есть проверка «вход валиден», а лишний рендер вытесненной версии дешевле сложной перестановки.

Новый метод в репозитории (вместо безусловного `set_status`):

```cpp
    /// Перевести статус, только если он всё ещё @p from. Возвращает false
    /// и когда документа нет, и когда он уже в другом состоянии — джобе
    /// достаточно знать, что ничего не записано.
    bool set_status_if(const std::string& org_id,
                       const std::string& id,
                       const std::string& from,
                       const std::string& to) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE documents SET status = $4 WHERE id = $1 AND org_id = $2 AND status = $3 RETURNING id",
                id,
                org_id,
                from,
                to);
            return !r.empty();
        });
    }
```

- [ ] **Step 3: `version_id` во всех четырёх местах постановки джобы**

Каждый производитель `docgen.render` обязан класть в полезную нагрузку `version_id`. Сразу после `repo.create(...)` (документ уже несёт версию 1) взять её id и подставить:

```cpp
            auto first_version = documents.latest_version(ctx.org_id, created.id);
            json payload = {{"org_id", ctx.org_id},
                            {"document_id", created.id},
                            {"version_id", first_version ? first_version->id : std::string{}},
                            {"slug", template_slug},
                            {"input", input}};
```

Места: `src/api/DocgenController.hpp` (`generate`), `src/api/TaxController.hpp` (`createFiling`), `src/api/PayrollController.hpp` (`generatePayslip`), `src/api/HrController.hpp` (`finish_generate_document` — там документ создаётся один раз для обоих HR-маршрутов, правка одна). В `HrController::finish_generate_document` репозиторий уже под рукой; если нет — завести локальный `Ledger::DocumentRepository documents;`.

Пустой `version_id` (если версия почему-то не нашлась) даст в джобе `kMissing` и штатный skip вместо падения — это лучше, чем 500 на запросе, который уже создал документ.

- [ ] **Step 4: Падающие тесты**

Дописать в `tests/integration/test_render_job.cpp`:

```cpp
TEST_F(RenderJobTest, WritesTheFileIntoTheAddressedVersionAndPublishesIt) {
    auto doc = repo_.create(org_.id, "invoice", "generated", "draft", std::nullopt, "invoice", "v1",
                            std::optional<nlohmann::json>{valid_invoice_input()});
    auto v1 = repo_.latest_version(org_.id, doc.id);
    ASSERT_TRUE(v1);
    Docgen::process_job(json{{"org_id", org_.id},
                             {"document_id", doc.id},
                             {"version_id", v1->id},
                             {"slug", "invoice"},
                             {"input", valid_invoice_input()}});
    auto after = repo_.find_in_org(doc.id, org_.id);
    ASSERT_TRUE(after);
    ASSERT_TRUE(after->current_version_id);
    EXPECT_EQ(*after->current_version_id, v1->id);
    EXPECT_EQ(after->status, "final");
    ASSERT_TRUE(after->s3_key);
}

TEST_F(RenderJobTest, IsANoOpForASupersededVersion) {
    auto doc = repo_.create(org_.id, "invoice", "generated", "draft", std::nullopt, "invoice", "v1",
                            std::optional<nlohmann::json>{valid_invoice_input()});
    auto v1 = repo_.latest_version(org_.id, doc.id);
    ASSERT_TRUE(v1);
    repo_.add_version(org_.id, doc.id, std::optional<nlohmann::json>{valid_invoice_input()}, std::string("v1"),
                      std::nullopt);  // v2 вытесняет v1
    auto result = Docgen::process_job(json{{"org_id", org_.id},
                                           {"document_id", doc.id},
                                           {"version_id", v1->id},
                                           {"slug", "invoice"},
                                           {"input", valid_invoice_input()}});
    EXPECT_EQ(result["skipped"].get<std::string>(), "superseded");
    auto after = repo_.find_in_org(doc.id, org_.id);
    ASSERT_TRUE(after);
    EXPECT_FALSE(after->current_version_id.has_value());
}

TEST_F(RenderJobTest, DoesNotOverwriteAnAlreadyPublishedVersionOnRerun) {
    auto doc = repo_.create(org_.id, "invoice", "generated", "draft", std::nullopt, "invoice", "v1",
                            std::optional<nlohmann::json>{valid_invoice_input()});
    auto v1 = repo_.latest_version(org_.id, doc.id);
    const json payload = {{"org_id", org_.id}, {"document_id", doc.id}, {"version_id", v1->id},
                          {"slug", "invoice"}, {"input", valid_invoice_input()}};
    Docgen::process_job(payload);
    const auto first = repo_.find_version(org_.id, doc.id, 1);
    ASSERT_TRUE(first);
    const std::string first_checksum = first->checksum_sha256.value_or("");
    Docgen::process_job(payload);  // повтор
    const auto again = repo_.find_version(org_.id, doc.id, 1);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->checksum_sha256.value_or(""), first_checksum);
    EXPECT_EQ(again->version_no, 1);
}

TEST_F(RenderJobTest, DoesNotResurrectAVoidedDocument) {
    // Требует колонок задачи 11; если она ещё не выполнена — тест
    // помечается DISABLED_ и включается ею же.
    auto doc = repo_.create(org_.id, "invoice", "generated", "draft", std::nullopt, "invoice", "v1",
                            std::optional<nlohmann::json>{valid_invoice_input()});
    auto v1 = repo_.latest_version(org_.id, doc.id);
    ASSERT_TRUE(repo_.void_document(org_.id, doc.id, user_.id, "ошибка в реквизитах"));
    auto result = Docgen::process_job(json{{"org_id", org_.id}, {"document_id", doc.id}, {"version_id", v1->id},
                                           {"slug", "invoice"}, {"input", valid_invoice_input()}});
    EXPECT_EQ(result["skipped"].get<std::string>(), "voided");
    auto after = repo_.find_in_org(doc.id, org_.id);
    ASSERT_TRUE(after);
    EXPECT_TRUE(after->voided_at.has_value());
    EXPECT_NE(after->status, "final");
}
```

- [ ] **Step 5: Гейты и коммит**

```bash
clang-format-17 -i src/docgen/RenderJob.hpp src/ledger/DocumentRepository.hpp src/api/DocgenController.hpp \
  src/api/TaxController.hpp src/api/PayrollController.hpp src/api/HrController.hpp \
  tests/integration/test_render_job.cpp
./scripts/check-test-buckets.sh
git add src/docgen/RenderJob.hpp src/ledger/DocumentRepository.hpp src/api/DocgenController.hpp \
  src/api/TaxController.hpp src/api/PayrollController.hpp src/api/HrController.hpp \
  tests/integration/test_render_job.cpp
git commit -m "fix(docgen): key the render job on version_id and skip voided or superseded versions"
```

---

### Task 11: Удаление против аннулирования

**Files:**
- Create: `migrations/019_document_voiding.sql`
- Modify: `src/ledger/Document.hpp` (три поля + `to_json`), `src/ledger/DocumentRepository.hpp` (`kColumns`, `has_posted_entry_link`, `remove`, `void_document`, `version_render_state`)
- Modify: `src/api/LedgerDocumentsController.hpp` (два маршрута + условие `voided_at` в `createVersion`)
- Modify: `src/api/Endpoints.hpp`, `docs/openapi.yaml`, `frontend/src/lib/api/schema.gen.ts`
- Test: `tests/integration/test_documents_api.cpp`

**Interfaces:**
- Consumes (задачи 6, 8, 10): `Tenancy::OrgPerm::Resource::kDocuments|kHrDocs`, `Ledger::DocumentVersion`, `DocumentRepository::version_render_state`.
- Produces (на это опираются задачи 10 и 13):
  - `Ledger::Document` дополнительно несёт `std::optional<std::string> voided_at, voided_by_user_id, void_reason;`
  - `bool DocumentRepository::has_posted_entry_link(const std::string& org_id, const std::string& document_id)`;
  - `enum class Ledger::DeleteOutcome { kDeleted, kNotFound, kHasPostedEntries, kReferenced };`
  - `Ledger::DeleteOutcome DocumentRepository::remove(const std::string& org_id, const std::string& document_id)`;
  - `bool DocumentRepository::void_document(const std::string& org_id, const std::string& document_id, const std::string& user_id, const std::string& reason)`;
  - `DELETE /api/v1/documents/{id}` → 204 / 403 / 404 / 409;
  - `POST /api/v1/documents/{id}/void` → тело `{reason}`, ответ 200 `{data: Document}`.

**Ключ — не статус, а связь с проведённой проводкой.** Первая редакция спеки предлагала «удалять черновики» по `status='draft'`, но такой статус бывает только у `source='generated'`: загруженные и пришедшие почтой документы живут в цикле `inbox → recognized → linked → archived` и не удалились бы никогда — ошибочно загруженный скан оставался бы навсегда. Правило:

```sql
NOT EXISTS (SELECT 1 FROM document_entries de
            JOIN journal_entries je ON je.id = de.entry_id
            WHERE de.document_id = $1 AND je.status IN ('posted','reversed'))
```

Иначе — только аннулирование. **Аннулирование хранится колонками**, а не значением статуса: `void` в `status` затёрло бы, был ли документ `final` или `sent` — ровно то, что нужно аудиту, — и пересеклось бы с уже занятым `archived`.

- [ ] **Step 1: Миграция**

`migrations/019_document_voiding.sql`:

```sql
-- Аннулирование документа отдельными колонками (спека P3 §4.2).
-- НЕ значением documents.status: одна колонка обслуживает два жизненных
-- цикла, и запись туда 'void' стёрла бы, был документ 'final' или 'sent'
-- — ровно то, что нужно аудиту. Плюс 'archived' уже занимает нишу
-- терминального состояния, и два неразличимых финала пользователю
-- объяснить нельзя.
--
-- Политика хранения объектов S3 (принятое решение, §4.2): ни физическое
-- удаление документа, ни аннулирование, ни каскад организации объектов в
-- хранилище НЕ трогают. Задачи-сборщика в P3 не появляется; удаляются
-- только метаданные. Это записано здесь явно, чтобы «молчаливая утечка
-- объектов» была осознанной политикой, а не забытым краем.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

ALTER TABLE documents ADD COLUMN IF NOT EXISTS voided_at         TIMESTAMPTZ;
ALTER TABLE documents ADD COLUMN IF NOT EXISTS voided_by_user_id UUID REFERENCES users(id) ON DELETE SET NULL;
ALTER TABLE documents ADD COLUMN IF NOT EXISTS void_reason       TEXT;

-- Все три колонки заполняются вместе либо не заполняются вовсе:
-- «аннулирован без причины и без автора» — не состояние, а потерянная
-- запись аудита.
ALTER TABLE documents DROP CONSTRAINT IF EXISTS documents_void_fields_together;
ALTER TABLE documents ADD CONSTRAINT documents_void_fields_together CHECK (
    (voided_at IS NULL AND voided_by_user_id IS NULL AND void_reason IS NULL)
    OR (voided_at IS NOT NULL AND void_reason IS NOT NULL)
);

CREATE INDEX IF NOT EXISTS idx_documents_org_voided ON documents (org_id, voided_at);
```

`voided_by_user_id` в CHECK'е допускает NULL и в аннулированном состоянии — иначе `ON DELETE SET NULL` при удалении пользователя нарушил бы constraint и заблокировал бы удаление, повторив ровно ту ошибку, которую задача 12 чинит для журнала.

- [ ] **Step 2: Домен и репозиторий**

В `src/ledger/Document.hpp` — три `std::optional<std::string>` поля (`voided_at`, `voided_by_user_id`, `void_reason`), их чтение в `from_row` и вывод в `to_json` (для NULL — `nullptr`). В `DocumentRepository::kColumns` дописать `d.voided_at, d.voided_by_user_id, d.void_reason`.

Три новых метода:

```cpp
    /// Есть ли у документа связь с ПРОВЕДЁННОЙ (или сторнированной)
    /// проводкой. Это, а не status, отделяет удаляемое от аннулируемого:
    /// 'draft' бывает только у source='generated', и по статусу
    /// ошибочно загруженный скан не удалился бы никогда.
    bool has_posted_entry_link(const std::string& org_id, const std::string& document_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT EXISTS (SELECT 1 FROM document_entries de "
                "                 JOIN journal_entries je ON je.id = de.entry_id "
                "                WHERE de.document_id = $1 AND de.org_id = $2 "
                "                  AND je.status IN ('posted','reversed'))",
                document_id,
                org_id);
            return r.at(0).at(0).template as<bool>();
        });
    }

    /**
     * @brief Физически удалить документ вместе с его версиями и связями.
     * @details Связь с ЧЕРНОВОЙ проводкой удалению не мешает:
     *          document_entries.document_id — ON DELETE CASCADE, черновик
     *          останется без основания, и это принято (факт удаления
     *          пишется в аудит вызывающим). Связь с ПРОВЕДЁННОЙ проводкой
     *          проверяется отдельно и даёт kHasPostedEntries.
     *
     *          hr_orders.document_id и tax_filings.document_id — NO ACTION
     *          (migrations/012_hr.sql, migrations/016_tax_filings.sql), и
     *          последний ещё и DEFERRABLE, то есть срабатывает на COMMIT.
     *          Поэтому SQLSTATE 23503 ловится здесь и превращается в
     *          kReferenced -> 409, а не всплывает 500-й: подписанный
     *          трудовой договор, на который ссылается приказ, физически
     *          уничтожить нельзя — только аннулировать.
     */
    DeleteOutcome remove(const std::string& org_id, const std::string& document_id) {
        if (has_posted_entry_link(org_id, document_id))
            return DeleteOutcome::kHasPostedEntries;
        try {
            return Database::get().execute_write([&](auto& txn) {
                auto r = txn.exec_params(
                    "DELETE FROM documents WHERE id = $1 AND org_id = $2 RETURNING id", document_id, org_id);
                return r.empty() ? DeleteOutcome::kNotFound : DeleteOutcome::kDeleted;
            });
        } catch (const pqxx::sql_error& e) {
            if (std::string_view(e.sqlstate()) == "23503")
                return DeleteOutcome::kReferenced;
            throw;
        }
    }

    /// Пометить документ аннулированным. Повторное аннулирование — no-op
    /// (`voided_at IS NULL` в WHERE): первое решение и его автор важнее
    /// последнего.
    bool void_document(const std::string& org_id,
                       const std::string& document_id,
                       const std::string& user_id,
                       const std::string& reason) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE documents SET voided_at = now(), voided_by_user_id = $3, void_reason = $4 "
                " WHERE id = $1 AND org_id = $2 AND voided_at IS NULL RETURNING id",
                document_id,
                org_id,
                user_id,
                reason);
            return !r.empty();
        });
    }
```

Ещё: в `version_render_state` (задача 10) вернуть настоящее условие `(d.voided_at IS NOT NULL) AS voided` вместо временного `FALSE AS voided`, и включить тест `RenderJobTest.DoesNotResurrectAVoidedDocument`, если он был `DISABLED_`.

- [ ] **Step 3: Два маршрута**

В `src/api/LedgerDocumentsController.hpp`:

```cpp
    ADD_METHOD_TO(LedgerDocumentsController::remove, "/api/v1/documents/{1}", Delete);
    ADD_METHOD_TO(LedgerDocumentsController::voidDocument, "/api/v1/documents/{1}/void", Post);
```

`remove` — гейт `kWrite` по типу документа (как в задаче 9), затем:

```cpp
            switch (repo.remove(ctx.org_id, id)) {
                case Ledger::DeleteOutcome::kDeleted:
                    // Объекты в S3 не удаляются — принятая политика
                    // хранения (migrations/019_document_voiding.sql).
                    // Факт удаления пишется в аудит, а не только в лог:
                    // связь с ЧЕРНОВОЙ проводкой каскадится молча, и
                    // черновик остаётся без основания — единственный
                    // след этого события живёт здесь.
                    Security::Audit::record(ctx.user_id,
                                            "document.delete",
                                            "document",
                                            id,
                                            {{"org_id", ctx.org_id}, {"doc_type", found->doc_type}});
                    callback(Response::no_content());
                    return;
                case Ledger::DeleteOutcome::kNotFound:
                    callback(ErrorResponse::not_found("document"));
                    return;
                case Ledger::DeleteOutcome::kHasPostedEntries:
                    callback(ErrorResponse::conflict(
                        "document_has_posted_entries",
                        "This document is linked to a posted journal entry — void it instead of deleting it"));
                    return;
                case Ledger::DeleteOutcome::kReferenced:
                    callback(ErrorResponse::conflict(
                        "document_referenced",
                        "This document is referenced by an HR order or a tax filing — void it instead of deleting it"));
                    return;
            }
```

Для `Security::Audit::record` нужен `#include "security/Audit.hpp"`; сигнатура — `record(actor_id, action, target_type, target_id, details = json::object())`, идиома вызова взята из `src/api/OrganizationsController.hpp:122`. `found` здесь — результат `repo.find_in_org(id, ctx.org_id)`, который надо прочитать ДО `remove()` (он же даёт `doc_type` для гейта прав). Аннулирование пишет в аудит так же: `Security::Audit::record(ctx.user_id, "document.void", "document", id, {{"reason", reason}});`.

`voidDocument` — гейт `kWrite`, тело `{reason}` обязательно и непусто после trim (иначе 400 `missing`/422 `blank`), документ должен существовать (404), уже аннулированный → 409 `already_voided`, иначе `void_document(...)` и `Response::ok({{"data", json(*repo.find_in_org(id, ctx.org_id))}})`.

В `createVersion` (задача 9) раскомментировать/добавить ветку:

```cpp
            if (found->voided_at) {
                callback(ErrorResponse::conflict("document_voided", "A voided document cannot be edited"));
                return;
            }
```

Если `Response::no_content()` в `src/api/HandlerSupport.hpp` отсутствует — использовать ту форму 204, которой уже пользуется `DELETE /api/v1/account/api-keys/{id}` в `src/api/ApiKeysController.hpp`, и повторить её дословно.

- [ ] **Step 4: Тройная синхронизация**

`src/api/Endpoints.hpp`:

```cpp
        {"DELETE", "/api/v1/documents/{id}", "Delete a document that is not linked to a posted journal entry"},
        {"POST", "/api/v1/documents/{id}/void", "Void a document (keeps the row, the file and the history)"},
```

`docs/openapi.yaml`: в `/api/v1/documents/{id}` добавить операцию `delete` (204/403/404/409); новый путь `/api/v1/documents/{id}/void` с телом `VoidDocumentRequest` (`{reason: string, minLength: 1}`) и ответом `DocumentDetailResponse`; в схему `Document` добавить `voided_at`, `voided_by_user_id`, `void_reason` (`type: ['string','null']`). В описании `delete` записать политику хранения S3: «объекты в хранилище не удаляются — удаляются только метаданные».

- [ ] **Step 5: Падающие тесты**

```cpp
TEST_F(LedgerDocumentsApiTest, DeletesADocumentWithNoPostedLink) {
    auto p = member("del@example.com", org_.id, "accountant");
    auto doc = repo_.create(org_.id, "incoming", "uploaded", "inbox");
    auto resp = call_delete(ctrl, authed(Delete, p), doc.id);
    EXPECT_EQ(resp->getStatusCode(), k204NoContent);
    EXPECT_FALSE(repo_.find_in_org(doc.id, org_.id).has_value());
}

TEST_F(LedgerDocumentsApiTest, DeletesADocumentLinkedOnlyToADraftEntry) {
    auto p = member("del2@example.com", org_.id, "accountant");
    auto doc = repo_.create(org_.id, "invoice", "generated", "draft");
    const std::string draft_entry = seed_draft_entry();
    ASSERT_TRUE(repo_.link_entry(org_.id, doc.id, draft_entry));
    EXPECT_EQ(call_delete(ctrl, authed(Delete, p), doc.id)->getStatusCode(), k204NoContent);
    // Черновая проводка осталась — связь каскадится, это принято.
    EXPECT_TRUE(journal_.find_in_org(draft_entry, org_.id).has_value());
}

TEST_F(LedgerDocumentsApiTest, RefusesToDeleteADocumentOnAPostedEntry) {
    auto p = member("del3@example.com", org_.id, "accountant");
    auto doc = repo_.create(org_.id, "invoice", "generated", "final");
    ASSERT_TRUE(repo_.link_entry(org_.id, doc.id, seed_posted_entry()));
    auto resp = call_delete(ctrl, authed(Delete, p), doc.id);
    EXPECT_EQ(resp->getStatusCode(), k409Conflict);
    EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "document_has_posted_entries");
    // ...и аннулирование при этом доступно.
    auto voided = call_void(ctrl, authed_json(Post, json{{"reason", "ошибка"}}, p), doc.id);
    EXPECT_EQ(voided->getStatusCode(), k200OK);
    EXPECT_FALSE(body_of(voided)["data"]["voided_at"].is_null());
    EXPECT_EQ(body_of(voided)["data"]["void_reason"].get<std::string>(), "ошибка");
    // status НЕ затёрт — аудит видит, чем документ был.
    EXPECT_EQ(body_of(voided)["data"]["status"].get<std::string>(), "final");
}

TEST_F(LedgerDocumentsApiTest, HrDocumentReferencedByAnOrderIsFourZeroNineNotFiveHundred) {
    auto p = member("del4@example.com", org_.id, "accountant");
    const std::string hr_doc = seed_hr_document_referenced_by_an_order();
    auto resp = call_delete(ctrl, authed(Delete, p), hr_doc);
    EXPECT_EQ(resp->getStatusCode(), k409Conflict);
    EXPECT_EQ(body_of(resp)["error"].get<std::string>(), "document_referenced");
}

TEST_F(LedgerDocumentsApiTest, VoidRequiresAReasonAndIsIdempotentlyRejected) {
    auto p = member("del5@example.com", org_.id, "accountant");
    auto doc = repo_.create(org_.id, "invoice", "generated", "final");
    EXPECT_EQ(call_void(ctrl, authed_json(Post, json::object(), p), doc.id)->getStatusCode(), k400BadRequest);
    EXPECT_EQ(call_void(ctrl, authed_json(Post, json{{"reason", "  "}}, p), doc.id)->getStatusCode(),
              k422UnprocessableEntity);
    EXPECT_EQ(call_void(ctrl, authed_json(Post, json{{"reason", "дубль"}}, p), doc.id)->getStatusCode(), k200OK);
    auto again = call_void(ctrl, authed_json(Post, json{{"reason", "ещё раз"}}, p), doc.id);
    EXPECT_EQ(again->getStatusCode(), k409Conflict);
    EXPECT_EQ(body_of(again)["error"].get<std::string>(), "already_voided");
}

TEST_F(LedgerDocumentsApiTest, ViewerCanNeitherDeleteNorVoid) {
    auto v = member("viewer2@example.com", org_.id, "viewer");
    auto doc = repo_.create(org_.id, "invoice", "generated", "final");
    EXPECT_EQ(call_delete(ctrl, authed(Delete, v), doc.id)->getStatusCode(), k403Forbidden);
    EXPECT_EQ(call_void(ctrl, authed_json(Post, json{{"reason", "нет"}}, v), doc.id)->getStatusCode(), k403Forbidden);
}
```

- [ ] **Step 6: Гейты и коммит**

```bash
clang-format-17 -i src/ledger/Document.hpp src/ledger/DocumentRepository.hpp \
  src/api/LedgerDocumentsController.hpp src/api/Endpoints.hpp tests/integration/test_documents_api.cpp
./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh && ./scripts/check-test-buckets.sh
make lint-openapi
(cd frontend && npm run codegen && npx tsc --noEmit)
git add migrations/019_document_voiding.sql src/ledger/Document.hpp src/ledger/DocumentRepository.hpp \
  src/api/LedgerDocumentsController.hpp src/api/Endpoints.hpp docs/openapi.yaml \
  frontend/src/lib/api/schema.gen.ts tests/integration/test_documents_api.cpp \
  tests/integration/test_render_job.cpp
git commit -m "feat(documents): split deletion from voiding by the posted-entry link"
```

---

### Task 12: Узкий каскадный вырез в `journal_entries_immutability()`

**Files:**
- Create: `migrations/020_journal_cascade_carveout.sql`
- Create: `tests/integration/test_journal_cascade.cpp`
- Modify: `tests/test_helpers.hpp` (комментарий к `wipe_org_data`)

**Interfaces:**
- Consumes: ничего из предыдущих задач (чистая SQL-правка + тест).
- Produces: `DELETE FROM organizations` и `DELETE FROM users` больше не блокируются триггером неизменяемости журнала; никаких новых C++-символов.

**Прошлая редакция спеки утверждала обратное — читайте функцию.** `migrations/009_journal.sql:97-105` запрещает удаление записей со статусом, **отличным от `draft`**; черновики удаляются свободно:

```sql
    IF TG_OP = 'DELETE' THEN
        IF OLD.status <> 'draft' THEN
            RAISE EXCEPTION 'posted/reversed journal entries are insert-only'
                USING ERRCODE = 'check_violation';
        END IF;
        RETURN OLD;
    END IF;
```

Скопировать исключение у соседней `journal_lines_frozen_after_post()` тоже нельзя: там оно означает «родительская запись уже удалена», а здесь удаляемая строка **и есть** запись.

**Область проблемы шире, чем считалось (выяснено при релизе v0.3.1).** Проведённая запись блокирует не только удаление организации, но и удаление **пользователя**, который её создал: `journal_entries.created_by_user_id UUID REFERENCES users(id) ON DELETE SET NULL` (`migrations/009_journal.sql:31`), а этот `UPDATE` триггер отвергает веткой `IF OLD.status = 'posted' ... RAISE`. То есть сотрудник, однажды проведший документ, не удаляется из системы вообще. Вырез обязан покрывать оба случая, а тесты — проверять оба.

**Штатное отключение арендатора — это `archived`, а не `DELETE`.** У `organizations` уже есть `status` со значением `'archived'` (`migrations/006_organizations.sql`), эндпоинта `DELETE /orgs/{id}` не существует и в P3 не появляется, а проведённый журнал обязан храниться (в РК — 5 лет). Вырез нужен **только** для очистки тестовых данных и для удаления пользователя.

**Категорически запрещены две «простые» альтернативы.** `pg_trigger_depth() > 1` снимает неизменяемость внутри ЛЮБОГО вложенного триггера, а не только нужного каскада. Сессионная переменная вида `SET LOCAL app.allow_cascade = on` вручает приложению рубильник от insert-only журнала — фундамент системы становится опциональным. Исполнитель выберет флаг, если ему не запретить: он проще. **Разрешена только форма `NOT EXISTS`.**

- [ ] **Step 1: Падающий тест каскада**

`tests/integration/test_journal_cascade.cpp` (фикстура — `TestHelpers::CoreBackedTest`, идиома seed-хелперов из `tests/integration/test_documents_api.cpp`):

```cpp
/**
 * @file test_journal_cascade.cpp
 * @brief Каскад проверяется ТЕСТОМ, а не рассуждением. Организация с
 *        проведённой проводкой, её сторно, документами, ФНО-филингом и
 *        кадровым приказом одновременно: reverses_entry_id — самоссылка
 *        NO ACTION, tax_filings.document_id и hr_orders.document_id — тоже
 *        NO ACTION (последний DEFERRABLE, то есть срабатывает на COMMIT).
 */

class JournalCascadeTest : public TestHelpers::CoreBackedTest { /* ... */ };

/// Организация со ВСЕМИ связями сразу: проведённая проводка + её сторно +
/// документ на проводке + ФНО-филинг на документе + кадровый приказ на
/// другом документе.
std::string seed_fully_wired_org();

TEST_F(JournalCascadeTest, DeletingAnOrganizationCascadesThroughPostedEntries) {
    const std::string org_id = seed_fully_wired_org();
    EXPECT_NO_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("DELETE FROM organizations WHERE id = $1", org_id);
        return 0;
    }));
    const auto left = Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec_params("SELECT COUNT(*) FROM journal_entries WHERE org_id = $1", org_id);
        return r.at(0).at(0).template as<long>();
    });
    EXPECT_EQ(left, 0);
}

TEST_F(JournalCascadeTest, DeletingAUserWhoPostedAnEntrySetsTheAuthorToNull) {
    const std::string org_id = seed_fully_wired_org();
    const std::string user_id = author_of_the_posted_entry(org_id);
    EXPECT_NO_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("DELETE FROM users WHERE id = $1", user_id);
        return 0;
    }));
    const auto orphaned = Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec_params(
            "SELECT COUNT(*) FROM journal_entries WHERE org_id = $1 AND created_by_user_id IS NULL", org_id);
        return r.at(0).at(0).template as<long>();
    });
    EXPECT_GT(orphaned, 0);
    // Сама проводка на месте и всё ещё posted — удаление автора не трогает
    // содержание записи.
    const auto still_posted = Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec_params(
            "SELECT COUNT(*) FROM journal_entries WHERE org_id = $1 AND status = 'posted'", org_id);
        return r.at(0).at(0).template as<long>();
    });
    EXPECT_GT(still_posted, 0);
}

TEST_F(JournalCascadeTest, DirectDeleteOfAPostedEntryIsStillRejected) {
    const std::string org_id = seed_fully_wired_org();
    const std::string entry_id = posted_entry_of(org_id);
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
                     txn.exec_params("DELETE FROM journal_entries WHERE id = $1", entry_id);
                     return 0;
                 }),
                 pqxx::sql_error);
}

TEST_F(JournalCascadeTest, DirectUpdateOfAPostedEntryIsStillRejected) {
    const std::string org_id = seed_fully_wired_org();
    const std::string entry_id = posted_entry_of(org_id);
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
                     txn.exec_params("UPDATE journal_entries SET description = 'подделка' WHERE id = $1", entry_id);
                     return 0;
                 }),
                 pqxx::sql_error);
    // И статус нельзя откатить в draft.
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
                     txn.exec_params("UPDATE journal_entries SET status = 'draft' WHERE id = $1", entry_id);
                     return 0;
                 }),
                 pqxx::sql_error);
}

TEST_F(JournalCascadeTest, WipeOrgDataNeedsNoManualTriggerDisabling) {
    seed_fully_wired_org();
    EXPECT_NO_THROW(TestHelpers::wipe_org_data());
}
```

- [ ] **Step 2: Миграция с вырезом**

`migrations/020_journal_cascade_carveout.sql`:

```sql
-- Узкий вырез в journal_entries_immutability() (спека P3 §4.3).
--
-- Что чинится, ровно два случая:
--   1. Каскад DELETE FROM organizations (очистка тестовых данных; штатное
--      отключение арендатора — это status='archived', эндпоинта удаления
--      организации не существует и не появляется).
--   2. DELETE FROM users для человека, который когда-либо провёл запись:
--      journal_entries.created_by_user_id объявлен ON DELETE SET NULL, и
--      этот UPDATE триггер тоже отвергал — то есть любой, кто хоть раз
--      провёл документ, не удалялся из системы вообще (найдено при
--      релизе v0.3.1).
--
-- Форма условия обязана быть именно NOT EXISTS по родительской строке.
-- ЗАПРЕЩЕНЫ две «простые» альтернативы, обе — дыра:
--   * pg_trigger_depth() > 1 — снимает неизменяемость внутри ЛЮБОГО
--     вложенного триггерного контекста, а не только нужного каскада;
--   * сессионный флаг вида SET LOCAL app.allow_cascade = on — вручает
--     приложению рубильник от insert-only журнала, то есть делает
--     фундамент системы опциональным.
-- Первое условие работает потому, что при каскаде ссылочной целостности
-- родительская строка organizations в этой транзакции УЖЕ удалена, а при
-- прямом DELETE FROM journal_entries она на месте.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

CREATE OR REPLACE FUNCTION journal_entries_immutability() RETURNS trigger AS $$
BEGIN
    IF TG_OP = 'DELETE' THEN
        -- Каскад от удалённой организации: родителя уже нет.
        IF NOT EXISTS (SELECT 1 FROM organizations WHERE id = OLD.org_id) THEN
            RETURN OLD;
        END IF;
        IF OLD.status <> 'draft' THEN
            RAISE EXCEPTION 'posted/reversed journal entries are insert-only'
                USING ERRCODE = 'check_violation';
        END IF;
        RETURN OLD;
    END IF;

    -- Каскад от удалённой организации на UPDATE (порядок обхода RI не
    -- гарантирован: строка может успеть получить UPDATE до собственного
    -- DELETE).
    IF NOT EXISTS (SELECT 1 FROM organizations WHERE id = OLD.org_id) THEN
        RETURN NEW;
    END IF;

    -- ON DELETE SET NULL от удалённого автора: единственное изменение —
    -- created_by_user_id стал NULL, всё остальное побайтово прежнее.
    -- Проверка перечисляет поля явно, а не «всё кроме автора»: новая
    -- колонка не должна автоматически попасть в разрешённое.
    IF OLD.created_by_user_id IS NOT NULL AND NEW.created_by_user_id IS NULL
       AND NOT EXISTS (SELECT 1 FROM users WHERE id = OLD.created_by_user_id)
       AND NEW.org_id = OLD.org_id
       AND NEW.entry_date = OLD.entry_date
       AND NEW.description = OLD.description
       AND NEW.status = OLD.status
       AND NEW.reverses_entry_id IS NOT DISTINCT FROM OLD.reverses_entry_id THEN
        RETURN NEW;
    END IF;

    IF OLD.status = 'posted' THEN
        IF NEW.status = 'reversed'
           AND NEW.entry_date = OLD.entry_date AND NEW.description = OLD.description
           AND NEW.org_id = OLD.org_id
           AND NEW.reverses_entry_id IS NOT DISTINCT FROM OLD.reverses_entry_id THEN
            RETURN NEW;  -- единственный легальный переход
        END IF;
        RAISE EXCEPTION 'posted journal entries are immutable (use storno)'
            USING ERRCODE = 'check_violation';
    ELSIF OLD.status = 'reversed' THEN
        RAISE EXCEPTION 'reversed journal entries are immutable'
            USING ERRCODE = 'check_violation';
    ELSIF OLD.status = 'draft' AND NEW.status = 'reversed' THEN
        RAISE EXCEPTION 'draft cannot be reversed (post it first)'
            USING ERRCODE = 'check_violation';
    ELSIF OLD.status = 'draft' AND NEW.status = 'posted'
          AND NOT EXISTS (SELECT 1 FROM journal_lines WHERE entry_id = NEW.id) THEN
        RAISE EXCEPTION 'cannot post an entry with no lines'
            USING ERRCODE = 'check_violation';
    END IF;
    RETURN NEW;  -- draft свободно правится (updated_at меняет touch-триггер)
END $$ LANGUAGE plpgsql;
```

Триггер пересоздавать не надо: `CREATE OR REPLACE FUNCTION` подменяет тело, а `trg_journal_entries_immutable` уже указывает на это имя.

Обратите внимание на условие удаления автора: оно требует, чтобы пользователя **уже не было** в `users` — то есть UPDATE действительно пришёл каскадом от `DELETE FROM users`, а не от приложения, решившего обнулить автора живой записи.

- [ ] **Step 3: Комментарий у `wipe_org_data`**

В `tests/test_helpers.hpp` к `wipe_org_data()` дописать: «`DELETE FROM organizations` проходит благодаря каскадному вырезу в `journal_entries_immutability()` (`migrations/020_journal_cascade_carveout.sql`); ручное отключение триггеров не требуется и запрещено».

- [ ] **Step 4: CI и коммит**

Ожидание: `JournalCascadeTest.*` — 5 тестов PASS; ни один существующий сьют не сломан (два теста «прямое удаление/правка проведённой записи по-прежнему отвергаются» — страховка именно от того, что вырез окажется слишком широким).

```bash
./scripts/check-test-buckets.sh
clang-format-17 -i tests/integration/test_journal_cascade.cpp tests/test_helpers.hpp
git add migrations/020_journal_cascade_carveout.sql tests/integration/test_journal_cascade.cpp tests/test_helpers.hpp
git commit -m "fix(journal): narrow cascade carve-out for org and author deletion"
```

---

### Task 13: SPA — версии документа, правка, удаление и аннулирование

**Files:**
- Modify: `frontend/src/pages/Documents.tsx`
- Modify: `frontend/src/lib/api/queryKeys.ts` (строка 39-51, блок `documents`)
- Modify: `frontend/src/lib/api/types.ts`
- Modify: `frontend/src/lib/schemas/documents.ts` (схема причины аннулирования)
- Create: `frontend/src/lib/schemas/documents.test.ts` (файла сейчас нет — в `frontend/src/lib/schemas/` лежат только `hr.test.ts`, `payroll.test.ts`, `tax.test.ts`; новый файл повторяет их форму: `import { describe, expect, it } from 'vitest';`)

**Interfaces:**
- Consumes (задачи 9, 11): `GET /api/v1/documents/{id}/versions` → `{data: DocumentVersion[]}`; `POST /api/v1/documents/{id}/versions` → 202 `{document_id, version_id, version_no, render_queued}`; `POST /api/v1/documents/{id}/versions/{version_no}/download-url` → `{url}`; `DELETE /api/v1/documents/{id}` → 204; `POST /api/v1/documents/{id}/void` → `{data: Document}`; поля `current_version_id`, `latest_version_no`, `voided_at`, `voided_by_user_id`, `void_reason` в `Document`.
- Produces: ничего для последующих задач, кроме зелёного `tsc`/eslint/vitest.

- [ ] **Step 1: Ключи запросов**

В `frontend/src/lib/api/queryKeys.ts`, в блок `documents`, добавить:

```ts
    versions: (id: string) => ['documents', 'versions', id] as const,
```

Префикс `['documents']` уже инвалидирует всё поддерево — существующие мутации после правки/удаления/аннулирования должны инвалидировать именно его.

- [ ] **Step 2: Схема причины аннулирования**

В `frontend/src/lib/schemas/documents.ts`:

```ts
/** Причина аннулирования — обязательна: аннулирование без причины
 *  бессмысленно для аудита, ради которого оно и существует. */
export const voidDocumentSchema = z.object({
  reason: z.string().trim().min(1, 'Укажите причину аннулирования'),
});
export type VoidDocumentValues = z.infer<typeof voidDocumentSchema>;
```

и тест в `frontend/src/lib/schemas/documents.test.ts`:

```ts
describe('voidDocumentSchema', () => {
  it('rejects a blank reason', () => {
    expect(voidDocumentSchema.safeParse({ reason: '   ' }).success).toBe(false);
    expect(voidDocumentSchema.safeParse({ reason: '' }).success).toBe(false);
  });
  it('trims the reason', () => {
    const parsed = voidDocumentSchema.parse({ reason: '  ошибка в реквизитах  ' });
    expect(parsed.reason).toBe('ошибка в реквизитах');
  });
});
```

- [ ] **Step 3: Карточка документа — вкладка «Версии»**

В `frontend/src/pages/Documents.tsx`, в детальном представлении документа (там, где сейчас живёт кнопка «Скачать», строка ~456), добавить блок истории:

- запрос `useQuery({ queryKey: qk.documents.versions(id), queryFn: () => api.get('/api/v1/documents/{id}/versions', ...) })`;
- таблица: колонки «Версия» (`version_no`), «Создана» (`created_at` через `formatDateTime` из `lib/dateFormat.ts` — никакого `toLocaleString`), «Размер» (`size_bytes`, прочерк при `null`), «Действие» — кнопка «Скачать» на строку, дергающая `POST /api/v1/documents/{id}/versions/{version_no}/download-url` и открывающая полученный `url`;
- текущая версия помечается бейджем «Текущая» (сравнение `version.id === document.current_version_id`);
- если `latest_version_no > (номер текущей версии)`, над таблицей показывается строка «Версия {latest_version_no} готовится — файл появится после рендера.»;
- версия без `s3_key` показывает вместо кнопки текст «Файл ещё не готов».

- [ ] **Step 4: Кнопка «Изменить»**

Кнопка видна только когда `document.source === 'generated'` и `document.voided_at === null`. Открывает диалог с формой, соответствующей `template_slug`:

- для `invoice`/`avr`/`waybill`/`tax_invoice` переиспользуется соответствующая форма из `GenerateDocument.tsx` (вынести её в экспортируемый компонент, если она сейчас локальная; предзаполнение — из `document.input_snapshot`), и отправляется тот же объект `input`, который строит `buildInvoiceInput`/`buildAvrInput`/`buildWaybillInput`/`buildTaxInvoiceInput` (с `total_tiyn`, без `total`/`total_words`);
- для `fno_910`/`fno_300` — два поля, «Руководитель» и «Бухгалтер»;
- для `hr_order` — «Руководитель», «Основание», «Детали»;
- для `labor_contract` — «Режим работы», «Испытательный срок, мес.», «Директор (работодатель)», «Адрес работодателя», «Адрес работника»;
- для `payslip` — формы нет вовсе, кнопка «Изменить» не показывается (у расчётного листка не осталось каллер-полей);
- для `reconciliation` — форма из `GenerateDocument.tsx`, без деривации суммы.

После 202 — инвалидировать `['documents']`, показать тост «Создана версия {version_no}, идёт рендер» и запустить существующий поллинг статуса документа.

Обработка ошибок по кодам: 409 `not_editable` → «Загруженные и присланные почтой документы не редактируются»; 409 `document_voided` → «Аннулированный документ изменить нельзя»; 422 `not_allowed_override` → «Поле {field} вычисляется сервером и не может быть задано вручную»; 422 `schema_validation_failed` → текст из ответа.

- [ ] **Step 5: «Удалить» и «Аннулировать»**

Две кнопки в карточке, обе под `ConfirmDialog` (идиома `frontend/src/pages/Counterparties.tsx`):

- «Удалить» — `DELETE /api/v1/documents/{id}`; подтверждение: «Документ будет удалён безвозвратно вместе со всеми версиями. Файлы в хранилище останутся.»; при 409 `document_has_posted_entries` показать тост «Документ связан с проведённой проводкой — его можно только аннулировать» и **сразу предложить аннулирование** (открыть второй диалог); при 409 `document_referenced` — «На документ ссылается кадровый приказ или налоговая отчётность — доступно только аннулирование»; после 204 — навигация к списку и инвалидация `['documents']`;
- «Аннулировать» — диалог с обязательным полем «Причина» по `voidDocumentSchema`, `POST /api/v1/documents/{id}/void`; после 200 — инвалидация и бейдж.

Аннулированный документ в карточке помечается заметной плашкой: «Аннулирован {formatDateTime(voided_at)} — {void_reason}». В таблице списка — бейджем «Аннулирован» рядом со статусом. Статус при этом продолжает показываться прежним (`final`/`sent`): аудиту важно, чем документ был.

Кнопки «Изменить», «Удалить» и «Аннулировать» на этом шаге НЕ прячутся по роли: роль в организации до SPA ещё не доходит — её приносит `/auth/me` только в задаче 14, и заводить ради этого второй источник (запрос `/orgs/mine`) значит создать точку рассинхронизации, которую та задача потом будет убирать. Вместо этого 403 `org_role_denied` от любой из трёх мутаций отображается тостом «У вашей роли в организации нет прав на это действие». Сервер — источник истины, интерфейс лишь не врёт про результат.

- [ ] **Step 6: Гейты и коммит**

```bash
cd frontend && npx tsc --noEmit && npm run lint && npm test
git add frontend/src/pages/Documents.tsx frontend/src/pages/GenerateDocument.tsx \
  frontend/src/lib/api/queryKeys.ts frontend/src/lib/api/types.ts frontend/src/lib/schemas/documents.ts \
  frontend/src/lib/schemas/documents.test.ts
git commit -m "feat(frontend): document versions, editing, deletion and voiding"
```

---

### Task 14: SPA — четыре раздела навигации и org-роль, доходящая до клиента

**Files:**
- Modify: `src/api/AuthController.hpp` (`me`, строки 264-277)
- Modify: `docs/openapi.yaml` (`MeResponse`), `frontend/src/lib/api/schema.gen.ts` (регенерация), `frontend/src/lib/api/types.ts`
- Modify: `frontend/src/hooks/useMe.ts`
- Create: `frontend/src/routes/navGroups.ts`
- Create: `frontend/src/routes/navGroups.test.ts`
- Modify: `frontend/src/routes/manifest.tsx`
- Modify: `frontend/src/components/Nav.tsx`
- Test: `tests/integration/test_auth_flow.cpp` (org-роль в `/me`)

**Interfaces:**
- Consumes (задача 7): значение роли `hr` валидно; матрица §5.3 определяет, что кому видно.
- Produces:
  - `GET /api/v1/auth/me` возвращает `{user: {...}, org_role: "owner"|"accountant"|"hr"|"viewer"|null}`;
  - `export type NavGroupId = 'accounting' | 'people' | 'tax' | 'settings';`
  - `export const NAV_GROUPS: readonly { id: NavGroupId; label: string }[]` — упорядоченный список разделов;
  - `RouteEntry` дополняется `navGroup?: NavGroupId` и `navRoles?: readonly string[]`;
  - `export function groupNavLinks(routes: RouteEntry[], user: PermissionUser | null, orgRole: string | null): { id: NavGroupId; label: string; links: RouteEntry[] }[]` — пустые разделы уже отфильтрованы;
  - `export function useOrgRole(): UseQueryResult<string | null>`.

**Три вещи, которые мешают сделать это сегодня.** (1) У `RouteEntry` нет поля группы — нужны `navGroup` **и отдельный упорядоченный список групп**, иначе порядок разделов будет следовать порядку маршрутов. (2) Фильтрация по роли невозможна: `Nav.tsx` фильтрует по **глобальным** битам прав (`userCan(user, guardPermission(r))` → `Permission.Administer`/`Permission.AuditRead` из `user.role.permissions`), а роль в организации (`org_members.role`) до SPA не доходит вообще — её отдаёт только `GET /api/v1/orgs/mine`. (3) `Nav.tsx` рендерит ссылки **дважды** — десктоп и `#mobile-nav`; правило «пустой раздел не рисуется» обязано применяться к обоим.

Пунктов в авторизованном меню **десять**: Организации, Контрагенты, Журнал проводок, Документы, Сотрудники, Кадры, Зарплата, Налоги, Администрирование, Аудит. Группировка 3 + 3 + 1 + 3:

- **Учёт** — журнал проводок, документы, контрагенты
- **Кадры и зарплата** — сотрудники, кадры, зарплата
- **Налоги** — налоги (вырожденный раздел из одного пункта: расчёты, сроки и ФНО внутри `/taxes` являются вкладками, а не маршрутами; принято оставить как есть)
- **Настройки** — организации, администрирование, аудит

`Главная` и `О сервисе` в группы не входят и рисуются плоско перед разделами.

- [ ] **Step 1: org-роль в `/auth/me`**

`src/api/AuthController.hpp`, хендлер `me` — заменить последнюю строку:

```cpp
        // Роль в ОРГАНИЗАЦИИ (org_members.role), не системная роль
        // пользователя: SPA нужна именно она, чтобы прятать разделы меню
        // по матрице §5.3, а до P3 её отдавал только GET /orgs/mine.
        // nullptr, когда у токена нет клейма org или членство отозвано —
        // ровно тот же fail-closed, что у Tenancy::org_context_of.
        auto org_ctx = Tenancy::org_context_of(req);
        const json payload = {{"user", json(*user)},
                              {"org_role", org_ctx ? json(org_ctx->role) : json(nullptr)}};
        callback(Response::ok(payload));
```

Добавить `#include "tenancy/OrgContext.hpp"`. Именованный `payload` — обязателен: двойная brace-инициализация прямо в `Response::ok` разрешается неоднозначно.

`docs/openapi.yaml`, схема `MeResponse`: добавить

```yaml
        org_role: { type: ['string', 'null'], enum: [owner, accountant, hr, viewer, null], description: 'The caller''s role in the organization the access token is scoped to; null when the token carries no org claim' }
```

Тест в `tests/integration/test_auth_flow.cpp`: пользователь с org-клеймом получает свою роль (в том числе `"hr"`); пользователь без клейма получает `null`; пользователь с отозванным членством получает `null`.

- [ ] **Step 2: `useMe` — один запрос, два представления**

`frontend/src/hooks/useMe.ts`:

```ts
/** Полный конверт /me: пользователь + его роль в текущей организации. */
export async function fetchMeEnvelope(): Promise<MeResponse | null> {
  const { data, error } = await api.GET('/api/v1/auth/me');
  if (error) {
    if (error.status === 401) return null; // нет сессии
    throw error; // сеть / 5xx — настоящая ошибка
  }
  if (!data) throw new Error('failed to fetch /me');
  return data;
}

/** Форма прежняя (пользователь или null) — ни один существующий
 *  потребитель useMe() не меняется. */
export function useMe() {
  return useQuery({
    queryKey: qk.me(),
    queryFn: fetchMeEnvelope,
    retry: shouldRetryMe,
    select: (envelope) => envelope?.user ?? null,
  });
}

/** Роль в организации из ТОГО ЖЕ ответа: один сетевой запрос, два среза
 *  кэша — отдельный useQuery на /orgs/mine дал бы второй запрос и вторую
 *  точку рассинхронизации. */
export function useOrgRole() {
  return useQuery({
    queryKey: qk.me(),
    queryFn: fetchMeEnvelope,
    retry: shouldRetryMe,
    select: (envelope) => envelope?.org_role ?? null,
  });
}
```

Экспорт `fetchMe` сохранить как тонкую обёртку над `fetchMeEnvelope`, если на неё ссылается существующий тест; иначе — обновить тест под новое имя.

- [ ] **Step 3: Группы и предикат видимости**

`frontend/src/routes/navGroups.ts`:

```ts
/**
 * Разделы верхнего меню (спека P3 §6). Порядок разделов задаётся ЗДЕСЬ,
 * а не порядком маршрутов в манифесте: иначе перестановка страницы молча
 * переставляла бы раздел.
 */
import type { RouteEntry } from '@/routes/manifest';
import { guardPermission } from '@/routes/manifest';
import { userCan, type PermissionUser } from '@/lib/auth/permissions';

export type NavGroupId = 'accounting' | 'people' | 'tax' | 'settings';

export const NAV_GROUPS: readonly { id: NavGroupId; label: string }[] = [
  { id: 'accounting', label: 'Учёт' },
  { id: 'people', label: 'Кадры и зарплата' },
  { id: 'tax', label: 'Налоги' },
  { id: 'settings', label: 'Настройки' },
] as const;

/**
 * Двухфакторный предикат: глобальный бит прав пользователя И роль в
 * организации. Маршрут без navRoles ролью не ограничен; маршрут с
 * navRoles невидим, пока роль неизвестна (orgRole === null) — fail-closed,
 * как и на сервере.
 */
export function isNavLinkVisible(
  route: RouteEntry,
  user: PermissionUser | null,
  orgRole: string | null,
): boolean {
  if (!route.navLabel) return false;
  if (!userCan(user, guardPermission(route))) return false;
  if (!route.navRoles) return true;
  return orgRole !== null && route.navRoles.includes(orgRole);
}

/** Ссылки без группы (Главная, О сервисе) — рисуются плоско перед разделами. */
export function ungroupedNavLinks(
  routes: RouteEntry[],
  user: PermissionUser | null,
  orgRole: string | null,
): RouteEntry[] {
  return routes.filter((r) => !r.navGroup && isNavLinkVisible(r, user, orgRole));
}

/** Разделы в порядке NAV_GROUPS; ПУСТЫЕ УЖЕ ОТФИЛЬТРОВАНЫ. */
export function groupNavLinks(
  routes: RouteEntry[],
  user: PermissionUser | null,
  orgRole: string | null,
): { id: NavGroupId; label: string; links: RouteEntry[] }[] {
  return NAV_GROUPS.map((g) => ({
    ...g,
    links: routes.filter((r) => r.navGroup === g.id && isNavLinkVisible(r, user, orgRole)),
  })).filter((g) => g.links.length > 0);
}
```

`frontend/src/routes/navGroups.test.ts`:

```ts
import { describe, expect, it } from 'vitest';

import { routes } from '@/routes/manifest';
import { groupNavLinks, ungroupedNavLinks, NAV_GROUPS } from '@/routes/navGroups';

const plainUser = { role: { permissions: 0x01 } } as never;
const adminUser = { role: { permissions: 0x40000000 } } as never;

describe('groupNavLinks', () => {
  it('keeps the declared group order regardless of route order', () => {
    const ids = groupNavLinks(routes, adminUser, 'owner').map((g) => g.id);
    expect(ids).toEqual(NAV_GROUPS.filter((g) => ids.includes(g.id)).map((g) => g.id));
  });

  it('gives the owner all four groups and ten links', () => {
    const groups = groupNavLinks(routes, adminUser, 'owner');
    expect(groups.map((g) => g.id)).toEqual(['accounting', 'people', 'tax', 'settings']);
    expect(groups.reduce((n, g) => n + g.links.length, 0)).toBe(10);
  });

  it('shows the hr role only the people group', () => {
    const groups = groupNavLinks(routes, plainUser, 'hr');
    expect(groups.map((g) => g.id)).toEqual(['people', 'settings']);
    const people = groups.find((g) => g.id === 'people');
    expect(people?.links.map((l) => l.path).sort()).toEqual(['/employees', '/hr']);
    // Зарплата кадровику невидима, а не «только для чтения».
    expect(people?.links.some((l) => l.path === '/payroll')).toBe(false);
  });

  it('drops empty groups entirely', () => {
    const groups = groupNavLinks(routes, plainUser, null);
    expect(groups.every((g) => g.links.length > 0)).toBe(true);
    expect(groups.map((g) => g.id)).not.toContain('accounting');
  });

  it('leaves the public links ungrouped', () => {
    expect(ungroupedNavLinks(routes, null, null).map((r) => r.path)).toEqual(['/', '/about']);
  });
});
```

- [ ] **Step 4: Манифест — `navGroup` и `navRoles`**

В `frontend/src/routes/manifest.tsx` в интерфейс `RouteEntry` добавить:

```ts
  /** Раздел верхнего меню; отсутствие поля = ссылка рисуется плоско. */
  navGroup?: NavGroupId;
  /** Роли в организации, которым ссылка видна. Отсутствие = всем. */
  navRoles?: readonly string[];
```

и `import type { NavGroupId } from '@/routes/navGroups';` (тип импортируется в обе стороны только как type — циклической рантайм-зависимости не возникает).

Проставить полям маршрутов (значения точные, роли — по матрице §5.3):

| Маршрут | `navGroup` | `navRoles` |
|---|---|---|
| `/journal` | `'accounting'` | `['owner', 'accountant', 'viewer']` |
| `/documents` | `'accounting'` | `['owner', 'accountant', 'viewer']` |
| `/counterparties` | `'accounting'` | `['owner', 'accountant', 'viewer']` |
| `/employees` | `'people'` | `['owner', 'accountant', 'hr', 'viewer']` |
| `/hr` | `'people'` | `['owner', 'accountant', 'hr', 'viewer']` |
| `/payroll` | `'people'` | `['owner', 'accountant', 'viewer']` |
| `/taxes` | `'tax'` | `['owner', 'accountant', 'viewer']` |
| `/organizations` | `'settings'` | — (не задавать: список своих организаций видит любая роль) |
| `/admin` | `'settings'` | — (уже гейтится `guard: 'admin'`) |
| `/admin/audit` | `'settings'` | — (уже гейтится `requirePermission: Permission.AuditRead`) |
| `/` и `/about` | — | — |

`/documents/generate` `navLabel` не имеет и в меню не попадает — не трогать.

- [ ] **Step 5: `Nav.tsx` — обе копии рендера**

Заменить `const navLinks: RouteEntry[] = routes.filter(...)` на:

```tsx
  const orgRole = useOrgRole().data ?? null;
  const flatLinks = ungroupedNavLinks(routes, user, orgRole);
  const groups = groupNavLinks(routes, user, orgRole);
```

**Десктопный кластер**: сначала `flatLinks` как раньше, затем по одному выпадающему разделу на элемент `groups`. Раздел — кнопка с меткой и шевроном, по клику раскрывающая абсолютно позиционированную панель со ссылками (никаких новых зависимостей; состояние — `useState<NavGroupId | null>`). Обязательные атрибуты доступности: `aria-expanded`, `aria-controls`, панель закрывается по `Escape` и по клику вне. Раздел подсвечивается как активный, если активен любой его пункт.

**Мобильная панель `#mobile-nav`**: `flatLinks` списком, затем на каждый раздел — заголовок (`<div>` с меткой, `text-xs uppercase text-muted-foreground`) и под ним ссылки с отступом. Никаких выпадашек.

Ключевое: **обе копии рендерят один и тот же массив `groups`**, у которого пустые разделы уже отфильтрованы в `groupNavLinks`. Не повторяйте фильтрацию в JSX — именно так одна из двух копий и забывается.

- [ ] **Step 6: Гейты и коммит**

```bash
clang-format-17 -i src/api/AuthController.hpp tests/integration/test_auth_flow.cpp
./scripts/check-openapi-drift.sh && make lint-openapi
cd frontend && npm run codegen && npx tsc --noEmit && npm run lint && npm test
```

Ожидание: `navGroups.test.ts` — 5 тестов PASS; `tsc` чист.

```bash
git add src/api/AuthController.hpp docs/openapi.yaml frontend/src/lib/api/schema.gen.ts \
  frontend/src/lib/api/types.ts frontend/src/hooks/useMe.ts frontend/src/routes/navGroups.ts \
  frontend/src/routes/navGroups.test.ts frontend/src/routes/manifest.tsx frontend/src/components/Nav.tsx \
  tests/integration/test_auth_flow.cpp
git commit -m "feat(frontend): group the navigation into four sections filtered by org role"
```

---

### Task 15: Релиз v0.4.0 и сквозной смоук фазы в проде

**Files:**
- Modify: `helm/cpp-env/values-cybercapybara.yaml` (теги образов)
- Modify: `CLAUDE.md` (если фаза добавила гейт или инвариант — правило самоподдержки этого файла)

**Interfaces:**
- Consumes: всё, что сделали задачи 1-14.
- Produces: работающий прод на v0.4.0 и отчёт о сквозном прогоне.

- [ ] **Step 1: Зелёный CI и мерж**

Дождаться зелёного CI на PR из `feature/p3-documents-roles-words` в `main`: сборка, clang-tidy, ASan+UBSan (+TSAN), gitleaks, Trivy, helm-render, `template-render`, гейты OpenAPI-дрейфа и регистрации маршрутов, фронтовые `tsc`/eslint/vitest. Смержить.

- [ ] **Step 2: Тег и релиз**

Тег `v0.4.0` (образы публикуются **без** префикса `v` — `0.4.0`). Дождаться, пока релизный workflow выложит образы api и worker.

- [ ] **Step 3: values и `helm upgrade`**

В `helm/cpp-env/values-cybercapybara.yaml` поднять теги обоих образов до `0.4.0`, выполнить `helm upgrade`. Проверить: поды Ready, миграции 017-020 применились. Проверка в БД:

```sql
SELECT version FROM schema_migrations ORDER BY version DESC LIMIT 6;
SELECT conname, pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'org_members_role_check';
SELECT COUNT(*) FROM document_versions;
SELECT COUNT(*) FROM documents WHERE NOT EXISTS (SELECT 1 FROM document_versions v WHERE v.document_id = documents.id);
```

Последний запрос обязан вернуть 0 — иначе бэкфилл 018 пропустил строки и старые PDF стали недостижимы; в этом случае откатывайте релиз, а не чините данные руками.

- [ ] **Step 4: Сквозной смоук через https://buh.cybercapybara.kz**

Каждый пункт — с выводом в отчёт.

1. **Прописи в первичке.** Создать счёт на 12 345,67 ₸ через интерфейс → в PDF строка «(Двенадцать тысяч триста сорок пять тенге 67 тиын)». В форме поля ввода прописи нет.
2. **Клиентская пропись отвергается.** `POST /api/v1/documents/generate` с `{"input": {..., "total_words": "Один тенге 00 тиын"}}` → 422 `not_allowed_override`.
3. **Чужой слаг.** `POST /api/v1/documents/generate` с `template_slug: "payslip"` → **422** `unsupported_template` (до релиза здесь было 500).
4. **ФНО и расчётный листок.** Сформировать ФНО 910 и расчётный листок — в обоих PDF пропись есть, руками её никто не вводил. Отдельно: расчёт НДС с сальдо к возврату → ФНО 300 рендерится, `balance_kind = to_refund`, пропись без минуса.
5. **Двуязычный трудовой договор.** Сгенерировать — в казахской половине стоит казахская пропись, в русской русская, и они различаются.
6. **Версии.** Открыть счёт из п.1 → «Изменить» → сумма 2 000,00 ₸ → 202, появилась версия 2; после рендера текущая версия 2, а PDF версии 1 по-прежнему скачивается по своей ссылке.
7. **Правка чужого поля.** `POST /api/v1/documents/{id}/versions` для ФНО-документа с `{"input": {"tax_tenge": "1,00"}}` → 422 `not_allowed_override`.
8. **Правка загруженного.** Загрузить скан → «Изменить» недоступно; вызов API напрямую → 409 `not_editable`.
9. **Удаление и аннулирование.** Ошибочно загруженный скан удаляется (204). Счёт, привязанный к проведённой проводке, на удалении даёт 409 `document_has_posted_entries`, аннулируется с причиной, в карточке плашка «Аннулирован», `status` остался `final`.
10. **Кадровый документ под приказом.** Удаление → 409 `document_referenced` (не 500).
11. **Роль кадровика.** Завести второго пользователя с ролью `hr`. Под ним: `/employees` и `/hr` открываются; `/payroll`, `/journal`, `/taxes`, `/counterparties` дают 403 `org_role_denied` **и на GET тоже**; в меню видны только разделы «Кадры и зарплата» (без «Зарплата») и «Настройки».
12. **Меню.** Под владельцем — четыре раздела в порядке Учёт / Кадры и зарплата / Налоги / Настройки, десять пунктов; на узком экране мобильная панель показывает те же разделы заголовками; пустых разделов нет ни в одном из двух рендеров.
13. **Удаление пользователя.** Удалить (в админке) учётную запись, которая когда-либо проводила запись в журнал → удаление проходит, проводка на месте, её автор стал пустым.

- [ ] **Step 5: Коммит values и пуш**

```bash
git add helm/cpp-env/values-cybercapybara.yaml CLAUDE.md
git commit -m "deploy: release v0.4.0 (amounts in words, document versions, hr role, grouped nav)"
```

---

## Definition of Done (фаза P3)

1. CI зелёный: новые сьюты — `test_amount_in_words` (русский и казахский, все golden-векторы §3.3 плюс краевые, отрицательное и потолок триллиона), `test_money_format`, `test_input_policy`, `test_org_permissions`, `test_org_read_gates`, `test_journal_cascade`, плюс расширенные `test_documents`, `test_documents_api`, `test_render_job`, `test_docgen_api`, `test_tax_api`, `test_payroll_api`, `test_hr_api`, `test_auth_flow`; фронтовые `navGroups.test.ts`, `documents.test.ts` и обновлённые `payroll/tax/hr.test.ts`.
2. `template-render` рендерит все шаблоны, включая четыре с новыми целочисленными полями и `labor_contract` с `salary_words_kk`.
3. Ни одна пропись не приходит от клиента: `grep -rn '_words\|прописью' frontend/src/ | grep -v schema.gen.ts` находит только `break-words` в `components/ui/toaster.tsx`; попытка прислать `*_words` на любом из пяти маршрутов генерации даёт 422 `not_allowed_override`.
4. `grep -rn 'role == "viewer"' src/api/` не находит ничего; каждый из 23 write-гейтов и 21 read-гейта проходит через `API_REQUIRE_ORG_PERM`; матрица §5.3 покрыта unit-тестом, включая deny-by-default.
5. Роль `hr` существует в БД, в `is_valid_role`, в `OrganizationsController`, в OpenAPI и в сгенерированном клиенте; на каждый закрытый для неё ресурс есть тест на 403 **включая GET**; `hr` не считается за владельца в защите последнего owner-а.
6. Файловые метаданные и `input_snapshot` живут в `document_versions`; у каждого существующего документа есть версия 1 (бэкфилл проверен запросом); правка порождает версию, старый PDF доступен, повторный рендер не перезаписывает файл и не снимает аннулирование.
7. Удаление и аннулирование разграничены по связи с проведённой проводкой, а не по статусу; SQLSTATE 23503 отображается в 409, а не в 500; политика хранения объектов S3 записана явно в миграции и в OpenAPI.
8. Каскадный вырез в `journal_entries_immutability()` имеет форму `NOT EXISTS (SELECT 1 FROM organizations ...)`; `pg_trigger_depth()` и сессионных GUC в миграции нет; тестами покрыты оба пути (удаление организации и удаление автора проведённой записи) и обе страховки (прямые DELETE и UPDATE проведённой записи по-прежнему отвергаются).
9. Меню — четыре раздела в заданном порядке, пустые скрыты в обоих рендерах `Nav.tsx`, org-роль приходит из `/auth/me` одним запросом.
10. Сквозной смоук задачи 15 Step 4 пройден в проде на v0.4.0, все тринадцать пунктов с выводом в отчёте.
