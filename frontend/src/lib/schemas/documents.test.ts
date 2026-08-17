import { describe, expect, it } from 'vitest';

import type { Document } from '@/lib/api/types';
import { formatTiynRu } from '@/lib/money';
import {
  documentActionAvailability,
  invoiceFormSchema,
  isAwaitingVersionRender,
  reconciliationFormSchema,
  ruMoneyToDecimal,
  snapshotToInvoiceValues,
  snapshotToLaborContractValues,
  snapshotToReconciliationValues,
  snapshotToTaxInvoiceValues,
  snapshotToWaybillValues,
  taxInvoiceFormSchema,
  vatRateFromSnapshot,
  voidDocumentSchema,
  STATUS_POLL_STATUS,
  waybillFormSchema,
} from './documents';

/**
 * The lifecycle half of the documents page: what may be done to a document,
 * and how a stored render input turns back into the form that produced it.
 *
 * Both are pure and both are load-bearing. `documentActionAvailability` is
 * the ONLY place that decides whether the page offers «Удалить» or the
 * honest «Аннулировать» instead, and the snapshot mappers are the inverse of
 * the money/percent formatting the builders apply on the way out — a
 * round-trip that silently loses a price would resubmit a different document
 * under the same number.
 */

/** Minimal Document row — only the three fields the rule actually reads. */
function doc(
  over: Partial<Document> = {},
): Pick<Document, 'source' | 'template_slug' | 'voided_at'> {
  return { source: 'generated', template_slug: 'invoice', voided_at: null, ...over };
}

describe('voidDocumentSchema', () => {
  it('rejects a blank reason', () => {
    expect(voidDocumentSchema.safeParse({ reason: '   ' }).success).toBe(false);
    expect(voidDocumentSchema.safeParse({ reason: '' }).success).toBe(false);
  });
  it('trims the reason', () => {
    const parsed = voidDocumentSchema.parse({ reason: '  ошибка в реквизитах  ' });
    expect(parsed.reason).toBe('ошибка в реквизитах');
  });
});

describe('documentActionAvailability', () => {
  it('offers all three actions on a plain generated document', () => {
    const a = documentActionAvailability(doc());
    expect([a.canEdit, a.canDelete, a.canVoid]).toEqual([true, true, true]);
    expect(a.deleteBlockReason).toBeNull();
  });

  it('offers voiding instead of deletion once the server reported a posted entry', () => {
    const a = documentActionAvailability(doc(), 'document_has_posted_entries');
    expect(a.canDelete).toBe(false);
    expect(a.canVoid).toBe(true);
    expect(a.deleteBlockReason).toBe(
      'Документ связан с проведённой проводкой — его можно только аннулировать.',
    );
  });

  it('explains a reference from an HR order or a filing', () => {
    const a = documentActionAvailability(doc({ doc_type: 'hr' }), 'document_referenced');
    expect(a.canDelete).toBe(false);
    expect(a.deleteBlockReason).toContain('кадровый приказ');
  });

  it('locks a voided document out of all three actions', () => {
    const a = documentActionAvailability(doc({ voided_at: '2026-08-14T10:00:00Z' }));
    expect([a.canEdit, a.canDelete, a.canVoid]).toEqual([false, false, false]);
    expect(a.editBlockReason).not.toBeNull();
    expect(a.deleteBlockReason).not.toBeNull();
    expect(a.voidBlockReason).not.toBeNull();
  });

  it('refuses to edit an uploaded or emailed document but still allows both other actions', () => {
    for (const source of ['uploaded', 'email'] as const) {
      const a = documentActionAvailability(doc({ source, template_slug: null }));
      expect(a.canEdit).toBe(false);
      expect(a.editBlockReason).toBe('Загруженные и присланные почтой документы не редактируются.');
      expect(a.canDelete).toBe(true);
      expect(a.canVoid).toBe(true);
    }
  });

  it('has no edit form for a payslip', () => {
    const a = documentActionAvailability(doc({ template_slug: 'payslip' }));
    expect(a.canEdit).toBe(false);
    expect(a.editBlockReason).toContain('расчётного листка');
  });

  it('edits every server-built form that has an allowlist', () => {
    for (const slug of ['fno_910', 'fno_300', 'hr_order', 'labor_contract', 'reconciliation']) {
      expect(documentActionAvailability(doc({ template_slug: slug })).canEdit).toBe(true);
    }
  });

  it('does not offer an edit form for an unknown template', () => {
    expect(documentActionAvailability(doc({ template_slug: 'sputnik_v1' })).canEdit).toBe(false);
  });
});

