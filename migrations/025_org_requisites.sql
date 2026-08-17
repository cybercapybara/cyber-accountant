-- Реквизиты организации и её расчётные счета (спека конструктора шаблонов §7.2).
--
-- ЗАЧЕМ. Сегодня реквизиты сторон в первичке присылает клиент целиком:
-- definitions.party в templates/docs/invoice/v1/schema.json объявляет name,
-- identifier, address, iik, bik, kbe, и сервер не подставляет из них ничего.
-- В organizations при этом нет ни адреса, ни директора, ни счетов — только
-- bin, name, tax_regime, vat_payer, status. То есть бухгалтер перенабирает
-- собственные банковские реквизиты в каждом счёте, и ничто не мешает двум
-- документам одной организации разойтись в номере счёта.
--
-- Счета вынесены ОТДЕЛЬНОЙ таблицей, а не колонками: у ТОО их обычно
-- несколько (тенге и валюта), у каждого свой банк, БИК и КБе.

ALTER TABLE organizations
    ADD COLUMN IF NOT EXISTS legal_address     TEXT NOT NULL DEFAULT '',
    ADD COLUMN IF NOT EXISTS director_name     TEXT NOT NULL DEFAULT '',
    ADD COLUMN IF NOT EXISTS director_position TEXT NOT NULL DEFAULT 'Директор';

CREATE TABLE IF NOT EXISTS bank_accounts (
    id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id     UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    iik        TEXT NOT NULL,                     -- IBAN KZ..; формат не валидируем на уровне БД
    bank_name  TEXT NOT NULL,
    bik        TEXT NOT NULL DEFAULT '',
    kbe        TEXT NOT NULL DEFAULT '',
    currency   CHAR(3) NOT NULL DEFAULT 'KZT',
    is_primary BOOLEAN NOT NULL DEFAULT FALSE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_id, iik)
);

DROP TRIGGER IF EXISTS trg_bank_accounts_touch ON bank_accounts;
CREATE TRIGGER trg_bank_accounts_touch BEFORE UPDATE ON bank_accounts
    FOR EACH ROW EXECUTE FUNCTION public.touch_updated_at();

CREATE INDEX IF NOT EXISTS idx_bank_accounts_org ON bank_accounts (org_id);

-- Основной счёт — не более одного на организацию. Частичный уникальный индекс,
-- а не CHECK: ограничение межстрочное, и БД обязана держать его сама, иначе
-- «основным» окажутся два счёта и подстановка в документ станет недетерминированной.
CREATE UNIQUE INDEX IF NOT EXISTS uq_bank_accounts_one_primary
    ON bank_accounts (org_id) WHERE is_primary;
