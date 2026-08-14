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
 */
const decimalAmountSchema = z
  .string()
  .trim()
  .regex(/^\d+(\.\d{1,2})?$/, 'Up to 2 decimal places, e.g. 1234.56')
  .refine((v) => toTiyn(v) > 0, 'Must be greater than zero');

/** Same shape as decimalAmountSchema, but blank is a valid "no entry" —
 *  reconciliation rows commonly post to only one side (debit XOR credit). */
const optionalDecimalAmountSchema = z
  .string()
  .trim()
  .refine((v) => v === '' || /^\d+(\.\d{1,2})?$/.test(v), 'Up to 2 decimal places, or leave blank');

/** dd.mm.yyyy — the date format every template's JSON Schema pattern requires. */
const dateDmySchema = z
  .string()
  .trim()
  .regex(/^\d{2}\.\d{2}\.\d{4}$/, 'Use DD.MM.YYYY');

/** VAT rate as entered, e.g. "16" or "16%" — parsed by parseVatRatePercent below. */
const vatRateSchema = z
  .string()
  .trim()
  .regex(/^\d+(\.\d+)?%?$/, 'e.g. 16 or 16%');

/**
 * Generic docgen `party` (definitions.party in every template schema).
 * Used both for the manually-entered seller ("my requisites", see
 * lib/docParty.ts) and — mapped from a Counterparty — the buyer/party_b.
 */
export const partySchema = z.object({
  name: z.string().trim().min(1, 'Name is required'),
  identifier: z.string().trim().min(1, 'Identifier is required'),
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
  name: z.string().trim().min(1, 'Required'),
  qty: z.string().trim().min(1, 'Required'),
  unit: z.string().trim().min(1, 'Required'),
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
  number: z.string().trim().min(1, 'Required'),
  date: dateDmySchema,
  seller: sellerDefaultsSchema,
  buyerCounterpartyId: z.string().trim().min(1, 'Select a counterparty'),
  contract: z.string().trim().default(''),
  items: z.array(lineItemSchema).min(1, 'Add at least one line'),
  vat_rate: z.string().trim().default(''),
  total_words: z.string().trim().min(1, 'Required'),
});
export type InvoiceFormValues = z.infer<typeof invoiceFormSchema>;

export const avrFormSchema = invoiceFormSchema.extend({
  act_period: z.string().trim().min(1, 'Required'),
});
export type AvrFormValues = z.infer<typeof avrFormSchema>;

export const waybillFormSchema = z.object({
  number: z.string().trim().min(1, 'Required'),
  date: dateDmySchema,
  seller: sellerDefaultsSchema,
  buyerCounterpartyId: z.string().trim().min(1, 'Select a counterparty'),
  basis: z.string().trim().min(1, 'Required'),
  items: z.array(lineItemSchema).min(1, 'Add at least one line'),
  total_words: z.string().trim().min(1, 'Required'),
  released_by: z.string().trim().min(1, 'Required'),
  received_by: z.string().trim().min(1, 'Required'),
});
export type WaybillFormValues = z.infer<typeof waybillFormSchema>;

export const taxInvoiceFormSchema = z.object({
  number: z.string().trim().min(1, 'Required'),
  date: dateDmySchema,
  seller: sellerDefaultsSchema,
  buyerCounterpartyId: z.string().trim().min(1, 'Select a counterparty'),
  buyerVatCertificate: z.string().trim().default(''),
  items: z.array(vatLineItemSchema).min(1, 'Add at least one line'),
  total_words: z.string().trim().min(1, 'Required'),
});
export type TaxInvoiceFormValues = z.infer<typeof taxInvoiceFormSchema>;

export const reconciliationRowSchema = z.object({
  date: dateDmySchema,
  doc: z.string().trim().min(1, 'Required'),
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
  counterpartyId: z.string().trim().min(1, 'Select a counterparty'),
  openingADebit: optionalDecimalAmountSchema,
  openingACredit: optionalDecimalAmountSchema,
  rows: z.array(reconciliationRowSchema).min(1, 'Add at least one row'),
  aSays: z.string().trim().min(1, 'Required'),
  bSays: z.string().trim().min(1, 'Required'),
});
export type ReconciliationFormValues = z.infer<typeof reconciliationFormSchema>;

/** Parse "16" or "16%" into a plain percentage number; unparsable → 0 so a
 *  still-being-typed rate never throws mid-edit (same contract as toTiyn). */
export function parseVatRatePercent(rate: string): number {
  const match = /^(\d+(?:\.\d+)?)/.exec(rate.trim());
  if (!match) return 0;
  return Number.parseFloat(match[1]);
}
