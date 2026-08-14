/**
 * Deterministic "ДД.ММ.ГГГГ, ЧЧ:ММ" timestamp formatter.
 *
 * Manual digit-padding rather than `Date#toLocaleString()` (with no locale
 * argument, or even `toLocaleString('ru-RU')`) — same rationale as
 * `formatTiynRu` in lib/money.ts: the no-argument form renders in the
 * *browser's* locale, so an en-US OS drops "8/14/2026, 3:00:00 PM" into an
 * otherwise all-Russian interface, and the exact separators/whether seconds
 * are included for an explicit locale are Intl-implementation-defined and
 * vary by runtime/ICU data availability. This is always the same 24-hour,
 * dot-separated shape regardless of the visitor's OS or browser.
 *
 * Uses the environment's local time zone (same as `toLocaleString()` would
 * have) — only the *formatting*, not the instant, is fixed.
 */
export function formatDateTimeRu(date: Date): string {
  const pad = (n: number) => String(n).padStart(2, '0');
  const day = pad(date.getDate());
  const month = pad(date.getMonth() + 1);
  const year = date.getFullYear();
  const hours = pad(date.getHours());
  const minutes = pad(date.getMinutes());
  return `${day}.${month}.${year}, ${hours}:${minutes}`;
}

/**
 * Parse-and-format an ISO 8601 timestamp string, falling back to the raw
 * input when it doesn't parse — the same "never throw on a weird value"
 * contract as `toTiyn` in lib/money.ts, since these values ultimately come
 * from a server response rendered straight into a table cell.
 */
export function formatIsoDateTimeRu(iso: string): string {
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? iso : formatDateTimeRu(d);
}

/**
 * Same, for a Unix epoch in seconds (the `Job.created_at`/`updated_at`
 * shape) — falsy/unparsable input renders as "—", matching the existing
 * admin/Jobs.tsx convention for "no timestamp yet".
 */
export function formatEpochSecondsRu(sec: number | undefined | null): string {
  if (!sec) return '—';
  const d = new Date(sec * 1000);
  return Number.isNaN(d.getTime()) ? '—' : formatDateTimeRu(d);
}
