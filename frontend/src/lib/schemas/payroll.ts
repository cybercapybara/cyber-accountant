import { z } from 'zod';

import type { PayrollRun, PayrollRunCreate, Payslip, PayslipDocumentExtra } from '@/lib/api/types';

/**
 * Form schemas, request-body builders and lifecycle derivation for the
 * «Зарплата» screen. Mirrors PayrollRunCreate / PayslipDocumentExtra in
 * docs/openapi.yaml.
 *
 * Three backend behaviours are encoded here rather than left to the page:
 *
 *  1. A run's lifecycle is draft → approved → posted, but only the FIRST
 *     two are `payroll_runs.status` values (the CHECK list is
 *     'draft'|'approved', see migrations/013_payroll.sql). "Posted" is
 *     `journal_entry_id IS NOT NULL` — the compare-and-swap guard
 *     PayrollService uses against a double post. `payrollRunStage` folds
 *     the two columns into the one stage the UI actually reasons about, so
 *     no call site can render an already-posted run as merely "Утверждён"
 *     and offer «Провести в учёт» a second time (which the backend answers
 *     with a 409, never a second journal entry).
 *
 *  2. `POST /payroll-runs` UPSERTS its period (PayrollController.hpp) — so
 *     recalculating a DRAFT run is a normal 200, while recalculating an
 *     approved one is a 409 `invalid_run_state`. `payrollRunActions` is the
 *     single source of which of the three actions is offered at all.
 *
 *  3. The payslip …/generate-document endpoint derives every figure itself,
 *     the net amount spelled out in Russian included — so its allowlist is
 *     EMPTY and the only valid body is `{}`. `buildPayslipDocumentExtra`
 *     returns exactly that, so echoing a server-derived value back — which
 *     the endpoint rejects with a 422 `not_allowed_override` — is
 *     impossible by construction.
 *
 * Every amount on a payslip is an integer count of TIYN (the columns are
 * BIGINT, unlike the ledger's decimal-string amounts), so the totals below
 * are plain integer addition — no float ever touches money here.
 */

/** Nominative month names, indexed 1..12 — `MONTHS_RU[0]` is unused. */
export const MONTHS_RU = [
  '',
  'Январь',
  'Февраль',
  'Март',
  'Апрель',
  'Май',
  'Июнь',
  'Июль',
  'Август',
  'Сентябрь',
  'Октябрь',
  'Ноябрь',
  'Декабрь',
] as const;

/** "Август 2026" — the period label used in headings and confirmations. */
export function payrollPeriodLabel(year: number, month: number): string {
  const name = MONTHS_RU[month];
  return name ? `${name} ${year}` : `${month}.${year}`;
}

/**
 * PayrollController::kMinYear / kMaxYear — a sanity window, deliberately
 * NOT a tax value (those come only from the server's rate tables). Mirrored
 * here so a typo is caught before the request instead of as a 422.
 */
export const MIN_PERIOD_YEAR = 2000;
export const MAX_PERIOD_YEAR = 2100;

export const payrollPeriodSchema = z.object({
  year: z
    .string()
    .trim()
    .regex(/^\d{4}$/, 'Укажите год в формате ГГГГ')
    .refine((v) => {
      const n = Number.parseInt(v, 10);
      return n >= MIN_PERIOD_YEAR && n <= MAX_PERIOD_YEAR;
    }, `Год должен быть между ${MIN_PERIOD_YEAR} и ${MAX_PERIOD_YEAR}`),
  // Kept as a string in the form (a <select> hands react-hook-form a
  // string) and parsed once, in the builder below.
  month: z
    .string()
    .trim()
    .regex(/^(?:[1-9]|1[0-2])$/, 'Выберите месяц'),
});

export type PayrollPeriodValues = z.infer<typeof payrollPeriodSchema>;

/** `{year, month}` as INTEGERS — the API answers a 400 for a string here. */
export function buildPayrollRunCreate(values: PayrollPeriodValues): PayrollRunCreate {
  return {
    year: Number.parseInt(values.year.trim(), 10),
    month: Number.parseInt(values.month.trim(), 10),
  };
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

/** The stage the UI reasons about — see note 1 in this file's header. */
export type PayrollRunStage = 'draft' | 'approved' | 'posted';

/**
 * Fold `status` + `journal_entry_id` into one stage. A posted run keeps
 * `status = 'approved'` forever (the backend never adds a third status), so
 * `journal_entry_id` is checked FIRST — reading the status alone would show
 * a posted run as merely approved and offer to post it again.
 */
export function payrollRunStage(
  run: Pick<PayrollRun, 'status' | 'journal_entry_id'>,
): PayrollRunStage {
  if (run.journal_entry_id) return 'posted';
  return run.status === 'approved' ? 'approved' : 'draft';
}

export interface PayrollRunActions {
  /** POST /payroll-runs for the same period — a 409 once the run left draft. */
  canRecalculate: boolean;
  /** POST /payroll-runs/{id}/approve — draft only. */
  canApprove: boolean;
  /** POST /payroll-runs/{id}/post-to-journal — approved-and-not-yet-posted only. */
  canPost: boolean;
}

/** Exactly one action is available per stage; a posted run offers none. */
export function payrollRunActions(
  run: Pick<PayrollRun, 'status' | 'journal_entry_id'>,
): PayrollRunActions {
  const stage = payrollRunStage(run);
  return {
    canRecalculate: stage === 'draft',
    canApprove: stage === 'draft',
    canPost: stage === 'approved',
  };
}

// ── Payslip totals ──────────────────────────────────────────────────────────

/** The nine money columns of a payslip, in the order the table shows them. */
export const PAYSLIP_AMOUNT_FIELDS = [
  'gross_tiyn',
  'opv',
  'vosms',
  'ipn',
  'net',
  'opvr',
  'so',
  'osms',
  'social_tax',
] as const;

export type PayslipAmountField = (typeof PAYSLIP_AMOUNT_FIELDS)[number];
export type PayslipAmounts = Pick<Payslip, PayslipAmountField>;

/**
 * Column-wise sum for the «Итого» row. Integer addition over tiyn — the
 * only arithmetic this app is allowed to do on money (DESIGN.md §7).
 */
export function sumPayslips(payslips: readonly PayslipAmounts[]): PayslipAmounts {
  const total = {} as Record<PayslipAmountField, number>;
  for (const field of PAYSLIP_AMOUNT_FIELDS) total[field] = 0;
  for (const slip of payslips) {
    for (const field of PAYSLIP_AMOUNT_FIELDS) total[field] += slip[field];
  }
  return total;
}

// ── Расчётный листок (generate-document) ────────────────────────────────────

/**
 * Since P3 the payslip has NO caller-supplied field left: the net amount in
 * words is spelled out server-side from `payslip.net`, and the allowlist is
 * empty (Docgen::InputPolicy::editable_fields returns none for "payslip").
 * The request body is an empty object; any key at all comes back as a 422
 * `not_allowed_override`. The builder is kept — rather than inlining `{}` at
 * the call site — so this rule has one place to live if a field is ever
 * added back.
 */
export function buildPayslipDocumentExtra(): PayslipDocumentExtra {
  return {};
}
