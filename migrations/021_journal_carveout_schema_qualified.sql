-- Fix round 1 к вырезу из 020: закрыть обход неизменяемости журнала через
-- pg_temp и ужесточить ветку удаления автора.
--
-- ЧТО БЫЛО СЛОМАНО (воспроизведено на живой PostgreSQL 16.15 обычной ролью,
-- без каких-либо особых привилегий).
--
-- journal_entries_immutability() — SECURITY INVOKER, а все три обращения к
-- таблицам внутри неё были БЕЗ схемы: organizations, users, journal_lines.
-- PostgreSQL ищет ИМЕНА ОТНОШЕНИЙ сначала в pg_temp (временная схема
-- подставляется в начало search_path неявно, даже если её там не написали),
-- а право создавать временные таблицы выдано PUBLIC по умолчанию. Отсюда:
--
--   CREATE TEMP TABLE organizations (id uuid);
--   UPDATE journal_entries SET description='подделка', status='draft' WHERE ...;
--   DELETE FROM journal_entries WHERE ...;                       -- обе прошли
--
-- Пустая временная organizations делает NOT EXISTS истинным — вырез считает,
-- что организация удалена, и пропускает ЛЮБУЮ правку и удаление проведённой
-- записи. То же с CREATE TEMP TABLE users открывало ветку удаления автора, а
-- CREATE TEMP TABLE journal_lines (с одной строкой) позволяло провести
-- проводку вообще без строк, обходя проверку из 009.
--
-- ЛЕЧЕНИЕ — квалифицировать имена схемой: public.organizations, public.users,
-- public.journal_lines. Временная схема при этом не участвует в разрешении
-- имени вовсе.
--
-- ALTER FUNCTION ... SET search_path = pg_catalog, public ЛЕЧЕНИЕМ НЕ
-- ЯВЛЯЕТСЯ и проверено, что не является: pg_temp всё равно просматривается
-- первой, пока она не названа в списке явно. Не заменять квалификацию на
-- search_path.
--
-- Имена ФУНКЦИЙ и операторов pg_temp перехватить не может (временная схема
-- никогда не просматривается для функций и операторов), но to_jsonb ниже
-- всё равно вызывается как pg_catalog.to_jsonb — в этой функции дешевле
-- перестраховаться, чем полагаться на тонкость разрешения имён.
--
-- ВТОРОЕ ИЗМЕНЕНИЕ — ветка «автор удалён» больше не перечисляет поля
-- вручную. Список из пяти колонок (org_id, entry_date, description, status,
-- reverses_entry_id) оставлял id, created_at и created_by_run_id
-- незакреплёнными, и созданный им зазор был реально использован: created_at
-- проведённой записи переписывался на 1970 год. Вместо списка сравнивается
-- ВСЯ строка целиком через to_jsonb: разрешено ровно одно отличие —
-- created_by_user_id стал NULL. Новая колонка попадает под защиту
-- автоматически, ничего не нужно дописывать сюда при следующей миграции.
-- updated_at из сравнения исключён намеренно: им владеет
-- trg_journal_entries_touch, который срабатывает ПОСЛЕ этого триггера (по
-- алфавиту immutable < touch) и всё равно перезаписывает значение на now().
--
-- Форма условия каскада не изменилась и меняться не должна: только
-- NOT EXISTS по родительской строке. ЗАПРЕЩЕНЫ pg_trigger_depth() > 1
-- (снимает неизменяемость внутри ЛЮБОГО вложенного триггерного контекста) и
-- сессионный флаг вида SET LOCAL app.allow_cascade = on (вручает приложению
-- рубильник от insert-only журнала).
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

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
    -- Единственное исключение — updated_at (см. шапку файла).
    -- Требование «пользователя уже нет в public.users» отсекает попытку
    -- приложения обнулить автора у живой записи: это была бы обычная правка
    -- проведённой проводки, а не каскад.
    IF OLD.created_by_user_id IS NOT NULL AND NEW.created_by_user_id IS NULL
       AND NOT EXISTS (SELECT 1 FROM public.users WHERE id = OLD.created_by_user_id)
       AND (pg_catalog.to_jsonb(NEW) - 'updated_at')
           = (pg_catalog.jsonb_set(pg_catalog.to_jsonb(OLD), '{created_by_user_id}', 'null'::jsonb)
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
