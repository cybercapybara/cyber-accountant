import { z } from 'zod';

import type { Document } from '@/lib/api/types';
import { toTiyn } from '@/lib/money';

/**
 * Docgen generation form schemas — Task 15.
 *
 * P1 deliberately skips a generic JSON-Schema-driven form engine (YAGNI,
 * per the task brief): GET /api/v1/doc-templates returns each template's
 * draft-07 JSON Schema, but there are only five registered slugs
 * (templates/docs/{invoice,avr,waybill,tax_invoice,reconciliation}/v1/
 * schema.json) and their shapes are stable, so each gets its own typed
 * zod schema + form component in pages/GenerateDocument.tsx instead.
 *
 * Money fields stay decimal strings end-to-end, same contract as
 * lib/schemas/journal.ts: an integer or up-to-two-decimal-place string,
 * parsed via toTiyn (never Number() + multiplication) so a running total
 * never drifts through float rounding.
 *
 * No form here asks for an amount in words, and none may grow one back:
 * since P3 the server derives every printed money string of the document
 * total (`total`/`total_words`, `totals.amount`/`vat`/`with_vat`) from the
 * integer tiyn the builders in pages/GenerateDocument.tsx send, and answers
 * a client-supplied one with a 422 `not_allowed_override`
 * (src/docgen/InputPolicy.hpp). The total is therefore not a form field at
 * all — it is computed from the line items on submit.
 */
const decimalAmountSchema = z
  .string()
  .trim()
  .regex(/^\d+(\.\d{1,2})?$/, 'Не более 2 знаков после запятой, например 1234.56')
  .refine((v) => toTiyn(v) > 0, 'Должно быть больше нуля');

/** Same shape as decimalAmountSchema, but blank is a valid "no entry" —
 *  reconciliation rows commonly post to only one side (debit XOR credit). */
const optionalDecimalAmountSchema = z
  .string()
  .trim()
  .refine(
    (v) => v === '' || /^\d+(\.\d{1,2})?$/.test(v),
    'Не более 2 знаков после запятой, либо оставьте пустым',
  );

/** dd.mm.yyyy — the date format every template's JSON Schema pattern requires. */
const dateDmySchema = z
  .string()
  .trim()
  .regex(/^\d{2}\.\d{2}\.\d{4}$/, 'Формат: ДД.ММ.ГГГГ');

/**
 * VAT rate as entered, e.g. "16" or "16%" — parsed by parseVatRatePercent
 * below. The shape mirrors the `vat_rate` pattern in
 * templates/docs/{invoice,avr}/v1/schema.json exactly, so a rate this form
 * accepts is never one the server answers with a 422: at most two digits, an
 * optional one-or-two-place decimal part, an optional percent sign, and
 * NOTHING else. The narrowness is a security property, not tidiness — the
 * rate is printed inside the VAT line's parentheses, right beside the
 * server-derived amount, and a rate free to contain ')', ':' or '₸' could
 * close the label and print a fabricated figure of its own.
 */
const vatRateSchema = z
  .string()
  .trim()
  .regex(/^\d{1,2}([.,]\d{1,2})?%?$/, 'например 16 или 16%');

/** The same rate, but blank is valid and means "this document has no VAT". */
const optionalVatRateSchema = z
  .string()
  .trim()
  .refine((v) => v === '' || /^\d{1,2}([.,]\d{1,2})?%?$/.test(v), 'например 16 или 16%');

/**
 * Generic docgen `party` (definitions.party in every template schema).
 * Used both for the manually-entered seller ("my requisites", see
 * lib/docParty.ts) and — mapped from a Counterparty — the buyer/party_b.
 */
export const partySchema = z.object({
  name: z.string().trim().min(1, 'Укажите наименование'),
  identifier: z.string().trim().min(1, 'Укажите идентификатор'),
  address: z.string().trim().default(''),
  iik: z.string().trim().default(''),
  bik: z.string().trim().default(''),
  bank: z.string().trim().default(''),
  kbe: z.string().trim().default(''),
});
export type PartyValues = z.infer<typeof partySchema>;

