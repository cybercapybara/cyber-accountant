import { z } from 'zod';

import type {
  Employee,
  EmployeeCreate,
  EmployeeUpdate,
  HrOrderCreate,
  LaborContractCreate,
  VacationCreate,
} from '@/lib/api/types';
import { toTiyn } from '@/lib/money';

/**
 * Form schemas + request-body builders for the HR screens (Employees,
 * Кадры). Mirrors EmployeeCreate / EmployeeUpdate / EmployeeDismiss /
 * HrOrderCreate / LaborContractCreate / VacationCreate in
 * docs/openapi.yaml.
 *
 * Two backend behaviours are encoded here rather than left to the pages:
 *
 *  1. PATCH /employees/{id} REJECTS `hired_on`, `status` and
 *     `dismissed_on` with a 422 (EmployeesController.hpp: "explicit
 *     failure over silent no-op") — dismissal goes through
 *     POST /employees/{id}/dismiss instead. `buildEmployeeUpdate` is
 *     therefore built from an allowlist of editable fields, so no future
 *     edit-form field can leak one of those three into a PATCH body.
 *
 *  2. The …/generate-document endpoints take ONLY the free-text fields the
 *     LaTeX template schemas need and that this codebase has no column for
 *     (director, salary_words, work_schedule, addresses); everything else —
 *     the employee's ИИН, name, position, salary, the org's name/БИН — is
 *     derived server-side and deep-merged underneath (RFC 7396). The two
 *     `build*DocumentExtra` builders below emit exactly that allowlist and
 *     nothing else, so echoing a server-derived value back (which a strict
 *     allowlist rejects with a 422) is impossible by construction.
 *
 * Money stays a decimal string on the wire (`salary: "300000.00"`, parsed
 * by Ledger::parse_tiyn server-side); nothing here does float arithmetic on
 * it — `toTiyn` (integer parse, lib/money.ts) is used only for the
 * "> 0" check.
 */

const ISO_DATE_MESSAGE = 'Формат: ГГГГ-ММ-ДД';
const ISO_DATE_RE = /^\d{4}-\d{2}-\d{2}$/;

/** Required calendar date, `YYYY-MM-DD` (the server re-checks it is a real date). */
function isoDate() {
  return z.string().trim().regex(ISO_DATE_RE, ISO_DATE_MESSAGE);
}

/** Optional calendar date — '' means "not set" and is stripped from the body. */
function optionalIsoDate() {
  return z
    .string()
    .trim()
    .default('')
    .refine((v) => v === '' || ISO_DATE_RE.test(v), ISO_DATE_MESSAGE);
}

/**
 * Decimal-string money, e.g. "300000.00" — the same client-side contract
 * lib/schemas/journal.ts uses for a ledger amount. The server re-parses it
 * (Ledger::parse_tiyn) and answers a 422 on anything it rejects.
 */
const salarySchema = z
  .string()
  .trim()
  .regex(/^\d+(\.\d{1,2})?$/, 'Не более 2 знаков после запятой, например 300000.00')
  .refine((v) => toTiyn(v) > 0, 'Оклад должен быть больше нуля');

// ── Employees ───────────────────────────────────────────────────────────────

/**
 * Everything PATCH /employees/{id} accepts. The create form extends this
 * with `hired_on` (create-only: immutable afterwards).
 */
export const employeeEditSchema = z.object({
  iin: z
    .string()
    .trim()
    .regex(/^[0-9]{12}$/, 'ИИН должен состоять ровно из 12 цифр'),
  last_name: z.string().trim().min(1, 'Укажите фамилию'),
  first_name: z.string().trim().min(1, 'Укажите имя'),
  middle_name: z.string().trim().default(''),
  position: z.string().trim().min(1, 'Укажите должность'),
  salary: salarySchema,
  ipn_deduction_claimed: z.boolean().default(false),
  opvr_exempt: z.boolean().default(false),
  payout_iik: z.string().trim().default(''),
});

export const employeeCreateSchema = employeeEditSchema.extend({
  hired_on: isoDate(),
});

export const employeeDismissSchema = z.object({
  dismissed_on: isoDate(),
});

export type EmployeeEditValues = z.infer<typeof employeeEditSchema>;
export type EmployeeCreateValues = z.infer<typeof employeeCreateSchema>;
export type EmployeeDismissValues = z.infer<typeof employeeDismissSchema>;

/**
 * PATCH body — the allowlist of editable columns, and only those. An empty
 * `middle_name` is omitted rather than sent as "" so a patch never
 * gratuitously blanks a name the user did not touch.
 */
export function buildEmployeeUpdate(values: EmployeeEditValues): EmployeeUpdate {
  const body: EmployeeUpdate = {
    iin: values.iin.trim(),
    last_name: values.last_name.trim(),
    first_name: values.first_name.trim(),
    position: values.position.trim(),
    salary: values.salary.trim(),
    ipn_deduction_claimed: values.ipn_deduction_claimed,
    opvr_exempt: values.opvr_exempt,
    payout_iik: values.payout_iik.trim(),
  };
  const middleName = values.middle_name.trim();
  if (middleName) body.middle_name = middleName;
  return body;
}

