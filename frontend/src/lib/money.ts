/**
 * Money as an integer count of tiyn (1/100 of a tenge).
 *
 * The wire format for every ledger amount is a decimal string —
 * `NUMERIC(18,2)` cast to text, e.g. "1234.56" (see JournalLine/JournalLine
 * Write in docs/openapi.yaml). Nothing in this module does float
 * arithmetic on a currency value: both directions go through integer
 * parseInt() on the whole/fractional parts, never through `Number()` +
 * multiplication (which would reintroduce binary-float rounding, e.g.
 * 0.1 + 0.2 !== 0.3).
 */

/**
 * Parse a decimal amount string into whole tiyn. Assumes the caller has
 * already shape-checked the string (the `^\d+(\.\d{1,2})?$` regex in
 * lib/schemas/journal.ts) — a string that doesn't match returns 0 rather
 * than throwing, so a live balance indicator can call this on
 * still-being-typed input without blowing up.
 */
export function toTiyn(amount: string): number {
  const trimmed = amount.trim();
  const match = /^(\d+)(?:\.(\d{1,2}))?$/.exec(trimmed);
  if (!match) return 0;
  const whole = Number.parseInt(match[1], 10);
  const fracStr = (match[2] ?? '').padEnd(2, '0');
  const frac = Number.parseInt(fracStr, 10);
  return whole * 100 + frac;
}

/** Render a signed tiyn integer back into a "1234.56"-style decimal string. */
export function formatTiyn(tiyn: number): string {
  const sign = tiyn < 0 ? '-' : '';
  const abs = Math.abs(Math.trunc(tiyn));
  const whole = Math.floor(abs / 100);
  const frac = abs % 100;
  return `${sign}${whole}.${String(frac).padStart(2, '0')}`;
}

/**
 * Render a signed tiyn integer as a "12 345,67"-style string — the money
 * format the docgen LaTeX templates expect for `price`/`amount`/`total`/etc
 * (see the schema.json and basic.json fixtures under templates/docs/ for
 * each template version: space-grouped thousands, comma decimal separator).
 * Manual digit-grouping
 * rather than `toLocaleString('ru-RU')` — Intl's ru-RU grouping separator is
 * U+00A0 (NBSP), an extra normalization step for no real benefit here, and
 * ICU locale data availability varies by runtime.
 */
export function formatTiynRu(tiyn: number): string {
  const sign = tiyn < 0 ? '-' : '';
  const abs = Math.abs(Math.trunc(tiyn));
  const whole = Math.floor(abs / 100);
  const frac = abs % 100;
  const grouped = String(whole).replace(/\B(?=(\d{3})+(?!\d))/g, ' ');
  return `${sign}${grouped},${String(frac).padStart(2, '0')}`;
}
