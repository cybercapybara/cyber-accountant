import { QueryClient } from '@tanstack/react-query';
import { describe, expect, it } from 'vitest';

import { qk } from '@/lib/api/queryKeys';
import type { MeResponse } from '@/lib/api/types';

import { meCacheSeed } from './useAuthMutations';
import { selectMeOrgRole, selectMeUser } from './useMe';

/**
 * Логин засевает кэш `['me']` вручную (setQueryData), а этот вызов НЕ
 * типизирован по ключу — форму значения не проверяет никто. Ровно в этот
 * шов проскочила регрессия: пока в ключе лежал голый User, а useMe() стал
 * читать конверт /me, `useMe().data` сразу после логина был null, и
 * ProtectedRoute отправлял вошедшего обратно на /login. Тесты гоняют посев
 * через настоящий QueryClient и те же select'ы, что и хуки.
 */
const user = { id: 'u1', email: 'a@b.c', role: { id: 1, name: 'User', permissions: 0x01 } };
const loginResponse = { user } as unknown as MeResponse;

describe('meCacheSeed (что логин кладёт в кэш ["me"])', () => {
  it('leaves useMe() AND useOrgRole() readable right after login', () => {
    const qc = new QueryClient();
    qc.setQueryData<MeResponse | null>(qk.me(), meCacheSeed(loginResponse));

    const cached = qc.getQueryData<MeResponse | null>(qk.me());
    // ProtectedRoute смотрит именно сюда: не null → редиректа на /login нет.
    expect(selectMeUser(cached)).toEqual(user);
    // Роль организации логин не отдаёт — до ответа /me меню fail-closed.
    expect(selectMeOrgRole(cached)).toBeNull();
  });

  it('regression: seeding the bare user logs the user straight back out', () => {
    const qc = new QueryClient();
    // Старое поведение — `setQueryData(qk.me(), user)`.
    qc.setQueryData(qk.me(), user);

    expect(selectMeUser(qc.getQueryData<MeResponse | null>(qk.me()))).toBeNull();
  });

  it('normalises a missing org_role to null instead of undefined', () => {
    expect(meCacheSeed(loginResponse)).toEqual({ user, org_role: null });
  });

  it('keeps an org_role the response does carry', () => {
    const withRole = { user, org_role: 'hr' } as unknown as MeResponse;
    expect(meCacheSeed(withRole).org_role).toBe('hr');
  });
});
