-- accounts: типовой план счетов РК (приказ МФ РК №185 от 23.05.2007, с изм. на
-- 01.01.2025 — сверено с полным текстом на adilet.zan.kz/rus/docs/V070004771_),
-- системные строки org_id IS NULL; субсчета тенантов org_id NOT NULL (спека
-- §6.1). Единственная таблица с NULLABLE org_id — см. Global Constraints плана.
CREATE TABLE IF NOT EXISTS accounts (
    id               UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id           UUID REFERENCES organizations(id) ON DELETE CASCADE,
    code             TEXT NOT NULL,
    name_ru          TEXT NOT NULL,
    name_kk          TEXT NOT NULL DEFAULT '',
    type             TEXT NOT NULL CHECK (type IN ('asset','liability','equity','income','expense')),
    parent_code      TEXT,
    currency_tracked BOOLEAN NOT NULL DEFAULT FALSE,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);
DROP TRIGGER IF EXISTS trg_accounts_touch ON accounts;
CREATE TRIGGER trg_accounts_touch BEFORE UPDATE ON accounts
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
-- Уникальность кода: среди системных и внутри каждого тенанта.
CREATE UNIQUE INDEX IF NOT EXISTS uq_accounts_system_code ON accounts (code) WHERE org_id IS NULL;
CREATE UNIQUE INDEX IF NOT EXISTS uq_accounts_org_code ON accounts (org_id, code) WHERE org_id IS NOT NULL;

-- Seed сверен построчно с официальным текстом приказа №185 (см. отчёт
-- task-3-report.md за перечнем источников); name_kk оставлены пустыми — их
-- заполнение отнесено к P2 (кадры/двуязычие печатных форм ФНО).
INSERT INTO accounts (code, name_ru, type) VALUES
 ('1010','Денежные средства в кассе','asset'),
 ('1030','Денежные средства на текущих банковских счетах','asset'),
 ('1210','Краткосрочная дебиторская задолженность покупателей и заказчиков','asset'),
 ('1250','Краткосрочная дебиторская задолженность работников','asset'),
 ('1310','Сырье и материалы','asset'),
 ('1330','Товары','asset'),
 ('1420','Налог на добавленную стоимость','asset'),
 ('1710','Краткосрочные авансы выданные','asset'),
 ('2410','Основные средства','asset'),
 ('2420','Амортизация основных средств','asset'),
 ('2730','Прочие нематериальные активы','asset'),
 ('3010','Краткосрочные финансовые обязательства, оцениваемые по амортизированной стоимости','liability'),
 ('3110','Корпоративный подоходный налог, подлежащий уплате','liability'),
 ('3120','Индивидуальный подоходный налог','liability'),
 ('3130','Налог на добавленную стоимость','liability'),
 ('3150','Социальный налог','liability'),
 ('3210','Обязательства по социальному страхованию','liability'),
 ('3220','Обязательства по пенсионным отчислениям','liability'),
 ('3230','Прочие обязательства по другим обязательным платежам','liability'),
 ('3310','Краткосрочная кредиторская задолженность поставщикам и подрядчикам','liability'),
 ('3350','Краткосрочная задолженность по оплате труда','liability'),
 ('3510','Краткосрочные авансы полученные','liability'),
 ('4110','Долгосрочная кредиторская задолженность поставщикам и подрядчикам','liability'),
 ('5030','Вклады и паи','equity'),
 ('5610','Нераспределенная прибыль (непокрытый убыток) отчетного года','equity'),
 ('5620','Нераспределенная прибыль (непокрытый убыток) предыдущих лет','equity'),
 ('6010','Доход от реализации продукции и оказания услуг','income'),
 ('6110','Доходы по вознаграждениям','income'),
 ('6250','Доходы от курсовой разницы','income'),
 ('6290','Прочие доходы','income'),
 ('7010','Себестоимость реализованной продукции и оказанных услуг','expense'),
 ('7110','Расходы по реализации продукции и оказанию услуг','expense'),
 ('7210','Административные расходы','expense'),
 ('7310','Расходы по вознаграждениям','expense'),
 ('7430','Расходы по курсовой разнице','expense'),
 ('7480','Прочие расходы','expense')
ON CONFLICT DO NOTHING;
