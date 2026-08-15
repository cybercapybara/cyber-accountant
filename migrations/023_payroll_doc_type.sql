-- doc_type='payroll' — отдельная корзина для зарплатных документов (спека
-- P3 §5.3: «Оклад видит, расчёты — нет»).
--
-- ЧТО ЧИНИМ. Расчётный листок создавался с doc_type='hr' (в CHECK ниже
-- просто не было ничего лучше), а Api::LedgerDocumentsController::
-- resource_for() относит doc_type='hr' к ресурсу hr_docs, который кадровику
-- открыт. В итоге кадровик читал расчётный листок — а это и есть расчёт:
-- gross, ОПВ, ВОСМС, ИПН, СО, ОСМС, ОПВР, соцналог и net. Ресурс payroll
-- в матрице кадровику невидим, и утечка шла мимо матрицы, через тип
-- документа. Найдено на приёмке v0.4.0 в проде.
--
-- ПОЧЕМУ ОТДЕЛЬНЫЙ doc_type, А НЕ РАЗБОР ПО template_slug. Сужение
-- реестра для кадровика (GET /documents) — это SQL-фильтр ПО doc_type,
-- один и тот же для строк и для count. Если бы расчётный листок остался
-- doc_type='hr', а решение принималось по второй колонке, сужение пришлось
-- бы учить второму условию отдельно от resource_for() — то есть завести
-- вторую копию правила «что кадровику видно», ровно ту, которую P3
-- специально сводил в один хелпер. С отдельным doc_type правило снова
-- одно и то же в обоих местах и опирается на одну колонку. Плюс это
-- закрывает и ЗАГРУЖЕННЫЙ (source='uploaded') расчётный листок, у
-- которого template_slug пустой в принципе.
--
-- BACKFILL СУЩЕСТВУЮЩИХ СТРОК. В проде расчётные листки уже есть. Они
-- опознаются точно и только по template_slug='payslip': этот slug ставит
-- единственное место — Api::PayrollController::generatePayslip
-- (kPayslipSlug), а source='uploaded'/'email' строки template_slug не
-- заполняют вообще (DocumentRepository::create вызывается там с
-- std::nullopt). Ни кадровые приказы (hr_order), ни трудовые договоры
-- (labor_contract) под условие не попадают и остаются doc_type='hr'.
-- UPDATE идемпотентен: повторный прогон присвоит то же значение тем же
-- строкам.
--
-- ПОРЯДОК ВАЖЕН: CHECK расширяется ДО backfill — иначе UPDATE упрётся в
-- старое ограничение, которое 'payroll' не знает. Оба стейтмента едут в
-- одной транзакции (её открывает раннер), так что промежуточного
-- состояния «CHECK уже новый, строки ещё старые» снаружи не видно.
--
-- CHECK в migrations/010_documents.sql безымянный, поэтому снимается по
-- автоимени, которое Postgres ему дал: <таблица>_<колонка>_check =
-- documents_doc_type_check.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

ALTER TABLE documents DROP CONSTRAINT IF EXISTS documents_doc_type_check;
ALTER TABLE documents ADD CONSTRAINT documents_doc_type_check
    CHECK (doc_type IN (
        'invoice', 'avr', 'waybill', 'tax_invoice', 'reconciliation',
        'power_of_attorney', 'incoming', 'bank_statement', 'hr', 'payroll', 'fno', 'other'
    ));

UPDATE documents SET doc_type = 'payroll' WHERE template_slug = 'payslip';
