import { describe, expect, it } from 'vitest';

import type { TaxCalculation } from '@/lib/api/types';
import {
  buildFno300DocumentInput,
  buildFno910DocumentInput,
  buildTaxCalculationCreate,
  buildTaxFilingCreate,
  calculationPeriod,
  calculationPeriodLabel,
  clampOrdinal,
  deadlineUrgency,
  filingKindFor,
  fno300DocumentSchema,
  fno910DocumentSchema,
  formatRateBpRu,
  snapshotTiyn,
  taxCalculationSchema,
  DEADLINE_WARNING_DAYS,
  type Fno300DocumentValues,
  type Fno910DocumentValues,
} from './tax';

describe('calculationPeriod', () => {
  it('expands a полугодие into its exact calendar bounds', () => {
    expect(calculationPeriod('snr_simplified', 2026, 1)).toEqual({
      period_from: '2026-01-01',
      period_to: '2026-06-30',
    });
    expect(calculationPeriod('snr_simplified', 2026, 2)).toEqual({
      period_from: '2026-07-01',
      period_to: '2026-12-31',
    });
  });

  it('expands each квартал into its exact calendar bounds', () => {
    expect(calculationPeriod('vat', 2026, 1)).toEqual({
      period_from: '2026-01-01',
      period_to: '2026-03-31',
    });
    expect(calculationPeriod('vat', 2026, 2)).toEqual({
      period_from: '2026-04-01',
      period_to: '2026-06-30',
    });
    expect(calculationPeriod('vat', 2026, 3)).toEqual({
      period_from: '2026-07-01',
      period_to: '2026-09-30',
    });
    expect(calculationPeriod('vat', 2026, 4)).toEqual({
      period_from: '2026-10-01',
      period_to: '2026-12-31',
    });
  });

  it('never emits a period_to before its period_from (migration 014 CHECKs it)', () => {
    for (const ordinal of [1, 2, 3, 4]) {
      const vat = calculationPeriod('vat', 2026, ordinal);
      expect(vat.period_to >= vat.period_from).toBe(true);
    }
    for (const ordinal of [1, 2]) {
      const snr = calculationPeriod('snr_simplified', 2026, ordinal);
      expect(snr.period_to >= snr.period_from).toBe(true);
    }
  });

  it('is pure string arithmetic — a leap year cannot shift a boundary', () => {
    // Every period this function produces ends in a 30/31-day month, so
    // February never appears as a period_to at all.
    expect(calculationPeriod('vat', 2024, 1).period_to).toBe('2024-03-31');
    expect(calculationPeriod('vat', 2025, 1).period_to).toBe('2025-03-31');
  });
});

describe('buildTaxCalculationCreate', () => {
  it('turns the year+ordinal form into the API contract dates', () => {
    expect(buildTaxCalculationCreate({ kind: 'vat', year: '2026', ordinal: '3' })).toEqual({
      kind: 'vat',
      period_from: '2026-07-01',
      period_to: '2026-09-30',
    });
  });

  it('clamps a stale ordinal so switching НДС→упрощёнка cannot file a "IV полугодие"', () => {
    expect(
      buildTaxCalculationCreate({ kind: 'snr_simplified', year: '2026', ordinal: '4' }),
    ).toEqual({
      kind: 'snr_simplified',
      period_from: '2026-07-01',
      period_to: '2026-12-31',
    });
  });
});

describe('clampOrdinal', () => {
  it('bounds the ordinal to the kind own period count', () => {
    expect(clampOrdinal('snr_simplified', 3)).toBe(2);
    expect(clampOrdinal('vat', 9)).toBe(4);
    expect(clampOrdinal('vat', 2)).toBe(2);
    expect(clampOrdinal('vat', 0)).toBe(1);
    expect(clampOrdinal('vat', Number.NaN)).toBe(1);
  });
});