/**
 * Every status the API can report (Document.status enum, docs/openapi.yaml).
 * Spelled out here rather than imported from the page so that ADDING a status
 * server-side and forgetting it in the exclusion rule shows up as a failure
 * the moment this list is updated.
 */
const ALL_STATUSES: Document['status'][] = [
  'inbox',
  'recognized',
  'linked',
  'archived',
  'draft',
  'final',
  'sent',
];

function renderDoc(
  over: Partial<Document> = {},
): Pick<Document, 'latest_version_no' | 'status' | 'voided_at'> {
  return { latest_version_no: 2, status: 'final', voided_at: null, ...over };
}

describe('isAwaitingVersionRender', () => {
  it('waits while the newest version has not become current yet', () => {
    expect(isAwaitingVersionRender(renderDoc({ latest_version_no: 2 }), 1)).toBe(true);
    expect(isAwaitingVersionRender(renderDoc({ latest_version_no: 5 }), 2)).toBe(true);
  });

  it('stops as soon as the render published the version', () => {
    expect(isAwaitingVersionRender(renderDoc({ latest_version_no: 2 }), 2)).toBe(false);
  });

  it('does not start before the history is known', () => {
    // currentVersionNo === null: nothing to compare against, so polling on it
    // would be an unbounded wait for a number we have not read yet.
    expect(isAwaitingVersionRender(renderDoc({ latest_version_no: 9 }), null)).toBe(false);
  });

  it('never waits on a voided document — it is never re-rendered', () => {
    expect(
      isAwaitingVersionRender(
        renderDoc({ latest_version_no: 3, voided_at: '2026-08-14T10:00:00Z' }),
        1,
      ),
    ).toBe(false);
  });

  /**
   * The load-bearing one. Two independent polls run on this page:
   * useDocumentRender waits on `status === 'draft'` (the FIRST render), and
   * this predicate waits on the current-version pointer (an EDIT). They must
   * never be true at once, or one document gets refetched twice as often
   * under two unrelated timeouts. The comment used to assert this "by
   * construction"; this pins it.
   */
  it('never overlaps the status poll', () => {
    for (const status of ALL_STATUSES) {
      for (const latest of [0, 1, 2, 7]) {
        for (const current of [null, 0, 1, 2]) {
          const document = renderDoc({ status, latest_version_no: latest });
          const versionPoll = isAwaitingVersionRender(document, current);
          const statusPoll = status === STATUS_POLL_STATUS;
          expect(
            versionPoll && statusPoll,
            `status=${status} latest=${latest} current=${String(current)}`,
          ).toBe(false);
        }
      }
    }
    // …and the exclusion is real, not vacuous: the same version gap that is
    // ignored on 'draft' does start the poll on every other status.
    expect(isAwaitingVersionRender(renderDoc({ status: 'draft', latest_version_no: 2 }), 1)).toBe(
      false,
    );
    for (const status of ALL_STATUSES.filter((s) => s !== STATUS_POLL_STATUS)) {
      expect(isAwaitingVersionRender(renderDoc({ status, latest_version_no: 2 }), 1)).toBe(true);
    }
  });
});