/**
 * The seller party plus the tax_invoice-only vat_certificate field —
 * everything localStorage remembers as "my requisites" (see
 * lib/docParty.ts's getSellerDefaults/setSellerDefaults).
 */
export const sellerDefaultsSchema = partySchema.extend({
  vat_certificate: z.string().trim().default(''),
});
export type SellerDefaultsValues = z.infer<typeof sellerDefaultsSchema>;

/** name/qty/unit/price line — invoice, avr, waybill. `amount` is client-computed. */
export const lineItemSchema = z.object({
  name: z.string().trim().min(1, 'Обязательное поле'),
  qty: z.string().trim().min(1, 'Обязательное поле'),
  unit: z.string().trim().min(1, 'Обязательное поле'),
  price: decimalAmountSchema,
});
export type LineItemValues = z.infer<typeof lineItemSchema>;
export const EMPTY_LINE_ITEM: LineItemValues = { name: '', qty: '1', unit: '', price: '' };

/** Same, plus a per-item vat_rate — tax_invoice only. amount/vat_amount/
 *  total_with_vat are all client-computed from price*qty and vat_rate. */
export const vatLineItemSchema = lineItemSchema.extend({
  vat_rate: vatRateSchema,
});
export type VatLineItemValues = z.infer<typeof vatLineItemSchema>;
export const EMPTY_VAT_LINE_ITEM: VatLineItemValues = { ...EMPTY_LINE_ITEM, vat_rate: '16' };

export const invoiceFormSchema = z.object({
  number: z.string().trim().min(1, 'Обязательное поле'),
  date: dateDmySchema,
  seller: sellerDefaultsSchema,
  buyerCounterpartyId: z.string().trim().min(1, 'Выберите контрагента'),
  contract: z.string().trim().default(''),
  items: z.array(lineItemSchema).min(1, 'Добавьте хотя бы одну строку'),
  vat_rate: optionalVatRateSchema.default(''),
});
export type InvoiceFormValues = z.infer<typeof invoiceFormSchema>;

export const avrFormSchema = invoiceFormSchema.extend({
  act_period: z.string().trim().min(1, 'Обязательное поле'),
});
export type AvrFormValues = z.infer<typeof avrFormSchema>;

export const waybillFormSchema = z.object({
  number: z.string().trim().min(1, 'Обязательное поле'),
  date: dateDmySchema,
  seller: sellerDefaultsSchema,
  buyerCounterpartyId: z.string().trim().min(1, 'Выберите контрагента'),
  basis: z.string().trim().min(1, 'Обязательное поле'),
  items: z.array(lineItemSchema).min(1, 'Добавьте хотя бы одну строку'),
  released_by: z.string().trim().min(1, 'Обязательное поле'),
  received_by: z.string().trim().min(1, 'Обязательное поле'),
});
export type WaybillFormValues = z.infer<typeof waybillFormSchema>;

export const taxInvoiceFormSchema = z.object({
  number: z.string().trim().min(1, 'Обязательное поле'),
  date: dateDmySchema,
  seller: sellerDefaultsSchema,
  buyerCounterpartyId: z.string().trim().min(1, 'Выберите контрагента'),
  buyerVatCertificate: z.string().trim().default(''),
  items: z.array(vatLineItemSchema).min(1, 'Добавьте хотя бы одну строку'),
});
export type TaxInvoiceFormValues = z.infer<typeof taxInvoiceFormSchema>;

