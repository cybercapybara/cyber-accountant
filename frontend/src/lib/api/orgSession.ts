/**
 * Client-side record of the organization the user chose to work in.
 *
 * POST /api/v1/orgs/{id}/switch mints a new access token carrying an
 * `org` claim and sets it via the __Host-access cookie — in this
 * template's cookie-mode auth there is no Bearer token for the SPA to
 * store, so switching an org needs no client-side token bookkeeping
 * (see client.ts's module comment on credentials: 'include').
 *
 * What DOES need bookkeeping is *intent*: docs/openapi.yaml documents
 * that a bare POST /api/v1/auth/refresh resets the access token's `org`
 * claim back to mint_session's single-membership default (empty for a
 * caller in 0 or >1 organizations) — so a multi-org user silently drops
 * out of the org they picked on every refresh. localStorage survives
 * refreshes and reloads, so client.ts replays /switch right after every
 * successful refresh using the id stored here (see tryRefresh there).
 *
 * The same id doubles as this tab's best guess at "the org whose access
 * token is currently live" for UI gating (e.g. only enabling member
 * management for the active org) — we can't read the `org` claim
 * ourselves since the cookie is HttpOnly, but we control every call to
 * /switch, so our own record of the last id we switched into is
 * accurate as long as it's kept in sync with every switch call site.
 */
const STORAGE_KEY = 'cyber-accountant:active-org-id';

export function getSelectedOrgId(): string | null {
  try {
    return localStorage.getItem(STORAGE_KEY);
  } catch {
    // Private browsing / storage disabled — degrade to "no org selected".
    return null;
  }
}

export function setSelectedOrgId(id: string): void {
  try {
    localStorage.setItem(STORAGE_KEY, id);
  } catch {
    /* ignore */
  }
}

export function clearSelectedOrgId(): void {
  try {
    localStorage.removeItem(STORAGE_KEY);
  } catch {
    /* ignore */
  }
}
