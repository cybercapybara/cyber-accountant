import { QueryClient } from '@tanstack/react-query';
import { describe, expect, it } from 'vitest';

import { selectMeOrgRole, selectMeUser } from '@/hooks/useMe';
import { qk } from '@/lib/api/queryKeys';
import type { MeResponse } from '@/lib/api/types';
import { routes } from '@/routes/manifest';
import { groupNavLinks } from '@/routes/navGroups';

import { SWITCH_ORG_INVALIDATES } from './Organizations';

/**
 * Смена активной организации меняет роль пользователя, а по роли Nav.tsx
 * прячет разделы меню. С `staleTime: 30_000` и `refetchOnWindowFocus: false`
 * (main.tsx) кэш `['me']` до полуминуты держал бы роль ПРЕДЫДУЩЕЙ
 * организации — и меню предлагало бы пункты, на которые сервер отвечает 403.
 * Здесь это проверяется на настоящем QueryClient, без React.
 */
const user = { id: 'u1', email: 'a@b.c', role: { id: 1, name: 'User', permissions: 0x01 } };

const envelopeFor = (role: string) => ({ user, org_role: role }) as unknown as MeResponse;

/** Разделы меню, которые увидел бы Nav.tsx при текущем состоянии кэша. */
function sections(qc: QueryClient): string[] {
  const cached = qc.getQueryData<MeResponse | null>(qk.me());
  return groupNavLinks(routes, selectMeUser(cached), selectMeOrgRole(cached)).map((g) => g.id);
}

describe('switching the active organization', () => {
  it('invalidates the /me envelope, not just the memberships list', () => {
    expect(SWITCH_ORG_INVALIDATES).toContainEqual(qk.me());
    expect(SWITCH_ORG_INVALIDATES).toContainEqual(qk.orgs.mine());
  });

  it('re-reads the role, so the sections follow the new organization', async () => {
    const qc = new QueryClient({
      defaultOptions: { queries: { retry: false, staleTime: 30_000 } },
    });
    // Роль, которую отдаёт /me прямо сейчас.
    let role = 'hr';
    const queryFn = async () => envelopeFor(role);

    await qc.fetchQuery({ queryKey: qk.me(), queryFn, staleTime: 30_000 });
    expect(sections(qc)).toEqual(['people', 'settings']);

    // Переключились на организацию, где пользователь — владелец. Без
    // инвалидации свежий запрос упирается в staleTime и отдаёт старую роль.
    role = 'owner';
    await qc.fetchQuery({ queryKey: qk.me(), queryFn, staleTime: 30_000 });
    expect(sections(qc)).toEqual(['people', 'settings']);

    // Ровно то, что чинит эта правка.
    await Promise.all(SWITCH_ORG_INVALIDATES.map((queryKey) => qc.invalidateQueries({ queryKey })));
    await qc.fetchQuery({ queryKey: qk.me(), queryFn, staleTime: 30_000 });
    expect(sections(qc)).toEqual(['accounting', 'people', 'tax', 'settings']);
  });
});