/**
 * POST body — the PATCH allowlist plus the create-only `hired_on`. Spelled
 * out rather than spread over `buildEmployeeUpdate`, because EmployeeCreate
 * declares the three defaulted fields as required while EmployeeUpdate
 * leaves them optional.
 */
export function buildEmployeeCreate(values: EmployeeCreateValues): EmployeeCreate {
  const patch = buildEmployeeUpdate(values);
  const body: EmployeeCreate = {
    iin: patch.iin,
    last_name: patch.last_name,
    first_name: patch.first_name,
    position: patch.position,
    salary: patch.salary,
    hired_on: values.hired_on.trim(),
    ipn_deduction_claimed: values.ipn_deduction_claimed,
    opvr_exempt: values.opvr_exempt,
    payout_iik: values.payout_iik.trim(),
  };
  if (patch.middle_name) body.middle_name = patch.middle_name;
  return body;
}

/** "Фамилия Имя Отчество", collapsing an absent/blank middle name. */
export function employeeFullName(
  employee: Pick<Employee, 'last_name' | 'first_name' | 'middle_name'>,
): string {
  return [employee.last_name, employee.first_name, employee.middle_name ?? '']
    .map((part) => part.trim())
    .filter(Boolean)
    .join(' ');
}

// ── HR orders ───────────────────────────────────────────────────────────────

export const HR_ORDER_KINDS = [
  'hire',
  'dismiss',
  'vacation',
  'business_trip',
  'salary_change',
] as const;
export type HrOrderKind = (typeof HR_ORDER_KINDS)[number];

export const HR_ORDER_KIND_LABELS: Record<HrOrderKind, string> = {
  hire: 'Приём на работу',
  dismiss: 'Увольнение',
  vacation: 'Отпуск',
  business_trip: 'Командировка',
  salary_change: 'Изменение оклада',
};

export interface HrOrderPayloadField {
  /** Key inside `hr_orders.payload` (a free-form JSON object server-side). */
  name: string;
  label: string;
  placeholder?: string;
}

/**
 * The five per-kind variants of the create form's extra fields. `payload`
 * has no server-side schema at all (`additionalProperties: true`), so this
 * table IS the contract: it keeps a приказ of one kind from carrying
 * another kind's keys.
 */
export const HR_ORDER_PAYLOAD_FIELDS: Record<HrOrderKind, HrOrderPayloadField[]> = {
  hire: [
    { name: 'position', label: 'Должность по приказу' },
    { name: 'probation_months', label: 'Испытательный срок, мес.', placeholder: '3' },
  ],
  dismiss: [
    { name: 'reason', label: 'Основание увольнения', placeholder: 'Соглашение сторон' },
    { name: 'article', label: 'Статья ТК РК', placeholder: 'пп. 1 п. 1 ст. 49' },
  ],
  vacation: [
    { name: 'days', label: 'Количество дней', placeholder: '24' },
    { name: 'basis', label: 'Основание', placeholder: 'Заявление сотрудника' },
  ],
  business_trip: [
    { name: 'destination', label: 'Место назначения', placeholder: 'г. Астана' },
    { name: 'purpose', label: 'Цель командировки' },
  ],
  salary_change: [
    { name: 'old_salary', label: 'Прежний оклад, ₸' },
    { name: 'new_salary', label: 'Новый оклад, ₸' },
  ],
};

export const hrOrderSchema = z.object({
  employee_id: z.string().trim().min(1, 'Выберите сотрудника'),
  kind: z.enum(HR_ORDER_KINDS),
  number: z.string().trim().min(1, 'Укажите номер приказа'),
  issued_on: isoDate(),
  effective_from: isoDate(),
  effective_to: optionalIsoDate(),
  /** Per-kind free-text fields, keyed by HR_ORDER_PAYLOAD_FIELDS[kind]. */
  payload: z.record(z.string()).default({}),
});

export type HrOrderValues = z.infer<typeof hrOrderSchema>;

/**
 * Only the current kind's non-empty fields end up in `payload` — switching
 * kind mid-form must not smuggle the previous kind's answers into the
 * order. Returns undefined when nothing is filled in, so the body omits
 * `payload` entirely rather than sending an empty object.
 */
export function buildHrOrderPayload(
  kind: HrOrderKind,
  payload: Record<string, string>,
): Record<string, string> | undefined {
  const picked: Record<string, string> = {};
  for (const field of HR_ORDER_PAYLOAD_FIELDS[kind]) {
    const value = (payload[field.name] ?? '').trim();
    if (value) picked[field.name] = value;
  }
  return Object.keys(picked).length > 0 ? picked : undefined;
}

export function buildHrOrderCreate(values: HrOrderValues): HrOrderCreate {
  const body: HrOrderCreate = {
    employee_id: values.employee_id,
    kind: values.kind,
    number: values.number.trim(),
    issued_on: values.issued_on.trim(),
    effective_from: values.effective_from.trim(),
  };
  const effectiveTo = values.effective_to.trim();
  if (effectiveTo) body.effective_to = effectiveTo;
  const payload = buildHrOrderPayload(values.kind, values.payload);
  if (payload) body.payload = payload;
  return body;
}

