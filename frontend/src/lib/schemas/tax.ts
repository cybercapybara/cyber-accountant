import { z } from 'zod';

import type {
  TaxCalculation,
  TaxCalculationCreate,
  TaxFiling,
  TaxFilingCreate,
} from '@/lib/api/types';

/**
 * Form schemas, request-body builders and pure derivations for the
 * «Налоги» screen. Mirrors TaxCalculationCreate / TaxFilingCreate in
 * docs/openapi.yaml.
 *
 * Four backend behaviours are encoded here rather than left to the page:
 *
 *  1. `kind` means two DIFFERENT vocabularies. A calculation is
 *     'snr_simplified'|'vat'; a ФНО filing is the FORM CODE '910.00'|'300.00'
 *     (see migrations/016's header). The pairing is enforced server-side
 *     with a 422 `kind_mismatch`, so `filingKindFor` is the one place the
 *     client maps between them — a НДС calculation can never be offered as
 *     a 910.00.
 *
 *  2. Both forms have a legally fixed reporting period: 910.00 is a
 *     полугодие (НК РК ст.722/727), 300.00 a квартал (ст.504-506). The form
 *     therefore asks for a year + an ordinal rather than two free dates,
 *     and `calculationPeriod` expands that into the exact
 *     `period_from`/`period_to` boundaries — a hand-typed period that
 *     straddles two quarters would compute a figure no ФНО can be filed
 *     from, and the API has no way to reject it.
 *
 *  3. `POST /tax/calculations` UPSERTS its (org, kind, period): recomputing
 *     a period REPLACES the stored row and answers 200, so the UI must
 *     treat a repeat as normal rather than as a duplicate.
 *
 *  4. The ФНО print templates require free-text fields no column holds —
 *     `director` and `accountant`. Those TWO per form are the server's
 *     entire allowlist (Docgen::InputPolicy::editable_fields), and it is a
 *     strict one: any other key in `document_input` is a
 *     `422 not_allowed_override`, not a silently ignored field. The two
 *     builders below emit exactly those allowlists — everything else is
 *     derived server-side and deep-merged underneath (RFC 7396).
 *
 *     The amount in words is one of the fields that is now derived: since
 *     P3 the server spells it out from the calculation's own integer total,
 *     so `tax_words`/`balance_words` may not be sent and are not asked for.
 *
 *     `sales_tenge` (the revenue turnover on line 001 of 300.00) used to be
 *     supplied here. It no longer is, and must not be re-added: the server
 *     snapshots the turnover into `result_snapshot.income_tiyn` and derives
 *     the field from it, because a caller may not declare their own revenue
 *     turnover on a legal tax filing.
 *
 * NOTHING here hardcodes a rate, threshold, МРП or МЗП: every such number
 * on the screen comes from `GET /tax/rates` or from a calculation's own
 * `result_snapshot`. The only literals below are the calendar boundaries of
 * a half-year/quarter, which are dates, not tax values.
 */

// ── Calculations ────────────────────────────────────────────────────────────

export const CALCULATION_KINDS = ['snr_simplified', 'vat'] as const;
export type CalculationKind = (typeof CALCULATION_KINDS)[number];

export const CALCULATION_KIND_LABELS: Record<CalculationKind, string> = {
  snr_simplified: 'Упрощёнка (СНР)',
  vat: 'НДС',
};

/** How many ordinals a kind's reporting period divides the year into. */
export const CALCULATION_PERIODS_PER_YEAR: Record<CalculationKind, number> = {
  snr_simplified: 2,
  vat: 4,
};

const ROMAN = ['', 'I', 'II', 'III', 'IV'] as const;

/** «I полугодие» / «III квартал» — the ordinal's own label for a select. */
export function calculationPeriodLabel(kind: CalculationKind, ordinal: number): string {
  const roman = ROMAN[ordinal] ?? String(ordinal);
  return kind === 'snr_simplified' ? `${roman} полугодие` : `${roman} квартал`;
}

const MONTH_END_DAY = ['', '31', '28', '31', '30', '31', '30', '31', '31', '30', '31', '30', '31'];

/**
 * The exact `[period_from, period_to]` boundaries of the kind's `ordinal`-th
 * reporting period in `year`, as `YYYY-MM-DD` strings. Pure string
 * arithmetic — no Date object, so no timezone can shift a boundary onto the
 * neighbouring period (the bug lib/dateFormat.ts documents for date-only
 * values). Every period this produces ends in a 30/31-day month, so
 * February's leap day never enters the calculation.
 */
