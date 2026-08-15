import { z } from 'zod';

import { toTiyn } from '@/lib/money';

/**
 * Docgen generation form schemas — Task 15.
 *
 * P1 deliberately skips a generic JSON-Schema-driven form engine (YAGNI,
 * per the task brief): GET /api/v1/doc-templates returns each template's
 * draft-07 JSON Schema, but there are only five registered slugs
 * (templates/latex/{invoice,avr,waybill,tax_invoice,reconciliation}/v1/
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

/** VAT rate as entered, e.g. "16" or "16%" — parsed by parseVatRatePercent below. */
const vatRateSchema = z
  .string()
  .trim()
  .regex(/^\d+(\.\d+)?%?$/, 'например 16 или 16%');

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
  vat_rate: z.string().trim().default(''),
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
