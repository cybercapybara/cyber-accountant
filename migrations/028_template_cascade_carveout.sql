-- Узкий вырез в document_templates_immutability() — по образцу
-- migrations/020_journal_cascade_carveout.sql.
--
-- ЧТО СЛОМАЛОСЬ. Триггер из 027 отвергал DELETE опубликованной версии
-- безусловно, а `document_templates.org_id` объявлен ON DELETE CASCADE. Значит
-- организация, у которой есть хоть один опубликованный шаблон, НЕ УДАЛЯЛАСЬ
-- ВООБЩЕ: каскад натыкался на запрет и валил всю транзакцию. Поймано сборкой —
-- восемь тестов RenderJobTest упали разом, потому что общая очистка тестовых
-- данных (`DELETE FROM organizations`) перестала проходить.
--
-- То же и со второй ссылкой: `created_by` объявлен ON DELETE SET NULL, то есть
-- удаление автора шаблона порождает UPDATE опубликованной строки. Без выреза
-- любой, кто когда-либо опубликовал шаблон, не удалялся из системы —
-- дословно тот дефект, что нашли в журнале при релизе v0.3.1.
--
-- ФОРМА УСЛОВИЯ обязана быть NOT EXISTS по РОДИТЕЛЬСКОЙ строке. Запрещены две
-- «простые» альтернативы, обе — дыра: pg_trigger_depth() > 1 снимает
-- неизменяемость в любом вложенном триггерном контексте, а сессионный флаг
-- вручает приложению рубильник от гарантии. Условие работает потому, что при
-- каскаде ссылочной целостности родительская строка в этой транзакции УЖЕ
-- удалена, а при прямом DELETE она на месте.
--
-- КРИТИЧНАЯ ТОНКОСТЬ, из-за которой здесь явная проверка на NULL:
-- у шаблона ПЛОЩАДКИ org_id IS NULL, и `NOT EXISTS (SELECT 1 FROM
-- organizations WHERE id = NULL)` истинно ВСЕГДА — вырез без этой проверки
-- снял бы неизменяемость с общих шаблонов целиком и навсегда. Тот же класс
-- ошибки, что pg_temp в 021: условие, которое выглядит узким, а на деле
-- открыто настежь.
--
-- Имена квалифицированы схемой (public.) — урок 021/022: PostgreSQL ищет имена
-- сначала в pg_temp, право создавать временные таблицы есть у PUBLIC, и
-- неквалифицированное имя позволяет подменить смысл проверки временной
-- таблицей. `SET search_path` лечением не является.
--
-- NOTE: MigrationRunner оборачивает файл в ОДНУ транзакцию. Без BEGIN/COMMIT.

CREATE OR REPLACE FUNCTION public.document_templates_immutability()
RETURNS TRIGGER AS $$
BEGIN
    IF TG_OP = 'DELETE' THEN
        -- Каскад от удалённой организации: родителя уже нет. Только для
        -- шаблонов организации — у шаблона площадки родителя нет вовсе, и
        -- вырез к нему не применяется НИКОГДА.
        IF OLD.org_id IS NOT NULL
           AND NOT EXISTS (SELECT 1 FROM public.organizations WHERE id = OLD.org_id) THEN
            RETURN OLD;
        END IF;
        IF OLD.status = 'published' THEN
            RAISE EXCEPTION 'published template versions are immutable (archive it instead)'
                USING ERRCODE = 'restrict_violation';
        END IF;
        RETURN OLD;
    END IF;

    -- Тот же вырез на UPDATE: порядок обхода ссылочной целостности не
    -- гарантирован, строка может получить UPDATE до собственного DELETE.
    IF OLD.org_id IS NOT NULL
       AND NOT EXISTS (SELECT 1 FROM public.organizations WHERE id = OLD.org_id) THEN
        RETURN NEW;
    END IF;

    -- Удаление АВТОРА шаблона: created_by ON DELETE SET NULL порождает UPDATE
    -- опубликованной строки. Разрешаем ровно это — обнуление created_by при
    -- уже отсутствующем пользователе, и ничего больше.
    IF OLD.status = 'published'
       AND NEW.created_by IS NULL
       AND OLD.created_by IS NOT NULL
       AND NOT EXISTS (SELECT 1 FROM public.users WHERE id = OLD.created_by)
       AND NEW.id = OLD.id
       AND NEW.org_id IS NOT DISTINCT FROM OLD.org_id
       AND NEW.slug = OLD.slug
       AND NEW.version = OLD.version
       AND NEW.mode = OLD.mode
       AND NEW.blocks IS NOT DISTINCT FROM OLD.blocks
       AND NEW.source = OLD.source
       AND NEW.schema = OLD.schema
       AND NEW.form = OLD.form
       AND NEW.expected IS NOT DISTINCT FROM OLD.expected
       AND NEW.status = OLD.status THEN
        RETURN NEW;
    END IF;

    IF OLD.status = 'published' THEN
        IF NEW.status = 'archived'
           AND NEW.id = OLD.id
           AND NEW.org_id IS NOT DISTINCT FROM OLD.org_id
           AND NEW.slug = OLD.slug
           AND NEW.version = OLD.version
           AND NEW.mode = OLD.mode
           AND NEW.blocks IS NOT DISTINCT FROM OLD.blocks
           AND NEW.source = OLD.source
           AND NEW.schema = OLD.schema
           AND NEW.form = OLD.form
           AND NEW.expected IS NOT DISTINCT FROM OLD.expected THEN
            RETURN NEW;
        END IF;
        RAISE EXCEPTION 'published template versions are immutable (publish a new version instead)'
            USING ERRCODE = 'restrict_violation';
    END IF;

    RETURN NEW;
END;
$$ LANGUAGE plpgsql;
