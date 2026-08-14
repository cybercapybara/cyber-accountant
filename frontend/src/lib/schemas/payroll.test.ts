import { describe, expect, it } from 'vitest';

import type { PayrollRun, Payslip } from '@/lib/api/types';
import {
  buildPayrollRunCreate,
  buildPayslipDocumentExtra,
  payrollPeriodLabel,
  payrollPeriodSchema,
  payrollRunActions,
  payrollRunStage,
  PAYSLIP_AMOUNT_FIELDS,
  sumPayslips,
} from './payroll';

/** A run header with only the two columns the lifecycle depends on. */
function run(
  status: PayrollRun['status'],
  journalEntryId: string | null,
): Pick<PayrollRun, 'status' | 'journal_entry_id'> {
  return { status, journal_entry_id: journalEntryId };
}

describe('buildPayrollRunCreate', () => {
  it('sends year and month as INTEGERS — the API answers 400 for a string', () => {
    const body = buildPayrollRunCreate({ year: '2026', month: '8' });
    expect(body).toEqual({ year: 2026, month: 8 });
    expect(typeof body.year).toBe('number');
    expect(typeof body.month).toBe('number');
  });

  it('trims before parsing so a padded field is not NaN', () => {
    expect(buildPayrollRunCreate({ year: ' 2026 ', month: ' 12 ' })).toEqual({
      year: 2026,
      month: 12,
    });
  });
});

describe('payrollPeriodSchema', () => {
  it('rejects a year outside the controller-supported 2000..2100 window', () => {
    expect(payrollPeriodSchema.safeParse({ year: '1999', month: '1' }).success).toBe(false);
    expect(payrollPeriodSchema.safeParse({ year: '2101', month: '1' }).success).toBe(false);
    expect(payrollPeriodSchema.safeParse({ year: '2000', month: '1' }).success).toBe(true);
    expect(payrollPeriodSchema.safeParse({ year: '2100', month: '1' }).success).toBe(true);
  });

  it('rejects a month outside 1..12', () => {
    expect(payrollPeriodSchema.safeParse({ year: '2026', month: '0' }).success).toBe(false);
    expect(payrollPeriodSchema.safeParse({ year: '2026', month: '13' }).success).toBe(false);
    expect(payrollPeriodSchema.safeParse({ year: '2026', month: '12' }).success).toBe(true);
  });
});

describe('payrollRunStage', () => {
  it('reads a draft run as draft', () => {
    expect(payrollRunStage(run('draft', null))).toBe('draft');
  });

  it('reads an approved-but-unposted run as approved', () => {
    expect(payrollRunStage(run('approved', null))).toBe('approved');
  });

  it('reads an approved run WITH a journal entry as posted — status stays "approved" forever', () => {
    // The regression this guards: reading `status` alone would call a
    // posted run "approved" and offer «Провести в учёт» again, which the
    // backend answers with a 409 rather than a second journal entry.
    expect(payrollRunStage(run('approved', 'e1b9c0de-0000-4000-8000-000000000001'))).toBe('posted');
  });
});

describe('payrollRunActions', () => {
  it('offers recalculate and approve — and NOT post — on a draft', () => {
    expect(payrollRunActions(run('draft', null))).toEqual({
      canRecalculate: true,
      canApprove: true,
      canPost: false,
    });
  });

  it('offers only post on an approved run — recalculating it is a 409', () => {
    expect(payrollRunActions(run('approved', null))).toEqual({
      canRecalculate: false,
      canApprove: false,
      canPost: true,
    });
  });

  it('offers NOTHING once the run is posted', () => {
    const actions = payrollRunActions(run('approved', 'e1b9c0de-0000-4000-8000-000000000001'));
    expect(actions.canPost).toBe(false);
    expect(actions.canApprove).toBe(false);
    expect(actions.canRecalculate).toBe(false);
  });
});

describe('sumPayslips', () => {
  const slip = (overrides: Partial<Payslip>): Payslip =>
    ({
      id: 'p1',
      org_id: 'o1',
      run_id: 'r1',
      employee_id: 'e1',
      gross_tiyn: 0,
      opv: 0,
      vosms: 0,
      ipn: 0,
      net: 0,
      opvr: 0,
      so: 0,
      osms: 0,
      social_tax: 0,
      created_at: '',
      updated_at: '',
      ...overrides,
    }) as Payslip;

  it('sums every money column independently', () => {
    const total = sumPayslips([
      slip({ gross_tiyn: 30_000_000, opv: 3_000_000, ipn: 2_430_000, net: 24_070_000 }),
      slip({ gross_tiyn: 15_000_050, opv: 1_500_005, ipn: 1_215_000, net: 12_035_045 }),
    ]);
    expect(total.gross_tiyn).toBe(45_000_050);
    expect(total.opv).toBe(4_500_005);
    expect(total.ipn).toBe(3_645_000);
    expect(total.net).toBe(36_105_045);
  });

  it('covers all nine columns and nothing else — a new column must be added deliberately', () => {
    const total = sumPayslips([slip({ social_tax: 7, osms: 5, so: 3, opvr: 2, vosms: 1 })]);
    expect(Object.keys(total).sort()).toEqual([...PAYSLIP_AMOUNT_FIELDS].sort());
    expect(total.social_tax).toBe(7);
    expect(total.osms).toBe(5);
    expect(total.so).toBe(3);
    expect(total.opvr).toBe(2);
    expect(total.vosms).toBe(1);
  });

  it('stays an exact integer count of tiyn — no float rounding creeps in', () => {
    const cents = Array.from({ length: 10 }, () => slip({ net: 1 }));
    expect(sumPayslips(cents).net).toBe(10);
    expect(Number.isInteger(sumPayslips(cents).net)).toBe(true);
  });

  it('returns all zeros for an empty run rather than an empty object', () => {
    const total = sumPayslips([]);
    for (const field of PAYSLIP_AMOUNT_FIELDS) expect(total[field]).toBe(0);
  });
});

describe('payrollPeriodLabel', () => {
  it('names the month in Russian', () => {
    expect(payrollPeriodLabel(2026, 8)).toBe('Август 2026');
    expect(payrollPeriodLabel(2026, 1)).toBe('Январь 2026');
    expect(payrollPeriodLabel(2026, 12)).toBe('Декабрь 2026');
  });

  it('falls back to a numeric period rather than "undefined 2026"', () => {
    expect(payrollPeriodLabel(2026, 13)).toBe('13.2026');
  });
});

describe('buildPayslipDocumentExtra', () => {
  it('sends net_words and NOTHING else — every other field is derived server-side', () => {
    const extra = buildPayslipDocumentExtra({ net_words: '  двести тысяч тенге 00 тиын  ' });
    expect(extra).toEqual({ net_words: 'двести тысяч тенге 00 тиын' });
    expect(Object.keys(extra)).toEqual(['net_words']);
  });
});
