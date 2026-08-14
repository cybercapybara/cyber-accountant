# P1: Учётное ядро + Docgen — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Работающий учётный контур: контрагенты → проводки двойной записи (insert-only, сторно) → документы → генерация PDF из LaTeX-шаблонов с хранением в MinIO — доступный через API и SPA.

**Architecture:** Два новых модуля по спеке §4: `src/ledger/` (план счетов, проводки, контрагенты, документы) и `src/docgen/` (реестр LaTeX-шаблонов, inja-рендер, XeLaTeX-джоба в worker); `files` использует уже готовый `Storage::S3Storage` шаблона (SigV4) против in-cluster MinIO. Все доменные таблицы — org-scoped через `Tenancy::OrgCrudBase` (P0). Рендер PDF — только в worker-образе (TeX Live добавляется туда и только туда).

**Tech Stack:** Drogon C++20, PostgreSQL 15 (DEFERRABLE constraint triggers), Redis jobs, inja (уже в FetchContent), json-schema-validator (новая vcpkg-зависимость), XeLaTeX (TeX Live в worker), MinIO (S3 SigV4), React SPA.

**Spec:** `docs/superpowers/specs/2026-08-14-cyber-accountant-design.md` (§6 ledger, §9 docgen, §13 API, §17 фаза P1)

## Global Constraints

- **Сборки/тесты только в GitHub Actions** (директива владельца): локально разрешены git, kubectl/helm, clang-format 17.0.6 (venv/pipx), `npx tsc --noEmit`, eslint, кодоген, shell-гейты `./scripts/check-*.sh`. Никаких make test / docker / npm run build локально. Тестовая истина — CI на PR.
- Процесс: ветка `feature/p1-ledger-docgen` от main, PR в main; имплементеры коммитят, НЕ пушат (пуш — контроллер).
- Triple-sync: каждый роут = контроллер + `src/api/Endpoints.hpp` + `docs/openapi.yaml`; гейты `check-openapi-drift.sh`/`check-routes-registered.sh` PASS перед каждым коммитом.
- `src/` header-only (ADR 0003); ошибки только `ErrorResponse::*` / `Api::Validation::*`; conventional commits **без AI-attribution трейлеров**.
- Все новые доменные таблицы: `org_id UUID NOT NULL REFERENCES organizations(id)`, репозитории наследуют `Tenancy::OrgCrudBase` (`src/tenancy/OrgScoped.hpp`) — глобальных методов чтения не существует. **Единственное задокументированное исключение:** `accounts.org_id NULLABLE` — NULL означает системную строку типового плана счетов (спека §6.1: системный план + субсчета тенанта); чтение всегда «системные + свои», никогда чужие.
- Ledger insert-only (спека §6.2): posted-запись неизменяема (триггер), исправление только сторно; Σдебет = Σкредит на каждую проводку (DEFERRABLE constraint trigger + сервис).
- Миграции `migrations/NNN_slug.sql` последовательно с 007, без BEGIN/COMMIT; updated_at — `touch_updated_at()` + DROP/CREATE TRIGGER (образец: `migrations/006_organizations.sql`).
- Идиомы кода — зеркалить P0-файлы: домен `src/tenancy/Organization.hpp` (шаблонный `from_row`, ADL `to_json`), репозитории `src/tenancy/OrganizationRepository.hpp` (`translate_sql` → типизированные конфликты), контроллеры `src/api/OrganizationsController.hpp` (guard-порядок, `parse_page_params`, `with_repo_errors`), тесты `tests/integration/test_organizations_api.cpp` (fixture, seed-хелперы, `API_REQUIRE_ORG`-путь).
- Деньги: `NUMERIC(18,2)` в БД, в C++ — строки в домене + `long long` тиынов в расчётных инвариантах (никаких double для денег).
- LaTeX-шаблоны версионируются `templates/latex/<slug>/v1/`; вход валидируется JSON Schema до рендера; `\write18` отключён (`-no-shell-escape`).

