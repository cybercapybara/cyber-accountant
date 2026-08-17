-- Пользовательские шаблоны документов (спека конструктора §5).
--
-- ЗАЧЕМ. Десять встроенных шаблонов лежат на диске и проходят гейт
-- `template-render` на каждой сборке. Пользовательский шаблон в репозиторий не
-- попадает никогда — значит, ему нужно другое место и другая проверка
-- (гейт при публикации, §9 спеки).
--
-- org_id IS NULL означает шаблон ПЛОЩАДКИ: виден всем арендаторам, менять его
-- вправе только администратор инсталляции. Это не «ничей» шаблон, а
-- общий — поэтому NULL, а не отдельная таблица.

CREATE TABLE IF NOT EXISTS document_templates (
    id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id     UUID REFERENCES organizations(id) ON DELETE CASCADE,
    slug       TEXT NOT NULL,
    version    INT  NOT NULL,
    mode       TEXT NOT NULL CHECK (mode IN ('blocks', 'source')),
    -- Источник правды для mode='blocks'. Для mode='source' NULL: блоков там
    -- нет, и притворяться, что они есть, значило бы хранить выдумку.
    blocks     JSONB,
    -- Текст Typst. Для 'blocks' ПОРОЖДЁН из blocks при публикации, для
    -- 'source' — авторский. Хранится в обоих случаях: рендер обязан быть
    -- воспроизводим без повторной сборки из блоков.
    source     TEXT NOT NULL,
    schema     JSONB NOT NULL,
    -- Метаданные формы: подписи (рус./каз.), порядок, группы, виджеты.
    -- Отдельно от schema намеренно: schema — контракт валидации, а не
    -- описание интерфейса, и смешивать их значит ломать оба.
    form       JSONB NOT NULL DEFAULT '{}'::jsonb,
    -- Статические подписи, которые шаблон ОБЯЗАН напечатать. Для 'blocks'
    -- порождены; для 'source' NULL — вывести их оттуда неоткуда, и это
    -- прямо записанное ограничение (§12 спеки), а не недоделка.
    expected   TEXT,
    status     TEXT NOT NULL DEFAULT 'draft' CHECK (status IN ('draft', 'published', 'archived')),
    created_by UUID REFERENCES users(id) ON DELETE SET NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- Версия уникальна в пределах (владелец, слаг). NULLS NOT DISTINCT, иначе
    -- у шаблонов площадки (org_id IS NULL) ограничение не работало бы вовсе:
    -- в обычном UNIQUE две строки с NULL считаются разными.
    CONSTRAINT uq_document_templates_version UNIQUE NULLS NOT DISTINCT (org_id, slug, version),
    -- Блоки обязательны для 'blocks' и запрещены для 'source': строка, у
    -- которой режим и содержимое разошлись, не поддаётся осмысленной сборке.
    CONSTRAINT ck_document_templates_blocks CHECK (
        (mode = 'blocks' AND blocks IS NOT NULL) OR (mode = 'source' AND blocks IS NULL)
    )
);

CREATE INDEX IF NOT EXISTS idx_document_templates_org_slug ON document_templates (org_id, slug);
-- Разрешение шаблона при рендере ищет ПОСЛЕДНЮЮ опубликованную версию.
CREATE INDEX IF NOT EXISTS idx_document_templates_published
    ON document_templates (org_id, slug, version DESC) WHERE status = 'published';

DROP TRIGGER IF EXISTS trg_document_templates_touch ON document_templates;
CREATE TRIGGER trg_document_templates_touch BEFORE UPDATE ON document_templates
    FOR EACH ROW EXECUTE FUNCTION public.touch_updated_at();

-- Опубликованная версия неизменяема.
--
-- Та же дисциплина, что у document_versions: документ, выпущенный вчера,
-- обязан оставаться воспроизводимым, а он ссылается на версию шаблона.
-- Правка опубликованного шаблона молча переписала бы прошлое — печать того же
-- документа дала бы другой PDF.
--
-- Разрешён РОВНО один переход: published -> archived. Архивация ничего не
-- переписывает, она лишь убирает шаблон из выбора; выпущенные документы
-- продолжают ссылаться на ту же строку.
--
-- ИМЕНА КВАЛИФИЦИРОВАНЫ СХЕМОЙ (public./pg_catalog.) — урок migrations/021 и
-- 022: PostgreSQL ищет имена сначала в pg_temp, право создавать временные
-- таблицы есть у PUBLIC, и неквалифицированное имя в SECURITY INVOKER
-- функции позволяет подменить смысл проверки. `SET search_path` лечением НЕ
-- является: pg_temp просматривается первой, пока не названа явно.
CREATE OR REPLACE FUNCTION public.document_templates_immutability()
RETURNS TRIGGER AS $$
BEGIN
    IF TG_OP = 'DELETE' THEN
        IF OLD.status = 'published' THEN
            RAISE EXCEPTION 'published template versions are immutable (archive it instead)'
                USING ERRCODE = 'restrict_violation';
        END IF;
        RETURN OLD;
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

DROP TRIGGER IF EXISTS trg_document_templates_immutability ON document_templates;
CREATE TRIGGER trg_document_templates_immutability
    BEFORE UPDATE OR DELETE ON document_templates
    FOR EACH ROW EXECUTE FUNCTION public.document_templates_immutability();
