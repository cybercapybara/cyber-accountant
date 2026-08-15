import { useMutation, useQueryClient } from '@tanstack/react-query';

import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import { clearSelectedOrgId } from '@/lib/api/orgSession';
import type { MeResponse } from '@/lib/api/types';

/**
 * Что кладётся в кэш `['me']` после логина. Ключ хранит КОНВЕРТ /me
 * ({user, org_role}) — useMe() и useOrgRole() читают его двумя select'ами,
 * — а не голого пользователя: голый User превратил бы `useMe().data` в
 * null (у него нет поля `user`), и ProtectedRoute немедленно отправил бы
 * только что вошедшего обратно на /login.
 *
 * `/auth/login` отдаёт ту же схему MeResponse, но БЕЗ `org_role` (роль в
 * организации знает только /me), поэтому здесь она нормализуется в null:
 * до ответа /me меню fail-closed показывает лишь то, что ролью не гейтится.
 * Вынесено отдельной чистой функцией, потому что setQueryData не типизирует
 * значение по ключу — это ровно тот шов, в который проскакивает
 * рассогласование формы кэша.
 */
export function meCacheSeed(response: MeResponse): MeResponse {
  return { user: response.user, org_role: response.org_role ?? null };
}

export function useLogin() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (vars: { email: string; password: string }) =>
      api.postJson('/api/v1/auth/login', { body: vars }),
    onSuccess: (response) => {
      // Seed the cache so the UI flips instantly, then revalidate.
      qc.setQueryData<MeResponse | null>(qk.me(), meCacheSeed(response));
      qc.invalidateQueries({ queryKey: qk.me() });
    },
  });
}

export function useRegister() {
  return useMutation({
    // No auto-login (flask-base parity): the backend sends a confirmation
    // email and the flow continues at /account/check-email. We don't read
    // the response body — registration succeeding is all the caller needs,
    // so this stays robust even if the endpoint stops echoing a user.
    mutationFn: async (vars: {
      email: string;
      password: string;
      first_name?: string;
      last_name?: string;
    }) => {
      await api.postJson('/api/v1/auth/register', { body: vars });
    },
  });
}

export function useLogout() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: async () => {
      await api.POST('/api/v1/auth/logout');
    },
    onSuccess: () => {
      qc.clear();
      // The next login is a different session (possibly a different
      // user) — a stale selected-org id must not leak into it and get
      // replayed by client.ts's post-refresh re-switch.
      clearSelectedOrgId();
    },
  });
}