describe('taxCalculationSchema', () => {
  it('rejects a non-4-digit year and an out-of-range ordinal', () => {
    expect(taxCalculationSchema.safeParse({ kind: 'vat', year: '26', ordinal: '1' }).success).toBe(
      false,
    );
    expect(
      taxCalculationSchema.safeParse({ kind: 'vat', year: '2026', ordinal: '5' }).success,
    ).toBe(false);
    expect(
      taxCalculationSchema.safeParse({ kind: 'vat', year: '2026', ordinal: '4' }).success,
    ).toBe(true);
  });
});

describe('calculationPeriodLabel', () => {
  it('names a полугодие for СНР and a квартал for НДС', () => {
    expect(calculationPeriodLabel('snr_simplified', 1)).toBe('I полугодие');
    expect(calculationPeriodLabel('snr_simplified', 2)).toBe('II полугодие');
    expect(calculationPeriodLabel('vat', 3)).toBe('III квартал');
    expect(calculationPeriodLabel('vat', 4)).toBe('IV квартал');
  });
});

describe('filingKindFor / buildTaxFilingCreate', () => {
  const calc = (kind: TaxCalculation['kind']): Pick<TaxCalculation, 'id' | 'kind'> => ({
    id: 'c0ffee00-0000-4000-8000-000000000001',
    kind,
  });

  it('files an упрощёнка calculation as 910.00 and a НДС one as 300.00', () => {
    expect(filingKindFor('snr_simplified')).toBe('910.00');
    expect(filingKindFor('vat')).toBe('300.00');
  });

  it('DERIVES kind from the calculation, so a kind_mismatch 422 is unreachable', () => {
    expect(buildTaxFilingCreate(calc('vat'), { director: 'Смирнов С.С.' })).toEqual({
      kind: '300.00',
      calculation_id: 'c0ffee00-0000-4000-8000-000000000001',
      document_input: { director: 'Смирнов С.С.' },
    });
    expect(buildTaxFilingCreate(calc('snr_simplified'), {}).kind).toBe('910.00');
  });
});

describe('buildFno910DocumentInput', () => {
  it('sends exactly the two fields the template cannot derive, trimmed', () => {
    const input = buildFno910DocumentInput({
      director: '  Смирнов С.С. ',
      accountant: ' Иванова И.И.  ',
    });
    expect(input).toEqual({ director: 'Смирнов С.С.', accountant: 'Иванова И.И.' });
    // Echoing a server-derived value back is a 422 under the strict schema
    // check — the allowlist must never grow one by accident.
    expect(Object.keys(input).sort()).toEqual(['accountant', 'director']);
  });

  it('NEVER sends the tax amount in words — the server spells it out', () => {
    // The P3 fix: the printed sum used to be whatever the client typed,
    // which let a declaration state one figure in its PDF and another in
    // the XML of the same filing. `tax_words` is now a 422
    // not_allowed_override, so not one key of that kind may leak through.
    const input = buildFno910DocumentInput({
      director: 'Смирнов С.С.',
      accountant: 'Иванова И.И.',
      tax_words: 'один тенге 00 тиын',
    } as Fno910DocumentValues);
    expect(Object.keys(input).some((k) => k.endsWith('_words'))).toBe(false);
  });
});

describe('buildFno300DocumentInput', () => {
  it('sends the two fields fno_300 allows, trimmed, and no derived ones', () => {
    const input = buildFno300DocumentInput({
      director: 'Смирнов С.С.',
      accountant: ' Иванова И.И. ',
    });
    expect(input).toEqual({ director: 'Смирнов С.С.', accountant: 'Иванова И.И.' });
    expect(Object.keys(input).sort()).toEqual(['accountant', 'director']);
  });

  it('NEVER sends the balance in words — the server spells it out', () => {
    const input = buildFno300DocumentInput({
      director: 'Смирнов С.С.',
      accountant: 'Иванова И.И.',
      balance_words: 'один тенге 00 тиын',
    } as Fno300DocumentValues);
    expect(Object.keys(input).some((k) => k.endsWith('_words'))).toBe(false);
  });

  it('NEVER sends sales_tenge — the turnover is derived from the ledger server-side', () => {
    // The regression this guards: `sales_tenge` was client-supplied until
    // the server started snapshotting the turnover
    // (result_snapshot.income_tiyn) and deriving the field from it. Sending
    // it now is a 422 not_allowed_override, and — worse — it was a hole
    // that let an accountant declare any revenue turnover they liked on a
    // legal tax filing. A stray extra key here breaks every 300.00 filing.
    const input = buildFno300DocumentInput({
      director: 'Смирнов С.С.',
      accountant: 'Иванова И.И.',
      // A caller that still holds a turnover value must not smuggle it
      // through the builder.
      sales_tenge: '9999999.00',
    } as Fno300DocumentValues);
    expect(input).not.toHaveProperty('sales_tenge');
  });
});

