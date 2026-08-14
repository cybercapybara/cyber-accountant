-- Migration 006: organizations + org_members — multitenancy foundation
-- (design spec §5). Every later domain table carries org_id NOT NULL
-- REFERENCES organizations(id); this migration lays down the tenant table
-- itself plus the membership/role join table that scopes users to tenants.
--
-- updated_at mechanism matches migrations/000_updated_at_trigger.sql (the
-- shared touch_updated_at() function + a per-table trigger), the same
-- pattern scripts/new-migration.sh --table emits. users.id is UUID (see
-- migrations/001_users_and_roles.sql), so org_members.user_id matches.
--
-- NOTE: MigrationRunner wraps this file in ONE transaction (under an
-- advisory lock) and records schema_migrations in that same transaction.
-- Do NOT add BEGIN/COMMIT here.

-- organizations: tenants of the system.
CREATE TABLE IF NOT EXISTS organizations (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    bin         CHAR(12) NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    tax_regime  TEXT NOT NULL DEFAULT 'snr_simplified'
                CHECK (tax_regime IN ('snr_simplified', 'standard')),
    vat_payer   BOOLEAN NOT NULL DEFAULT FALSE,
    status      TEXT NOT NULL DEFAULT 'active'
                CHECK (status IN ('active', 'suspended', 'archived')),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

DROP TRIGGER IF EXISTS organizations_touch_updated_at ON organizations;
CREATE TRIGGER organizations_touch_updated_at
    BEFORE UPDATE ON organizations
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

-- org_members: membership of a user in an organization, with a role.
CREATE TABLE IF NOT EXISTS org_members (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id      UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    user_id     UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    role        TEXT NOT NULL CHECK (role IN ('owner', 'accountant', 'viewer')),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_id, user_id)
);

DROP TRIGGER IF EXISTS org_members_touch_updated_at ON org_members;
CREATE TRIGGER org_members_touch_updated_at
    BEFORE UPDATE ON org_members
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

CREATE INDEX IF NOT EXISTS idx_org_members_user ON org_members (user_id);
