import { describe, expect, it } from 'vitest';

import { formatDateTimeRu, formatEpochSecondsRu, formatIsoDateTimeRu } from './dateFormat';

// Every instant below is built with Date.UTC so the test itself carries no
// dependence on the host/CI runner's timezone — only formatDateTimeRu's own
// fixed +5h shift should affect the result.

describe('formatDateTimeRu', () => {
  it('renders a UTC instant shifted into Kazakhstan time (UTC+5)', () => {
    const utcInstant = new Date(Date.UTC(2026, 7, 14, 10, 0)); // 14 Aug 2026, 10:00 UTC
    expect(formatDateTimeRu(utcInstant)).toBe('14.08.2026, 15:00'); // 15:00 in Almaty
  });

  it('rolls over onto the next calendar date in Kazakhstan time', () => {
    // 20:00 UTC + 5h = 01:00 the next day in Almaty — the exact case a
    // local-timezone or bare-UTC formatter would get wrong.
    const utcInstant = new Date(Date.UTC(2026, 7, 14, 20, 0));
    expect(formatDateTimeRu(utcInstant)).toBe('15.08.2026, 01:00');
  });

  it('zero-pads day, month, hours and minutes', () => {
    const utcInstant = new Date(Date.UTC(2026, 0, 5, 4, 5)); // 05 Jan 2026, 04:05 UTC
    expect(formatDateTimeRu(utcInstant)).toBe('05.01.2026, 09:05'); // 09:05 in Almaty
  });

  it('renders midnight in Kazakhstan time as 00:00, not 24:00 or 12 AM', () => {
    const utcInstant = new Date(Date.UTC(2026, 4, 31, 19, 0)); // 31 May 2026, 19:00 UTC
    expect(formatDateTimeRu(utcInstant)).toBe('01.06.2026, 00:00'); // midnight, 1 June, Almaty
  });

  it('renders noon in Kazakhstan time correctly', () => {
    const utcInstant = new Date(Date.UTC(2026, 7, 14, 7, 0)); // 14 Aug 2026, 07:00 UTC
    expect(formatDateTimeRu(utcInstant)).toBe('14.08.2026, 12:00'); // noon in Almaty
  });
});

describe('formatIsoDateTimeRu', () => {
  it('parses an ISO 8601 string and formats it in Kazakhstan time', () => {
    const iso = new Date(Date.UTC(2026, 7, 14, 10, 0)).toISOString();
    expect(formatIsoDateTimeRu(iso)).toBe('14.08.2026, 15:00');
  });

  it('falls back to the raw input on an unparsable string', () => {
    expect(formatIsoDateTimeRu('not-a-date')).toBe('not-a-date');
  });
});

describe('formatEpochSecondsRu', () => {
  it('formats a Unix epoch (seconds) timestamp in Kazakhstan time', () => {
    const epochSeconds = Date.UTC(2026, 7, 14, 10, 0) / 1000;
    expect(formatEpochSecondsRu(epochSeconds)).toBe('14.08.2026, 15:00');
  });

  it('renders "—" for undefined, null, or zero', () => {
    expect(formatEpochSecondsRu(undefined)).toBe('—');
    expect(formatEpochSecondsRu(null)).toBe('—');
    expect(formatEpochSecondsRu(0)).toBe('—');
  });
});
