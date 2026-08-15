-- Версии документов (спека P3 §4.1). Правка документа порождает НОВУЮ
-- версию с новым рендером; предыдущий PDF остаётся. Бухгалтерский документ
-- — свидетельство: тихая перезапись файла, на который могли сослаться,
-- рвёт аудиторский след.
--
-- Файловые метаданные и input_snapshot переезжают с `documents` сюда;
-- `documents` хранит указатель на ТЕКУЩУЮ версию. Версии по отдельности не
-- удаляются никогда — удаление и аннулирование действуют на документ
-- целиком (§4.2).
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

CREATE TABLE IF NOT EXISTS document_versions (
    id                 UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id             UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    document_id        UUID NOT NULL,
    version_no         INTEGER NOT NULL CHECK (version_no >= 1),
    s3_key             TEXT,
    checksum_sha256    CHAR(64),
    mime               TEXT,
    size_bytes         BIGINT,
    template_version   TEXT,
    input_snapshot     JSONB,
    created_by_user_id UUID REFERENCES users(id) ON DELETE SET NULL,
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (document_id, version_no),
    -- Составная цель для указателя documents.current_version_id ниже —
    -- та же идиома, что UNIQUE(id, org_id) на documents/journal_entries:
    -- указатель пиннится сразу по обоим столбцам, поэтому текущая версия
    -- документа провабельно принадлежит тому же тенанту.
    UNIQUE (id, org_id),
    FOREIGN KEY (document_id, org_id) REFERENCES documents (id, org_id) ON DELETE CASCADE
);
DROP TRIGGER IF EXISTS trg_document_versions_touch ON document_versions;
CREATE TRIGGER trg_document_versions_touch BEFORE UPDATE ON document_versions
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
CREATE INDEX IF NOT EXISTS idx_document_versions_document ON document_versions (document_id, version_no DESC);

-- Указатель на текущую (опубликованную) версию. NULL — легальное
-- состояние: «версия создана, рендер ещё не закончился» (§4.2 п.3), и
-- ровно так же выглядит только что созданный документ до успеха джобы.
ALTER TABLE documents ADD COLUMN IF NOT EXISTS current_version_id UUID;

-- Бэкфилл: каждому существующему документу — версия 1 с его нынешними
-- метаданными, указатель ставится только если файл реально есть (иначе
-- документ и был без файла).
--
-- ГВАРД НА ПОВТОРНОЕ ПРИМЕНЕНИЕ. Обычно мигратор применяет файл ровно один
-- раз, но если журнал schema_migrations пропадает, весь набор 000..NNN
-- проигрывается заново поверх УЖЕ мигрированной схемы. Тогда 010
-- (CREATE TABLE IF NOT EXISTS documents) не делает ничего, а этот SELECT
-- читает documents.s3_key — колонку, которую ЭТОТ ЖЕ файл дропает ниже, —
-- и весь прогон падает на `column "s3_key" does not exist`. Ровно так
-- падал build-and-test: tests/integration/test_migrations.cpp роняет
-- schema_migrations на общей тестовой базе, и каждый следующий фикстур
-- умирал в Application initialization.
--
-- Тело DO разбирается ПРИ ВЫПОЛНЕНИИ, поэтому невзятая ветка не
-- обращается к несуществующей колонке даже на этапе планирования. На
-- чистой базе колонка есть, ветка берётся, поведение прежнее.
DO $backfill_document_versions$
BEGIN
    IF EXISTS (SELECT 1
                 FROM information_schema.columns
                WHERE table_schema = 'public'
                  AND table_name = 'documents'
                  AND column_name = 's3_key') THEN
        INSERT INTO document_versions (org_id, document_id, version_no, s3_key, checksum_sha256, mime, size_bytes,
                                       template_version, input_snapshot, created_at, updated_at)
        SELECT org_id, id, 1, s3_key, checksum_sha256, mime, size_bytes, template_version, input_snapshot,
               created_at, updated_at
          FROM documents
         WHERE NOT EXISTS (SELECT 1 FROM document_versions v WHERE v.document_id = documents.id);
    END IF;
END
$backfill_document_versions$;

-- trg_documents_touch выключается ровно на время бэкфилла: перестановка
-- указателя — это переезд схемы, а не правка документа, и она не имеет
-- права переписать updated_at у КАЖДОЙ существующей строки. Тот же
-- аргумент, что и у самой таблицы версий: время последнего изменения
-- документа — часть аудиторского следа, а не служебное поле. Права те же,
-- что уже требует ALTER TABLE ... ADD COLUMN выше (владелец таблицы), и
-- при любой ошибке ниже транзакция мигратора откатит и это тоже.
ALTER TABLE documents DISABLE TRIGGER trg_documents_touch;

UPDATE documents d
   SET current_version_id = v.id
  FROM document_versions v
 WHERE v.document_id = d.id AND v.version_no = 1 AND v.s3_key IS NOT NULL
   AND d.current_version_id IS NULL;

ALTER TABLE documents ENABLE TRIGGER trg_documents_touch;

-- Указатель добавляется ПОСЛЕ бэкфилла: иначе NOT VALID-состояние пришлось
-- бы разруливать отдельно. DEFERRABLE — потому что связь циклическая
-- (documents -> document_versions -> documents), и вставка документа с его
-- первой версией происходит в одной транзакции; она же нужна удалению
-- документа (задача 11): строка documents и строки его версий исчезают
-- одним каскадом, и промежуточное состояние обязано быть легальным.
ALTER TABLE documents DROP CONSTRAINT IF EXISTS documents_current_version_fk;
ALTER TABLE documents ADD CONSTRAINT documents_current_version_fk
    FOREIGN KEY (current_version_id, org_id) REFERENCES document_versions (id, org_id)
    DEFERRABLE INITIALLY DEFERRED;

-- Старые колонки уходят: два источника истины на один и тот же файл —
-- это гарантированное расхождение. Всё, что их читало, переведено на
-- LEFT JOIN к текущей версии (src/ledger/DocumentRepository.hpp).
ALTER TABLE documents DROP COLUMN IF EXISTS s3_key;
ALTER TABLE documents DROP COLUMN IF EXISTS checksum_sha256;
ALTER TABLE documents DROP COLUMN IF EXISTS mime;
ALTER TABLE documents DROP COLUMN IF EXISTS size_bytes;
ALTER TABLE documents DROP COLUMN IF EXISTS template_version;
ALTER TABLE documents DROP COLUMN IF EXISTS input_snapshot;
