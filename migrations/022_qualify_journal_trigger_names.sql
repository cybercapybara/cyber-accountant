-- Fix round 2: доводит до конца квалификацию имён во ВСЕХ триггерных
-- функциях журнала. 021 закрыл подмену имён ОТНОШЕНИЙ в
-- journal_entries_immutability(); здесь закрыты (а) подмена имени ТИПА,
-- которую 021 сам же и внёс, и (б) те же дыры в двух соседних функциях из
-- 009, куда 021 не дотянулся.
--
-- Класс уязвимости один и тот же: pg_temp просматривается при разрешении
-- имён ОТНОШЕНИЙ и ТИПОВ первым — неявно, даже когда его нет в search_path,
-- — а право CREATE TEMP выдано PUBLIC. Имена ФУНКЦИЙ и ОПЕРАТОРОВ так
-- перехватить нельзя (для них временная схема не просматривается никогда),
-- поэтому now(), to_jsonb, jsonb_set и операторы трогать не нужно.
-- Проверено на живой PostgreSQL 16.15 обычной ролью, до и после.
--
-- ЧТО ЧИНИТСЯ, три пункта.
--
-- 1. journal_entry_must_balance() — SELECT ... FROM journal_lines БЕЗ схемы.
--    Это КРИТИЧНО и живёт в проде с миграции 009: обычная роль подсовывает
--
--      CREATE TEMP TABLE journal_lines AS SELECT <entry>::uuid AS entry_id,
--             'debit'::text AS side, 0::numeric AS amount;
--
--    и проверка Σdebit=Σcredit считает сумму по ПОДСТАВНОЙ таблице. Так
--    была закоммичена проводка с единственной дебетовой строкой 999.00 без
--    кредита. Двойная запись — самое глубокое, что в этой системе можно
--    испортить, и именно она оказалась не защищена.
--    Плюс DECLARE eid UUID: временная таблица с именем uuid ломает уже саму
--    компиляцию тела («malformed record literal»), то есть отказ в
--    обслуживании на всей записи в journal_lines.
--
-- 2. journal_lines_frozen_after_post() — SELECT ... FROM journal_entries без
--    схемы и DECLARE st TEXT. Здесь подмена приводит к отказу, а не к
--    обходу (подставная таблица не отдаёт status, ветка st IS NULL считает
--    родителя удалённым и... пропускает — поэтому квалифицируем и её), а
--    временная таблица с именем text валит функцию целиком.
--
-- 3. journal_entries_immutability() — единственное оставшееся приведение
--    типа 'null'::jsonb. Обхода оно не даёт (падает закрыто), но временная
--    таблица с именем jsonb превращает ЛЮБОЙ UPDATE по journal_entries в
--    ошибку «malformed record literal: "null"»: и правку черновика, и
--    легальное сторно posted -> reversed, и каскад DELETE FROM users —
--    ровно тот сценарий, ради которого вся эта серия и затевалась. Каст не
--    нужен: третий аргумент jsonb_set и так jsonb, литерал 'null'
--    приводится к нему сам. Убираем каст, а не квалифицируем его: меньше
--    текста — меньше поверхности.
--
-- Тела функций ниже в остальном побайтово совпадают с 009/021: изменены
-- только имена таблиц (public.*), объявленные типы (pg_catalog.*) и снят
-- один каст. Триггеры пересоздавать не надо — CREATE OR REPLACE FUNCTION
-- подменяет тело, а trg_journal_balance / trg_journal_lines_frozen /
-- trg_journal_entries_immutable уже указывают на эти имена.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

-- (a) Баланс Σdebit=Σcredit per entry (см. 009 §(a)).
CREATE OR REPLACE FUNCTION journal_entry_must_balance() RETURNS trigger AS $$
DECLARE
    eid pg_catalog.uuid;
    diff pg_catalog.numeric(18,2);
BEGIN
    eid := COALESCE(NEW.entry_id, OLD.entry_id);
    SELECT COALESCE(SUM(CASE side WHEN 'debit' THEN amount ELSE -amount END), 0)
      INTO diff FROM public.journal_lines WHERE entry_id = eid;
    IF diff <> 0 THEN
        RAISE EXCEPTION 'journal entry % is unbalanced by %', eid, diff
            USING ERRCODE = 'check_violation';
    END IF;
    RETURN NULL;
END $$ LANGUAGE plpgsql;

