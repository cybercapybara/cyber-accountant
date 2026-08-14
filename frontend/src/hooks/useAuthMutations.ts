import { useMutation, useQueryClient } from '@tanstack/react-query';

import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import { clearSelectedOrgId } from '@/lib/api/orgSession';

export function useLogin() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: async (vars: { email: string; password: string }) =>
      (await api.postJson('/api/v1/auth/login', { body: vars })).user,
    onSuccess: (user) => {
      // Seed the cache so the UI flips instantly, then revalidate.
      qc.setQueryData(qk.me(), user);
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