describe('ruMoneyToDecimal', () => {
  it('inverts formatTiynRu for a grouped amount', () => {
    expect(ruMoneyToDecimal(formatTiynRu(123456789))).toBe('1234567.89');
    expect(ruMoneyToDecimal('1 234,56')).toBe('1234.56');
    expect(ruMoneyToDecimal('0,05')).toBe('0.05');
  });
  it('accepts a non-breaking grouping space', () => {
    expect(ruMoneyToDecimal('1\u00a0234,56')).toBe('1234.56');
    expect(ruMoneyToDecimal('1\u202f234,56')).toBe('1234.56');
  });
  it('yields an empty field rather than a fake zero for anything unparsable', () => {
    expect(ruMoneyToDecimal('')).toBe('');
    expect(ruMoneyToDecimal('—')).toBe('');
    expect(ruMoneyToDecimal(undefined)).toBe('');
    expect(ruMoneyToDecimal(1234)).toBe('');
  });
});

describe('vatRateFromSnapshot', () => {
  it('strips the percent sign the builder added', () => {
    expect(vatRateFromSnapshot('16%')).toBe('16');
    expect(vatRateFromSnapshot('12')).toBe('12');
    expect(vatRateFromSnapshot(undefined)).toBe('');
  });
});

describe('snapshotToInvoiceValues', () => {
  const snapshot = {
    number: 'СЧ-42',
    date: '14.08.2026',
    seller: {
      name: 'ТОО «Ромашка»',
      identifier: '123456789012',
      address: 'г. Алматы',
      iik: 'KZ123',
      bik: 'ABCDKZKX',
      bank: 'АО «Банк»',
      kbe: '17',
    },
    buyer: { name: 'ТОО «Василёк»', identifier: '210987654321' },
    contract: 'Договор № 7',
    items: [{ name: 'Услуга', qty: '2', unit: 'усл.', price: '1 000,50', amount: '2 001,00' }],
    vat_rate: '16%',
    // Server-derived — must never come back into the form.
    total: '2 321,16',
    total_words: 'две тысячи триста двадцать один тенге 16 тиын',
  };

  it('round-trips into a form the schema accepts', () => {
    const values = snapshotToInvoiceValues(snapshot, 'cp-1');
    expect(invoiceFormSchema.safeParse(values).success).toBe(true);
    expect(values.number).toBe('СЧ-42');
    expect(values.buyerCounterpartyId).toBe('cp-1');
    expect(values.contract).toBe('Договор № 7');
    expect(values.vat_rate).toBe('16');
    expect(values.items).toEqual([{ name: 'Услуга', qty: '2', unit: 'усл.', price: '1000.50' }]);
    // Продавца в форме больше нет: его пишет сервер из реквизитов
    // организации, и клиент не может его ни задать, ни прислать.
    expect('seller' in values).toBe(false);
  });

  it('never carries a server-derived total back into the form', () => {
    const values = snapshotToInvoiceValues(snapshot, 'cp-1');
    const serialised = JSON.stringify(values);
    expect(serialised).not.toContain('total_words');
    expect(serialised).not.toContain('2 321,16');
    // `seller` в этом списке больше нет: реквизиты продавца берутся из
    // карточки организации, и форма их не хранит (см. SellerNotice).
    expect(Object.keys(values).sort()).toEqual([
      'buyerCounterpartyId',
      'contract',
      'date',
      'items',
      'number',
      'vat_rate',
    ]);
  });

  it('survives a missing counterparty and an empty snapshot', () => {
    const values = snapshotToInvoiceValues(null, null);
    expect(values.buyerCounterpartyId).toBe('');
    expect(values.items).toHaveLength(1);
    expect(values.items[0].price).toBe('');
  });
});

