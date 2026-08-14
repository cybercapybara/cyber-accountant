import type { Counterparty } from '@/lib/api/types';
import type { PartyValues, SellerDefaultsValues } from '@/lib/schemas/documents';

const SELLER_DEFAULTS_KEY = 'cyber-accountant.docgen.sellerDefaults';

const EMPTY_SELLER_DEFAULTS: SellerDefaultsValues = {
  name: '',
  identifier: '',
  address: '',
  iik: '',
  bik: '',
  bank: '',
  kbe: '',
  vat_certificate: '',
};

/**
 * "My requisites" — the org's own bank/legal details used as the seller
 * party on every generated document — persisted in localStorage.
 *
 * TEMPORARY P1 SOLUTION: GET /api/v1/orgs only exposes {bin, name,
 * tax_regime, vat_payer} (Organization in docs/openapi.yaml) — there is
 * no bank-detail field on the org, so there is nowhere on the backend to
 * store IIK/BIK/bank name/KBE per organization yet. Until P2 adds an
 * org-profile endpoint for this, each browser remembers its own copy
 * (seeded once, editable inline on the generation form, saved on every
 * change). This does NOT sync across devices or team members.
 */
export function getSellerDefaults(): SellerDefaultsValues {
  if (typeof localStorage === 'undefined') return { ...EMPTY_SELLER_DEFAULTS };
  try {
    const raw = localStorage.getItem(SELLER_DEFAULTS_KEY);
    if (!raw) return { ...EMPTY_SELLER_DEFAULTS };
    const parsed = JSON.parse(raw) as Partial<SellerDefaultsValues>;
    return { ...EMPTY_SELLER_DEFAULTS, ...parsed };
  } catch {
    return { ...EMPTY_SELLER_DEFAULTS };
  }
}

export function setSellerDefaults(values: SellerDefaultsValues): void {
  if (typeof localStorage === 'undefined') return;
  localStorage.setItem(SELLER_DEFAULTS_KEY, JSON.stringify(values));
}

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
