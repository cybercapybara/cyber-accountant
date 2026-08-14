/**
 * Kazakhstan runs a single fixed UTC+5 offset nationwide (the country
 * dropped its former east/west split and any DST in March 2024). Because
 * it's fixed and DST-free, we can hardcode the offset in milliseconds
 * instead of pulling in a timezone database — every timestamp in this app
 * renders in Kazakhstan time for every visitor, regardless of their own
 * device's locale *or* timezone. Do not swap this for
 * `toLocaleString(..., { timeZone: ... })`: the codebase's decision is
 * manual formatting (see formatTiynRu in lib/money.ts for the same
 * rationale applied to money).
 */
const KZ_OFFSET_MS = 5 * 60 * 60 * 1000;

/**
 * Deterministic "ДД.ММ.ГГГГ, ЧЧ:ММ" timestamp formatter, always rendered in
 * Kazakhstan time (UTC+5) — never the browser's local timezone and never
 * bare UTC. The instant is shifted by the fixed KZ offset and then read
 * back out with the *UTC* getters, so nothing about the runtime's own
 * timezone leaks into the result: shifting first and reading UTC after is
 * what makes this deterministic across visitors and across CI runners.
 */
export function formatDateTimeRu(date: Date): string {
  const pad = (n: number) => String(n).padStart(2, '0');
  const kz = new Date(date.getTime() + KZ_OFFSET_MS);
  const day = pad(kz.getUTCDate());
  const month = pad(kz.getUTCMonth() + 1);
  const year = kz.getUTCFullYear();
  const hours = pad(kz.getUTCHours());
  const minutes = pad(kz.getUTCMinutes());
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
 * Format a bare calendar date (`YYYY-MM-DD` — the shape of every `*_on`
 * column in this API: `hired_on`, `issued_on`, `starts_on`, …) as
 * `ДД.ММ.ГГГГ`.
 *
 * Deliberately NOT routed through `formatIsoDateTimeRu`: a date-only value
 * denotes a calendar day, not an instant, so it must not be shifted by the
 * Kazakhstan offset at all. `new Date('2026-08-14')` is read by JS as UTC
 * midnight, and shifting that by +5h would render a time nobody asked for
 * (and shifting the other way would flip the day). Reformatting the three
 * components directly is both exact and deterministic. Anything that isn't
 * a YYYY-MM-DD string falls back to the raw input, and a null/empty value
 * renders as "—" — the same "never throw on a weird value" contract the
 * other helpers in this module keep.
 */
export function formatIsoDateRu(date: string | null | undefined): string {
  if (!date) return '—';
  const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(date.trim());
  return match ? `${match[3]}.${match[2]}.${match[1]}` : date;
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
