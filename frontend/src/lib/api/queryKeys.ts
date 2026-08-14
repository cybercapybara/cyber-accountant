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
    members: (orgId: string) => ['orgs', 'members', orgId] as const,
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
  documents: {
    /**
     * `filters` (type/status) is serialised into the key, same pattern as
     * journal.entries above — a changed filter is a fresh cache entry,
     * while the bare prefix (['documents','all']) still matches every
     * variant for invalidation after upload/confirm-upload/generate.
     * usePagedQuery appends the page number on top.
     */
    all: (filters?: Record<string, string>) =>
      filters === undefined
        ? (['documents', 'all'] as const)
        : (['documents', 'all', JSON.stringify(filters)] as const),
    detail: (id: string) => ['documents', 'detail', id] as const,
  },
  docTemplates: {
    all: () => ['docTemplates', 'all'] as const,
  },
  employees: {
    all: (page?: number) =>
      page === undefined ? (['employees', 'all'] as const) : (['employees', 'all', page] as const),
    detail: (id: string) => ['employees', 'detail', id] as const,
  },
  /**
   * HR orders / labor contracts / vacations, keyed by the active
   * `employee_id` filter ('' = "every employee", which only hr-orders and
   * vacations support — labor-contracts REQUIRES an employee_id, see
   * HrController.hpp). Calling a factory with no argument yields the bare
   * prefix, which still matches every filtered variant for invalidation
   * after a create. hr-orders and vacations are paginated
   * (parse_page_params(50, 200)) — usePagedQuery appends the page number on
   * top, same as journal.entries/documents.all above. labor-contracts stays
   * unpaginated (always scoped to one employee_id).
   */
  hr: {
    orders: (employeeId?: string) =>
      employeeId === undefined
        ? (['hr', 'orders'] as const)
        : (['hr', 'orders', employeeId] as const),
    contracts: (employeeId?: string) =>
      employeeId === undefined
        ? (['hr', 'contracts'] as const)
        : (['hr', 'contracts', employeeId] as const),
    vacations: (employeeId?: string) =>
      employeeId === undefined
        ? (['hr', 'vacations'] as const)
        : (['hr', 'vacations', employeeId] as const),
  },
  /**
   * Payroll runs and their payslips. `runs` is keyed by the active ?year
   * filter ('' = every year); the bare prefix still matches every variant,
   * so a calculate/approve/post invalidation hits them all. usePagedQuery
   * appends the page number on top.
   */
  payroll: {
    runs: (year?: string) =>
      year === undefined ? (['payroll', 'runs'] as const) : (['payroll', 'runs', year] as const),
    payslips: (runId?: string) =>
      runId === undefined
        ? (['payroll', 'payslips'] as const)
        : (['payroll', 'payslips', runId] as const),
  },
  /**
   * Tax reference data, calculations, alerts and ФНО filings. `rates`,
   * `deadlines` and `alerts` take no argument: the pages always ask for
   * "today", which is the server's own default for `?on`.
   */
  tax: {
    rates: () => ['tax', 'rates'] as const,
    deadlines: () => ['tax', 'deadlines'] as const,
    alerts: () => ['tax', 'alerts'] as const,
    calculations: (kind?: string) =>
      kind === undefined
        ? (['tax', 'calculations'] as const)
        : (['tax', 'calculations', kind] as const),
    filings: (kind?: string) =>
      kind === undefined ? (['tax', 'filings'] as const) : (['tax', 'filings', kind] as const),
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