**Известный входной факт:** секрет `s3-credentials` уже существует в namespace `cyber-accountant` (S3_ENDPOINT=http://minio.minio.svc.cluster.local:9000, S3_BUCKET=cyber-accountant-prod, S3_REGION=us-east-1, S3_ACCESS_KEY/S3_SECRET_KEY); бакет создан, доступ изолирован на бакет.

---

### Task 1: Валидатор БИН/ИИН (контрольный разряд)

**Files:**
- Create: `src/ledger/KzIdentifiers.hpp`
- Test: `tests/unit/test_kz_identifiers.cpp`

**Interfaces:**
- Produces: `namespace Ledger { bool is_valid_bin_iin(const std::string& id12); }` — 12 цифр + контрольный разряд по алгоритму РК; `false` на всё остальное (длина, нецифры, неверная сумма). Используется Task 2 (контрагенты) и позже HR/ЭСФ.

- [ ] **Step 1: Сгенерировать golden-векторы**

Алгоритм РК: `d[0..11]`; `s = Σ d[i]*w1[i] mod 11`, `w1 = [1..11]`; если `s == 10`: `s = Σ d[i]*w2[i] mod 11`, `w2 = [3,4,5,6,7,8,9,10,11,1,2]`; если снова 10 — идентификатор невалиден; иначе валиден ⇔ `d[11] == s`. Сгенерировать 6 валидных векторов независимой реализацией:

```bash
python3 - <<'EOF'
import random
w1=list(range(1,12)); w2=[3,4,5,6,7,8,9,10,11,1,2]
def check(d):
    s=sum(a*b for a,b in zip(d,w1))%11
    if s==10:
        s=sum(a*b for a,b in zip(d,w2))%11
        if s==10: return None
    return s
random.seed(42)
out=[]
while len(out)<6:
    d=[random.randint(0,9) for _ in range(11)]
    c=check(d)
    if c is not None: out.append(''.join(map(str,d))+str(c))
print(out)
EOF
```

Вывод скрипта (детерминирован seed'ом) вставить в тест как валидные векторы.

- [ ] **Step 2: Падающий unit-тест**

`tests/unit/test_kz_identifiers.cpp`:

```cpp
#include <gtest/gtest.h>
#include "ledger/KzIdentifiers.hpp"

TEST(KzIdentifiers, AcceptsValidChecksums) {
    for (const auto* id : {/* 6 векторов из Step 1 */}) EXPECT_TRUE(Ledger::is_valid_bin_iin(id)) << id;
}
TEST(KzIdentifiers, RejectsMutatedCheckDigit) {
    // каждый валидный вектор с последней цифрой (d+1)%10 → false
}
TEST(KzIdentifiers, RejectsMalformed) {
    for (const auto* id : {"", "123", "12345678901", "1234567890123", "12345678901a", "12345678901 "})
        EXPECT_FALSE(Ledger::is_valid_bin_iin(id)) << id;
}
```

Тела дописать полностью (векторы из Step 1 инлайном).

- [ ] **Step 3: Реализация**

```cpp
#pragma once
#include <array>
#include <cctype>
#include <string>

namespace Ledger {

/// Контрольный разряд БИН/ИИН РК (12 цифр, две системы весов, 10 → невалидно).
inline bool is_valid_bin_iin(const std::string& id) {
    if (id.size() != 12) return false;
    std::array<int, 12> d{};
    for (int i = 0; i < 12; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(id[i]))) return false;
        d[i] = id[i] - '0';
    }
    static constexpr std::array<int, 11> w1{1,2,3,4,5,6,7,8,9,10,11};
    static constexpr std::array<int, 11> w2{3,4,5,6,7,8,9,10,11,1,2};
    auto weighted = [&](const std::array<int, 11>& w) {
        int s = 0;
        for (int i = 0; i < 11; ++i) s += d[i] * w[i];
        return s % 11;
    };
    int s = weighted(w1);
    if (s == 10) {
        s = weighted(w2);
        if (s == 10) return false;
    }
    return d[11] == s;
}

}  // namespace Ledger
```

- [ ] **Step 4: Локальные гейты + коммит**

```bash
make fmt 2>/dev/null || <clang-format-17> -i src/ledger/KzIdentifiers.hpp tests/unit/test_kz_identifiers.cpp
./scripts/check-test-buckets.sh
git add src/ledger tests/unit/test_kz_identifiers.cpp
git commit -m "feat: KZ BIN/IIN checksum validator"
```

---

### Task 2: Контрагенты — миграция 007, домен, репозиторий

**Files:**
- Create: `migrations/007_counterparties.sql`, `src/ledger/Counterparty.hpp`, `src/ledger/CounterpartyRepository.hpp`
- Test: `tests/integration/test_counterparties.cpp`

**Interfaces:**
- Consumes: `Tenancy::OrgCrudBase`, `Ledger::is_valid_bin_iin` (валидация на API-слое — Task 12; репозиторий хранит как есть).
- Produces: `Ledger::Counterparty { std::string id, org_id, identifier /*БИН/ИИН*/, name, address, iik, bik, kbe, contact_email, created_at, updated_at; bool is_resident, vat_payer; }` + `from_row`/ADL `to_json`; `Ledger::CounterpartyRepository : Tenancy::OrgCrudBase<...>` (kTable="counterparties", kOrgColumn="org_id") c `create(org_id, Counterparty draft) → Counterparty` (конфликт `DuplicateCounterparty` на UNIQUE(org_id, identifier)), `update(org_id, id, Counterparty patch) → std::optional<Counterparty>`, `find_by_identifier(org_id, identifier) → std::optional<Counterparty>`.

- [ ] **Step 1: Миграция**

```sql
-- counterparties: контрагенты организации (спека §6.3). Org-scoped.
CREATE TABLE IF NOT EXISTS counterparties (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id        UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    identifier    CHAR(12) NOT NULL,           -- БИН или ИИН
    name          TEXT NOT NULL,
    address       TEXT NOT NULL DEFAULT '',
    iik           TEXT NOT NULL DEFAULT '',    -- IBAN KZ.. (формат не валидируем в P1)
    bik           TEXT NOT NULL DEFAULT '',
    kbe           TEXT NOT NULL DEFAULT '',
    is_resident   BOOLEAN NOT NULL DEFAULT TRUE,
    vat_payer     BOOLEAN NOT NULL DEFAULT FALSE,
    contact_email TEXT NOT NULL DEFAULT '',
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_id, identifier)
);
DROP TRIGGER IF EXISTS trg_counterparties_touch ON counterparties;
CREATE TRIGGER trg_counterparties_touch BEFORE UPDATE ON counterparties
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
CREATE INDEX IF NOT EXISTS idx_counterparties_org ON counterparties (org_id);
```

(`make new-migration SLUG=counterparties` для номера; триггер-идиому сверить с 006.)

- [ ] **Step 2: Падающие интеграционные тесты** — по fixture-идиоме `tests/integration/test_tenancy_repositories.cpp` (CoreBackedTest, truncate в SetUp, `return 0;` в write-лямбдах): CreateFindByIdentifier; DuplicateIdentifierSameOrgRejected (типизированный конфликт); SameIdentifierDifferentOrgsAllowed (две организации, один identifier — обе создаются); UpdatePatchesFields; CrossOrgReadIsolated (find_in_org чужого — nullopt). Каждый — полным телом.

- [ ] **Step 3: Домен + репозиторий** — зеркалить `Organization.hpp`/`OrganizationRepository.hpp` (включая `translate_sql` → `DuplicateCounterparty : ConflictError`). `update` — один UPDATE со всеми правимыми полями + `RETURNING`, `WHERE id=$1 AND org_id=$2`.

- [ ] **Step 4: Гейты + коммит** — `feat: counterparties registry`

---

### Task 3: План счетов — миграция 008 с seed, домен, репозиторий

**Files:**
- Create: `migrations/008_accounts.sql`, `src/ledger/Account.hpp`, `src/ledger/AccountRepository.hpp`
- Test: `tests/integration/test_accounts.cpp`

**Interfaces:**
- Produces: `Ledger::Account { std::string id, code, name_ru, name_kk, type /*asset|liability|equity|income|expense*/; std::optional<std::string> org_id /*NULL=системный*/, parent_code; bool currency_tracked; }`; `Ledger::AccountRepository` (НЕ OrgCrudBase — задокументированное исключение): `list_visible(org_id) → std::vector<Account>` (системные + свои, ORDER BY code), `find_visible(org_id, code) → std::optional<Account>`, `create_subaccount(org_id, code, name_ru, name_kk, parent_code) → Account` (родитель обязан существовать и быть видимым; код субсчёта начинается с кода родителя; конфликт `DuplicateAccount`).

- [ ] **Step 1: Миграция + seed**

```sql
-- accounts: типовой план счетов РК (приказ МФ РК №185), системные строки org_id IS NULL;
-- субсчета тенантов org_id NOT NULL (спека §6.1). Единственная таблица с NULLABLE org_id —
-- см. Global Constraints плана.
CREATE TABLE IF NOT EXISTS accounts (
    id               UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id           UUID REFERENCES organizations(id) ON DELETE CASCADE,
    code             TEXT NOT NULL,
    name_ru          TEXT NOT NULL,
    name_kk          TEXT NOT NULL DEFAULT '',
    type             TEXT NOT NULL CHECK (type IN ('asset','liability','equity','income','expense')),
    parent_code      TEXT,
    currency_tracked BOOLEAN NOT NULL DEFAULT FALSE,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);
DROP TRIGGER IF EXISTS trg_accounts_touch ON accounts;
CREATE TRIGGER trg_accounts_touch BEFORE UPDATE ON accounts
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
-- Уникальность кода: среди системных и внутри каждого тенанта.
CREATE UNIQUE INDEX IF NOT EXISTS uq_accounts_system_code ON accounts (code) WHERE org_id IS NULL;
CREATE UNIQUE INDEX IF NOT EXISTS uq_accounts_org_code ON accounts (org_id, code) WHERE org_id IS NOT NULL;

INSERT INTO accounts (code, name_ru, type) VALUES
 ('1010','Денежные средства в кассе','asset'),
 ('1030','Денежные средства на текущих банковских счетах','asset'),
 ('1210','Краткосрочная дебиторская задолженность покупателей и заказчиков','asset'),
 ('1250','Краткосрочная дебиторская задолженность работников','asset'),
 ('1310','Сырье и материалы','asset'),
 ('1330','Товары','asset'),
 ('1420','Налог на добавленную стоимость','asset'),
 ('1610','Краткосрочные авансы выданные','asset'),
 ('2410','Основные средства','asset'),
 ('2420','Амортизация основных средств','asset'),
 ('2730','Прочие нематериальные активы','asset'),
 ('3010','Краткосрочные финансовые обязательства','liability'),
 ('3110','Корпоративный подоходный налог, подлежащий уплате','liability'),
 ('3120','Индивидуальный подоходный налог','liability'),
 ('3130','Налог на добавленную стоимость','liability'),
 ('3150','Социальный налог','liability'),
 ('3210','Обязательства по социальному страхованию','liability'),
 ('3220','Обязательства по пенсионным отчислениям','liability'),
 ('3230','Прочие обязательства по другим обязательным платежам','liability'),
 ('3310','Краткосрочная кредиторская задолженность поставщикам и подрядчикам','liability'),
 ('3350','Краткосрочная задолженность по оплате труда','liability'),
 ('3510','Краткосрочные авансы полученные','liability'),
 ('4110','Долгосрочная кредиторская задолженность поставщикам и подрядчикам','liability'),
 ('5010','Объявленный уставный капитал','equity'),
 ('5510','Нераспределенная прибыль (непокрытый убыток) отчетного года','equity'),
 ('5520','Нераспределенная прибыль (непокрытый убыток) предыдущих лет','equity'),
 ('6010','Доход от реализации продукции и оказания услуг','income'),
 ('6110','Доходы по вознаграждениям','income'),
 ('6250','Доход от курсовой разницы','income'),
 ('6280','Прочие доходы','income'),
 ('7010','Себестоимость реализованной продукции и оказанных услуг','expense'),
 ('7110','Расходы по реализации продукции и оказанию услуг','expense'),
 ('7210','Административные расходы','expense'),
 ('7310','Расходы по вознаграждениям','expense'),
 ('7430','Расходы по курсовой разнице','expense'),
 ('7470','Прочие расходы','expense')
ON CONFLICT DO NOTHING;
```

Шаг обязателен: **сверить коды/названия с официальным текстом приказа №185** (WebSearch «типовой план счетов приказ 185 РК» — kgd.gov.kz/adilet.zan.kz) и поправить расхождения до коммита; в отчёте перечислить проверенные источники. name_kk заполняются в P2 (кадры/двуязычие печатных форм ФНО), для P1 пустые — это отражено в дефолте.

- [ ] **Step 2: Падающие тесты**: SeedVisibleToAnyOrg (list_visible нового org ≥ 36 строк, содержит 1030 и 6010, отсортирован); CreateSubaccountUnderSystemParent (org создаёт '1030.1' с parent 1030); SubaccountCodeMustExtendParent (parent 1030, code '9999' → отказ типизированной ошибкой `InvalidSubaccount`); SubaccountsIsolatedBetweenOrgs; DuplicateSystemCodeForbidden (create_subaccount с code '1030' → конфликт).

- [ ] **Step 3: Реализация** — репозиторий с ручными запросами (`WHERE org_id IS NULL OR org_id = $1`); `create_subaccount` наследует `type`/`currency_tracked` родителя.

- [ ] **Step 4: Гейты + коммит** — `feat: chart of accounts with KZ standard seed`

---

### Task 4: Журнал — миграция 009 (entries, lines, инварианты-триггеры)

**Files:**
- Create: `migrations/009_journal.sql`
- Test: `tests/integration/test_journal_schema.cpp`

**Interfaces:**
- Produces: таблицы `journal_entries` (status draft|posted|reversed, reverses_entry_id, created_by_user_id, created_by_run_id NULL — задел под агента) и `journal_lines` (account_code, side debit|credit, amount NUMERIC(18,2)>0, counterparty_id NULL, vat_amount NUMERIC(18,2) NULL); триггеры: (a) DEFERRABLE INITIALLY DEFERRED constraint-trigger баланса Σdebit=Σcredit per entry на COMMIT; (b) немутируемость: UPDATE journal_entries при OLD.status='posted' разрешён только как смена status→'reversed' без изменения прочих полей; DELETE posted/reversed запрещён; UPDATE/DELETE journal_lines разрешён только пока entry в draft.

- [ ] **Step 1: Падающие тесты схемы** (полными телами, идиома test_tenancy_schema.cpp): BalancedEntryCommits (шапка + 2 строки 100.00 D/K → commit ок); UnbalancedEntryRejectedAtCommit (одна строка → исключение на commit); PostedEntryImmutable (UPDATE description у posted → исключение); PostedLinesImmutable (UPDATE amount строки posted-проводки → исключение); DraftFreelyEditable; PostedCanOnlyTransitionToReversed.

- [ ] **Step 2: Миграция**

```sql
-- journal: двойная запись, insert-only (спека §6.2).
CREATE TABLE IF NOT EXISTS journal_entries (
    id                 UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id             UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    entry_date         DATE NOT NULL,
    description        TEXT NOT NULL DEFAULT '',
    status             TEXT NOT NULL DEFAULT 'draft' CHECK (status IN ('draft','posted','reversed')),
    reverses_entry_id  UUID REFERENCES journal_entries(id),
    created_by_user_id UUID REFERENCES users(id),
    created_by_run_id  UUID,             -- agent_runs появятся в P4; без FK до тех пор
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);
DROP TRIGGER IF EXISTS trg_journal_entries_touch ON journal_entries;
CREATE TRIGGER trg_journal_entries_touch BEFORE UPDATE ON journal_entries
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
CREATE INDEX IF NOT EXISTS idx_journal_entries_org_date ON journal_entries (org_id, entry_date DESC);

CREATE TABLE IF NOT EXISTS journal_lines (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    entry_id        UUID NOT NULL REFERENCES journal_entries(id) ON DELETE CASCADE,
    account_code    TEXT NOT NULL,
    side            TEXT NOT NULL CHECK (side IN ('debit','credit')),
    amount          NUMERIC(18,2) NOT NULL CHECK (amount > 0),
    counterparty_id UUID REFERENCES counterparties(id),
    vat_amount      NUMERIC(18,2) CHECK (vat_amount IS NULL OR vat_amount >= 0),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_journal_lines_entry ON journal_lines (entry_id);
CREATE INDEX IF NOT EXISTS idx_journal_lines_org_account ON journal_lines (org_id, account_code);

CREATE OR REPLACE FUNCTION journal_entry_must_balance() RETURNS trigger AS $$
DECLARE
    eid UUID;
    diff NUMERIC(18,2);
BEGIN
    eid := COALESCE(NEW.entry_id, OLD.entry_id);
    SELECT COALESCE(SUM(CASE side WHEN 'debit' THEN amount ELSE -amount END), 0)
      INTO diff FROM journal_lines WHERE entry_id = eid;
    IF diff <> 0 THEN
        RAISE EXCEPTION 'journal entry % is unbalanced by %', eid, diff
            USING ERRCODE = 'check_violation';
    END IF;
    RETURN NULL;
END $$ LANGUAGE plpgsql;
DROP TRIGGER IF EXISTS trg_journal_balance ON journal_lines;
CREATE CONSTRAINT TRIGGER trg_journal_balance
    AFTER INSERT OR UPDATE OR DELETE ON journal_lines
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW
    EXECUTE FUNCTION journal_entry_must_balance();

CREATE OR REPLACE FUNCTION journal_entries_immutability() RETURNS trigger AS $$
BEGIN
    IF TG_OP = 'DELETE' THEN
        IF OLD.status <> 'draft' THEN
            RAISE EXCEPTION 'posted/reversed journal entries are insert-only'
                USING ERRCODE = 'check_violation';
        END IF;
        RETURN OLD;
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
    END IF;
    RETURN NEW;  -- draft свободно правится (updated_at меняет touch-триггер)
END $$ LANGUAGE plpgsql;
DROP TRIGGER IF EXISTS trg_journal_entries_immutable ON journal_entries;
CREATE TRIGGER trg_journal_entries_immutable BEFORE UPDATE OR DELETE ON journal_entries
    FOR EACH ROW EXECUTE FUNCTION journal_entries_immutability();

CREATE OR REPLACE FUNCTION journal_lines_frozen_after_post() RETURNS trigger AS $$
DECLARE st TEXT;
BEGIN
    SELECT status INTO st FROM journal_entries WHERE id = COALESCE(NEW.entry_id, OLD.entry_id);
    IF st IS DISTINCT FROM 'draft' AND TG_OP <> 'INSERT' THEN
        RAISE EXCEPTION 'lines of a % entry are immutable', st USING ERRCODE = 'check_violation';
    END IF;
    IF st IS DISTINCT FROM 'draft' AND TG_OP = 'INSERT' THEN
        RAISE EXCEPTION 'cannot add lines to a % entry', st USING ERRCODE = 'check_violation';
    END IF;
    RETURN COALESCE(NEW, OLD);
END $$ LANGUAGE plpgsql;
DROP TRIGGER IF EXISTS trg_journal_lines_frozen ON journal_lines;
CREATE TRIGGER trg_journal_lines_frozen BEFORE INSERT OR UPDATE OR DELETE ON journal_lines
    FOR EACH ROW EXECUTE FUNCTION journal_lines_frozen_after_post();
```

Нюанс: сторно-проводка вставляет строки в НОВУЮ draft-проводку — `trg_journal_lines_frozen` этому не мешает. Проведение (draft→posted) строк не трогает.

- [ ] **Step 3: Тесты зелёные по CI-логике** — коммит после локальных гейтов: `feat: double-entry journal schema with immutability triggers`

---

### Task 5: Журнал — домен, репозиторий, сервис (draft/post/reverse)

**Files:**
- Create: `src/ledger/JournalEntry.hpp` (+ `JournalLine` в нём), `src/ledger/JournalRepository.hpp`, `src/ledger/JournalService.hpp`
- Test: `tests/integration/test_journal_service.cpp`

**Interfaces:**
- Consumes: таблицы Task 4, `AccountRepository::find_visible`, `CounterpartyRepository` (find_in_org), `Tenancy::OrgCrudBase`.
- Produces:
  - `Ledger::JournalLine { std::string id, entry_id, account_code, side, amount; std::optional<std::string> counterparty_id, vat_amount; }`;
  - `Ledger::JournalEntry { std::string id, org_id, entry_date, description, status; std::optional<std::string> reverses_entry_id, created_by_user_id; std::vector<JournalLine> lines; }`;
  - `Ledger::JournalRepository : Tenancy::OrgCrudBase<...>` (kTable="journal_entries") + `load_lines(entry) `, `list_in_org_with_lines(org_id, limit, offset)`;
  - `Ledger::JournalService`: `create_draft(org_id, user_id, entry_date, description, std::vector<JournalLine> lines) → JournalEntry` (валидации: ≥2 строк, все счета видимы org'у, контрагенты принадлежат org'у, Σдебет=Σкредит по тиынам — до БД, чтобы отдавать 422 вместо 500), `post(org_id, entry_id) → std::optional<JournalEntry>` (draft→posted), `reverse(org_id, entry_id, user_id) → std::optional<JournalEntry>` (для posted: новая проводка с зеркальными сторонами строк, description «Сторно: <оригинал>», reverses_entry_id; оригинал → status reversed; ВСЁ в одной транзакции), typed-ошибки `UnbalancedEntry`, `UnknownAccount`, `InvalidEntryState`.
- Деньги в сервисе: `amount` строк приходит строкой "1234.56"; парсер в тиыны `long long parse_tiyn(const std::string&)` (две цифры после точки максимум, отказ на прочее) — положить в `JournalService.hpp`, юнит-покрыть в этом же тест-файле через integration-бакет.

- [ ] **Step 1: Падающие тесты** (полные тела): CreateDraftBalanced; CreateDraftUnbalancedRejected422Path (typed UnbalancedEntry ДО БД); UnknownAccountRejected; ForeignCounterpartyRejected (контрагент другого org); PostTransitions; PostIdempotentRejected (post уже posted → InvalidEntryState); ReverseCreatesMirrorAndMarksOriginal (стороны строк перевёрнуты, суммы равны, оригинал reversed, новая posted? — решение: сторно создаётся сразу posted в той же транзакции); ReverseDraftRejected; CrossOrgInvisible.

- [ ] **Step 2: Реализация** — репозиторий зеркалит OrgMemberRepository-стиль; сервис — заголовок с чистой логикой поверх репозитория, транзакционность через `Database::get().execute_write` с одной лямбдой на всю операцию reverse.

- [ ] **Step 3: Гейты + коммит** — `feat: journal service with draft/post/storno lifecycle`

---

### Task 6: Документы — миграция 010, домен, репозиторий

**Files:**
- Create: `migrations/010_documents.sql`, `src/ledger/Document.hpp`, `src/ledger/DocumentRepository.hpp`
- Test: `tests/integration/test_documents.cpp`

**Interfaces:**
- Produces: `documents` (спека §6.4: doc_type CHECK IN ('invoice','avr','waybill','tax_invoice','reconciliation','power_of_attorney','incoming','bank_statement','hr','fno','other'); source CHECK IN ('generated','uploaded','email'); status CHECK IN ('inbox','recognized','linked','archived','draft','final','sent'); counterparty_id NULL; s3_key, checksum_sha256, mime, size_bytes, template_slug NULL, template_version NULL, input_snapshot JSONB NULL); `document_entries` (document_id, entry_id, UNIQUE pair, оба FK CASCADE, org_id NOT NULL); `Ledger::Document` домен + `Ledger::DocumentRepository : OrgCrudBase` c `create(...) → Document`, `set_file(org_id, id, s3_key, checksum, mime, size) → bool`, `set_status(org_id, id, status) → bool`, `link_entry(org_id, document_id, entry_id) → bool` (оба объекта того же org — проверка в SQL через EXISTS), `list_for_entry(org_id, entry_id)`.

- [ ] **Step 1: Миграция** — полный SQL по контракту выше (идиомы 006/007; оба индекса: `(org_id, created_at DESC)`, `(org_id, doc_type)`).
- [ ] **Step 2: Падающие тесты**: CreateGeneratedDraft; LinkEntrySameOrg; LinkEntryForeignOrgRejected; StatusTransitions (draft→final→sent — без машины состояний в P1, любой из CHECK-списка, но тест фиксирует happy-path); ListByType.
- [ ] **Step 3: Реализация + гейты + коммит** — `feat: documents registry with journal links`

---

### Task 7: files — S3-конфиг, presigned URL, раскладка, MinIO в тестовом контуре

**Files:**
- Modify: `config/config.json` (+`storage.s3.*` блок), `docker/docker-compose.yml` (сервис minio для интеграционных тестов), `.github/workflows/ci.yml` (если тестовый compose-профиль требует правки env), `src/storage/Storage.hpp` (ТОЛЬКО если presign отсутствует — см. Step 1)
- Create: `src/files/FileKeys.hpp`
- Test: `tests/integration/test_s3_storage.cpp`

**Interfaces:**
- Consumes: `Storage::S3Storage` (уже в шаблоне: put/get/remove/exists/list/url, SigV4, `Storage.hpp:177+`), init-плюмбинг `Storage.hpp:490-510`.
- Produces: `Files::org_key(org_id, kind /*"generated"|"inbox"|"statements"*/, filename) → std::string` («org/{org_id}/{kind}/{uuid}-{sanitized-filename}» — uuid генерирует вызывающий через существующую утилиту шаблона, найти её grep'ом `uuid` по src/utils); `Storage::S3Storage::presign(key, method /*GET|PUT*/, ttl_sec) → std::string` — если в шаблоне уже есть presigned-механизм в `url()`, переиспользовать/обернуть; если нет — добавить SigV4 query-подпись (X-Amz-Algorithm/Credential/Date/Expires/SignedHeaders/Signature) по образцу существующего `request()`-подписанта в том же файле.

- [ ] **Step 1: Разведка** — прочитать `src/storage/Storage.hpp:300-520` целиком: что делает `url()`, есть ли query-подпись. Решение зафиксировать в отчёте.
- [ ] **Step 2: MinIO в тестовый compose** — сервис `minio` (образ `quay.io/minio/minio:RELEASE.2024-12-18T13-15-44Z`, `server /data`, MINIO_ROOT_USER=test, MINIO_ROOT_PASSWORD=test-secret-key, healthcheck `mc ready local` или curl :9000/minio/health/live) + init-контейнер/step создания бакета `test-bucket` (образ quay.io/minio/mc, `mc alias set + mc mb`). Подключить к тому же профилю, что Postgres/Redis тестов (посмотреть, как test-джоба compose поднимает зависимости — зеркалить).
- [ ] **Step 3: Падающие интеграционные тесты**: PutGetRoundTrip; ExistsRemove; ListPrefix; PresignedGetFetchesViaCurl (подписанный URL скачивается голым curl-вызовом изнутри теста — через существующий http-клиент утилит или popen curl); PresignedPutUploads; OrgKeyLayout (unit-подобный, формат ключа + санитизация имени файла: пробелы→_, только [A-Za-z0-9._-]). Тесты используют env `S3_TEST_ENDPOINT` etc. с дефолтами на compose-сервис; при недоступности MinIO — GTEST_SKIP (идиома «требует инфраструктуры» — сверить, как это делают существующие integration-тесты с Redis).
- [ ] **Step 4: Реализация** (config-блок: `"s3": { "endpoint": "${S3_ENDPOINT:-}", "region": "${S3_REGION:-us-east-1}", "bucket": "${S3_BUCKET:-}", "access_key": "${S3_ACCESS_KEY:-}", "secret_key": "${S3_SECRET_KEY:-}", "public_base_url": "${S3_PUBLIC_BASE_URL:-}" }` — точные ключи сверить с парсером Storage.hpp:490-510; расхождение чинится в config.json, не в парсере).
- [ ] **Step 5: Гейты + коммит** — `feat: S3 file layout, presigned URLs, MinIO test harness`

---

### Task 8: Docgen-ядро — реестр шаблонов, inja-рендер, XeLaTeX-джоба, TeX в worker

**Files:**
- Create: `src/docgen/TemplateRegistry.hpp`, `src/docgen/LatexEscape.hpp`, `src/docgen/Renderer.hpp`, `src/docgen/RenderJob.hpp`, `scripts/render-templates.sh`, `templates/latex/README.md`
- Modify: `docker/Dockerfile` (worker-стадия: TeX Live), `vcpkg.json` (+`json-schema-validator`), `.github/workflows/ci.yml` (джоба `template-render`), `src/worker_main.cpp` ИЛИ фактическая точка регистрации джобов (`JOBS_REGISTER`-идиома, `src/jobs/Dispatcher.hpp:88`)
- Test: `tests/unit/test_latex_escape.cpp`, `tests/unit/test_template_registry.cpp`, `tests/integration/test_render_job.cpp`

**Interfaces:**
- Consumes: inja (FetchContent, см. CMakeLists.txt `$comment` в vcpkg.json), jobs-фреймворк (`Dispatcher::register_handler`), `Storage::storage()`, `Ledger::DocumentRepository::set_file/set_status`, `Files::org_key`.
- Produces:
  - `Docgen::escape_latex(const std::string&) → std::string` — экранирует `\ { } $ & # ^ _ % ~ < >` (^ и ~ через `\textasciicircum{}`/`\textasciitilde{}`, < > через `\textless{}`/`\textgreater{}`, `\` → `\textbackslash{}`);
  - `Docgen::TemplateRegistry`: сканирует `templates/latex/<slug>/v<N>/` (template.tex + schema.json + fixtures/*.json), `latest(slug) → TemplateInfo{slug, version, tex_path, schema}`, `validate(slug, input_json) → std::optional<std::string /*error*/> ` (json-schema-validator);
  - `Docgen::render_tex(TemplateInfo, input_json) → std::string` — inja-рендер: все строковые значения прогоняются через escape_latex автоматически (callback/предобработка дерева), числа/booleans — как есть; inja-делимитеры по умолчанию `{{ }}`;
  - `Docgen::RenderJob` — обработчик типа `docgen.render` (payload: org_id, document_id, slug, input json): validate → render_tex → записать main.tex во временную директорию → `xelatex -interaction=nonstopmode -halt-on-error -no-shell-escape` дважды (ссылки/итоги) с таймаутом (posix_spawn/`Utils`-обёртка процессов — если в шаблоне есть готовая, использовать её; иначе `popen` с `timeout(1)`-префиксом координатной утилиты `/usr/bin/timeout 60`) → PDF → sha256 (`Utils::Crypto`) → `storage().put(org_key(...), pdf, "application/pdf")` → `documents.set_file + set_status('final')`; при любой ошибке — job fail (ретраи/DLQ существующего фреймворка), документ остаётся draft;
  - worker-образ: TeX Live (`texlive-xetex texlive-latex-recommended texlive-latex-extra texlive-lang-cyrillic fonts-noto-core`; пакеты уточнить под базовый дистрибутив Dockerfile) — ТОЛЬКО в worker-стадию; api-образ не растёт;
  - `scripts/render-templates.sh` — для каждого `templates/latex/*/v*/fixtures/*.json`: validate + render + xelatex → PASS/FAIL; запускается в CI-джобе `template-render` на worker-образе (собранном в этом же ране — зеркалить, как e2e-джоба получает образ) при изменениях в `templates/**`, `src/docgen/**`, `docker/Dockerfile`.
- Тестовая стратегия (важно): unit-тесты покрывают escape/registry/validate/render_tex (без xelatex); `test_render_job.cpp` проверяет пайплайн джобы с подменённым «компилятором» (env `DOCGEN_LATEX_CMD` — в тестах указывает на `/bin/true`-скрипт, копирующий заранее собранный минимальный PDF-фикстур; фактический XeLaTeX гоняется только CI-джобой template-render). Переменная DOCGEN_LATEX_CMD — часть конфига (`docgen.latex_cmd`, дефолт `xelatex`).

- [ ] **Step 1: vcpkg + падающие unit-тесты** (escape: все спецсимволы + пустая строка + кириллица/казахские ә-ғ-қ-ң-ө-ұ-ү-һ-і проходят насквозь; registry: фикстурный шаблон в tmp-директории с schema, latest берёт максимальную версию, validate ловит отсутствующее обязательное поле; render_tex: подстановка + автоэкранирование строки `50% & "спецы"` в теле).
- [ ] **Step 2: Реализация ядра** (без джобы) до зелёных юнитов.
- [ ] **Step 3: Джоба + Dockerfile + скрипт + CI-джоба.** `./scripts/new-job.sh` посмотреть — если генерирует каркас, использовать.
- [ ] **Step 4: Гейты + коммит** — `feat: docgen core with XeLaTeX render job` (+ отдельный коммит `ci: template smoke-render job on worker image`, если правки CI объёмны)

---

### Task 9: Шаблон «Счёт на оплату» (invoice) — эталон

**Files:**
- Create: `templates/latex/invoice/v1/template.tex`, `templates/latex/invoice/v1/schema.json`, `templates/latex/invoice/v1/fixtures/basic.json`

**Interfaces:**
- Consumes: Docgen-ядро Task 8 (inja `{{ }}`, автоэкранирование строк).
- Produces: рабочий шаблон slug `invoice`; его структура (documentclass, преамбула со шрифтами, шапка реквизитов, таблица позиций, итоги, подписи) — эталон для Task 10-11.

- [ ] **Step 1: schema.json**

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "type": "object",
  "required": ["number", "date", "seller", "buyer", "items", "total", "total_words"],
  "properties": {
    "number": {"type": "string", "minLength": 1},
    "date": {"type": "string", "pattern": "^\\d{2}\\.\\d{2}\\.\\d{4}$"},
    "seller": {"$ref": "#/definitions/party"},
    "buyer": {"$ref": "#/definitions/party"},
    "contract": {"type": "string"},
    "items": {"type": "array", "minItems": 1, "items": {
      "type": "object",
      "required": ["name", "qty", "unit", "price", "amount"],
      "properties": {
        "name": {"type": "string"}, "qty": {"type": "string"}, "unit": {"type": "string"},
        "price": {"type": "string"}, "amount": {"type": "string"}
      }}},
    "vat_rate": {"type": "string"}, "vat_amount": {"type": "string"},
    "total": {"type": "string"}, "total_words": {"type": "string"}
  },
  "definitions": {
    "party": {
      "type": "object",
      "required": ["name", "identifier"],
      "properties": {
        "name": {"type": "string"}, "identifier": {"type": "string"},
        "address": {"type": "string"}, "iik": {"type": "string"},
        "bik": {"type": "string"}, "bank": {"type": "string"}, "kbe": {"type": "string"}
      }}
  }
}
```

Суммы — строки, отформатированные вызывающим («12 345,67»): шаблон не считает, только печатает (спека §9 — воспроизводимость снапшота).

- [ ] **Step 2: template.tex** — полный, XeLaTeX:

```latex
\documentclass[a4paper,10pt]{article}
\usepackage[margin=18mm]{geometry}
\usepackage{fontspec}
\setmainfont{Noto Sans}[Scale=0.92]
\usepackage{polyglossia}\setmainlanguage{russian}
\usepackage{tabularx,booktabs,array}
\usepackage{fancyhdr}\pagestyle{empty}
\newcommand{\field}[1]{\textbf{#1}}
\begin{document}
\begin{center}\Large\bfseries Счёт на оплату № {{ number }} от {{ date }}\end{center}
\vspace{6mm}
\noindent\field{Поставщик:} {{ seller.name }}, БИН/ИИН {{ seller.identifier }}\\
{% if seller.address %}\hphantom{\field{Поставщик:} }{{ seller.address }}\\{% endif %}
{% if seller.iik %}ИИК {{ seller.iik }}{% if seller.bank %}, {{ seller.bank }}{% endif %}{% if seller.bik %}, БИК {{ seller.bik }}{% endif %}{% if seller.kbe %}, КБе {{ seller.kbe }}{% endif %}\\{% endif %}
\vspace{2mm}
\noindent\field{Покупатель:} {{ buyer.name }}, БИН/ИИН {{ buyer.identifier }}\\
{% if buyer.address %}\hphantom{\field{Покупатель:} }{{ buyer.address }}\\{% endif %}
{% if contract %}\noindent\field{Основание:} {{ contract }}\\{% endif %}
\vspace{4mm}
\noindent\begin{tabularx}{\textwidth}{@{}c X r r r r@{}}
\toprule
№ & Наименование & Кол-во & Ед. & Цена, ₸ & Сумма, ₸ \\
\midrule
{% for item in items %}{{ loop.index1 }} & {{ item.name }} & {{ item.qty }} & {{ item.unit }} & {{ item.price }} & {{ item.amount }} \\
{% endfor %}
\bottomrule
\end{tabularx}
\vspace{3mm}
{% if vat_amount %}\hfill НДС ({{ vat_rate }}): {{ vat_amount }} ₸\par{% endif %}
\hfill\field{Итого к оплате: {{ total }} ₸}\par
\noindent Всего наименований {{ length(items) }}, на сумму {{ total }} ₸\\
({{ total_words }})
\vspace{12mm}
\noindent Руководитель \hrulefill\qquad Гл. бухгалтер \hrulefill
\end{document}
```

Синтаксис inja-блоков `{% %}`/`{{ }}` не конфликтует с LaTeX в этом шаблоне; если xelatex-прогон CI выявит конфликт — экранировать проблемное место `{% raw %}`.

- [ ] **Step 3: fixtures/basic.json** — заполненный пример (продавец «Cyber Capybara ТОО», БИН из валидных векторов Task 1, 2 позиции, НДС 16%, total_words «Двенадцать тысяч триста сорок пять тенге 67 тиын») + case-фикстура `special-chars.json` (в наименовании позиции: `50% скидка & "кавычки" #1 _test_`).
- [ ] **Step 4: Локально** только `python3 -m json.tool` на оба json + clang-format не применим; коммит — `feat(templates): invoice (счёт на оплату) v1`. Фактический рендер проверит CI-джоба template-render.

---

### Task 10: Шаблоны «АВР» и «Накладная»

**Files:**
- Create: `templates/latex/avr/v1/{template.tex,schema.json,fixtures/basic.json}`, `templates/latex/waybill/v1/{template.tex,schema.json,fixtures/basic.json}`

**Interfaces:** Consumes эталон Task 9 (та же преамбула/шрифты/идиомы).

- [ ] **Step 1: АВР (акт выполненных работ/оказанных услуг)** — schema: number, date, act_period (строка), seller/buyer (party как в Task 9 — продублировать definitions в свой schema.json, схемы самодостаточны), contract, items[{name, unit, qty, price, amount}], total, vat_rate?, vat_amount?, total_words; template.tex: шапка «Акт выполненных работ (оказанных услуг) № … от …», таблица, строка «Вышеперечисленные работы (услуги) выполнены полностью и в срок. Заказчик претензий по объёму, качеству и срокам не имеет.», подписи Исполнитель/Заказчик с расшифровкой.
- [ ] **Step 2: Накладная на отпуск запасов (З-2)** — schema: number, date, seller/buyer, basis (строка), items[{name, unit, qty, price, amount}], total, total_words, released_by, received_by; template.tex: альбомная ориентация (`\usepackage[margin=14mm,landscape]{geometry}`), таблица с колонками № / Наименование / Ед. / Кол-во / Цена / Сумма, подписи «Отпустил / Получил».
- [ ] **Step 3: Фикстуры с кириллицей + спецсимволами; коммит** — `feat(templates): AVR act and waybill v1`

---

### Task 11: Шаблоны «Счёт-фактура» и «Акт сверки»

**Files:**
- Create: `templates/latex/tax_invoice/v1/{template.tex,schema.json,fixtures/basic.json}`, `templates/latex/reconciliation/v1/{template.tex,schema.json,fixtures/basic.json}`

- [ ] **Step 1: Счёт-фактура (печатная форма)** — schema: number, date, seller/buyer (party + `vat_certificate` опционально), items[{name, unit, qty, price, amount, vat_rate, vat_amount, total_with_vat}], totals{amount, vat, with_vat}, total_words; template.tex: альбомная, полная СФ-таблица (№, наименование, ед., кол-во, цена, стоимость без НДС, НДС ставка/сумма, всего с НДС), строки итогов, подписи руководителя и главбуха. Это ПЕЧАТНАЯ форма для контрагентов, не ЭСФ-XML (ЭСФ — фаза интеграций, спека §12).
- [ ] **Step 2: Акт сверки взаиморасчётов** — schema: period_from, period_to (dd.mm.yyyy), party_a/party_b (party), opening_balance{a_debit?, a_credit?} (строки), rows[{date, doc, a_debit, a_credit, b_debit, b_credit}] (пустые строки — ""), closing{a_says, b_says} (строки «Задолженность в пользу … на … составляет … ₸»), подписи обеих сторон; template.tex: двухсторонняя таблица «По данным {{ party_a.name }} / По данным {{ party_b.name }}».
- [ ] **Step 3: Фикстуры; коммит** — `feat(templates): tax invoice print form and reconciliation act v1`

---

### Task 12: API — контрагенты, счета, документы, файлы

**Files:**
- Create: `src/api/CounterpartiesController.hpp`, `src/api/AccountsController.hpp`, `src/api/LedgerDocumentsController.hpp`
- Modify: `src/api/Api.hpp`, `src/api/Endpoints.hpp`, `docs/openapi.yaml`
- Test: `tests/integration/test_counterparties_api.cpp`, `tests/integration/test_accounts_api.cpp`, `tests/integration/test_documents_api.cpp`

**Interfaces:**
- Consumes: репозитории Task 2/3/6, `API_REQUIRE_ORG` (роль: viewer — чтение; accountant/owner — мутации; правило для ВСЕХ ledger-роутов: мутации требуют `ctx.role != "viewer"`), `Ledger::is_valid_bin_iin`, `Files::org_key`, `Storage::storage().presign`.
- Produces (все под `/api/v1`, triple-sync, пагинация `parse_page_params` как в OrganizationsController):
  - `GET/POST /counterparties`, `GET/PATCH /counterparties/{id}` — POST/PATCH валидируют identifier через is_valid_bin_iin → 422 `Api::Validation`; 409 на дубль.
  - `GET /accounts` (list_visible), `POST /accounts` (create_subaccount; 422 InvalidSubaccount, 409 дубль).
  - `GET /documents` (?type=&status= фильтры по allowlist), `GET /documents/{id}`, `POST /documents/{id}/download-url` (presigned GET, TTL 300с; 409 если s3_key пуст), `POST /documents/uploads` (тело {filename, mime} → создаёт документ source=uploaded status=draft + presigned PUT TTL 600с + s3_key; клиент грузит сам), `POST /documents/{id}/confirm-upload` (тело {size_bytes, checksum_sha256} → exists() в S3 → set_file + status final; 409 если объекта нет).
- [ ] **Step 1: Падающие API-тесты** — на каждый роут: happy path + RBAC-отказ viewer'а на мутации + org-изоляция (объект чужого org → 404) + валидационные случаи (identifier "123" → 422; download-url без файла → 409). Идиомы test_organizations_api.cpp.
- [ ] **Step 2: Контроллеры** — зеркалить OrganizationsController (guard-порядок: API_REQUIRE_ORG первым, потом is_valid_uuid — консистентно с решением P0).
- [ ] **Step 3: Triple-sync + гейты + коммит** — `feat: counterparties, accounts and documents API`

---

### Task 13: API — журнал и генерация документов

**Files:**
- Create: `src/api/JournalController.hpp`, `src/api/DocgenController.hpp`
- Modify: `src/api/Api.hpp`, `src/api/Endpoints.hpp`, `docs/openapi.yaml`
- Test: `tests/integration/test_journal_api.cpp`, `tests/integration/test_docgen_api.cpp`

**Interfaces:**
- Consumes: `Ledger::JournalService` (Task 5), `Docgen::TemplateRegistry` (Task 8), `Ledger::DocumentRepository`, jobs-очередь (`Jobs::enqueue` — фактическую сигнатуру взять из `src/jobs/Jobs.hpp` и существующих вызовов в контроллерах/BuiltinHandlers).
- Produces:
  - `GET /journal-entries` (с фильтрами from/to по entry_date), `GET /journal-entries/{id}` (с lines), `POST /journal-entries` (draft; 422 на Unbalanced/UnknownAccount), `POST /journal-entries/{id}/post`, `POST /journal-entries/{id}/reverse` — все мутации не-viewer.
  - `GET /doc-templates` (реестр: slug, version, schema — для форм фронтенда), `POST /documents/generate` (тело {template_slug, input, counterparty_id?, link_entry_id?}: validate по schema → 422 с текстом ошибки; создать Document(source=generated, doc_type=slug-маппинг, status=draft, template_slug/version, input_snapshot) → enqueue `docgen.render` → 202 {document_id}). Статус готовности клиент опрашивает GET /documents/{id} (status final + download-url).
- [ ] **Step 1: Падающие тесты** — журнал: полный lifecycle через API (create draft 201 → post 200 → reverse 200 → в списке обе проводки со связкой; unbalanced → 422 с полем ошибки; viewer post → 403); docgen: generate с валидным входом → 202 + документ draft с input_snapshot; невалидный вход → 422; enqueue-вызов проверяется появлением джобы в очереди (идиома из существующих tests/api/test_jobs_endpoints.cpp / integration test_jobs.cpp — рендер в тесте НЕ исполняется).
- [ ] **Step 2: Реализация + triple-sync + гейты + коммит** — `feat: journal and document generation API`

---

### Task 14: Frontend — справочники и журнал

**Files:**
- Create: `frontend/src/pages/Counterparties.tsx`, `frontend/src/pages/Journal.tsx`
- Modify: `frontend/src/routes/manifest.tsx`, `frontend/src/lib/api/types.ts` (реэкспорты кодогена), сгенерированный `schema.gen.ts`

**Interfaces:**
- Consumes: роуты Task 12/13 через регенерированный openapi-typescript клиент; идиомы страниц P0 (`Organizations.tsx`: usePagedQuery, qk-фабрики, useApiMutation, error-toast).
- Produces: страница «Контрагенты» (таблица+поиск не нужен в P1: пагинация; форма создания с клиентской проверкой 12 цифр; редактирование), страница «Журнал» (список проводок с датой/описанием/суммой/статусом; форма draft-проводки: дата, описание, строки [счёт-селект из GET /accounts, сторона, сумма, контрагент-селект], живой индикатор баланса Σд−Σк; кнопки Провести/Сторно с confirm-диалогом).
- [ ] **Step 1: Кодоген + tsc.** `npm run generate:api`-скрипт из package.json.
- [ ] **Step 2: Страницы + роуты** (guard 'confirmed', navIcon — как соседи).
- [ ] **Step 3: tsc --noEmit + eslint чисто; коммит** — `feat(frontend): counterparties and journal pages`

---

### Task 15: Frontend — документы и генерация

**Files:**
- Create: `frontend/src/pages/Documents.tsx`, `frontend/src/pages/GenerateDocument.tsx` (или диалог внутри Documents — по месту, зеркаля стиль соседних страниц)
- Modify: `frontend/src/routes/manifest.tsx`

**Interfaces:**
- Consumes: Task 12/13 API; `GET /doc-templates` отдаёт JSON Schema — форма генерации строится по схеме МИНИМАЛЬНО: в P1 не пишем универсальный schema-form-движок (YAGNI), а делаем типизированные формы для пяти известных слагов (общие поля: номер/дата/контрагент-селект → маппинг в party; таблица позиций с автоподсчётом сумм на клиенте и формированием строковых сумм «12 345,67»; total_words — поле ручного ввода в P1).
- Produces: список документов (фильтр по типу/статусу, кнопка скачивания через download-url → window.open presigned), генерация: выбор шаблона → форма → generate → поллинг статуса (refetchInterval, пока draft) → скачивание; загрузка файла (uploads: выбор файла → presigned PUT fetch'ем → confirm-upload).
- [ ] **Step 1: Страницы; Step 2: tsc+eslint; Step 3: коммит** — `feat(frontend): documents list, generation and upload`

---

### Task 16: Деплой и сквозной смоук

**Files:**
- Modify: `helm/cpp-env/values-cybercapybara.yaml` (env из секрета s3-credentials для api и worker: STORAGE_BACKEND=s3 + S3_*, `envFrom secretRef` — механизм передачи env сверить с шаблонами чартов cpp-api/cpp-worker: есть ли extraEnv/envFrom values; если нет — добавить в чарт минимальную поддержку `extraEnvFrom`), тег образов на релиз P1

**Interfaces:** Consumes: весь P1; секрет s3-credentials в namespace.

- [ ] **Step 1:** После зелёного CI и мержа PR — тег `v0.2.0` → релиз → образы `0.2.0`.
- [ ] **Step 2:** values: теги 0.2.0, envFrom s3-credentials + STORAGE_BACKEND=s3; `helm upgrade --install ... -n cyber-accountant`; все поды Running.
- [ ] **Step 3: Сквозной смоук** (kubectl port-forward + curl, admin-JWT как в P0-смоуке): создать контрагента (валидный БИН из Task 1 векторов) → создать draft-проводку 2 строки (1210/6010 на 100 000,00) → post → `POST /documents/generate` invoice с этим контрагентом → дождаться status final (поллинг ≤60с) → download-url → curl presigned → PDF-магия `%PDF` в первых байтах → `mc ls` бакета показывает объект в `org/<id>/generated/`. Каждый шаг и вывод — в отчёт.
- [ ] **Step 4: Коммит values + пуш через контроллера** — `deploy: P1 release with S3 storage enabled`

---

## Definition of Done (фаза P1)

1. CI зелёный: все новые сьюты (валидатор, контрагенты, счета, журнал-схема, журнал-сервис, документы, S3, docgen-юниты, render-job, 5×API-тестов) + CI-джоба template-render рендерит все 5 шаблонов (включая спецсимвольные фикстуры) в валидные PDF.
2. Релиз v0.2.0 задеплоен; сквозной смоук Task 16 Step 3 пройден полностью (контрагент → проводка → PDF в MinIO → скачивание по presigned URL).
3. Инварианты доказаны тестами: несбалансированная проводка не коммитится; posted неизменяем; сторно зеркально; org-изоляция на каждом новом репозитории; чужой org везде получает 404/пусто.
4. SPA: страницы Контрагенты, Журнал, Документы работают против задеплоенного бэка.
5. Ни одного нового unscoped-метода чтения доменных данных (кроме задокументированного accounts-исключения).
