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
