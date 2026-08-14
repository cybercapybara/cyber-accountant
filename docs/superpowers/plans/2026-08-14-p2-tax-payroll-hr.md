# P2: Налоги, зарплата и кадры — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ТОО на упрощёнке с НДС может через систему посчитать зарплатные налоги и налоги с дохода, получить ФНО 910.00 и 300.00 (XML для СОНО + печатный PDF), вести кадры с двуязычными документами и видеть календарь налоговых дедлайнов.

**Architecture:** Два новых доменных модуля по спеке §4: `src/tax/` (ставки-константы, чистые калькуляторы, ФНО-генераторы) и `src/hr/` + `src/payroll/` (сотрудники, кадровые документы, расчёт зарплаты). Все ставки и пороги — строки в БД с датами действия (`effective_from`/`effective_to`), расчёт на дату берёт действующую строку; ни одного числа в коде. Расчёты — чистые функции над целыми тиынами, результат сохраняется снапшотом (воспроизводимость по спеке §7.2). ФНО и кадровые документы печатаются через готовый docgen (P1).

**Tech Stack:** Drogon C++20, PostgreSQL 15, XeLaTeX (готов), pugixml или ручная сборка XML (решается в Task 7), React SPA, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-08-14-cyber-accountant-design.md` (§7 налоговый движок, §8 зарплата и кадры, §13 API, §17 фаза P2)

## Global Constraints

- **Сборки/тесты только в GitHub Actions** (директива владельца): локально — git, kubectl/helm, clang-format 17.0.6 (pip-venv), `npx tsc --noEmit`, eslint, кодоген, shell-гейты `./scripts/check-*.sh`. CI на PR — источник истины, итерация ~40 минут: перед пушем вычитывать код на компилируемость самому.
- Ветка `feature/p2-tax-payroll-hr` от main; PR в main; имплементеры коммитят, НЕ пушат (пуш — контроллер); `git add` только своими путями.
- **Ни одной налоговой константы в коде** — всё через `tax_rates`/`tax_constants` с `effective_from`; расчёт получает дату и резолвит строку. Нарушение = дефект.
- **Деньги — целые тиыны** (`long long`) во всех расчётах; в БД `NUMERIC(18,2)`; в API/домене — строки «1234.56». Никаких double. Переиспользовать `Ledger::parse_tiyn` (`src/ledger/JournalService.hpp`) и его границы.
- Каждая доменная таблица: `org_id UUID NOT NULL REFERENCES organizations(id)`; репозитории наследуют `Tenancy::OrgCrudBase`; API берёт `org_id` только из `ctx` (`API_REQUIRE_ORG`), мутации требуют `ctx.role != "viewer"`.
- Triple-sync (контроллер + `src/api/Endpoints.hpp` + `docs/openapi.yaml`); ошибки — `ErrorResponse::*` / `Api::Validation::*`; 400 = кривая форма, 422 = кривое значение, 409 = конфликт состояния.
- `src/` header-only; миграции `migrations/NNN_slug.sql` последовательно с 011, без BEGIN/COMMIT; updated_at через `touch_updated_at()`.
- LaTeX-шаблоны: `templates/latex/<slug>/v1/{template.tex,schema.json,fixtures/{basic,special-chars}.json}`; inja-комментарии `((# #))`; условия по опциональным строкам — `{% if X != "" %}`; преамбула как у `templates/latex/invoice/v1` (fontspec `[Scale=0.92]{Noto Sans}`, polyglossia).
- Conventional commits **без AI-attribution трейлеров**; Doxygen-шапка в каждом новом файле.
- Фикстуры интеграционных тестов чистят данные через `TestHelpers::wipe_org_data()`; при добавлении таблиц с блокирующими триггерами — расширить хелпер.

**Точка входа:** main после v0.2.1 (учётное ядро, docgen, S3, API, SPA — готовы и в проде).

---

### Task 1: Справочник ставок и констант — миграция 011 + seed НК РК-2026

**Files:**
- Create: `migrations/011_tax_reference.sql`, `src/tax/TaxRate.hpp`, `src/tax/TaxReferenceRepository.hpp`
- Test: `tests/integration/test_tax_reference.cpp`

**Interfaces:**
- Produces:
  - `Tax::RateKind` — строковые ключи: `"vat"`, `"snr_simplified"`, `"ipn"`, `"opv"`, `"opvr"`, `"so"`, `"osms"`, `"vosms"`, `"social_tax"`;
  - `Tax::Rate { std::string id, kind, effective_from, effective_to /*пусто = бессрочно*/, source_note; long long rate_bp /*базисные пункты: 16% = 1600, 3.5% = 350*/; std::optional<std::string> region; }`;
  - `Tax::Constant { std::string id, key /*"mrp","mzp","ipn_deduction_mrp","vat_threshold_tenge","snr_income_limit_mrp"*/, effective_from, effective_to, source_note; long long value_tiyn /*денежные — в тиынах; кратности МРП/МЗП — целым числом в value_units*/; std::optional<long long> value_units; }`;
  - `Tax::TaxReferenceRepository` (НЕ OrgCrudBase — справочник системный, общий для всех тенантов; задокументировать как второе исключение после accounts): `rate_on(kind, date, region_or_empty) → std::optional<Rate>`, `constant_on(key, date) → std::optional<Constant>`, `list_rates_on(date) → std::vector<Rate>`, `list_constants_on(date) → std::vector<Constant>`.
- Ставки в базисных пунктах (целые), чтобы не вносить дробную арифметику: 16% → 1600, 3.5% → 350, 10% → 1000.

- [ ] **Step 1: Официальная сверка (обязательна, до написания SQL)**

Проверить каждое значение по первоисточникам (WebSearch/WebFetch): adilet.zan.kz (текст НК РК, вступивший в силу 01.01.2026), kgd.gov.kz, egov.kz (МРП/МЗП из закона о республиканском бюджете на 2026), плюс минимум два вторичных источника (mybuh.kz, pro1c.kz, uchet.kz) для перекрёстной проверки. Для КАЖДОЙ строки зафиксировать в колонке `source_note` короткую ссылку на статью/акт (например `НК РК ст.686-2 (2026)` или `Закон о респбюджете на 2026`). Расхождения между источниками — разрешать в пользу adilet/kgd и отмечать в отчёте.

Значения, которые нужно подтвердить (черновик из вторичных источников — НЕ доверять без сверки): МРП 4325 ₸; МЗП 85000 ₸; вычет по ИПН 30 МРП; ИПН 10% (и порог 8500 МРП → 15%, если подтвердится); ОПВ 10% (верхний предел базы — 50 МЗП по одному источнику, «коридор 1–7 МЗП» по другому — разрешить противоречие!); ВОСМС 2% (предел 20 МЗП); ОПВР 3.5%; СО 5% (база = доход − ОПВ, пределы 1–7 МЗП); ОСМС 3% (предел 40 МЗП); соцналог 6% (для ОУР; на упрощёнке не платится — подтвердить); НДС 16%, порог регистрации 40 млн ₸; упрощёнка 4% (региональный диапазон 2–6%), лимит дохода 600 000 МРП.

- [ ] **Step 2: Падающий тест справочника**

`tests/integration/test_tax_reference.cpp` (фикстура — `TestHelpers::CoreBackedTest`, очистка не нужна: справочник системный и seed'ится миграцией):

```cpp
TEST_F(TaxReferenceTest, SeedHasVatAndSnrForTwentyTwentySix) {
    Tax::TaxReferenceRepository repo;
    auto vat = repo.rate_on("vat", "2026-06-01", "");
    ASSERT_TRUE(vat);
    EXPECT_EQ(vat->rate_bp, 1600);
    auto snr = repo.rate_on("snr_simplified", "2026-06-01", "");
    ASSERT_TRUE(snr);
    EXPECT_EQ(snr->rate_bp, 400);
}

TEST_F(TaxReferenceTest, ConstantsResolveByDate) {
    Tax::TaxReferenceRepository repo;
    auto mrp = repo.constant_on("mrp", "2026-06-01");
    ASSERT_TRUE(mrp);
    EXPECT_EQ(mrp->value_tiyn, 432500);  // 4325 ₸ в тиынах
    EXPECT_FALSE(repo.constant_on("mrp", "2019-01-01").has_value());
}

TEST_F(TaxReferenceTest, PayrollRatesPresent) {
    Tax::TaxReferenceRepository repo;
    for (const auto* kind : {"ipn", "opv", "opvr", "so", "osms", "vosms", "social_tax"})
        EXPECT_TRUE(repo.rate_on(kind, "2026-06-01", "").has_value()) << kind;
}

TEST_F(TaxReferenceTest, EveryRowCarriesSourceNote) {
    Tax::TaxReferenceRepository repo;
    for (const auto& r : repo.list_rates_on("2026-06-01")) EXPECT_FALSE(r.source_note.empty());
    for (const auto& c : repo.list_constants_on("2026-06-01")) EXPECT_FALSE(c.source_note.empty());
}
```

- [ ] **Step 3: Миграция**

```sql
-- Справочник ставок и констант (спека §7.1). СИСТЕМНЫЙ, без org_id — второе
-- задокументированное исключение из правила org-скоупа (первое — accounts).
-- Ставки в базисных пунктах (16% = 1600), деньги в тиынах: только целая арифметика.
CREATE TABLE IF NOT EXISTS tax_rates (
    id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    kind           TEXT NOT NULL CHECK (kind IN
                     ('vat','snr_simplified','ipn','opv','opvr','so','osms','vosms','social_tax')),
    rate_bp        INTEGER NOT NULL CHECK (rate_bp >= 0 AND rate_bp <= 10000),
    region         TEXT,
    effective_from DATE NOT NULL,
    effective_to   DATE,
    source_note    TEXT NOT NULL,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (effective_to IS NULL OR effective_to >= effective_from)
);
CREATE INDEX IF NOT EXISTS idx_tax_rates_lookup ON tax_rates (kind, effective_from DESC);

CREATE TABLE IF NOT EXISTS tax_constants (
    id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    key            TEXT NOT NULL,
    value_tiyn     BIGINT NOT NULL,
    value_units    BIGINT,
    effective_from DATE NOT NULL,
    effective_to   DATE,
    source_note    TEXT NOT NULL,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (effective_to IS NULL OR effective_to >= effective_from)
);
CREATE INDEX IF NOT EXISTS idx_tax_constants_lookup ON tax_constants (key, effective_from DESC);

-- Seed НК РК-2026. ЗНАЧЕНИЯ ПОДСТАВЛЯЕТ ИМПЛЕМЕНТЕР ПО РЕЗУЛЬТАТАМ Step 1;
-- source_note обязателен в каждой строке.
INSERT INTO tax_rates (kind, rate_bp, effective_from, source_note) VALUES
 ('vat',            1600, DATE '2026-01-01', '<источник>'),
 ('snr_simplified',  400, DATE '2026-01-01', '<источник>'),
 ('ipn',            1000, DATE '2026-01-01', '<источник>'),
 ('opv',            1000, DATE '2026-01-01', '<источник>'),
 ('opvr',            350, DATE '2026-01-01', '<источник>'),
 ('so',              500, DATE '2026-01-01', '<источник>'),
 ('osms',            300, DATE '2026-01-01', '<источник>'),
 ('vosms',           200, DATE '2026-01-01', '<источник>'),
 ('social_tax',      600, DATE '2026-01-01', '<источник>')
ON CONFLICT DO NOTHING;

INSERT INTO tax_constants (key, value_tiyn, value_units, effective_from, source_note) VALUES
 ('mrp',                    432500,   NULL, DATE '2026-01-01', '<источник>'),
 ('mzp',                   8500000,   NULL, DATE '2026-01-01', '<источник>'),
 ('ipn_deduction_mrp',           0,     30, DATE '2026-01-01', '<источник>'),
 ('vat_threshold_tenge', 4000000000,  NULL, DATE '2026-01-01', '<источник>'),
 ('snr_income_limit_mrp',        0, 600000, DATE '2026-01-01', '<источник>'),
 ('opv_base_max_mzp',            0,     50, DATE '2026-01-01', '<источник>'),
 ('so_base_min_mzp',             0,      1, DATE '2026-01-01', '<источник>'),
 ('so_base_max_mzp',             0,      7, DATE '2026-01-01', '<источник>'),
 ('vosms_base_max_mzp',          0,     20, DATE '2026-01-01', '<источник>'),
 ('osms_base_max_mzp',           0,     40, DATE '2026-01-01', '<источник>')
ON CONFLICT DO NOTHING;
```

Плейсхолдеры `<источник>` обязаны быть заменены реальными ссылками; предельные значения (`*_max_mzp`) — подтверждёнными в Step 1 (если официальный текст даёт коридор ОПВ 1–7 МЗП вместо 50 МЗП — поправить и строку, и её имя).

- [ ] **Step 4: Домен + репозиторий**

`src/tax/TaxRate.hpp` — обе структуры с `from_row` (шаблонный, `.template as<>()`) и ADL `to_json`, по образцу `src/ledger/Account.hpp`. `src/tax/TaxReferenceRepository.hpp` — ручные запросы вида:

```sql
SELECT ... FROM tax_rates
 WHERE kind = $1 AND effective_from <= $2::date
   AND (effective_to IS NULL OR effective_to >= $2::date)
   AND (region IS NULL OR region = $3)
 ORDER BY region NULLS LAST, effective_from DESC LIMIT 1
```

(региональная строка приоритетнее общей). Doxygen: почему справочник не org-scoped.

- [ ] **Step 5: Гейты + коммит**

```bash
<clang-format-17> -i src/tax/*.hpp tests/integration/test_tax_reference.cpp
./scripts/check-test-buckets.sh
git add migrations/011_tax_reference.sql src/tax tests/integration/test_tax_reference.cpp
git commit -m "feat: tax rate and constant reference with KZ-2026 seed"
```

---

### Task 2: Калькулятор зарплаты — чистые функции + golden-тесты

**Files:**
- Create: `src/payroll/PayrollCalculator.hpp`
- Test: `tests/unit/test_payroll_calculator.cpp`

**Interfaces:**
- Consumes: `Tax::TaxReferenceRepository` — но НЕ напрямую: калькулятор чистый, принимает уже разрезолвленные ставки.
- Produces:
  - `Payroll::Rates { long long ipn_bp, opv_bp, opvr_bp, so_bp, osms_bp, vosms_bp, social_tax_bp; long long mrp_tiyn, mzp_tiyn; long long ipn_deduction_mrp, opv_base_max_mzp, so_base_min_mzp, so_base_max_mzp, vosms_base_max_mzp, osms_base_max_mzp; bool social_tax_applies; }`;
  - `Payroll::Input { long long gross_tiyn; bool ipn_deduction_claimed /*заявление на вычет*/; bool opvr_exempt /*пенсионер/инвалид*/; }`;
  - `Payroll::Result { long long opv, vosms, ipn, net /*к выплате*/, opvr, so, osms, social_tax, employer_cost_total; }` (все — тиыны);
  - `Payroll::Result Payroll::calculate(const Input&, const Rates&)` — чистая функция;
  - `long long Payroll::apply_bp(long long base_tiyn, long long rate_bp)` — умножение на базисные пункты с округлением half-up до тиына: `(base*bp + 5000) / 10000`.
- Порядок расчёта (подтверждается Task 1 Step 1; в коде — комментарием со ссылкой на статью):
  1. `opv = apply_bp(min(gross, mzp*opv_base_max_mzp), opv_bp)`
  2. `vosms = apply_bp(min(gross, mzp*vosms_base_max_mzp), vosms_bp)`
  3. `ipn_base = max(0, gross - opv - vosms - (ipn_deduction_claimed ? mrp*ipn_deduction_mrp : 0))`; `ipn = apply_bp(ipn_base, ipn_bp)`
  4. `net = gross - opv - vosms - ipn`
  5. `so_base = clamp(gross - opv, mzp*so_base_min_mzp, mzp*so_base_max_mzp)`; `so = apply_bp(so_base, so_bp)`
  6. `osms = apply_bp(min(gross, mzp*osms_base_max_mzp), osms_bp)`
  7. `opvr = opvr_exempt ? 0 : apply_bp(min(gross, mzp*opv_base_max_mzp), opvr_bp)`
  8. `social_tax = social_tax_applies ? apply_bp(gross - opv - vosms, social_tax_bp) : 0` (на упрощёнке `social_tax_applies=false`)
  9. `employer_cost_total = gross + opvr + so + osms + social_tax`

- [ ] **Step 1: Golden-векторы**

Сгенерировать независимой реализацией (python3 по формулам выше) и вставить в тест 5 наборов: (а) оклад 85 000 ₸ (=1 МЗП) с вычетом; (б) 300 000 ₸ с вычетом; (в) 300 000 ₸ без вычета; (г) 1 000 000 ₸ (проверка потолков СО); (д) 60 000 ₸ (ниже МЗП — нижний предел базы СО). Скрипт и его вывод — в отчёт. Дополнительно: сверить хотя бы один вектор с онлайн-калькулятором (profinance.kz/calculators/tax или mybuh.kz), расхождение — повод перепроверить порядок расчёта и зафиксировать вывод в отчёте.

- [ ] **Step 2: Падающие unit-тесты**

`tests/unit/test_payroll_calculator.cpp`: пять golden-кейсов (каждое поле `Result` — точное значение); `ApplyBpRoundsHalfUp` (`apply_bp(333, 1000) == 33`, `apply_bp(335, 1000) == 34`); `SocialTaxSkippedOnSnr` (`social_tax_applies=false` → 0); `OpvrExemptZero`; `SoBaseFloorApplies` (маленький оклад → база СО = 1 МЗП); `CeilingsApply` (огромный оклад → базы обрезаны).

- [ ] **Step 3: Реализация** — `src/payroll/PayrollCalculator.hpp`, только целочисленная арифметика, без обращений к БД. Doxygen с порядком расчёта и ссылками на статьи.

- [ ] **Step 4: Гейты + коммит** — `feat: payroll calculator with KZ withholding order`

---

### Task 3: Кадры — миграция 012, домен, репозитории

**Files:**
- Create: `migrations/012_hr.sql`, `src/hr/Employee.hpp`, `src/hr/HrDocuments.hpp` (LaborContract, HrOrder, Vacation), `src/hr/EmployeeRepository.hpp`, `src/hr/HrRepository.hpp`
- Test: `tests/integration/test_hr.cpp`

**Interfaces:**
- Produces:
  - таблицы `employees` (org_id, iin CHAR(12), last_name, first_name, middle_name, position, salary_tiyn BIGINT, hired_on DATE, dismissed_on DATE NULL, ipn_deduction_claimed BOOL, opvr_exempt BOOL, payout_iik TEXT, status CHECK IN ('active','dismissed'), UNIQUE(org_id, iin)), `labor_contracts` (org_id, employee_id, number, signed_on, starts_on, ends_on NULL, terms_json JSONB), `hr_orders` (org_id, employee_id, kind CHECK IN ('hire','dismiss','vacation','business_trip','salary_change'), number, issued_on, effective_from, effective_to NULL, payload JSONB, document_id UUID NULL REFERENCES documents(id)), `vacations` (org_id, employee_id, starts_on, ends_on, days INTEGER, kind CHECK IN ('annual','unpaid','sick'));
  - `Hr::Employee`, `Hr::LaborContract`, `Hr::HrOrder`, `Hr::Vacation` (домены с from_row/to_json);
  - `Hr::EmployeeRepository : Tenancy::OrgCrudBase` + `create(org_id, Employee draft)`, `update(org_id, id, Employee patch)`, `dismiss(org_id, id, date)`, `list_active(org_id)`;
  - `Hr::HrRepository : Tenancy::OrgCrudBase<...,HrOrder,...>` + `create_order(...)`, `list_orders(org_id, employee_id_opt)`, `create_contract(...)`, `list_contracts(org_id, employee_id)`, `create_vacation(...)`, `list_vacations(org_id, employee_id_opt)`, `attach_document(org_id, order_id, document_id)`.
- ИИН валидируется на API-слое через `Ledger::is_valid_bin_iin` (P1).

- [ ] **Step 1: Миграция** — SQL по контракту выше (идиомы 007/010: `org_id NOT NULL REFERENCES organizations(id) ON DELETE CASCADE`, `UNIQUE (id, org_id)` на `employees` и `hr_orders` для будущих составных FK, touch-триггеры, индексы `(org_id, status)`, `(org_id, employee_id)`).
- [ ] **Step 2: Падающие тесты** (полные тела): `CreateFindEmployee`; `DuplicateIinRejected` (typed conflict); `DismissSetsStatusAndDate`; `ListActiveExcludesDismissed`; `OrderWithPayloadRoundTrips` (JSONB); `VacationDaysStored`; `CrossOrgIsolated`.
- [ ] **Step 3: Домен + репозитории** (идиомы `src/ledger/CounterpartyRepository.hpp`).
- [ ] **Step 4: Гейты + коммит** — `feat: HR employees, contracts, orders and vacations`

---

### Task 4: Зарплата — миграция 013 + PayrollService

**Files:**
- Create: `migrations/013_payroll.sql`, `src/payroll/Payslip.hpp`, `src/payroll/PayrollRepository.hpp`, `src/payroll/PayrollService.hpp`
- Test: `tests/integration/test_payroll_service.cpp`

**Interfaces:**
- Consumes: `Payroll::calculate` (Task 2), `Tax::TaxReferenceRepository` (Task 1), `Hr::EmployeeRepository` (Task 3), `Ledger::JournalService` (P1, для проводок).
- Produces:
  - таблицы `payroll_runs` (org_id, period_year INT, period_month INT, status CHECK IN ('draft','approved'), calculated_at, rates_snapshot JSONB, UNIQUE(org_id, period_year, period_month), UNIQUE(id, org_id)), `payslips` (org_id, run_id, employee_id, gross_tiyn, opv, vosms, ipn, net, opvr, so, osms, social_tax — все BIGINT, составной FK (run_id, org_id) → payroll_runs(id, org_id));
  - `Payroll::PayrollService`: `calculate_run(org_id, year, month) → Run` (снимает ставки на последний день месяца, считает всем активным сотрудникам, пересчёт draft-прогона перезаписывает payslips; approved — 409 `InvalidRunState`), `approve(org_id, run_id) → Run` (draft→approved), `find_run(org_id, year, month)`, `payslips_of(org_id, run_id)`;
  - `Payroll::PayrollService::post_to_journal(org_id, run_id, user_id) → std::string /*entry_id*/` — создаёт проводку зарплаты через `JournalService::create_draft` + `post`: дебет 7210 (админрасходы) на `gross+opvr+so+osms+social_tax`, кредиты 3350 (зарплата к выплате, `net`), 3120 (ИПН), 3220 (ОПВ+ОПВР), 3210 (СО), 3230 (ОСМС+ВОСМС), 3150 (соцналог, если ≠ 0). Суммы форматируются в строки через существующий хелпер (в `JournalService` есть форматирование тиынов — переиспользовать; если нет, добавить `Ledger::format_tiyn(long long) → std::string` рядом с `parse_tiyn` и покрыть тестом).
- Инвариант: сумма кредитов = дебету (иначе проводка не пройдёт триггер баланса — тест обязан это доказать).

- [ ] **Step 1: Падающие тесты**: `CalculateRunProducesPayslipPerActiveEmployee`; `RecalculateDraftReplacesPayslips`; `ApproveBlocksRecalculation` (409); `RatesSnapshotStored` (в `rates_snapshot` лежат использованные ставки — проверить наличие ключей); `PostToJournalBalances` (проводка создана, Σдебет=Σкредит, статус posted); `DismissedEmployeeExcluded`; `CrossOrgIsolated`.
- [ ] **Step 2: Миграция + реализация**.
- [ ] **Step 3: Гейты + коммит** — `feat: payroll runs, payslips and journal posting`

---

### Task 5: Налоговые расчёты — миграция 014 + TaxService (упрощёнка и НДС)

**Files:**
- Create: `migrations/014_tax_calculations.sql`, `src/tax/TaxCalculation.hpp`, `src/tax/TaxCalculationRepository.hpp`, `src/tax/TaxService.hpp`
- Test: `tests/integration/test_tax_service.cpp`

**Interfaces:**
- Consumes: `Tax::TaxReferenceRepository`, `Ledger::JournalRepository` (обороты по счетам), `Payroll::PayrollRepository` (зарплатные суммы за период).
- Produces:
  - таблицы `tax_calculations` (org_id, kind CHECK IN ('snr_simplified','vat'), period_from DATE, period_to DATE, computed_at, input_snapshot JSONB, result_snapshot JSONB, total_tiyn BIGINT, UNIQUE(org_id, kind, period_from, period_to), UNIQUE(id, org_id));
  - `Tax::TaxService::calculate_snr(org_id, half_year_start, half_year_end) → Calculation` — доход берётся как кредитовый оборот счетов группы 6000 (`6010`,`6110`,`6250`,`6290`) за период по posted-проводкам; налог = `apply_bp(income, snr_rate_bp)`; в `result_snapshot`: `{income_tiyn, rate_bp, tax_tiyn, ipn_part_tiyn, social_tax_part_tiyn}` — распределение по НК (Task 1 Step 1 уточняет пропорцию; если официально распределения нет — поле опускается, задокументировать);
  - `Tax::TaxService::calculate_vat(org_id, quarter_start, quarter_end) → Calculation` — НДС к начислению = сумма `journal_lines.vat_amount` по кредитовым строкам счетов 6000-группы; к зачёту = по дебетовым строкам счетов 1330/7010/7210 с `vat_amount`; сальдо = начислено − зачтено (может быть отрицательным → к возврату);
  - `Tax::TaxService::threshold_alerts(org_id, on_date) → std::vector<Alert>` — `Alert { kind /*"vat_registration"|"snr_limit"*/, message, current_tiyn, threshold_tiyn }`: доход за 12 месяцев против `vat_threshold_tenge` и `snr_income_limit_mrp * mrp`.
- Все расчёты — целые тиыны; `input_snapshot` содержит использованные ставки/константы и агрегаты (воспроизводимость по спеке §7.2).

- [ ] **Step 1: Падающие тесты**: `SnrTaxOnPostedIncome` (две проводки дохода → налог = 4% суммы, точное значение); `SnrIgnoresDraftEntries`; `VatBalanceFromLineVatAmounts` (начислено − зачтено, точное значение, включая отрицательное сальдо); `SnapshotsStored` (в input_snapshot есть rate_bp и границы периода); `RecalculateOverwrites` (повторный расчёт того же периода обновляет строку, не плодит); `ThresholdAlertFiresNearVatLimit` (доход выше порога → алерт с числами); `CrossOrgIsolated`.
- [ ] **Step 2: Миграция + реализация**.
- [ ] **Step 3: Гейты + коммит** — `feat: SNR and VAT calculations with reproducible snapshots`

---

### Task 6: Календарь налоговых дедлайнов

**Files:**
- Create: `migrations/015_tax_deadlines.sql`, `src/tax/TaxCalendar.hpp`
- Test: `tests/integration/test_tax_calendar.cpp`

**Interfaces:**
- Produces: таблица `tax_deadlines` (системная, без org_id — третье исключение, задокументировать: form CHECK IN ('910.00','300.00','200.00'), kind CHECK IN ('report','payment'), period_kind CHECK IN ('month','quarter','half_year','year'), due_day INT, due_month_offset INT /*сколько месяцев после конца периода*/, effective_from DATE, source_note TEXT); `Tax::TaxCalendar::upcoming(on_date, horizon_days) → std::vector<Deadline>` — разворачивает правила в конкретные даты (`Deadline { form, kind, period_label /*«I полугодие 2026»*/, due_date, days_left }`), с переносом на следующий рабочий день, если срок падает на выходной (суббота/воскресенье; праздники РК — вне скоупа P2, задокументировать).
- Seed: сроки сдачи и уплаты для 910.00 и 300.00 по официальным источникам (Task 1 Step 1 их же и уточняет — вторичные источники противоречат: 910 «ежемесячно» vs «полугодие»; разрешить по НК и записать в `source_note`). 200.00 — задел, seed добавляется, если форма подтверждена как обязательная для нашего профиля.

- [ ] **Step 1: Падающие тесты**: `UpcomingReturnsSortedByDueDate`; `WeekendDueDateShiftsForward` (срок на субботу → понедельник); `HorizonFiltersFarDeadlines`; `PeriodLabelHumanReadable`; `SeedHasFormsNineTenAndThreeHundred`.
- [ ] **Step 2: Миграция + реализация**.
- [ ] **Step 3: Гейты + коммит** — `feat: tax deadline calendar`

---

### Task 7: ФНО 910.00 — XML-генератор

**Files:**
- Create: `src/tax/FnoXml.hpp`, `src/tax/Fno910.hpp`
- Modify: `vcpkg.json` (+`pugixml`, если выбран — см. Step 1), `CMakeLists.txt`
- Test: `tests/unit/test_fno910_xml.cpp`

**Interfaces:**
- Consumes: `Tax::TaxService::calculate_snr` результат, `Tenancy::Organization` (БИН, название), период.
- Produces: `Tax::Fno910::build_xml(const Calculation&, const OrgInfo&) → std::string`; `Tax::FnoXml` — общие хелперы (экранирование, форматирование сумм в тенге без тиынов — ФНО в целых тенге, округление half-up; формирование шапки документа с БИН/периодом).
- `OrgInfo { std::string bin, name, tax_period_year, tax_period_half; }` — определить в `FnoXml.hpp`, переиспользуется Task 8.

- [ ] **Step 1: Разведка формата (обязательна)**

Найти актуальную структуру XML ФНО 910.00 за 2026: XSD/примеры из СОНО (kgd.gov.kz — раздел «Программное обеспечение»/«Форматы электронных документов»), либо публичные репозитории интеграций. Зафиксировать в отчёте: найден ли официальный XSD; если да — положить в `templates/fno/910.00.xsd` и валидировать в тестах; если нет — строить XML по структуре из документации/примера и **пометить артефакт экспериментальным** (в API-ответе поле `"schema_validated": false`, в отчёте — риск). Выбор библиотеки: `pugixml` из vcpkg (если XSD-валидация не нужна) или ручная сборка строкой (если структура плоская); решение — в отчёт с обоснованием.

- [ ] **Step 2: Падающие тесты**: `BuildsWellFormedXml` (парсится обратно); `ContainsBinAndPeriod`; `AmountsInWholeTenge` (тиыны округлены half-up, без дробей); `EscapesSpecialCharsInOrgName` (`ТОО "Ромашка" & Ко` не ломает XML); при наличии XSD — `ValidatesAgainstXsd`.
- [ ] **Step 3: Реализация**.
- [ ] **Step 4: Гейты + коммит** — `feat: FNO 910.00 XML generator`

---

### Task 8: ФНО 300.00 — XML-генератор

**Files:**
- Create: `src/tax/Fno300.hpp`
- Test: `tests/unit/test_fno300_xml.cpp`

**Interfaces:**
- Consumes: `Tax::FnoXml` (Task 7), результат `calculate_vat`.
- Produces: `Tax::Fno300::build_xml(const Calculation&, const OrgInfo&) → std::string` — декларация по НДС за квартал: обороты по реализации, НДС начисленный, НДС в зачёт, сальдо к уплате/возврату.

- [ ] **Step 1: Разведка структуры 300.00** (как в Task 7 Step 1; XSD в `templates/fno/300.00.xsd`, если найден).
- [ ] **Step 2: Падающие тесты**: `BuildsWellFormedXml`; `NegativeBalanceGoesToRefundField` (сальдо < 0 → сумма к возврату, к уплате 0); `AmountsInWholeTenge`; `ContainsQuarterPeriod`; при наличии XSD — `ValidatesAgainstXsd`.
- [ ] **Step 3: Реализация + гейты + коммит** — `feat: FNO 300.00 XML generator`

---

### Task 9: Печатные формы ФНО — LaTeX-шаблоны 910 и 300

**Files:**
- Create: `templates/latex/fno_910/v1/{template.tex,schema.json,fixtures/basic.json,fixtures/special-chars.json}`, `templates/latex/fno_300/v1/{...}`

**Interfaces:** Consumes: docgen (P1) — inja `{{ }}`/`{% %}`, комментарии `((# #))`, гарды `!= ""`, автоэкранирование строк.

- [ ] **Step 1: fno_910** — schema: `{org: {bin, name}, period: {year, half}, income_tenge, rate_percent, tax_tenge, tax_words, signed_on, director, accountant}`; template.tex: шапка «Упрощённая декларация для субъектов малого бизнеса (форма 910.00)», блок реквизитов, таблица показателей (строка/код/сумма), подписи. Печатная форма — читабельный документ, не факсимиле бланка (задокументировать в README шаблона).
- [ ] **Step 2: fno_300** — schema: `{org, period: {year, quarter}, sales_tenge, vat_charged_tenge, vat_credited_tenge, balance_tenge, balance_kind /*"to_pay"|"to_refund"*/, balance_words, signed_on, director, accountant}`; аналогичная структура.
- [ ] **Step 3: Фикстуры (арифметика сходится, стресс-строка из 12 спецсимволов) + коммит** — `feat(templates): FNO 910 and 300 print forms`

---

### Task 10: Кадровые LaTeX-шаблоны (двуязычные)

**Files:**
- Create: `templates/latex/labor_contract/v1/{...}`, `templates/latex/hr_order/v1/{...}`, `templates/latex/payslip/v1/{...}`

**Interfaces:** Consumes: docgen. Двуязычность по спеке §8: каждый документ — две колонки (каз/рус) либо два блока подряд; для казахского нужен шрифт с полным кириллическим набором (Noto Sans покрывает ә/ғ/қ/ң/ө/ұ/ү/һ/і — проверено в P1 escape-тестах).

- [ ] **Step 1: labor_contract** — schema: `{number, signed_on, employer: {name, bin, address, director}, employee: {full_name, iin, address, position}, salary_tenge, salary_words, starts_on, ends_on, probation_months, work_schedule}` + казахские подписи полей в шаблоне (`Еңбек шарты / Трудовой договор` и т.п.). Обязательные разделы ТК РК: предмет, срок, режим работы, оплата, права/обязанности, реквизиты и подписи сторон.
- [ ] **Step 2: hr_order** — универсальный приказ: schema `{kind /*hire|dismiss|vacation|business_trip|salary_change*/, number, issued_on, employer, employee, effective_from, effective_to, reason, details, director}`; в template.tex заголовок и тело выбираются по `kind` через `{% if kind == "hire" %}` … (все пять веток, двуязычные заголовки).
- [ ] **Step 3: payslip** — расчётный листок: `{period_label, employer, employee, gross_tenge, opv, vosms, ipn, net, opvr, so, osms, social_tax, net_words}` — таблица начислений/удержаний/к выплате.
- [ ] **Step 4: Фикстуры каждому + коммит** — `feat(templates): bilingual labor contract, HR order and payslip`

---

### Task 11: API — кадры

**Files:**
- Create: `src/api/EmployeesController.hpp`, `src/api/HrController.hpp`
- Modify: `src/api/Api.hpp`, `src/api/Endpoints.hpp`, `docs/openapi.yaml`
- Test: `tests/integration/test_employees_api.cpp`, `tests/integration/test_hr_api.cpp`

**Interfaces:**
- Produces (все под `/api/v1`, guard-порядок как в `src/api/CounterpartiesController.hpp`): `GET/POST /employees`, `GET/PATCH /employees/{id}`, `POST /employees/{id}/dismiss` ({dismissed_on}); `GET/POST /hr-orders` (фильтр ?employee_id), `POST /hr-orders/{id}/generate-document` (→ docgen `hr_order`, привязка `document_id`, ответ 202 как у `/documents/generate`), `GET/POST /labor-contracts`, `POST /labor-contracts/{id}/generate-document`, `GET/POST /vacations`.
- Валидация: ИИН через `is_valid_bin_iin` → 422; `salary` строкой → `parse_tiyn` → 422 на кривой формат; даты — календарно-валидные (переиспользовать `is_valid_date` из `src/api/JournalController.hpp`; если он там `static` внутри файла — вынести в `src/api/Validation.hpp` и обновить обоих вызывающих).

- [ ] **Step 1: Падающие тесты** — на каждый роут: happy + viewer-403 + cross-org-404 + валидационные (ИИН «123» → 422; дата 2026-02-30 → 422; зарплата «abc» → 422).
- [ ] **Step 2: Контроллеры + triple-sync**.
- [ ] **Step 3: Гейты + коммит** — `feat: employees and HR API`

---

### Task 12: API — зарплата и налоги

**Files:**
- Create: `src/api/PayrollController.hpp`, `src/api/TaxController.hpp`
- Modify: `src/api/Api.hpp`, `src/api/Endpoints.hpp`, `docs/openapi.yaml`
- Test: `tests/integration/test_payroll_api.cpp`, `tests/integration/test_tax_api.cpp`

**Interfaces:**
- Produces: `GET /payroll-runs` (?year), `POST /payroll-runs` ({year, month} → расчёт, 200 с payslips), `POST /payroll-runs/{id}/approve`, `POST /payroll-runs/{id}/post-to-journal` (→ entry_id), `GET /payroll-runs/{id}/payslips`, `POST /payroll-runs/{id}/payslips/{employee_id}/generate-document` (docgen `payslip`);
  `GET /tax/rates` (?on=YYYY-MM-DD — действующие ставки и константы, для UI-подсказок), `POST /tax/calculations` ({kind: snr_simplified|vat, period_from, period_to} → расчёт со снапшотами), `GET /tax/calculations` (?kind&year), `GET /tax/alerts` (пороги), `GET /tax/deadlines` (?horizon_days=90), `POST /tax/filings` ({kind, calculation_id} → XML в S3 + печатный PDF через docgen; ответ 202 `{filing_id, xml_ready, render_queued}`), `GET /tax/filings` / `GET /tax/filings/{id}`, `POST /tax/filings/{id}/download-url` (?artifact=xml|pdf → presigned).
- Таблица `tax_filings` (org_id, kind, period_from, period_to, status CHECK IN ('draft','generated','submitted_manually'), calculation_id, xml_s3_key, document_id, schema_validated BOOL) — миграция 016 создаётся в этой задаче (SQL по идиомам 010; `UNIQUE(id, org_id)`, составные FK на calculations и documents).

- [ ] **Step 1: Падающие тесты** — по роутам: happy/viewer-403/cross-org-404/422; отдельно `PostToJournalReturnsBalancedEntry`; `FilingStoresXmlAndQueuesPdf` (в S3 появился объект с XML, документ создан, джоба поставлена); `DownloadUrlArtifactSwitch` (xml и pdf дают разные ключи; неизвестный artifact → 422).
- [ ] **Step 2: Миграция 016 + контроллеры + triple-sync**.
- [ ] **Step 3: Гейты + коммит** — `feat: payroll and tax API with FNO filings`

---

### Task 13: Frontend — сотрудники и кадры

**Files:**
- Create: `frontend/src/pages/Employees.tsx`, `frontend/src/pages/HrOrders.tsx`, `frontend/src/lib/schemas/hr.ts`
- Modify: `frontend/src/routes/manifest.tsx`, `frontend/src/lib/api/{types.ts,queryKeys.ts,schema.gen.ts}`

**Interfaces:** Consumes: API Task 11 через регенерированный клиент; идиомы `frontend/src/pages/Counterparties.tsx` (usePagedQuery, useApiMutation, ConfirmDialog, zod-схемы форм, деньги через `money.ts`).

- [ ] **Step 1: Кодоген + страница «Сотрудники»**: таблица (ФИО, ИИН, должность, оклад, статус), форма создания/редактирования (ИИН — 12 цифр, оклад строкой с маской «до 2 знаков»), действие «Уволить» (дата + ConfirmDialog).
- [ ] **Step 2: Страница «Кадры»**: приказы (список с фильтром по сотруднику, форма создания по типу приказа — пять вариантов полей), кнопка «Сформировать документ» → поллинг статуса → скачивание; трудовые договоры и отпуска — вкладками на той же странице.
- [ ] **Step 3: tsc/eslint чисто; роуты в манифест; коммит** — `feat(frontend): employees and HR pages`

---

### Task 14: Frontend — зарплата и налоги

**Files:**
- Create: `frontend/src/pages/Payroll.tsx`, `frontend/src/pages/Taxes.tsx`, `frontend/src/lib/schemas/tax.ts`
- Modify: `frontend/src/routes/manifest.tsx`, api-файлы

**Interfaces:** Consumes: API Task 12.

- [ ] **Step 1: «Зарплата»**: выбор периода (год/месяц) → кнопка «Рассчитать» → таблица payslip'ов (сотрудник, оклад, ОПВ, ВОСМС, ИПН, к выплате, ОПВР, СО, ОСМС, соцналог) с итогами; кнопки «Утвердить» и «Провести в учёт» (ConfirmDialog, после проведения — ссылка на проводку); «Расчётный листок» на строке → генерация документа.
- [ ] **Step 2: «Налоги»**: три блока — (а) дедлайны (список с днями до срока, подсветка ≤7 дней); (б) расчёты: выбор вида (упрощёнка/НДС) и периода → «Рассчитать» → карточка с числами из снапшота; (в) ФНО: кнопка «Сформировать» → 202 → поллинг → две ссылки (XML и PDF), баннер «схема не валидирована», если `schema_validated=false`; плюс блок алертов о порогах (НДС-регистрация, лимит упрощёнки) с текущими суммами.
- [ ] **Step 3: tsc/eslint; коммит** — `feat(frontend): payroll and taxes pages`

---

### Task 15: Деплой v0.3.0 и сквозной смоук фазы

**Files:**
- Modify: `helm/cpp-env/values-cybercapybara.yaml` (теги)

- [ ] **Step 1:** После зелёного CI и мержа — тег `v0.3.0` → релиз → образы `0.3.0`.
- [ ] **Step 2:** values 0.3.0 → `helm upgrade` → поды Ready; миграции 011-016 применились (проверить `tax_rates`, `employees`, `payroll_runs`, `tax_filings` в БД).
- [ ] **Step 3: Сквозной смоук** (через https://buh.cybercapybara.kz, admin-JWT): создать сотрудника с окладом 300 000 ₸ → рассчитать зарплату за месяц → payslip содержит ожидаемые суммы (сверить с golden-вектором Task 2) → утвердить → провести в учёт (проводка сбалансирована, статус posted) → сгенерировать расчётный листок (PDF в MinIO) → создать доходную проводку → рассчитать упрощёнку за полугодие → сформировать ФНО 910 (XML в S3 + PDF) → скачать оба по presigned-ссылкам (XML парсится, PDF начинается с `%PDF`) → `GET /tax/deadlines` возвращает ближайшие сроки. Каждый шаг с выводом — в отчёт.
- [ ] **Step 4: Коммит values + пуш** — `deploy: release v0.3.0 (tax, payroll, HR)`

---

## Definition of Done (фаза P2)

1. CI зелёный: новые сьюты (справочник ставок, калькулятор с golden-векторами, кадры, payroll-сервис, налоговые расчёты, календарь, оба XML-генератора, 5 API-сьютов) + `template-render` рендерит 5 новых шаблонов (910, 300, трудовой договор, приказ, расчётный листок) в PDF.
2. Ни одной налоговой константы в коде — все значения резолвятся из `tax_rates`/`tax_constants` по дате; каждая seed-строка несёт `source_note` со ссылкой на официальный источник.
3. Golden-тесты зарплаты воспроизводят независимо посчитанные значения до тиына; расхождение с публичным калькулятором объяснено в отчёте.
4. ФНО 910 и 300 генерируются в XML (валидны структурно; при наличии XSD — валидируются) и в печатный PDF; артефакты лежат в S3, скачиваются по presigned-ссылкам.
5. Кадровые документы двуязычны и рендерятся; расчётный листок сходится с payslip.
6. Зарплата проводится в учёт сбалансированной проводкой (Σдебет = Σкредит, posted).
7. Сквозной смоук P2 (Task 15 Step 3) пройден в проде на v0.3.0.