export const reconciliationRowSchema = z.object({
  date: dateDmySchema,
  doc: z.string().trim().min(1, 'Обязательное поле'),
  a_debit: optionalDecimalAmountSchema,
  a_credit: optionalDecimalAmountSchema,
  b_debit: optionalDecimalAmountSchema,
  b_credit: optionalDecimalAmountSchema,
});
export type ReconciliationRowValues = z.infer<typeof reconciliationRowSchema>;
export const EMPTY_RECONCILIATION_ROW: ReconciliationRowValues = {
  date: '',
  doc: '',
  a_debit: '',
  a_credit: '',
  b_debit: '',
  b_credit: '',
};

export const reconciliationFormSchema = z.object({
  period_from: dateDmySchema,
  period_to: dateDmySchema,
  partyA: sellerDefaultsSchema,
  counterpartyId: z.string().trim().min(1, 'Выберите контрагента'),
  openingADebit: optionalDecimalAmountSchema,
  openingACredit: optionalDecimalAmountSchema,
  rows: z.array(reconciliationRowSchema).min(1, 'Добавьте хотя бы одну строку'),
  aSays: z.string().trim().min(1, 'Обязательное поле'),
  bSays: z.string().trim().min(1, 'Обязательное поле'),
});
export type ReconciliationFormValues = z.infer<typeof reconciliationFormSchema>;

/** Parse "16" or "16%" into a plain percentage number; unparsable → 0 so a
 *  still-being-typed rate never throws mid-edit (same contract as toTiyn). */
export function parseVatRatePercent(rate: string): number {
  const match = /^(\d+(?:\.\d+)?)/.exec(rate.trim());
  if (!match) return 0;
  return Number.parseFloat(match[1]);
}

// ── Аннулирование ───────────────────────────────────────────────────────────

/** Причина аннулирования — обязательна: аннулирование без причины
 *  бессмысленно для аудита, ради которого оно и существует. */
export const voidDocumentSchema = z.object({
  reason: z.string().trim().min(1, 'Укажите причину аннулирования'),
});
export type VoidDocumentValues = z.infer<typeof voidDocumentSchema>;

// ── Что с документом вообще можно сделать ───────────────────────────────────

/**
 * Почему сервер отказал в удалении. Клиент не может узнать это заранее:
 * в `Document` нет ни поля «связан с проведённой проводкой», ни «на меня
 * ссылается приказ» — оба условия живут в других таблицах и наружу не
 * выставлены (см. LedgerDocumentsController::remove). Поэтому интерфейс
 * делает две вещи: правило написано рядом с кнопками ДО первого клика, а
 * полученный от сервера отказ запоминается и превращает кнопку «Удалить» в
 * объяснение — второй раз пользователь в ту же стену не упирается.
 */
export type DeleteBlockCode = 'document_has_posted_entries' | 'document_referenced';

export const DELETE_BLOCK_REASONS: Record<DeleteBlockCode, string> = {
  document_has_posted_entries:
    'Документ связан с проведённой проводкой — его можно только аннулировать.',
  document_referenced:
    'На документ ссылается кадровый приказ или налоговая отчётность — доступно только аннулирование.',
};

/**
 * Слаги, у которых есть форма правки. `payslip` отсутствует намеренно: с
 * P3 у расчётного листка не осталось каллер-полей вовсе — всё, включая
 * сумму прописью, выводится из сохранённой ведомости, и пустая правка
 * означала бы «перерендерить то же самое».
 */
export const EDITABLE_TEMPLATE_SLUGS = [
  'invoice',
  'avr',
  'waybill',
  'tax_invoice',
  'reconciliation',
  'fno_910',
  'fno_300',
  'hr_order',
  'labor_contract',
] as const;
export type EditableTemplateSlug = (typeof EDITABLE_TEMPLATE_SLUGS)[number];

export function isEditableTemplateSlug(slug: string | null): slug is EditableTemplateSlug {
  return !!slug && (EDITABLE_TEMPLATE_SLUGS as readonly string[]).includes(slug);
}

