import { describe, expect, it } from 'vitest';

import { formatDateTimeRu, formatEpochSecondsRu, formatIsoDateTimeRu } from './dateFormat';

describe('formatDateTimeRu', () => {
  it('renders DD.MM.YYYY, HH:MM regardless of host locale', () => {
    expect(formatDateTimeRu(new Date(2026, 7, 14, 15, 0))).toBe('14.08.2026, 15:00');
  });

  it('zero-pads day, month, hours and minutes', () => {
    expect(formatDateTimeRu(new Date(2026, 0, 5, 9, 5))).toBe('05.01.2026, 09:05');
  });

  it('renders midnight as 00:00, not 24:00 or 12 AM', () => {
    expect(formatDateTimeRu(new Date(2026, 5, 1, 0, 0))).toBe('01.06.2026, 00:00');
  });
});

describe('formatIsoDateTimeRu', () => {
  it('parses an ISO 8601 string and formats it', () => {
    const iso = new Date(2026, 7, 14, 15, 0).toISOString();
    expect(formatIsoDateTimeRu(iso)).toBe(formatDateTimeRu(new Date(iso)));
  });

  it('falls back to the raw input on an unparsable string', () => {
    expect(formatIsoDateTimeRu('not-a-date')).toBe('not-a-date');
  });
});

describe('formatEpochSecondsRu', () => {
  it('formats a Unix epoch (seconds) timestamp', () => {
    const d = new Date(2026, 7, 14, 15, 0);
    expect(formatEpochSecondsRu(Math.floor(d.getTime() / 1000))).toBe(formatDateTimeRu(d));
  });

  it('renders "—" for undefined, null, or zero', () => {
    expect(formatEpochSecondsRu(undefined)).toBe('—');
    expect(formatEpochSecondsRu(null)).toBe('—');
    expect(formatEpochSecondsRu(0)).toBe('—');
  });
});