export function calculationPeriod(
  kind: CalculationKind,
  year: number,
  ordinal: number,
): { period_from: string; period_to: string } {
  const months = 12 / CALCULATION_PERIODS_PER_YEAR[kind];
  const firstMonth = (ordinal - 1) * months + 1;
  const lastMonth = firstMonth + months - 1;
  const pad = (n: number) => String(n).padStart(2, '0');
  return {
    period_from: `${year}-${pad(firstMonth)}-01`,
    period_to: `${year}-${pad(lastMonth)}-${MONTH_END_DAY[lastMonth]}`,
  };
}

export const taxCalculationSchema = z.object({
  kind: z.enum(CALCULATION_KINDS),
  year: z
    .string()
    .trim()
    .regex(/^\d{4}$/, 'Укажите год в формате ГГГГ'),
  // A <select> hands react-hook-form a string; parsed once, in the builder.
  ordinal: z
    .string()
    .trim()
    .regex(/^[1-4]$/, 'Выберите период'),
});

export type TaxCalculationValues = z.infer<typeof taxCalculationSchema>;

/**
 * Body for `POST /tax/calculations`. The ordinal is clamped to the kind's
 * own count first: switching «НДС, IV квартал» to «Упрощёнка» must not ask
 * for a fourth полугодие (there are only two), which the server would
 * silently accept as a period ending in December of the wrong year-half.
 */
export function buildTaxCalculationCreate(values: TaxCalculationValues): TaxCalculationCreate {
  const kind = values.kind;
  const year = Number.parseInt(values.year.trim(), 10);
  const ordinal = clampOrdinal(kind, Number.parseInt(values.ordinal.trim(), 10));
  return { kind, ...calculationPeriod(kind, year, ordinal) };
}

/** 1..CALCULATION_PERIODS_PER_YEAR[kind]. */
export function clampOrdinal(kind: CalculationKind, ordinal: number): number {
  const max = CALCULATION_PERIODS_PER_YEAR[kind];
  if (!Number.isFinite(ordinal) || ordinal < 1) return 1;
  return ordinal > max ? max : ordinal;
}

/**
 * Read one integer-tiyn figure out of a calculation snapshot. The snapshots
 * are free-form JSON server-side (`additionalProperties: true`), so a key a
 * given kind does not record — `ipn_part_tiyn`, deliberately omitted from
 * the СНР snapshot rather than fabricated — comes back as null and the page
 * renders «—» rather than a misleading 0 ₸.
 */