-- (b) Немутируемость шапки (см. 009 §(b), 020 и 021).
CREATE OR REPLACE FUNCTION journal_entries_immutability() RETURNS trigger AS $$
BEGIN
    IF TG_OP = 'DELETE' THEN
        -- Каскад от удалённой организации: родителя уже нет.
        IF NOT EXISTS (SELECT 1 FROM public.organizations WHERE id = OLD.org_id) THEN
            RETURN OLD;
        END IF;
        IF OLD.status <> 'draft' THEN
            RAISE EXCEPTION 'posted/reversed journal entries are insert-only'
                USING ERRCODE = 'check_violation';
        END IF;
        RETURN OLD;
    END IF;

    -- Тот же каскад, но на UPDATE. Ветка ЗАЩИТНАЯ: сконструировать порядок
    -- обхода ссылочной целостности, при котором строка получает UPDATE
    -- раньше собственного DELETE, не удалось — но порядок обхода нигде и не
    -- обещан, а цена ветки нулевая (организации уже нет, все её проводки
    -- всё равно исчезают в этом же операторе).
    IF NOT EXISTS (SELECT 1 FROM public.organizations WHERE id = OLD.org_id) THEN
        RETURN NEW;
    END IF;

    -- ON DELETE SET NULL от удалённого автора. Разрешено РОВНО одно отличие
    -- строки: created_by_user_id стал NULL. Сравнение идёт по всей строке
    -- (to_jsonb), а не по списку колонок, поэтому защищены в том числе id,
    -- created_at и created_by_run_id, и любая колонка, добавленная позже.
    -- Единственное исключение — updated_at: им владеет touch-триггер,
    -- который срабатывает ПОСЛЕ этого (по алфавиту immutable < touch) и всё
    -- равно перезаписывает значение на now().
    -- Требование «пользователя уже нет в public.users» отсекает попытку
    -- приложения обнулить автора у живой записи: это была бы обычная правка
    -- проведённой проводки, а не каскад.
    IF OLD.created_by_user_id IS NOT NULL AND NEW.created_by_user_id IS NULL
       AND NOT EXISTS (SELECT 1 FROM public.users WHERE id = OLD.created_by_user_id)
       AND (pg_catalog.to_jsonb(NEW) - 'updated_at')
           = (pg_catalog.jsonb_set(pg_catalog.to_jsonb(OLD), '{created_by_user_id}', 'null')
              - 'updated_at') THEN
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
          AND NOT EXISTS (SELECT 1 FROM public.journal_lines WHERE entry_id = NEW.id) THEN
        RAISE EXCEPTION 'cannot post an entry with no lines'
            USING ERRCODE = 'check_violation';
    END IF;
    RETURN NEW;  -- draft свободно правится (updated_at меняет touch-триггер)
END $$ LANGUAGE plpgsql;

-- (c) Строки заморожены, как только шапка не в draft (см. 009 §(c)).
CREATE OR REPLACE FUNCTION journal_lines_frozen_after_post() RETURNS trigger AS $$
DECLARE st pg_catalog.text;
BEGIN
    SELECT status INTO st FROM public.journal_entries WHERE id = COALESCE(NEW.entry_id, OLD.entry_id);
    IF st IS NULL THEN
        -- Родительская проводка уже удалена в ЭТОЙ ЖЕ транзакции (её
        -- собственный DELETE каскадится сюда через FK: строка ON DELETE
        -- CASCADE у journal_lines) — SELECT выше не находит родителя и
        -- возвращает NULL, а не "проводка не draft". Без этой ветки DELETE
        -- draft-проводки со строками (прямой DELETE FROM journal_entries
        -- или каскад через DELETE FROM organizations) падал бы здесь:
        -- «удаление строк уже удалённой draft-проводки» — легальная
        -- операция, а не попытка изменить чужие строки задним числом.
        RETURN COALESCE(NEW, OLD);
    END IF;
    IF st IS DISTINCT FROM 'draft' AND TG_OP <> 'INSERT' THEN
        RAISE EXCEPTION 'lines of a % entry are immutable', st USING ERRCODE = 'check_violation';
    END IF;
    IF st IS DISTINCT FROM 'draft' AND TG_OP = 'INSERT' THEN
        RAISE EXCEPTION 'cannot add lines to a % entry', st USING ERRCODE = 'check_violation';
    END IF;
    -- Строка не может "переехать" в другую проводку или другой org одним
    -- UPDATE: st выше отражает статус ЦЕЛЕВОЙ (NEW) проводки, а не
    -- исходной — перенос строки ИЗ posted-проводки В draft-проводку прошёл
    -- бы предыдущие две проверки не заметив, что исходная проводка
    -- потеряла строку, участвовавшую в её проверенном балансе.
    IF TG_OP = 'UPDATE' AND (NEW.entry_id <> OLD.entry_id OR NEW.org_id <> OLD.org_id) THEN
        RAISE EXCEPTION 'journal lines cannot move between entries or orgs'
            USING ERRCODE = 'check_violation';
    END IF;
    RETURN COALESCE(NEW, OLD);
END $$ LANGUAGE plpgsql;
