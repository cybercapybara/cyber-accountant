import type { Counterparty } from '@/lib/api/types';
import type { PartyValues, SellerDefaultsValues } from '@/lib/schemas/documents';

/**
 * Пустая сторона — начальное значение формы там, где сторону всё ещё вводят
 * руками (акт сверки: какая из сторон «мы», решает вызывающий, и догадка
 * напечатала бы чужие реквизиты).
 *
 * ЗДЕСЬ БЫЛО ХРАНИЛИЩЕ «моих реквизитов» в localStorage — временное решение,
 * потому что на сервере их держать было негде. Теперь есть: реквизиты
 * организации и её расчётные счета живут в БД
 * (migrations/025_org_requisites.sql), продавца в документ пишет СЕРВЕР, а
 * присланный клиентом `seller` отвергается. Копия в браузере не пережила бы
 * смену устройства и позволяла двум документам одной организации разойтись в
 * номере счёта — ровно то, ради чего всё это переносилось.
 */
export const EMPTY_PARTY: SellerDefaultsValues = {
  name: '',
  identifier: '',
  address: '',
  iik: '',
  bik: '',
  bank: '',
  kbe: '',
  vat_certificate: '',
};

/** Map a selected counterparty onto the generic docgen `party` shape. */
export function counterpartyToParty(cp: Counterparty): PartyValues {
  return {
    name: cp.name,
    identifier: cp.identifier,
    address: cp.address,
    iik: cp.iik,
    bik: cp.bik,
    // Counterparty (docs/openapi.yaml) has no bank-NAME field, only
    // iik/bik/kbe — leave blank; `party` doesn't require it either.
    bank: '',
    kbe: cp.kbe,
  };
}

/**
 * Trim a party form value down to the fields the docgen JSON Schema
 * actually wants, dropping blank optional fields rather than sending
 * empty strings for every one of them.
 */
export function buildPartyInput(
  party: PartyValues,
  extra?: { vat_certificate?: string },
): Record<string, string> {
  const out: Record<string, string> = {
    name: party.name.trim(),
    identifier: party.identifier.trim(),
  };
  if (party.address.trim()) out.address = party.address.trim();
  if (party.iik.trim()) out.iik = party.iik.trim();
  if (party.bik.trim()) out.bik = party.bik.trim();
  if (party.bank.trim()) out.bank = party.bank.trim();
  if (party.kbe.trim()) out.kbe = party.kbe.trim();
  if (extra?.vat_certificate?.trim()) out.vat_certificate = extra.vat_certificate.trim();
  return out;
}
