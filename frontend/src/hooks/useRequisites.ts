import { useQuery } from '@tanstack/react-query';

import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { BankAccount, Organization } from '@/lib/api/types';

/**
 * Реквизиты текущей организации — то, что сервер печатает как продавца.
 *
 * ЗАЧЕМ ЭТО ПОЯВИЛОСЬ. Раньше «мои реквизиты» лежали в localStorage браузера
 * (lib/docParty.ts прямо называл это временным решением, потому что на
 * сервере их хранить было негде) и уезжали в теле каждого документа. Отсюда
 * два следствия: реквизиты не переживали смену браузера, и два счёта одной
 * организации могли разойтись в номере расчётного счёта.
 *
 * Теперь их источник один — организация и её ОСНОВНОЙ счёт, а подставляет их
 * сервер. Клиент их только показывает: отправлять `seller` в теле документа
 * теперь запрещено (422 not_allowed_override).
 */
export interface Requisites {
  org: Organization | null;
  /// Все счета организации: страница реквизитов показывает список, формы —
  /// только основной. Один запрос обслуживает обоих.
  accounts: BankAccount[];
  primaryAccount: BankAccount | null;
  /** Заполнены ли реквизиты настолько, чтобы документ выглядел прилично. */
  complete: boolean;
}

export function useRequisites() {
  return useQuery({
    queryKey: qk.requisites.current(),
    queryFn: async (): Promise<Requisites> => {
      const [orgsRes, accountsRes] = await Promise.all([
        api.GET('/api/v1/orgs/mine'),
        api.GET('/api/v1/bank-accounts', { query: { limit: 200 } }),
      ]);
      if (orgsRes.error) throw orgsRes.error;

      // `mine` отдаёт организации пользователя вместе с его ролью; текущая —
      // та, в контексте которой выдан токен, и сервер ставит её первой.
      const org = (orgsRes.data?.data?.[0] as Organization | undefined) ?? null;

      // Список счетов может быть закрыт для роли (кадровик их не видит) —
      // это не ошибка страницы, просто банковских строк не будет.
      const accounts = accountsRes.error ? [] : (accountsRes.data?.data ?? []);
      const primaryAccount = accounts.find((a) => a.is_primary) ?? null;

      return {
        org,
        accounts,
        primaryAccount,
        complete: Boolean(org?.legal_address && org?.director_name && primaryAccount),
      };
    },
  });
}