export interface DocumentActionAvailability {
  canEdit: boolean;
  canDelete: boolean;
  canVoid: boolean;
  /** Почему действие недоступно — показывается вместо кнопки, а не вместо ошибки. */
  editBlockReason: string | null;
  deleteBlockReason: string | null;
  voidBlockReason: string | null;
}

/**
 * Единственное место, где решается, что предложить пользователю над
 * конкретным документом. Чистая функция от строки документа и от уже
 * полученного (если был) отказа сервера в удалении — поэтому она
 * тестируется без рендера.
 *
 * Главное правило, которое интерфейс обязан показывать, а не прятать за
 * ошибкой: документ, ставший основанием проведённой проводки, удалить
 * нельзя никогда — его аннулируют. Значит и предлагать надо аннулирование.
 */
export function documentActionAvailability(
  doc: Pick<Document, 'source' | 'template_slug' | 'voided_at'>,
  deleteBlock?: DeleteBlockCode | null,
): DocumentActionAvailability {
  if (doc.voided_at) {
    return {
      canEdit: false,
      canDelete: false,
      canVoid: false,
      editBlockReason: 'Аннулированный документ изменить нельзя — он больше не перерендеривается.',
      deleteBlockReason:
        'Аннулированный документ удалить нельзя: он остаётся в реестре как след решения.',
      voidBlockReason: 'Документ уже аннулирован.',
    };
  }

  let editBlockReason: string | null = null;
  if (doc.source !== 'generated') {
    editBlockReason = 'Загруженные и присланные почтой документы не редактируются.';
  } else if (doc.template_slug === 'payslip') {
    editBlockReason =
      'У расчётного листка не осталось полей для правки — он целиком выводится из ведомости.';
  } else if (!isEditableTemplateSlug(doc.template_slug)) {
    editBlockReason = 'Для этого шаблона правка не поддерживается.';
  }

  const deleteBlockReason = deleteBlock ? DELETE_BLOCK_REASONS[deleteBlock] : null;

  return {
    canEdit: editBlockReason === null,
    canDelete: deleteBlockReason === null,
    canVoid: true,
    editBlockReason,
    deleteBlockReason,
    voidBlockReason: null,
  };
}

/**
 * Статус, на котором поллит `useDocumentRender` (`refetchInterval` там стоит
 * ровно на `status === 'draft'`). Вынесен в константу, чтобы условие второго
 * поллинга ниже ссылалось на ТО ЖЕ значение, а не на свою копию строки.
 */
export const STATUS_POLL_STATUS = 'draft';

/**
 * Ждёт ли документ рендера НОВОЙ версии — того, что запускает правка.
 *
 * Это второй, независимый источник поллинга на странице документов, и он
 * обязан не пересекаться с первым. Первый — `useDocumentRender` — ждёт
 * ПЕРВОГО рендера и опрашивает сервер, пока `status === 'draft'`. Правка
 * же `status` не трогает вовсе: двигается указатель текущей версии, и ждать
 * приходится, пока `latest_version_no` не догонит его.
 *
 * Взаимоисключение держится на явной проверке `status !== 'draft'` ниже, а
 * не на рассуждении «так получается»: два поллинга по одному документу
 * удваивают нагрузку и путают таймауты. Свойство закреплено тестом
 * (documents.test.ts, «never overlaps the status poll»).
 *
 * Аннулированный документ не перерендеривается никогда — ждать нечего.
 * `currentVersionNo === null` означает, что история ещё не загружена или
 * текущей версии нет вовсе: сравнивать не с чем, поллинг не начинаем.
 */
export function isAwaitingVersionRender(
  doc: Pick<Document, 'latest_version_no' | 'status' | 'voided_at'>,
  currentVersionNo: number | null,
): boolean {
  if (currentVersionNo === null) return false;
  if (doc.voided_at) return false;
  if (doc.status === STATUS_POLL_STATUS) return false;
  return doc.latest_version_no > currentVersionNo;
}

// ── Снапшот версии → значения формы правки ──────────────────────────────────

