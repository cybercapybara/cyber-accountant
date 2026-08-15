import { describe, expect, it } from 'vitest';

import type {
  AvrFormValues,
  InvoiceFormValues,
  PartyValues,
  SellerDefaultsValues,
  TaxInvoiceFormValues,
  WaybillFormValues,
} from '@/lib/schemas/documents';

import {
  buildAvrInput,
  buildInvoiceInput,
  buildTaxInvoiceInput,
  buildWaybillInput,
} from './GenerateDocument';

/**
 * The four request builders of the generation page (the page itself renders
 * forms and there is no @testing-library in the stack, so the load-bearing
 * pure functions are tested directly — same approach as
 * JoinFromInvite.test.ts).
 *
 * What these guard is the P3 forgery fix: a document's printed total and the
 * same total spelled out in words are BOTH derived server-side from one
 * integer number of tiyn. A builder that emits a money string again, or any
 * `*_words` key, is a 422 on every document — and, before the server started
 * refusing them, was a PDF that could state a figure its own XML did not.
 */

const seller: SellerDefaultsValues = {
  name: 'ТОО «Ромашка»',
  identifier: '123456789012',
  address: 'г. Алматы, ул. Абая 1',
  iik: 'KZ123',
  bik: 'ABCDKZKX',
  bank: 'АО «Банк»',
  kbe: '17',
  vat_certificate: '600900000001',
};

const buyer: PartyValues = {
  name: 'ТОО «Василёк»',
  identifier: '210987654321',
  address: 'г. Астана, ул. Кенесары 2',
  iik: '',
  bik: '',
  bank: '',
  kbe: '',
};

const invoiceValues: InvoiceFormValues = {
  number: '17',
  date: '14.08.2026',
  seller,
  buyerCounterpartyId: 'c0ffee00-0000-4000-8000-000000000001',
  contract: '',
  // 2 × 1500.55 = 3001.10 ₸ = 300110 тиын
  items: [{ name: 'Услуга', qty: '2', unit: 'шт', price: '1500.55' }],
  vat_rate: '',
};

/** Every key of `input`, including the leaves of nested objects. */
function deepKeys(value: unknown, prefix = ''): string[] {
  if (Array.isArray(value)) return value.flatMap((v, i) => deepKeys(v, `${prefix}[${i}]`));
  if (value === null || typeof value !== 'object') return [];
  return Object.entries(value as Record<string, unknown>).flatMap(([k, v]) => [
    prefix ? `${prefix}.${k}` : k,
    ...deepKeys(v, prefix ? `${prefix}.${k}` : k),
  ]);
}

describe('buildInvoiceInput', () => {
  it('sends the total as ONE integer of tiyn and no money strings of its own', () => {
    const input = buildInvoiceInput(invoiceValues, buyer);
    expect(input.total_tiyn).toBe(300110);
    expect(Number.isInteger(input.total_tiyn)).toBe(true);
    // The server formats `total` and spells out `total_words` — sending
    // either is a 422 not_allowed_override.
    expect(input).not.toHaveProperty('total');
    expect(input).not.toHaveProperty('total_words');
  });

  it('adds the VAT to the integer total instead of formatting a new one', () => {
    const input = buildInvoiceInput({ ...invoiceValues, vat_rate: '16%' }, buyer);
    // 300110 + round(300110 × 16 / 100) = 300110 + 48018
    expect(input.total_tiyn).toBe(348128);
    expect(input.vat_amount).toBe('480,18');
  });

  it('emits no *_words key anywhere in the input', () => {
    const input = buildInvoiceInput({ ...invoiceValues, vat_rate: '16' }, buyer);
    expect(deepKeys(input).filter((k) => k.endsWith('_words'))).toEqual([]);
  });
});

describe('buildAvrInput', () => {
  const avrValues: AvrFormValues = { ...invoiceValues, act_period: 'август 2026' };

  it('carries the invoice contract plus act_period, still without words', () => {
    const input = buildAvrInput(avrValues, buyer);
    expect(input.total_tiyn).toBe(300110);
    expect(input.act_period).toBe('август 2026');
    expect(deepKeys(input).filter((k) => k.endsWith('_words'))).toEqual([]);
    expect(input).not.toHaveProperty('total');
  });
});

describe('buildWaybillInput', () => {
  const waybillValues: WaybillFormValues = {
    number: '4',
    date: '14.08.2026',
    seller,
    buyerCounterpartyId: 'c0ffee00-0000-4000-8000-000000000001',
    basis: 'Договор №1',
    items: [
      { name: 'Товар А', qty: '3', unit: 'шт', price: '99.99' },
      { name: 'Товар Б', qty: '1', unit: 'шт', price: '0.01' },
    ],
    released_by: 'Смирнов С.С.',
    received_by: 'Иванова И.И.',
  };

  it('sends total_tiyn as the integer sum of the lines and no words', () => {
    const input = buildWaybillInput(waybillValues, buyer);
    expect(input.total_tiyn).toBe(29998); // 3 × 9999 + 1
    expect(input).not.toHaveProperty('total');
    expect(deepKeys(input).filter((k) => k.endsWith('_words'))).toEqual([]);
  });
});

describe('buildTaxInvoiceInput', () => {
  const taxInvoiceValues: TaxInvoiceFormValues = {
    number: '9',
    date: '14.08.2026',
    seller,
    buyerCounterpartyId: 'c0ffee00-0000-4000-8000-000000000001',
    buyerVatCertificate: '600900000002',
    items: [
      // The VAT of both lines rounds (1600.48 down, 1601.76 up), so a
      // total re-derived from the lines rather than from the two parts
      // could drift by a tiyn — which the server refuses.
      { name: 'Услуга А', qty: '1', unit: 'шт', price: '100.03', vat_rate: '16' },
      { name: 'Услуга Б', qty: '3', unit: 'шт', price: '33.37', vat_rate: '16' },
    ],
  };

  it('sends three integers whose parts sum EXACTLY to the total', () => {
    const totals = buildTaxInvoiceInput(taxInvoiceValues, buyer).totals as Record<string, number>;
    expect(totals.amount_tiyn).toBe(20014); // 10003 + 3 × 3337
    expect(totals.vat_tiyn).toBe(3202); // round(10003 × .16) + round(10011 × .16)
    // The server compares these for exact equality and answers a one-tiyn
    // disagreement with a 422 inconsistent_total.
    expect(totals.with_vat_tiyn).toBe(totals.amount_tiyn + totals.vat_tiyn);
    for (const v of Object.values(totals)) expect(Number.isInteger(v)).toBe(true);
  });

  it('sends no formatted total and no amount in words', () => {
    const input = buildTaxInvoiceInput(taxInvoiceValues, buyer);
    const totals = input.totals as Record<string, unknown>;
    expect(Object.keys(totals).sort()).toEqual(['amount_tiyn', 'vat_tiyn', 'with_vat_tiyn']);
    expect(deepKeys(input).filter((k) => k.endsWith('_words'))).toEqual([]);
  });
});
