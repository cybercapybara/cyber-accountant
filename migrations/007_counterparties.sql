-- counterparties: контрагенты организации (спека §6.3). Org-scoped.
CREATE TABLE IF NOT EXISTS counterparties (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id        UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    identifier    CHAR(12) NOT NULL,           -- БИН или ИИН
    name          TEXT NOT NULL,
    address       TEXT NOT NULL DEFAULT '',
    iik           TEXT NOT NULL DEFAULT '',    -- IBAN KZ.. (формат не валидируем в P1)
    bik           TEXT NOT NULL DEFAULT '',
    kbe           TEXT NOT NULL DEFAULT '',
    is_resident   BOOLEAN NOT NULL DEFAULT TRUE,
    vat_payer     BOOLEAN NOT NULL DEFAULT FALSE,
    contact_email TEXT NOT NULL DEFAULT '',
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_id, identifier)
);
DROP TRIGGER IF EXISTS trg_counterparties_touch ON counterparties;
CREATE TRIGGER trg_counterparties_touch BEFORE UPDATE ON counterparties
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
CREATE INDEX IF NOT EXISTS idx_counterparties_org ON counterparties (org_id);
