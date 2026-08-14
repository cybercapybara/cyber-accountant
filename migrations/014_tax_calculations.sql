-- tax_calculations: persisted, reproducible outputs of Tax::TaxService
-- (спека §7.2, фаза P2, Task 5). Org-scoped — every calculation belongs to
-- exactly one tenant, unlike tax_rates/tax_constants (migration 011), which
-- are system-wide facts about KZ law with no org_id column at all.
--
-- One row per (org_id, kind, period_from, period_to): a re-run of the SAME
-- period (Tax::TaxService::calculate_snr/calculate_vat called twice for the
-- same half-year/quarter, e.g. after a late journal correction) is expected
-- to REPLACE the previous result, not accumulate a second row next to it —
-- the UNIQUE constraint below is exactly what lets the repository's INSERT
-- ... ON CONFLICT (org_id, kind, period_from, period_to) DO UPDATE upsert
-- work; without it "recalculate" would either violate no constraint at all
-- (silently duplicating rows) or need an application-side
-- SELECT-then-UPDATE-or-INSERT race.
--
-- input_snapshot / result_snapshot (both JSONB, both NOT NULL) are the
-- reproducibility mechanism the design spec §7.2 calls for: input_snapshot
-- records every rate/constant/account-set/period boundary the calculation
-- actually used, result_snapshot records every intermediate and final
-- number it produced — together they let a human (or a future migration)
-- answer "why was the tax THIS number" without re-deriving it from the raw
-- ledger, even if `tax_rates` is re-seeded later with different values.
-- total_tiyn duplicates one figure already inside result_snapshot (the tax
-- due for snr_simplified, the net balance for vat) as a plain BIGINT column
-- so simple "how much tax does this org owe" queries/reports never need to
-- unpack JSONB just to sum a total.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

CREATE TABLE IF NOT EXISTS tax_calculations (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    kind            TEXT NOT NULL CHECK (kind IN ('snr_simplified', 'vat')),
    period_from     DATE NOT NULL,
    period_to       DATE NOT NULL,
    computed_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    input_snapshot  JSONB NOT NULL,
    result_snapshot JSONB NOT NULL,
    total_tiyn      BIGINT NOT NULL,
    UNIQUE (org_id, kind, period_from, period_to),
    -- Composite target for a future tenant-safe FK onto tax_calculations
    -- (e.g. a P3 filing/report row linking back to the calculation it was
    -- generated from) — same forward-looking rationale as
    -- journal_entries.UNIQUE(id, org_id) (migration 009) /
    -- documents.UNIQUE(id, org_id) (migration 010) / employees.UNIQUE(id, org_id)
    -- (migration 012).
    UNIQUE (id, org_id),
    CHECK (period_to >= period_from)
);
CREATE INDEX IF NOT EXISTS idx_tax_calculations_org_kind ON tax_calculations (org_id, kind, period_from DESC);

-- ---------------------------------------------------------------------------
-- Fix-round item from Task 1's review, applied here (not in migration 011,
-- which may already be applied in other environments — renumbering/editing
-- an already-shipped migration is unsafe): migration 011's seed INSERTs use
-- `ON CONFLICT DO NOTHING` on tax_rates/tax_constants, but neither table has
-- a UNIQUE index covering the columns that make a seed row "the same row" on
-- a re-run (kind/region/effective_from for rates, key/effective_from for
-- constants). Without a matching unique index, Postgres has no arbiter to
-- match ON CONFLICT against, so the clause is a silent no-op: re-running
-- migration 011's INSERTs (e.g. a future migration that re-seeds the same
-- effective_from with corrected values, or a manual reapply) would raise a
-- plain duplicate-data problem instead of the idempotent skip the author
-- clearly intended. Adding the indexes here (014) rather than editing 011
-- makes ON CONFLICT DO NOTHING actually mean something from this point
-- forward, without touching a migration file that may have already run
-- elsewhere. COALESCE(region,'') mirrors TaxReferenceRepository::rate_on's
-- own "empty string stands in for NULL region" convention (see that file),
-- so the same (kind, NULL-region, effective_from) triple can never be
-- seeded twice under two different `region` spellings.
CREATE UNIQUE INDEX IF NOT EXISTS uq_tax_rates_kind_region_from
    ON tax_rates (kind, COALESCE(region, ''), effective_from);
CREATE UNIQUE INDEX IF NOT EXISTS uq_tax_constants_key_from
    ON tax_constants (key, effective_from);
