-- HR: сотрудники и кадровый документооборот (спека §7.2, фаза P2). Org-scoped
-- — каждая из четырёх таблиц каскадно висит на organizations, идиомы те же,
-- что в migrations/007_counterparties.sql (UNIQUE(org_id, identifier-like
-- column)) и migrations/009_journal.sql/010_documents.sql (UNIQUE(id, org_id)
-- как цель составного FK, чтобы дочерняя строка не могла сослаться на
-- employee из ЧУЖОЙ организации).
--
-- employees — карточка сотрудника. iin — 12-значный ИИН (формат/контрольная
-- цифра проверяются на API-слое через Ledger::is_valid_bin_iin, P1 — эта
-- миграция и репозиторий его не валидируют, ровно как identifier в
-- counterparties). salary_tiyn — оклад в тиынах (целая арифметика, без
-- double — то же соглашение, что и amount-поля в migrations/009_journal.sql
-- README). status — простой двухпозиционный лайфцикл ('active' -> при
-- увольнении -> 'dismissed'); P2 не строит state machine поверх него (та же
-- позиция, что и у documents.status в migrations/010_documents.sql — CHECK
-- на БД, переходы валидирует вызывающий код).
--
-- labor_contracts / hr_orders / vacations ссылаются на employees через
-- составной FK (employee_id, org_id) -> employees(id, org_id): попытка
-- привязать сотрудника ЧУЖОЙ организации падает на уровне ограничения
-- (SQLSTATE 23503), а не требует отдельной EXISTS-проверки в репозитории —
-- тот же приём, что и document_entries в migrations/010_documents.sql.
--
-- hr_orders.document_id — ПРОСТОЙ (не составной) FK на documents(id), без
-- ON DELETE: приказ может ссылаться на скан/сгенерированный документ любой
-- организации в схеме БД, но приложение (Task 3/HrRepository.attach_document)
-- всегда передаёт document_id, полученный из документа ТОЙ ЖЕ org_id — то же
-- допущение, что и у documents.counterparty_id в migrations/010_documents.sql
-- (доверие вызывающему коду на этой оси).
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

CREATE TABLE IF NOT EXISTS employees (
    id                     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id                 UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    iin                    CHAR(12) NOT NULL,
    last_name              TEXT NOT NULL,
    first_name             TEXT NOT NULL,
    middle_name            TEXT,
    "position"             TEXT NOT NULL,
    salary_tiyn            BIGINT NOT NULL CHECK (salary_tiyn >= 0),
    hired_on               DATE NOT NULL,
    dismissed_on           DATE,
    ipn_deduction_claimed  BOOLEAN NOT NULL DEFAULT FALSE,
    opvr_exempt            BOOLEAN NOT NULL DEFAULT FALSE,
    payout_iik             TEXT NOT NULL DEFAULT '',
    status                 TEXT NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'dismissed')),
    created_at             TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at             TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_id, iin),
    -- Composite target for labor_contracts/hr_orders/vacations' tenant-safe
    -- FK below — same rationale as journal_entries.UNIQUE(id, org_id)
    -- (migration 009) / documents.UNIQUE(id, org_id) (migration 010).
    UNIQUE (id, org_id)
);
DROP TRIGGER IF EXISTS trg_employees_touch ON employees;
CREATE TRIGGER trg_employees_touch BEFORE UPDATE ON employees
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
CREATE INDEX IF NOT EXISTS idx_employees_org_status ON employees (org_id, status);

CREATE TABLE IF NOT EXISTS labor_contracts (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id       UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    employee_id  UUID NOT NULL,
    number       TEXT NOT NULL,
    signed_on    DATE NOT NULL,
    starts_on    DATE NOT NULL,
    ends_on      DATE,
    -- Free-form contract terms (schedule, probation, benefits, ...) — no
    -- fixed shape in P2, hence JSONB rather than dedicated columns. Same
    -- idiom as documents.input_snapshot (migrations/010_documents.sql).
    terms_json   JSONB,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    FOREIGN KEY (employee_id, org_id) REFERENCES employees (id, org_id) ON DELETE CASCADE
);
DROP TRIGGER IF EXISTS trg_labor_contracts_touch ON labor_contracts;
CREATE TRIGGER trg_labor_contracts_touch BEFORE UPDATE ON labor_contracts
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
CREATE INDEX IF NOT EXISTS idx_labor_contracts_org_employee ON labor_contracts (org_id, employee_id);

CREATE TABLE IF NOT EXISTS hr_orders (
    id               UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id           UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    employee_id      UUID NOT NULL,
    kind             TEXT NOT NULL CHECK (kind IN
                         ('hire', 'dismiss', 'vacation', 'business_trip', 'salary_change')),
    number           TEXT NOT NULL,
    issued_on        DATE NOT NULL,
    effective_from   DATE NOT NULL,
    effective_to     DATE,
    -- Kind-specific details (e.g. new salary_tiyn for 'salary_change',
    -- destination/purpose for 'business_trip') — no fixed shape per kind, so
    -- JSONB rather than one nullable column per kind. Same idiom as
    -- labor_contracts.terms_json above.
    payload          JSONB,
    document_id      UUID REFERENCES documents (id),
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    FOREIGN KEY (employee_id, org_id) REFERENCES employees (id, org_id) ON DELETE CASCADE,
    -- Composite target for a future tenant-safe FK onto hr_orders (e.g. a
    -- P3 payroll run item linking back to the order that triggered it) —
    -- same forward-looking rationale as employees.UNIQUE(id, org_id) above.
    UNIQUE (id, org_id)
);
DROP TRIGGER IF EXISTS trg_hr_orders_touch ON hr_orders;
CREATE TRIGGER trg_hr_orders_touch BEFORE UPDATE ON hr_orders
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
CREATE INDEX IF NOT EXISTS idx_hr_orders_org_employee ON hr_orders (org_id, employee_id);

CREATE TABLE IF NOT EXISTS vacations (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id       UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    employee_id  UUID NOT NULL,
    starts_on    DATE NOT NULL,
    ends_on      DATE NOT NULL,
    days         INTEGER NOT NULL CHECK (days > 0),
    kind         TEXT NOT NULL CHECK (kind IN ('annual', 'unpaid', 'sick')),
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    FOREIGN KEY (employee_id, org_id) REFERENCES employees (id, org_id) ON DELETE CASCADE,
    CHECK (ends_on >= starts_on)
);
DROP TRIGGER IF EXISTS trg_vacations_touch ON vacations;
CREATE TRIGGER trg_vacations_touch BEFORE UPDATE ON vacations
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
CREATE INDEX IF NOT EXISTS idx_vacations_org_employee ON vacations (org_id, employee_id);
