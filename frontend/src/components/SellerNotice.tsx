import { Link } from 'react-router-dom';

import { useRequisites } from '@/hooks/useRequisites';

/**
 * Показывает реквизиты, которые сервер напечатает как продавца.
 *
 * Это НЕ поле ввода, и в этом весь смысл. Раньше на его месте стояла форма
 * «Продавец (мои реквизиты)», значения которой хранились в localStorage
 * браузера и уезжали в теле каждого документа. Сервер теперь заполняет
 * продавца сам из организации и её основного счёта, а присланный клиентом
 * `seller` отвергает (422 not_allowed_override) — иначе счёт можно было бы
 * выпустить от чужого имени и с чужим расчётным счётом, и он остался бы
 * законным документом этой организации.
 *
 * Незаполненные реквизиты — не ошибка: документ выпустится и без них, просто
 * без соответствующих строк. Поэтому предупреждение мягкое и со ссылкой, а
 * не блокирующее.
 */
export function SellerNotice() {
  const { data, isPending } = useRequisites();

  if (isPending) {
    return (
      <fieldset className="rounded-md border p-4">
        <legend className="px-1 text-sm font-medium">Мои реквизиты</legend>
        <p className="text-sm text-muted-foreground">Загружаются…</p>
      </fieldset>
    );
  }

  const org = data?.org ?? null;
  const account = data?.primaryAccount ?? null;

  return (
    <fieldset className="rounded-md border p-4">
      <legend className="px-1 text-sm font-medium">Мои реквизиты</legend>
      <p className="mb-2 text-sm text-muted-foreground">
        Подставляются сервером из карточки организации — в документе они всегда одни и те же.
      </p>
      <dl className="grid gap-1 text-sm sm:grid-cols-[max-content_1fr] sm:gap-x-4">
        <dt className="text-muted-foreground">Организация</dt>
        <dd>
          {org?.name ?? '—'}
          {org?.bin ? `, БИН ${org.bin}` : ''}
        </dd>
        {org?.legal_address ? (
          <>
            <dt className="text-muted-foreground">Адрес</dt>
            <dd>{org.legal_address}</dd>
          </>
        ) : null}
        {account ? (
          <>
            <dt className="text-muted-foreground">Счёт</dt>
            <dd>
              {account.iik}
              {account.bank_name ? `, ${account.bank_name}` : ''}
              {account.bik ? `, БИК ${account.bik}` : ''}
            </dd>
          </>
        ) : null}
      </dl>
      {!data?.complete ? (
        <p className="mt-3 text-sm">
          Реквизиты заполнены не полностью — документ выпустится, но без части строк.{' '}
          <Link className="underline" to="/requisites">
            Заполнить
          </Link>
        </p>
      ) : null}
    </fieldset>
  );
}