/**
 * `input_snapshot` — это то, что уходило в рендер: суммы в нём уже
 * отформатированы («1 234,56»), ставка НДС записана как «16%». Формы же
 * работают с сырыми десятичными строками, поэтому предзаполнение — не
 * присваивание, а обратное преобразование, и оно покрыто тестами.
 *
 * Ни одна из этих функций НЕ достаёт из снапшота `total`, `total_words`,
 * `totals.*` и прочие серверные производные: их вычисляет сервер, а
 * присланные клиентом — 422 `not_allowed_override`. Итог формы всегда
 * пересчитывается из позиций.
 */
type Snapshot = Record<string, unknown> | null | undefined;

function snapshotString(value: unknown): string {
  return typeof value === 'string' ? value : '';
}

function snapshotObject(value: unknown): Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
}

function snapshotArray(value: unknown): unknown[] {
  return Array.isArray(value) ? value : [];
}

/**
 * «1 234,56» → «1234.56» — обратное к `formatTiynRu`. Неразобранное
 * значение становится пустой строкой, а не нулём и не мусором: пустое поле
 * поймает zod при отправке, а «0» тихо подменил бы цену.
 */
export function ruMoneyToDecimal(value: unknown): string {
  // \s already covers NBSP and the narrow no-break space, so a value grouped
  // by anything other than formatTiynRu's plain ASCII space parses too.
  const raw = snapshotString(value).replace(/\s/g, '').replace(',', '.');
  return /^\d+(\.\d{1,2})?$/.test(raw) ? raw : '';
}

/** «16%» → «16»; пусто остаётся пустым (НДС в счёте необязателен). */
export function vatRateFromSnapshot(value: unknown): string {
  return snapshotString(value).replace('%', '').trim();
}

function snapshotToParty(value: unknown): SellerDefaultsValues {
  const party = snapshotObject(value);
  return {
    name: snapshotString(party.name),
    identifier: snapshotString(party.identifier),
    address: snapshotString(party.address),
    iik: snapshotString(party.iik),
    bik: snapshotString(party.bik),
    bank: snapshotString(party.bank),
    kbe: snapshotString(party.kbe),
    vat_certificate: snapshotString(party.vat_certificate),
  };
}

function snapshotToLineItems(value: unknown): LineItemValues[] {
  const items = snapshotArray(value).map((raw) => {
    const item = snapshotObject(raw);
    return {
      name: snapshotString(item.name),
      qty: snapshotString(item.qty),
      unit: snapshotString(item.unit),
      price: ruMoneyToDecimal(item.price),
    };
  });
  return items.length > 0 ? items : [{ ...EMPTY_LINE_ITEM }];
}

export function snapshotToInvoiceValues(
  snapshot: Snapshot,
  counterpartyId: string | null,
): InvoiceFormValues {
  const input = snapshotObject(snapshot);
  return {
    number: snapshotString(input.number),
    date: snapshotString(input.date),
    seller: snapshotToParty(input.seller),
    buyerCounterpartyId: counterpartyId ?? '',
    contract: snapshotString(input.contract),
    items: snapshotToLineItems(input.items),
    vat_rate: vatRateFromSnapshot(input.vat_rate),
  };
}

export function snapshotToAvrValues(
  snapshot: Snapshot,
  counterpartyId: string | null,
): AvrFormValues {
  const input = snapshotObject(snapshot);
  return {
    ...snapshotToInvoiceValues(snapshot, counterpartyId),
    act_period: snapshotString(input.act_period),
  };
}

export function snapshotToWaybillValues(
  snapshot: Snapshot,
  counterpartyId: string | null,
): WaybillFormValues {
  const input = snapshotObject(snapshot);
  return {
    number: snapshotString(input.number),
    date: snapshotString(input.date),
    seller: snapshotToParty(input.seller),
    buyerCounterpartyId: counterpartyId ?? '',
    basis: snapshotString(input.basis),
    items: snapshotToLineItems(input.items),
    released_by: snapshotString(input.released_by),
    received_by: snapshotString(input.received_by),
  };
}