describe('snapshotToWaybillValues', () => {
  it('keeps the two signatory fields and the basis', () => {
    const values = snapshotToWaybillValues(
      {
        number: 'Н-1',
        date: '01.08.2026',
        seller: { name: 'ТОО «Ромашка»', identifier: '123456789012' },
        basis: 'Требование № 5',
        items: [{ name: 'Товар', qty: '3', unit: 'шт.', price: '500,00' }],
        released_by: 'Иванов И.',
        received_by: 'Петров П.',
        total: '1 500,00',
      },
      'cp-2',
    );
    expect(waybillFormSchema.safeParse(values).success).toBe(true);
    expect(values.released_by).toBe('Иванов И.');
    expect(values.items[0].price).toBe('500.00');
  });
});

describe('snapshotToTaxInvoiceValues', () => {
  it('reads the per-item VAT rate and the buyer certificate', () => {
    const values = snapshotToTaxInvoiceValues(
      {
        number: 'СФ-9',
        date: '02.08.2026',
        seller: { name: 'ТОО «Ромашка»', identifier: '123456789012', vat_certificate: '600900' },
        buyer: { name: 'ТОО «Василёк»', identifier: '210987654321', vat_certificate: '600901' },
        items: [
          { name: 'Услуга', qty: '1', unit: 'усл.', price: '10 000,00', vat_rate: '16%' },
          { name: 'Товар', qty: '2', unit: 'шт.', price: '250,00', vat_rate: '12%' },
        ],
        totals: { amount: '10 500,00', vat: '1 630,00', with_vat: '12 130,00' },
      },
      'cp-3',
    );
    expect(taxInvoiceFormSchema.safeParse(values).success).toBe(true);
    // Свидетельство ПРОДАВЦА по НДС теперь берётся из карточки
    // организации и в форму не попадает; свидетельство ПОКУПАТЕЛЯ —
    // по-прежнему поле формы, это реквизит контрагента.
    expect('seller' in values).toBe(false);
    expect(values.buyerVatCertificate).toBe('600901');
    expect(values.items.map((i) => i.vat_rate)).toEqual(['16', '12']);
    expect(values.items[0].price).toBe('10000.00');
    // `totals` is entirely server-derived and has no form field at all.
    expect(JSON.stringify(values)).not.toContain('12 130,00');
  });
});

describe('snapshotToReconciliationValues', () => {
  it('splits the opening balance and the closing statements back out', () => {
    const values = snapshotToReconciliationValues(
      {
        period_from: '01.07.2026',
        period_to: '31.07.2026',
        party_a: { name: 'ТОО «Ромашка»', identifier: '123456789012' },
        party_b: { name: 'ТОО «Василёк»', identifier: '210987654321' },
        opening_balance: { a_debit: '1 000,00', a_credit: '' },
        rows: [{ date: '05.07.2026', doc: 'Счёт № 1', a_debit: '2 000,00', a_credit: '' }],
        closing: { a_says: 'долг 3 000', b_says: 'долг 3 000' },
      },
      'cp-4',
    );
    expect(reconciliationFormSchema.safeParse(values).success).toBe(true);
    expect(values.openingADebit).toBe('1000.00');
    expect(values.openingACredit).toBe('');
    expect(values.rows[0].a_debit).toBe('2000.00');
    expect(values.rows[0].b_credit).toBe('');
    expect(values.aSays).toBe('долг 3 000');
    expect(values.counterpartyId).toBe('cp-4');
  });
});

describe('snapshotToLaborContractValues', () => {
  it('reads the two nested allowlisted paths, not their authoritative siblings', () => {
    const values = snapshotToLaborContractValues({
      work_schedule: '5/2, 40 часов',
      probation_months: 3,
      employer: {
        name: 'ТОО «Ромашка»',
        bin: '123456789012',
        director: 'Иванов И.И.',
        address: 'Алматы',
      },
      employee: { full_name: 'Петров П.П.', iin: '900101300123', address: 'Астана' },
      salary: '300 000,00',
      salary_words: 'триста тысяч тенге 00 тиын',
    });
    expect(values).toEqual({
      director: 'Иванов И.И.',
      work_schedule: '5/2, 40 часов',
      probation_months: '3',
      employer_address: 'Алматы',
      employee_address: 'Астана',
    });
  });
});
