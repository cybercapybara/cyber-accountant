import { describe, expect, it } from 'vitest';

import type { Employee } from '@/lib/api/types';
import {
  buildEmployeeCreate,
  buildEmployeeUpdate,
  buildHrOrderCreate,
  buildHrOrderDocumentExtra,
  buildHrOrderPayload,
  buildLaborContractCreate,
  buildLaborContractDocumentExtra,
  buildVacationCreate,
  employeeCreateSchema,
  employeeFullName,
  vacationSchema,
  type EmployeeCreateValues,
} from './hr';

const EMPLOYEE_VALUES: EmployeeCreateValues = {
  iin: '900101300123',
  last_name: '  Иванов ',
  first_name: 'Иван',
  middle_name: '  ',
  position: 'Бухгалтер',
  salary: '300000.00',
  hired_on: '2026-01-15',
  ipn_deduction_claimed: true,
  opvr_exempt: false,
  payout_iik: 'KZ123456789012345678',
};

describe('buildEmployeeUpdate', () => {
  it('never sends hired_on, status or dismissed_on — the three fields PATCH rejects with 422', () => {
    const body = buildEmployeeUpdate(EMPLOYEE_VALUES) as Record<string, unknown>;
    expect(body).not.toHaveProperty('hired_on');
    expect(body).not.toHaveProperty('status');
    expect(body).not.toHaveProperty('dismissed_on');
  });

  it('sends exactly the editable allowlist, trimmed', () => {
    const body = buildEmployeeUpdate(EMPLOYEE_VALUES);
    expect(body).toEqual({
      iin: '900101300123',
      last_name: 'Иванов',
      first_name: 'Иван',
      position: 'Бухгалтер',
      salary: '300000.00',
      ipn_deduction_claimed: true,
      opvr_exempt: false,
      payout_iik: 'KZ123456789012345678',
    });
  });

  it('omits a blank middle name instead of blanking the stored one', () => {
    expect(buildEmployeeUpdate(EMPLOYEE_VALUES)).not.toHaveProperty('middle_name');
    expect(buildEmployeeUpdate({ ...EMPLOYEE_VALUES, middle_name: ' Петрович ' })).toMatchObject({
      middle_name: 'Петрович',
    });
  });
});

describe('buildEmployeeCreate', () => {
  it('adds hired_on on top of the editable allowlist', () => {
    expect(buildEmployeeCreate(EMPLOYEE_VALUES)).toMatchObject({
      iin: '900101300123',
      hired_on: '2026-01-15',
    });
  });
});

describe('employeeCreateSchema', () => {
  it('rejects an ИИН that is not exactly 12 digits', () => {
    expect(employeeCreateSchema.safeParse({ ...EMPLOYEE_VALUES, iin: '12345' }).success).toBe(
      false,
    );
  });

  it('rejects a salary with more than two decimal places, and a zero salary', () => {
    expect(employeeCreateSchema.safeParse({ ...EMPLOYEE_VALUES, salary: '100.005' }).success).toBe(
      false,
    );
    expect(employeeCreateSchema.safeParse({ ...EMPLOYEE_VALUES, salary: '0.00' }).success).toBe(
      false,
    );
  });

  it('rejects a hire date that is not ГГГГ-ММ-ДД', () => {
    expect(
      employeeCreateSchema.safeParse({ ...EMPLOYEE_VALUES, hired_on: '15.01.2026' }).success,
    ).toBe(false);
  });
});

describe('employeeFullName', () => {
  const base: Pick<Employee, 'last_name' | 'first_name' | 'middle_name'> = {
    last_name: 'Иванов',
    first_name: 'Иван',
    middle_name: null,
  };

  it('joins surname, name and patronymic', () => {
    expect(employeeFullName({ ...base, middle_name: 'Петрович' })).toBe('Иванов Иван Петрович');
  });

  it('collapses a null or blank patronymic', () => {
    expect(employeeFullName({ ...base, middle_name: null })).toBe('Иванов Иван');
    expect(employeeFullName({ ...base, middle_name: '   ' })).toBe('Иванов Иван');
  });
});

describe('buildHrOrderPayload', () => {
  it('keeps only the current kind’s fields, so switching kind cannot leak the previous answers', () => {
    const typed = { destination: 'г. Астана', reason: 'Соглашение сторон' };
    expect(buildHrOrderPayload('business_trip', typed)).toEqual({ destination: 'г. Астана' });
    expect(buildHrOrderPayload('dismiss', typed)).toEqual({ reason: 'Соглашение сторон' });
  });

  it('drops blank fields and returns undefined when nothing is filled in', () => {
    expect(buildHrOrderPayload('hire', { position: '  ', probation_months: '' })).toBeUndefined();
    expect(buildHrOrderPayload('hire', { position: ' Бухгалтер ' })).toEqual({
      position: 'Бухгалтер',
    });
  });
});

