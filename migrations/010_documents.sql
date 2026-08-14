-- documents: единый реестр первички любого происхождения (спека §6.4).
-- One row is EITHER something our own docgen produced (source='generated',
-- lifecycle draft -> final -> sent) OR something that arrived from outside
-- (source='uploaded'|'email', lifecycle inbox -> recognized -> linked ->
-- archived). P1 does not build a state machine over `status` (unlike
-- journal_entries in migration 009) — the CHECK below simply allows every
-- value from BOTH lifecycles; the API layer (Task 12/13) is responsible for
-- only ever requesting a transition that makes sense for a given document's
-- source, and this repository trusts it (see DocumentRepository::set_status).
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

CREATE TABLE IF NOT EXISTS documents (
    id               UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id           UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    doc_type         TEXT NOT NULL CHECK (doc_type IN (
                         'invoice', 'avr', 'waybill', 'tax_invoice', 'reconciliation',
                         'power_of_attorney', 'incoming', 'bank_statement', 'hr', 'fno', 'other'
                     )),
    source           TEXT NOT NULL CHECK (source IN ('generated', 'uploaded', 'email')),
    status           TEXT NOT NULL CHECK (status IN (
                         'inbox', 'recognized', 'linked', 'archived', 'draft', 'final', 'sent'
                     )),
    -- No ON DELETE clause — mirrors journal_lines.counterparty_id (migration
    -- 009): a counterparty referenced by a document is protected from
    -- deletion the same way one referenced by a journal line is (default
    -- NO ACTION).
    counterparty_id  UUID REFERENCES counterparties(id),
    -- File metadata: NULL until DocumentRepository::set_file() lands the
    -- rendered/uploaded object in S3 (design spec §6.4: "файлы в S3, в БД —
    -- метаданные, ключ S3, checksum, MIME").
    s3_key           TEXT,
    -- Lowercase hex sha256 digest (Utils::Crypto::sha256_hex,
    -- src/utils/Crypto.hpp) is always exactly 64 characters — CHAR(64)
    -- mirrors the fixed-length-code idiom already used for
    -- organizations.bin / counterparties.identifier (CHAR(12)).
    checksum_sha256  CHAR(64),
    mime             TEXT,
    size_bytes       BIGINT,
    -- Set only for source='generated' rows (Docgen::TemplateRegistry,
    -- templates/latex/<slug>/v<N>/). template_version is TEXT, not INTEGER:
    -- it is carried forward opaquely from the registry's on-disk version
    -- label — nothing in this table does arithmetic or ordering on it.
    template_slug    TEXT,
    template_version TEXT,
    -- Snapshot of the exact input JSON a 'generated' document was rendered
    -- from (design spec §9: reproducibility). NULL — not '{}' — for any
    -- document that never went through docgen; the two are semantically
    -- different (see Ledger::Document's doc comment).
    input_snapshot   JSONB,
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- Composite target for document_entries' tenant-safe FK below — same
    -- rationale as journal_entries.UNIQUE(id, org_id) in migration 009: lets
    -- that FK pin BOTH document_id and org_id at once, so a link's org_id is
    -- provably the same tenant as the document it points to, not just any
    -- tenant's document with a matching id.
    UNIQUE (id, org_id)
);
DROP TRIGGER IF EXISTS trg_documents_touch ON documents;
CREATE TRIGGER trg_documents_touch BEFORE UPDATE ON documents
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
CREATE INDEX IF NOT EXISTS idx_documents_org_created ON documents (org_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_documents_org_type ON documents (org_id, doc_type);

-- document_entries: many-to-many link between documents and journal_entries
-- (спека §6.4: "связь many-to-many с journal_entries"). No UNIQUE PK-only
-- constraint carries meaning beyond UNIQUE(document_id, entry_id) below —
-- global document-number uniqueness is explicitly out of scope for P1.
--
-- Both FKs are composite, against (id, org_id) of their target, NOT a plain
-- id -> table(id) FK: a plain FK only proves the target row EXISTS, not that
-- it belongs to the SAME org as this link row — exactly the tenant-isolation
-- hole journal_lines.entry_id closed in migration 009 after the security
-- scan flagged it there. Pinning both columns here means
-- DocumentRepository::link_entry() needs NO application-side EXISTS check
-- for a cross-org mismatch: a document from one org paired with an entry
-- from another simply cannot satisfy both FKs at once, so the INSERT fails
-- with SQLSTATE 23503 (foreign_key_violation) — see that method's doc
-- comment for how the failure is surfaced to the caller.
CREATE TABLE IF NOT EXISTS document_entries (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id       UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    document_id  UUID NOT NULL,
    entry_id     UUID NOT NULL,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (document_id, entry_id),
    FOREIGN KEY (document_id, org_id) REFERENCES documents (id, org_id) ON DELETE CASCADE,
    FOREIGN KEY (entry_id, org_id) REFERENCES journal_entries (id, org_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_document_entries_document ON document_entries (document_id);
CREATE INDEX IF NOT EXISTS idx_document_entries_entry ON document_entries (entry_id);