export function snapshotTiyn(
  snapshot: Record<string, unknown> | undefined,
  key: string,
): number | null {
  const value = snapshot?.[key];
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

/**
 * Basis points → a Russian percent string: 400 → "4 %", 350 → "3,5 %",
 * 1625 → "16,25 %". Pure integer arithmetic (the same posture
 * lib/money.ts takes) — no float ever touches a rate, and the rate itself
 * always comes from the server, never from a constant in this file.
 */
export function formatRateBpRu(rateBp: number): string {
  const sign = rateBp < 0 ? '-' : '';
  const abs = Math.abs(Math.trunc(rateBp));
  const whole = Math.floor(abs / 100);
  const frac = abs % 100;
  if (frac === 0) return `${sign}${whole} %`;
  const fracStr = String(frac).padStart(2, '0').replace(/0$/, '');
  return `${sign}${whole},${fracStr} %`;
}

// ── Reference data (GET /tax/rates) ─────────────────────────────────────────

/**
 * Display names for the rate/constant ROWS — the labels only. Every VALUE
 * (percentage, МРП, МЗП, threshold) comes from the response itself; this
 * app never carries a tax number in its own source.
 */
export const RATE_KIND_LABELS: Record<string, string> = {
  vat: 'НДС',
  snr_simplified: 'СНР, упрощённая декларация',
  ipn: 'ИПН',
  opv: 'ОПВ',
  opvr: 'ОПВР',
  so: 'СО',
  osms: 'ОСМС',
  vosms: 'ВОСМС',
  social_tax: 'Социальный налог',
};

export const TAX_CONSTANT_LABELS: Record<string, string> = {
  mrp: 'МРП',
  mzp: 'МЗП',
  ipn_deduction_mrp: 'Вычет по ИПН, в МРП',
  vat_threshold_tenge: 'Порог постановки на учёт по НДС',
  snr_income_limit_mrp: 'Предел дохода на упрощёнке, в МРП',
};

// ── Alerts ──────────────────────────────────────────────────────────────────

export type TaxAlertKind = 'vat_registration' | 'snr_limit';

/**
 * The alert's own `message` field is composed in English server-side
 * (TaxService::threshold_alerts) with the amounts inlined as raw tiyn
 * integers. The UI is Russian-only and renders money through <Money>, so it
 * uses these titles plus the alert's `current_tiyn`/`threshold_tiyn`
 * instead of echoing that string.
 */
export const TAX_ALERT_TITLES: Record<TaxAlertKind, string> = {
  vat_registration: 'Приближение к порогу постановки на учёт по НДС',
  snr_limit: 'Приближение к пределу дохода для упрощённой декларации',
};

export const TAX_ALERT_NOTES: Record<TaxAlertKind, string> = {
  vat_registration: 'НК РК ст. 99 п. 4 пп. 2 / ст. 101 п. 3',
  snr_limit: 'НК РК ст. 722',
};

// ── Deadlines ───────────────────────────────────────────────────────────────

/** Deadlines at or inside this many days are highlighted. */
export const DEADLINE_WARNING_DAYS = 7;

export type DeadlineUrgency = 'overdue' | 'urgent' | 'normal';

/**
 * `days_left` → how loudly the row shouts. A deadline that falls today (or,
 * defensively, in the past) is `overdue`; anything inside
 * DEADLINE_WARNING_DAYS is `urgent`; everything else is `normal`.
 */
export function deadlineUrgency(daysLeft: number): DeadlineUrgency {
  if (daysLeft <= 0) return 'overdue';
  return daysLeft <= DEADLINE_WARNING_DAYS ? 'urgent' : 'normal';
}

export const DEADLINE_KIND_LABELS: Record<string, string> = {
  report: 'Отчёт',
  payment: 'Оплата',
};

// ── ФНО filings ─────────────────────────────────────────────────────────────

export const FILING_KINDS = ['910.00', '300.00'] as const;
export type FilingKind = (typeof FILING_KINDS)[number];

export const FILING_KIND_LABELS: Record<FilingKind, string> = {
  '910.00': 'ФНО 910.00 — упрощённая декларация',
  '300.00': 'ФНО 300.00 — декларация по НДС',
};

export const FILING_STATUS_LABELS: Record<TaxFiling['status'], string> = {
  draft: 'Черновик',
  generated: 'Сформирована',
  submitted_manually: 'Сдана вручную',
};

/**
 * The FORM a calculation of this kind is filed as — the pairing the server
 * enforces with a 422 `kind_mismatch`.
 */
export function filingKindFor(kind: TaxCalculation['kind']): FilingKind {
  return kind === 'snr_simplified' ? '910.00' : '300.00';
}

const SIGNATORY_FIELDS = {
  director: z.string().trim().min(1, 'Укажите ФИО руководителя'),
  accountant: z.string().trim().min(1, 'Укажите ФИО бухгалтера'),
};

/** The 910.00 allowlist: two signatories. The tax amount in words is
 *  derived server-side from the calculation's integer total. */
export const fno910DocumentSchema = z.object({ ...SIGNATORY_FIELDS });

/** The 300.00 allowlist: the same shape — the balance in words is derived
 *  server-side too. */
export const fno300DocumentSchema = z.object({ ...SIGNATORY_FIELDS });

export type Fno910DocumentValues = z.infer<typeof fno910DocumentSchema>;
export type Fno300DocumentValues = z.infer<typeof fno300DocumentSchema>;

export function buildFno910DocumentInput(values: Fno910DocumentValues): Record<string, unknown> {
  return {
    director: values.director.trim(),
    accountant: values.accountant.trim(),
  };
}

export function buildFno300DocumentInput(values: Fno300DocumentValues): Record<string, unknown> {
  return {
    director: values.director.trim(),
    accountant: values.accountant.trim(),
  };
}

/**
 * Body for `POST /tax/filings`. `kind` is derived from the calculation
 * rather than taken from the form, so the pairing rule (note 1 above) holds
 * by construction instead of by the user picking correctly.
 */
export function buildTaxFilingCreate(
  calculation: Pick<TaxCalculation, 'id' | 'kind'>,
  documentInput: Record<string, unknown>,
): TaxFilingCreate {
  return {
    kind: filingKindFor(calculation.kind),
    calculation_id: calculation.id,
    document_input: documentInput,
  };
}