// ── Labor contracts ─────────────────────────────────────────────────────────

export const laborContractSchema = z.object({
  employee_id: z.string().trim().min(1, 'Выберите сотрудника'),
  number: z.string().trim().min(1, 'Укажите номер договора'),
  signed_on: isoDate(),
  starts_on: isoDate(),
  ends_on: optionalIsoDate(),
});

export type LaborContractValues = z.infer<typeof laborContractSchema>;

export function buildLaborContractCreate(values: LaborContractValues): LaborContractCreate {
  const body: LaborContractCreate = {
    employee_id: values.employee_id,
    number: values.number.trim(),
    signed_on: values.signed_on.trim(),
    starts_on: values.starts_on.trim(),
  };
  const endsOn = values.ends_on.trim();
  if (endsOn) body.ends_on = endsOn;
  return body;
}

// ── Vacations ───────────────────────────────────────────────────────────────

export const VACATION_KINDS = ['annual', 'unpaid', 'sick'] as const;
export type VacationKind = (typeof VACATION_KINDS)[number];

export const VACATION_KIND_LABELS: Record<VacationKind, string> = {
  annual: 'Ежегодный оплачиваемый',
  unpaid: 'Без сохранения заработной платы',
  sick: 'Больничный',
};

export const vacationSchema = z
  .object({
    employee_id: z.string().trim().min(1, 'Выберите сотрудника'),
    kind: z.enum(VACATION_KINDS),
    starts_on: isoDate(),
    ends_on: isoDate(),
    // Kept as a string in the form (an <input type="number"> hands
    // react-hook-form a string) and parsed once, in the builder below.
    days: z
      .string()
      .trim()
      .regex(/^[1-9]\d*$/, 'Укажите целое число дней больше нуля'),
  })
  // Mirrors migrations/012_hr.sql's CHECK (ends_on >= starts_on), which the
  // server pre-checks with a 422 — ISO dates compare lexicographically the
  // same as chronologically, so this is the identical comparison.
  .refine((v) => v.ends_on >= v.starts_on, {
    message: 'Дата окончания не может быть раньше даты начала',
    path: ['ends_on'],
  });

export type VacationValues = z.infer<typeof vacationSchema>;

export function buildVacationCreate(values: VacationValues): VacationCreate {
  return {
    employee_id: values.employee_id,
    starts_on: values.starts_on.trim(),
    ends_on: values.ends_on.trim(),
    days: Number.parseInt(values.days, 10),
    kind: values.kind,
  };
}

// ── generate-document free-text extras ──────────────────────────────────────

/**
 * templates/latex/hr_order/v1/schema.json requires `director` on top of
 * everything the server derives; `reason` and `details` are optional.
 */
export const hrOrderDocumentSchema = z.object({
  director: z.string().trim().min(1, 'Укажите ФИО руководителя'),
  reason: z.string().trim().default(''),
  details: z.string().trim().default(''),
});

export type HrOrderDocumentValues = z.infer<typeof hrOrderDocumentSchema>;

export function buildHrOrderDocumentExtra(values: HrOrderDocumentValues): Record<string, unknown> {
  const extra: Record<string, unknown> = { director: values.director.trim() };
  const reason = values.reason.trim();
  if (reason) extra.reason = reason;
  const details = values.details.trim();
  if (details) extra.details = details;
  return extra;
}

/**
 * templates/latex/labor_contract/v1/schema.json requires `salary_words`,
 * `work_schedule` and `employer.director`; `probation_months` and the two
 * addresses are optional. `employer`/`employee` are sent as partial objects
 * — the merge patch fills in name/БИН and ФИО/ИИН/должность underneath, and
 * re-sending them from the client would be both redundant and (under the
 * strict allowlist) a 422.
 */
export const laborContractDocumentSchema = z.object({
  director: z.string().trim().min(1, 'Укажите ФИО руководителя'),
  salary_words: z.string().trim().min(1, 'Укажите оклад прописью'),
  work_schedule: z.string().trim().min(1, 'Укажите режим рабочего времени'),
  probation_months: z.string().trim().default(''),
  employer_address: z.string().trim().default(''),
  employee_address: z.string().trim().default(''),
});

export type LaborContractDocumentValues = z.infer<typeof laborContractDocumentSchema>;

export function buildLaborContractDocumentExtra(
  values: LaborContractDocumentValues,
): Record<string, unknown> {
  const employer: Record<string, string> = { director: values.director.trim() };
  const employerAddress = values.employer_address.trim();
  if (employerAddress) employer.address = employerAddress;

  const extra: Record<string, unknown> = {
    employer,
    salary_words: values.salary_words.trim(),
    work_schedule: values.work_schedule.trim(),
  };
  const probation = values.probation_months.trim();
  if (probation) extra.probation_months = probation;
  const employeeAddress = values.employee_address.trim();
  if (employeeAddress) extra.employee = { address: employeeAddress };
  return extra;
}