describe('the ФНО document schemas', () => {
  it('require both signatories', () => {
    expect(fno910DocumentSchema.safeParse({ director: '', accountant: 'И.И.' }).success).toBe(
      false,
    );
    expect(fno910DocumentSchema.safeParse({ director: 'С.С.', accountant: ' ' }).success).toBe(
      false,
    );
    expect(fno300DocumentSchema.safeParse({ director: 'С.С.', accountant: '' }).success).toBe(
      false,
    );
    expect(fno300DocumentSchema.safeParse({ director: '  ', accountant: 'И.И.' }).success).toBe(
      false,
    );
  });

  it('declare EXACTLY the fields the server allowlists, and no others', () => {
    // Docgen::InputPolicy::editable_fields("fno_910"/"fno_300") — the
    // server rejects any other key with a 422 not_allowed_override, so
    // these two shapes must not drift.
    expect(Object.keys(fno910DocumentSchema.shape).sort()).toEqual(['accountant', 'director']);
    expect(Object.keys(fno300DocumentSchema.shape).sort()).toEqual(['accountant', 'director']);
  });
});

describe('formatRateBpRu', () => {
  it('renders whole percents without a decimal tail', () => {
    expect(formatRateBpRu(400)).toBe('4 %');
    expect(formatRateBpRu(1600)).toBe('16 %');
    expect(formatRateBpRu(0)).toBe('0 %');
  });

  it('renders fractional percents with a comma, trimming one trailing zero', () => {
    expect(formatRateBpRu(350)).toBe('3,5 %');
    expect(formatRateBpRu(1625)).toBe('16,25 %');
    expect(formatRateBpRu(5)).toBe('0,05 %');
  });

  it('never produces a float artefact — 0.30000000000000004 and friends', () => {
    expect(formatRateBpRu(30)).toBe('0,3 %');
    expect(formatRateBpRu(1010)).toBe('10,1 %');
  });
});

describe('snapshotTiyn', () => {
  it('reads an integer figure out of a free-form snapshot', () => {
    expect(snapshotTiyn({ income_tiyn: 123_456 }, 'income_tiyn')).toBe(123_456);
    expect(snapshotTiyn({ balance_tiyn: -500 }, 'balance_tiyn')).toBe(-500);
  });

  it('returns null — never 0 — for a key the kind does not record', () => {
    // The СНР snapshot deliberately omits ipn_part_tiyn rather than
    // fabricating it, so the UI must say «—», not «0,00 ₸».
    expect(snapshotTiyn({ income_tiyn: 1 }, 'ipn_part_tiyn')).toBeNull();
    expect(snapshotTiyn(undefined, 'income_tiyn')).toBeNull();
    expect(snapshotTiyn({ income_tiyn: '123' }, 'income_tiyn')).toBeNull();
    expect(snapshotTiyn({ income_tiyn: null }, 'income_tiyn')).toBeNull();
  });
});

describe('deadlineUrgency', () => {
  it('treats today and anything past it as overdue', () => {
    expect(deadlineUrgency(0)).toBe('overdue');
    expect(deadlineUrgency(-3)).toBe('overdue');
  });

  it('highlights everything inside the warning window', () => {
    expect(deadlineUrgency(1)).toBe('urgent');
    expect(deadlineUrgency(DEADLINE_WARNING_DAYS)).toBe('urgent');
  });

  it('leaves anything past the window unhighlighted', () => {
    expect(deadlineUrgency(DEADLINE_WARNING_DAYS + 1)).toBe('normal');
    expect(deadlineUrgency(90)).toBe('normal');
  });
});