describe('buildHrOrderCreate', () => {
  const values = {
    employee_id: '11111111-1111-1111-1111-111111111111',
    kind: 'vacation' as const,
    number: ' 12-К ',
    issued_on: '2026-06-01',
    effective_from: '2026-07-01',
    effective_to: '',
    payload: {},
  };

  it('omits effective_to and payload when they are empty', () => {
    const body = buildHrOrderCreate(values) as Record<string, unknown>;
    expect(body).not.toHaveProperty('effective_to');
    expect(body).not.toHaveProperty('payload');
    expect(body.number).toBe('12-К');
  });

  it('includes effective_to and the kind-scoped payload when filled in', () => {
    expect(
      buildHrOrderCreate({
        ...values,
        effective_to: '2026-07-24',
        payload: { days: '24', destination: 'г. Астана' },
      }),
    ).toMatchObject({ effective_to: '2026-07-24', payload: { days: '24' } });
  });
});

describe('buildLaborContractCreate', () => {
  const values = {
    employee_id: '11111111-1111-1111-1111-111111111111',
    number: 'ТД-7',
    signed_on: '2026-01-10',
    starts_on: '2026-01-15',
    ends_on: '',
  };

  it('omits ends_on for an open-ended contract', () => {
    expect(buildLaborContractCreate(values)).not.toHaveProperty('ends_on');
  });

  it('sends ends_on for a fixed-term contract', () => {
    expect(buildLaborContractCreate({ ...values, ends_on: '2027-01-14' })).toMatchObject({
      ends_on: '2027-01-14',
    });
  });
});

describe('vacationSchema / buildVacationCreate', () => {
  const values = {
    employee_id: '11111111-1111-1111-1111-111111111111',
    kind: 'annual' as const,
    starts_on: '2026-07-01',
    ends_on: '2026-07-24',
    days: '24',
  };

  it('parses days into an integer', () => {
    expect(buildVacationCreate(values).days).toBe(24);
  });

  it('rejects an end date before the start date', () => {
    const parsed = vacationSchema.safeParse({ ...values, ends_on: '2026-06-30' });
    expect(parsed.success).toBe(false);
  });

  it('accepts a single-day vacation (ends_on === starts_on)', () => {
    expect(
      vacationSchema.safeParse({ ...values, ends_on: values.starts_on, days: '1' }).success,
    ).toBe(true);
  });

  it('rejects a non-positive or non-integer day count', () => {
    expect(vacationSchema.safeParse({ ...values, days: '0' }).success).toBe(false);
    expect(vacationSchema.safeParse({ ...values, days: '2.5' }).success).toBe(false);
  });
});

describe('buildHrOrderDocumentExtra', () => {
  it('sends only the free-text fields the hr_order template needs', () => {
    expect(
      buildHrOrderDocumentExtra({ director: ' Смирнов С.С. ', reason: '', details: '' }),
    ).toEqual({ director: 'Смирнов С.С.' });
  });

  it('never echoes back a server-derived value', () => {
    const extra = buildHrOrderDocumentExtra({
      director: 'Смирнов С.С.',
      reason: 'Заявление',
      details: 'Полный текст',
    });
    expect(Object.keys(extra).sort()).toEqual(['details', 'director', 'reason']);
  });
});

describe('buildLaborContractDocumentExtra', () => {
  const values = {
    director: 'Смирнов С.С.',
    salary_words: 'триста тысяч тенге',
    work_schedule: 'пятидневная рабочая неделя',
    probation_months: '',
    employer_address: '',
    employee_address: '',
  };

  it('nests director under employer without re-sending the org name or БИН', () => {
    const extra = buildLaborContractDocumentExtra(values);
    expect(extra.employer).toEqual({ director: 'Смирнов С.С.' });
    expect(Object.keys(extra).sort()).toEqual(['employer', 'salary_words', 'work_schedule']);
  });

  it('never echoes back the employee’s ИИН, name, position or salary', () => {
    const extra = buildLaborContractDocumentExtra({
      ...values,
      probation_months: '3',
      employer_address: 'г. Алматы, ул. Абая 1',
      employee_address: 'г. Алматы, ул. Сатпаева 2',
    });
    expect(extra).not.toHaveProperty('salary_tenge');
    expect(extra).not.toHaveProperty('number');
    expect(extra.employee).toEqual({ address: 'г. Алматы, ул. Сатпаева 2' });
    expect(extra.employer).toEqual({
      director: 'Смирнов С.С.',
      address: 'г. Алматы, ул. Абая 1',
    });
    expect(extra.probation_months).toBe('3');
  });
});