export function snapshotToTaxInvoiceValues(
  snapshot: Snapshot,
  counterpartyId: string | null,
): TaxInvoiceFormValues {
  const input = snapshotObject(snapshot);
  const items = snapshotArray(input.items).map((raw) => {
    const item = snapshotObject(raw);
    return {
      name: snapshotString(item.name),
      qty: snapshotString(item.qty),
      unit: snapshotString(item.unit),
      price: ruMoneyToDecimal(item.price),
      vat_rate: vatRateFromSnapshot(item.vat_rate),
    };
  });
  return {
    number: snapshotString(input.number),
    date: snapshotString(input.date),
    seller: snapshotToParty(input.seller),
    buyerCounterpartyId: counterpartyId ?? '',
    buyerVatCertificate: snapshotString(snapshotObject(input.buyer).vat_certificate),
    items: items.length > 0 ? items : [{ ...EMPTY_VAT_LINE_ITEM }],
  };
}

export function snapshotToReconciliationValues(
  snapshot: Snapshot,
  counterpartyId: string | null,
): ReconciliationFormValues {
  const input = snapshotObject(snapshot);
  const opening = snapshotObject(input.opening_balance);
  const closing = snapshotObject(input.closing);
  const rows = snapshotArray(input.rows).map((raw) => {
    const row = snapshotObject(raw);
    return {
      date: snapshotString(row.date),
      doc: snapshotString(row.doc),
      a_debit: ruMoneyToDecimal(row.a_debit),
      a_credit: ruMoneyToDecimal(row.a_credit),
      b_debit: ruMoneyToDecimal(row.b_debit),
      b_credit: ruMoneyToDecimal(row.b_credit),
    };
  });
  return {
    period_from: snapshotString(input.period_from),
    period_to: snapshotString(input.period_to),
    partyA: snapshotToParty(input.party_a),
    counterpartyId: counterpartyId ?? '',
    openingADebit: ruMoneyToDecimal(opening.a_debit),
    openingACredit: ruMoneyToDecimal(opening.a_credit),
    rows: rows.length > 0 ? rows : [{ ...EMPTY_RECONCILIATION_ROW }],
    aSays: snapshotString(closing.a_says),
    bSays: snapshotString(closing.b_says),
  };
}

/** Подписанты ФНО 910.00/300.00 — единственное, что каллеру дозволено. */
export function snapshotToSignatories(snapshot: Snapshot): {
  director: string;
  accountant: string;
} {
  const input = snapshotObject(snapshot);
  return {
    director: snapshotString(input.director),
    accountant: snapshotString(input.accountant),
  };
}

/** Кадровый приказ: «Руководитель», «Основание», «Детали». */
export function snapshotToHrOrderValues(snapshot: Snapshot): {
  director: string;
  reason: string;
  details: string;
} {
  const input = snapshotObject(snapshot);
  return {
    director: snapshotString(input.director),
    reason: snapshotString(input.reason),
    details: snapshotString(input.details),
  };
}

/** Трудовой договор: пять allowlisted-полей, два из них — вложенные. */
export function snapshotToLaborContractValues(snapshot: Snapshot): {
  director: string;
  work_schedule: string;
  probation_months: string;
  employer_address: string;
  employee_address: string;
} {
  const input = snapshotObject(snapshot);
  const employer = snapshotObject(input.employer);
  const employee = snapshotObject(input.employee);
  const probation = input.probation_months;
  return {
    director: snapshotString(employer.director),
    work_schedule: snapshotString(input.work_schedule),
    probation_months: typeof probation === 'number' ? String(probation) : snapshotString(probation),
    employer_address: snapshotString(employer.address),
    employee_address: snapshotString(employee.address),
  };
}
