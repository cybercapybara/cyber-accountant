-- Payroll: расчётные прогоны и расчётные листки (спека §7.2, фаза P2, Task 4).
-- Org-scoped — та же идиома составного FK, что и в migrations/012_hr.sql:
-- payslips ссылается на payroll_runs через (run_id, org_id) -> payroll_runs
-- (id, org_id) и на employees через (employee_id, org_id) -> employees
-- (id, org_id), так что расчётный листок не может сослаться на прогон или
-- сотрудника ЧУЖОЙ организации на уровне ограничения (SQLSTATE 23503), без
-- отдельной EXISTS-проверки в репозитории.
--
-- payroll_runs.rates_snapshot — фактически использованные ставки/константы
-- (Tax::TaxReferenceRepository), снятые на последний день периода, чтобы
-- расчёт был воспроизводим даже после того, как migrations/011_tax_reference.sql
-- получит новую строку ставки на будущую дату (Src::Payroll::PayrollService
-- заполняет этот столбец, схема лишь хранит JSONB). UNIQUE(org_id,
-- period_year, period_month) — один прогон на период на организацию;
-- пересчёт ПЕРЕЗАПИСЫВАЕТ существующую draft-строку (см. PayrollService), а
-- не плодит вторую — то же соглашение, что и у tax_calculations (migrations/
-- 014_tax_calculations.sql) для его собственного UNIQUE-ключа периода.
--
-- payslips хранит только денежные суммы (BIGINT, тиыны — целая арифметика,
-- то же соглашение, что и salary_tiyn в migrations/012_hr.sql / amount в
-- migrations/009_journal.sql). UNIQUE(run_id, employee_id) — один листок на
-- сотрудника в прогоне; пересчёт удаляет старые листки прогона и вставляет
-- новые (см. PayrollService::calculate_run), поэтому это ограничение никогда
-- не должно сработать в штатном пути, но остаётся защитой от гонки/бага.
--
-- payroll_runs.journal_entry_id / posted_at (Fix round 1, code review):
-- record WHICH journal entry a run was posted to and WHEN, so
-- PayrollService::post_to_journal can atomically refuse a second post
-- (journal_entry_id already set) instead of creating a second, fully
-- balanced, duplicate journal entry for the same period — see that method's
-- Doxygen for the exact compare-and-swap. Plain (non-composite) FK onto
-- journal_entries(id), no ON DELETE — same trust-the-caller posture as
-- hr_orders.document_id (migrations/012_hr.sql): PayrollService always
-- writes an entry_id it just created for this SAME org, so there is no
-- cross-org row to defend against here.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

CREATE TABLE IF NOT EXISTS payroll_runs (
    id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id         UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    period_year    INTEGER NOT NULL,
    period_month   INTEGER NOT NULL CHECK (period_month BETWEEN 1 AND 12),
    status         TEXT NOT NULL DEFAULT 'draft' CHECK (status IN ('draft', 'approved')),
    calculated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- Used rates/constants (kind/key -> value), snapshotted at calculation
    -- time — see PayrollService::calculate_run and file header above.
    rates_snapshot JSONB NOT NULL DEFAULT '{}'::jsonb,
    -- NULL until PayrollService::post_to_journal succeeds; both are set
    -- together in the same statement, and journal_entry_id IS NULL is the
    -- CAS guard that makes a second post_to_journal() call a no-op-turned-409
    -- instead of a duplicate ledger entry — see file header above.
    journal_entry_id UUID REFERENCES journal_entries (id),
    posted_at      TIMESTAMPTZ,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_id, period_year, period_month),
    -- Composite target for payslips' tenant-safe FK below — same rationale
    -- as employees.UNIQUE(id, org_id) (migrations/012_hr.sql).
    UNIQUE (id, org_id)
);
DROP TRIGGER IF EXISTS trg_payroll_runs_touch ON payroll_runs;
CREATE TRIGGER trg_payroll_runs_touch BEFORE UPDATE ON payroll_runs
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
CREATE INDEX IF NOT EXISTS idx_payroll_runs_org_period ON payroll_runs (org_id, period_year, period_month);

CREATE TABLE IF NOT EXISTS payslips (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id       UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    run_id       UUID NOT NULL,
    employee_id  UUID NOT NULL,
    -- Every amount below is Payroll::Result's matching field, in tiyn.
    gross_tiyn   BIGINT NOT NULL CHECK (gross_tiyn >= 0),
    opv          BIGINT NOT NULL CHECK (opv >= 0),
    vosms        BIGINT NOT NULL CHECK (vosms >= 0),
    ipn          BIGINT NOT NULL CHECK (ipn >= 0),
    net          BIGINT NOT NULL CHECK (net >= 0),
    opvr         BIGINT NOT NULL CHECK (opvr >= 0),
    so           BIGINT NOT NULL CHECK (so >= 0),
    osms         BIGINT NOT NULL CHECK (osms >= 0),
    social_tax   BIGINT NOT NULL CHECK (social_tax >= 0),
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    FOREIGN KEY (run_id, org_id) REFERENCES payroll_runs (id, org_id) ON DELETE CASCADE,
    FOREIGN KEY (employee_id, org_id) REFERENCES employees (id, org_id) ON DELETE CASCADE,
    UNIQUE (run_id, employee_id)
);
DROP TRIGGER IF EXISTS trg_payslips_touch ON payslips;
CREATE TRIGGER trg_payslips_touch BEFORE UPDATE ON payslips
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
CREATE INDEX IF NOT EXISTS idx_payslips_run ON payslips (run_id);
