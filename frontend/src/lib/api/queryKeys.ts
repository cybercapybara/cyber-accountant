/**
 * Centralised TanStack Query key factory.
 *
 * One place owns the cache-key shapes so invalidation stays consistent:
 * a mutation invalidates `qk.admin.users()` and every paged variant
 * (usePagedQuery appends the page number to the end) is matched by the
 * prefix. Keep keys as `as const` tuples so TypeScript narrows them.
 */
export const qk = {
  me: () => ['me'] as const,
  orgs: {
    all: (page?: number) =>
      page === undefined ? (['orgs', 'all'] as const) : (['orgs', 'all', page] as const),
    mine: () => ['orgs', 'mine'] as const,
  },
  counterparties: {
    all: (page?: number) =>
      page === undefined
        ? (['counterparties', 'all'] as const)
        : (['counterparties', 'all', page] as const),
  },
  accounts: {
    all: () => ['accounts', 'all'] as const,
  },
  journal: {
    /**
     * `filters` (from/to) is serialised into the key so a changed filter is
     * a fresh cache entry, while the bare prefix (['journal','entries'])
     * still matches every variant for invalidation after post/reverse.
     * usePagedQuery appends the page number on top.
     */
    entries: (filters?: Record<string, string>) =>
      filters === undefined
        ? (['journal', 'entries'] as const)
        : (['journal', 'entries', JSON.stringify(filters)] as const),
    entry: (id: string) => ['journal', 'entry', id] as const,
  },
  admin: {
    users: (page?: number) =>
      page === undefined ? (['admin', 'users'] as const) : (['admin', 'users', page] as const),
    user: (id: string) => ['admin', 'user', id] as const,
    roles: () => ['admin', 'roles'] as const,
    jobs: (filter?: string, page?: number) => {
      if (filter === undefined) return ['admin', 'jobs'] as const;
      if (page === undefined) return ['admin', 'jobs', filter] as const;
      return ['admin', 'jobs', filter, page] as const;
    },
    jobsDlq: () => ['admin', 'jobs-dlq'] as const,
    /**
     * Audit trail list. `filters` is the active filter object; serialising
     * it into the key means a changed filter is a fresh cache entry, while
     * the bare prefix (['admin','audit']) still matches every variant for
     * invalidation. usePagedQuery appends the page number on top.
     */
    audit: (filters?: Record<string, string>) =>
      filters === undefined
        ? (['admin', 'audit'] as const)
        : (['admin', 'audit', JSON.stringify(filters)] as const),
  },
} as const;
