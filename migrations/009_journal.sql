-- journal: двойная запись, insert-only (спека §6.2). journal_entries — шапка
-- проводки (status draft|posted|reversed); journal_lines — строки debit/credit.
-- Инварианты живут в БД, не в приложении:
--   (a) Σdebit = Σcredit per entry, проверяется DEFERRABLE constraint-триггером
--       на COMMIT (одна несбалансированная строка не должна падать на INSERT —
--       остальные строки того же INSERT/UPDATE ещё не применены);
--   (b) posted-проводка неизменяема, кроме единственного легального перехода
--       status posted -> reversed (сторно) без изменения прочих полей;
--       reversed — неизменяема полностью; DELETE запрещён кроме draft;
--   (c) строки проводки можно вставлять/менять/удалять, только пока сама
--       проводка в draft — но вставка строк в НОВУЮ draft-проводку, созданную
--       сторно-флоу, разрешена как обычно (триггер смотрит статус RODITEL'
--       entry на момент операции, а не факт "проводка уже где-то упомянута
--       как reverses_entry_id").
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

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

-- (a) Баланс Σdebit=Σcredit per entry. CONSTRAINT TRIGGER + DEFERRABLE
-- INITIALLY DEFERRED так, чтобы проверка выполнялась один раз на COMMIT
-- транзакции (после того, как ВСЕ строки шапки уже вставлены), а не после
-- каждой отдельной строки — иначе первая же строка двухстрочной проводки
-- всегда была бы "несбалансирована" сама по себе.
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

-- (b) Немутируемость шапки: posted можно перевести ТОЛЬКО в reversed, и
-- только этим одним полем (сторно, не правка задним числом); reversed и
-- DELETE posted/reversed запрещены целиком.
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

-- (c) Строки заморожены, как только шапка не в draft. Сторно-флоу вставляет
-- строки в НОВУЮ draft-проводку (созданную под сторно) — эта функция смотрит
-- статус entry_id самой строки на момент операции, так что новой draft-шапке
-- она не мешает; проведение (draft->posted) строк вообще не трогает.
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
