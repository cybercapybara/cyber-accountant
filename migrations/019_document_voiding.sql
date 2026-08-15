-- Аннулирование документа отдельными колонками (спека P3 §4.2).
-- НЕ значением documents.status: одна колонка обслуживает два жизненных
-- цикла, и запись туда 'void' стёрла бы, был документ 'final' или 'sent'
-- — ровно то, что нужно аудиту. Плюс 'archived' уже занимает нишу
-- терминального состояния, и два неразличимых финала пользователю
-- объяснить нельзя.
--
-- Что отделяет УДАЛЯЕМОЕ от АННУЛИРУЕМОГО — не статус, а связь с
-- ПРОВЕДЁННОЙ (или сторнированной) проводкой:
--
--   NOT EXISTS (SELECT 1 FROM document_entries de
--                 JOIN journal_entries je ON je.id = de.entry_id
--                WHERE de.document_id = $1 AND je.status IN ('posted','reversed'))
--
-- Первая редакция спеки предлагала «удалять черновики» по status='draft',
-- но такой статус бывает только у source='generated': загруженные и
-- пришедшие почтой документы живут в цикле inbox -> recognized -> linked
-- -> archived и не удалились бы никогда, то есть ошибочно загруженный скан
-- оставался бы в реестре навсегда. Журнал append-only и правится только
-- сторно, поэтому документ под проведённой проводкой нельзя уничтожить —
-- его можно только пометить аннулированным, а проводку исправить её
-- собственным механизмом.
--
-- Политика хранения объектов S3 (принятое решение, §4.2): ни физическое
-- удаление документа, ни аннулирование, ни каскад организации объектов в
-- хранилище НЕ трогают. Задачи-сборщика в P3 не появляется; удаляются
-- только метаданные. Это записано здесь явно, чтобы «молчаливая утечка
-- объектов» была осознанной политикой, а не забытым краем.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

ALTER TABLE documents ADD COLUMN IF NOT EXISTS voided_at         TIMESTAMPTZ;
ALTER TABLE documents ADD COLUMN IF NOT EXISTS voided_by_user_id UUID REFERENCES users(id) ON DELETE SET NULL;
ALTER TABLE documents ADD COLUMN IF NOT EXISTS void_reason       TEXT;

-- Все три колонки заполняются вместе либо не заполняются вовсе:
-- «аннулирован без причины и без автора» — не состояние, а потерянная
-- запись аудита.
--
-- voided_by_user_id в аннулированном состоянии допускает NULL сознательно:
-- иначе ON DELETE SET NULL при удалении пользователя нарушил бы CHECK и
-- заблокировал бы удаление самого пользователя — ровно та ошибка, которую
-- задача 12 чинит для журнала.
ALTER TABLE documents DROP CONSTRAINT IF EXISTS documents_void_fields_together;
ALTER TABLE documents ADD CONSTRAINT documents_void_fields_together CHECK (
    (voided_at IS NULL AND voided_by_user_id IS NULL AND void_reason IS NULL)
    OR (voided_at IS NOT NULL AND void_reason IS NOT NULL)
);

CREATE INDEX IF NOT EXISTS idx_documents_org_voided ON documents (org_id, voided_at);
