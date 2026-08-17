import { useEffect } from 'react';
import { useForm } from 'react-hook-form';

import { FormField } from '@/components/FormField';
import { PageHeader } from '@/components/PageHeader';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { useRequisites } from '@/hooks/useRequisites';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';

/**
 * RequisitesPage — реквизиты организации и её расчётные счета.
 * Маршрут: /requisites.
 *
 * ЗАЧЕМ. До этой страницы «мои реквизиты» жили в localStorage браузера и
 * уезжали в теле каждого документа: они не переживали смену устройства, а два
 * счёта одной организации могли разойтись в номере расчётного счёта. Теперь
 * это единственное место, где они задаются, и продавца в документ пишет
 * сервер — присланный клиентом `seller` он отвергает.
 *
 * Права: менять — только владелец. Это не иерархия, а защита от мошенничества:
 * подменённый ИИК уводит платежи покупателей на чужой счёт, и замечают это
 * недели спустя. Бухгалтер счета видит (он выпускает по ним документы) и
 * оспорит подмену, но не меняет их сам — сервер отвечает 403.
 */

interface RequisitesFormValues {
  legal_address: string;
  director_name: string;
  director_position: string;
  vat_certificate: string;
}

interface AccountFormValues {
  iik: string;
  bank_name: string;
  bik: string;
  kbe: string;
  is_primary: boolean;
}

const EMPTY_ACCOUNT: AccountFormValues = {
  iik: '',
  bank_name: '',
  bik: '',
  kbe: '',
  is_primary: false,
};

export function RequisitesPage() {
  const toast = useToast();
  const { data, isPending } = useRequisites();
  const org = data?.org ?? null;
  const accounts = data?.accounts ?? [];

  const orgForm = useForm<RequisitesFormValues>({
    defaultValues: {
      legal_address: '',
      director_name: '',
      director_position: 'Директор',
      vat_certificate: '',
    },
  });

  // Значения приходят асинхронно, поэтому форма заполняется через reset, а не
  // через defaultValues: на первом рендере организации ещё нет.
  useEffect(() => {
    if (!org) return;
    orgForm.reset({
      legal_address: org.legal_address ?? '',
      director_name: org.director_name ?? '',
      director_position: org.director_position || 'Директор',
      vat_certificate: org.vat_certificate ?? '',
    });
  }, [org, orgForm]);

  const saveOrg = useApiMutation(
    (values: RequisitesFormValues) =>
      api.patchJson(`/api/v1/orgs/${org?.id ?? ''}/requisites`, { body: values }),
    {
      invalidate: [qk.requisites.current(), qk.orgs.mine()],
      onSuccess: () => toast.success('Реквизиты сохранены'),
    },
  );
  useErrorToast(saveOrg.error);

  const accountForm = useForm<AccountFormValues>({ defaultValues: EMPTY_ACCOUNT });

  const addAccount = useApiMutation(
    (values: AccountFormValues) => api.postJson('/api/v1/bank-accounts', { body: values }),
    {
      invalidate: [qk.requisites.current()],
      onSuccess: () => {
        accountForm.reset(EMPTY_ACCOUNT);
        toast.success('Счёт добавлен');
      },
    },
  );
  useErrorToast(addAccount.error);

  const promote = useApiMutation(
    (id: string) => api.patchJson(`/api/v1/bank-accounts/${id}`, { body: { is_primary: true } }),
    {
      invalidate: [qk.requisites.current()],
      onSuccess: () => toast.success('Основной счёт изменён'),
    },
  );
  useErrorToast(promote.error);

  const removeAccount = useApiMutation(
    (id: string) => api.deleteJson(`/api/v1/bank-accounts/${id}`),
    {
      invalidate: [qk.requisites.current()],
      onSuccess: () => toast.success('Счёт удалён'),
    },
  );
  useErrorToast(removeAccount.error);

  return (
    <div className="space-y-6">
      <PageHeader
        title="Реквизиты"
        description="Печатаются в документах как реквизиты вашей стороны. Заполняются один раз — дальше сервер подставляет их сам."
      />

      <Card>
        <CardHeader>
          <CardTitle>Организация</CardTitle>
        </CardHeader>
        <CardContent>
          {isPending ? (
            <p className="text-sm text-muted-foreground">Загружается…</p>
          ) : (
            <form
              className="space-y-4"
              onSubmit={orgForm.handleSubmit((values) => saveOrg.mutate(values))}
            >
              <p className="text-sm text-muted-foreground">
                {org?.name}
                {org?.bin ? `, БИН ${org.bin}` : ''}
              </p>
              <FormField
                id="legal_address"
                label="Юридический адрес"
                {...orgForm.register('legal_address')}
              />
              <FormField
                id="director_name"
                label="ФИО подписанта"
                {...orgForm.register('director_name')}
              />
              <FormField
                id="director_position"
                label="Должность подписанта"
                {...orgForm.register('director_position')}
              />
              {/* Печатается только в счёте-фактуре: это единственная схема,
                  где у стороны есть такое поле. У неплательщика НДС его нет. */}
              <FormField
                id="vat_certificate"
                label="Свидетельство по НДС (только для счёта-фактуры)"
                {...orgForm.register('vat_certificate')}
              />
              <Button type="submit" disabled={saveOrg.isPending}>
                {saveOrg.isPending ? 'Сохранение…' : 'Сохранить'}
              </Button>
            </form>
          )}
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>Расчётные счета</CardTitle>
        </CardHeader>
        <CardContent className="space-y-4">
          <p className="text-sm text-muted-foreground">
            В документ подставляется ОСНОВНОЙ счёт. Основным может быть только один — назначение
            нового снимает признак с прежнего.
          </p>

          {accounts.length === 0 ? (
            <p className="text-sm text-muted-foreground">Счетов пока нет.</p>
          ) : (
            <ul className="divide-y">
              {accounts.map((a) => (
                <li key={a.id} className="flex flex-wrap items-center gap-3 py-2 text-sm">
                  <span className="font-medium">{a.iik}</span>
                  <span className="text-muted-foreground">
                    {a.bank_name}
                    {a.bik ? `, БИК ${a.bik}` : ''}
                    {a.kbe ? `, КБе ${a.kbe}` : ''}
                  </span>
                  {a.is_primary ? (
                    <span className="rounded bg-muted px-2 py-0.5">основной</span>
                  ) : (
                    <Button type="button" variant="ghost" onClick={() => promote.mutate(a.id)}>
                      Сделать основным
                    </Button>
                  )}
                  <Button type="button" variant="ghost" onClick={() => removeAccount.mutate(a.id)}>
                    Удалить
                  </Button>
                </li>
              ))}
            </ul>
          )}

          <form
            className="grid gap-4 sm:grid-cols-2"
            onSubmit={accountForm.handleSubmit((values) => addAccount.mutate(values))}
          >
            <FormField id="iik" label="ИИК (IBAN)" {...accountForm.register('iik')} />
            <FormField id="bank_name" label="Банк" {...accountForm.register('bank_name')} />
            <FormField id="bik" label="БИК" {...accountForm.register('bik')} />
            <FormField id="kbe" label="КБе" {...accountForm.register('kbe')} />
            <label className="flex items-center gap-2 text-sm sm:col-span-2">
              <input type="checkbox" {...accountForm.register('is_primary')} />
              Сделать основным
            </label>
            <div className="sm:col-span-2">
              <Button type="submit" disabled={addAccount.isPending}>
                {addAccount.isPending ? 'Добавление…' : 'Добавить счёт'}
              </Button>
            </div>
          </form>
        </CardContent>
      </Card>
    </div>
  );
}
