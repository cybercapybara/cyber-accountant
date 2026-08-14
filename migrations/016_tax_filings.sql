-- tax_filings: одна сформированная налоговая отчётность (ФНО) — XML-артефакт
-- в объектном хранилище плюс печатная форма-документ (спека §7.2, фаза P2,
-- Task 12). Org-scoped, идиомы взяты из migrations/010_documents.sql и
-- migrations/014_tax_calculations.sql.
--
-- `kind` — КОД ФОРМЫ ("910.00" / "300.00"), а не kind расчёта
-- ('snr_simplified' / 'vat', migrations/014_tax_calculations.sql). Это
-- сознательно разные словари: одна и та же величина налога может в будущем
-- сдаваться разными формами (и наоборот), а Tax::Fno910/Tax::Fno300 —
-- генераторы КОНКРЕТНЫХ форм, а не расчётов. Соответствие
-- 910.00 <-> snr_simplified и 300.00 <-> vat проверяет API-слой
-- (Api::TaxController::createFiling, 422 kind_mismatch), а не БД: добавление
-- третьей формы не должно требовать миграции.
--
-- `status` ('draft' | 'generated' | 'submitted_manually'):
--   * 'draft'  — строка создана, но XML в хранилище НЕ появился (единственный
--                штатный путь сюда — сбой Storage::put при формировании; см.
--                TaxController::createFiling, поле xml_ready в ответе);
--   * 'generated' — XML лежит в xml_s3_key;
--   * 'submitted_manually' — бухгалтер отчитался через СОНО/кабинет НП сам;
--                v1 по спеке НЕ сдаёт отчётность в госсистемы, поэтому это
--                отметка человека, а не результат интеграции. Эндпоинта для
--                перевода в этот статус в Task 12 нет — значение заведено
--                вместе с остальным жизненным циклом, чтобы не менять CHECK
--                миграцией позже.
--
-- `schema_validated` — валидировался ли XML против официальной XSD. У КГД
-- content-XSD ни для одной ФНО не опубликована (см. src/tax/FnoXml.hpp и
-- src/tax/Fno910.hpp: Tax::Fno910::kSchemaValidated == false), поэтому
-- сегодня столбец всегда false. Он хранится ЯВНО, а не выводится из kind:
-- когда/если XSD появится, признак должен остаться привязанным к КОНКРЕТНОМУ
-- уже сформированному файлу, а не пересчитываться задним числом по коду.
--
-- Составные FK (требование брифа): (calculation_id, org_id) ->
-- tax_calculations(id, org_id) и (document_id, org_id) -> documents(id, org_id)
-- — расчёт или печатный документ ЧУЖОЙ организации отсекается ограничением
-- (SQLSTATE 23503), без EXISTS-проверки в репозитории; то же, что делает
-- payslips в migrations/013_payroll.sql.
--
-- FK на documents объявлен DEFERRABLE INITIALLY DEFERRED (в отличие от FK на
-- tax_calculations, где ON DELETE CASCADE снимает вопрос). Причина: удаление
-- организации каскадит ОДНОВРЕМЕННО по двум путям — organizations ->
-- documents и organizations -> tax_filings, а также organizations ->
-- tax_calculations -> tax_filings. При NO ACTION без отсрочки корректность
-- зависела бы от порядка обработки очереди AFTER-триггеров (успела ли уйти
-- строка tax_filings до проверки ссылки на удаляемый documents-ряд).
-- Отсрочка до COMMIT делает порядок неважным: к моменту проверки обе стороны
-- уже удалены. ON DELETE CASCADE здесь был бы хуже — удаление печатной формы
-- утаскивало бы за собой запись о сданной отчётности вместе с ключом XML.
--
-- Уникальности по (org_id, kind, period_from, period_to) сознательно НЕТ, в
-- отличие от tax_calculations: расчёт периода — идемпотентная величина
-- (пересчёт ЗАМЕЩАЕТ строку), а сдача отчётности — событие. Повторное
-- формирование ФНО за тот же период — это второй артефакт (исправленная
-- версия, перегенерация после правки в журнале), и затирать первый, на
-- который бухгалтер уже мог сослаться, нельзя.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

CREATE TABLE IF NOT EXISTS tax_filings (
    id               UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id           UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    kind             TEXT NOT NULL CHECK (kind IN ('910.00', '300.00')),
    period_from      DATE NOT NULL,
    period_to        DATE NOT NULL,
    status           TEXT NOT NULL DEFAULT 'draft'
                     CHECK (status IN ('draft', 'generated', 'submitted_manually')),
    calculation_id   UUID NOT NULL,
    -- NULL пока XML не сформирован (status = 'draft').
    xml_s3_key       TEXT,
    -- NULL, если печатная форма не создавалась.
    document_id      UUID,
    schema_validated BOOLEAN NOT NULL DEFAULT false,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- Составная цель для будущих tenant-safe FK на tax_filings — та же
    -- forward-looking мотивация, что у tax_calculations.UNIQUE(id, org_id).
    UNIQUE (id, org_id),
    FOREIGN KEY (calculation_id, org_id)
        REFERENCES tax_calculations (id, org_id) ON DELETE CASCADE,
    FOREIGN KEY (document_id, org_id)
        REFERENCES documents (id, org_id) DEFERRABLE INITIALLY DEFERRED,
    CHECK (period_to >= period_from)
);
DROP TRIGGER IF EXISTS trg_tax_filings_touch ON tax_filings;
CREATE TRIGGER trg_tax_filings_touch BEFORE UPDATE ON tax_filings
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
CREATE INDEX IF NOT EXISTS idx_tax_filings_org_kind ON tax_filings (org_id, kind, period_from DESC);
CREATE INDEX IF NOT EXISTS idx_tax_filings_calculation ON tax_filings (calculation_id);
