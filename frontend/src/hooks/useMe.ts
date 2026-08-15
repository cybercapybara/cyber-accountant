import { useQuery } from '@tanstack/react-query';

import { api, ApiClientError } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { MeResponse } from '@/lib/api/types';

/**
 * Pulls /api/auth/me. The TanStack Query cache (key ['me']) is the
 * single source of truth for the current session — components read
 * `useMe().data` directly.
 *
 * 401 from /me means "no valid access cookie" — we resolve to null so the
 * route guard redirects to /login. Any OTHER failure (network, 5xx) is a
 * real error: we throw so `me.isError` is true and the guard can show an
 * error/retry state instead of bouncing a logged-in user to /login.
 */
/**
 * The query function behind useMe, exported so its 401-vs-error contract
 * can be unit-tested without rendering a React tree (the test stack has no
 * @testing-library/react). 401 → null (logged out); anything else throws.
 *
 * Полный конверт /me: пользователь + его роль в текущей организации.
 */
export async function fetchMeEnvelope(): Promise<MeResponse | null> {
  const { data, error } = await api.GET('/api/v1/auth/me');
  if (error) {
    if (error.status === 401) return null; // no session
    throw error; // network / 5xx — surface as a real error
  }
  if (!data) throw new Error('failed to fetch /me');
  return data;
}

/** The user slice of the envelope — kept as a named export because it is
 *  the shape the route guard and its tests reason about. */
export async function fetchMe(): Promise<MeResponse['user'] | null> {
  return (await fetchMeEnvelope())?.user ?? null;
}

/**
 * Retry predicate for useMe: never retry a deliberate "logged out" (401);
 * retry transient failures up to twice. Exported for the same reason as
 * fetchMe — it carries the "don't bounce on a blip" policy.
 */
export function shouldRetryMe(failureCount: number, error: unknown): boolean {
  if (error instanceof ApiClientError && error.status === 401) return false;
  return failureCount < 2;
}

/** Форма прежняя (пользователь или null) — ни один существующий
 *  потребитель useMe() не меняется. */
export function useMe() {
  return useQuery({
    queryKey: qk.me(),
    queryFn: fetchMeEnvelope,
    // Don't retry a deliberate "logged out" answer; do retry transient
    // failures (default exponential backoff applies to thrown errors).
    retry: shouldRetryMe,
    select: (envelope) => envelope?.user ?? null,
  });
}

/**
 * Роль в организации (org_members.role) из ТОГО ЖЕ ответа: один сетевой
 * запрос, два среза кэша — отдельный useQuery на /orgs/mine дал бы второй
 * запрос и вторую точку рассинхронизации. null = у токена нет клейма org
 * (или членство отозвано); потребители обязаны трактовать это как «прав
 * нет», а не «прав сколько угодно».
 */
export function useOrgRole() {
  return useQuery({
    queryKey: qk.me(),
    queryFn: fetchMeEnvelope,
    retry: shouldRetryMe,
    select: (envelope) => envelope?.org_role ?? null,
  });
}
