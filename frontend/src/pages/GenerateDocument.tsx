import { useMemo, useState } from 'react';
import { Controller, useFieldArray, useForm, useWatch } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { useNavigate } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';

import { FormField } from '@/components/FormField';
import { PageHeader } from '@/components/PageHeader';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Label } from '@/components/ui/label';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type {
  Counterparty,
  CounterpartyListResponse,
  DocTemplateListResponse,
  GenerateDocumentCreate,
  GenerateDocumentResponse,
} from '@/lib/api/types';
import {
  buildPartyInput,
  counterpartyToParty,
  getSellerDefaults,
  setSellerDefaults,
} from '@/lib/docParty';
import { formatTiynRu, toTiyn } from '@/lib/money';
import {
  avrFormSchema,
  invoiceFormSchema,
  parseVatRatePercent,
  reconciliationFormSchema,
  taxInvoiceFormSchema,
  waybillFormSchema,
  EMPTY_LINE_ITEM,
  EMPTY_RECONCILIATION_ROW,
  EMPTY_VAT_LINE_ITEM,
  type AvrFormValues,
  type InvoiceFormValues,
  type LineItemValues,
  type PartyValues,
  type ReconciliationFormValues,
  type SellerDefaultsValues,
  type TaxInvoiceFormValues,
  type VatLineItemValues,
  type WaybillFormValues,
} from '@/lib/schemas/documents';

/**
 * GenerateDocumentPage — Task 15. Route: /documents/generate (guard: confirmed).
 *
 * Template select (GET /api/v1/doc-templates) + one typed form per known
 * `template_slug`. Deliberately NOT a generic JSON-Schema-driven form
 * builder (YAGNI, per the task brief) — the schema each template exposes
 * describes the *docgen render input*, not a UI; with only five stable
 * slugs, five hand-written forms are simpler to build, read and validate
 * than a schema-form engine that would only ever render five shapes.
 *
 * Every form shares two building blocks:
 *   - the seller party ("my requisites") — see lib/docParty.ts for why
 *     this is a temporary localStorage stopgap rather than an org field;
 *   - a counterparty <select> mapped onto the generic `party` shape
 *     (lib/docParty.ts's counterpartyToParty).
 *
 * On success (202) this navigates to /documents?focus=<id>&queued=<0|1>,
 * where DocumentsPage polls that one document until it leaves 'draft'.
 */
const TEMPLATE_LABELS: Record<string, string> = {
  invoice: 'Счёт на оплату',
  avr: 'Акт выполненных работ',
  waybill: 'Накладная на отпуск товара',
  tax_invoice: 'Счёт-фактура',
  reconciliation: 'Акт сверки взаимных расчётов',
};

// The only slugs POST /api/v1/documents/generate accepts
// (GenerateDocumentCreate.template_slug enum, docs/openapi.yaml) — and the
// only five typed forms below. A template registered under any other slug
// is simply not offered on this page.
const KNOWN_SLUGS = ['invoice', 'avr', 'waybill', 'tax_invoice', 'reconciliation'] as const;
type KnownSlug = (typeof KNOWN_SLUGS)[number];

export function GenerateDocumentPage() {
  const navigate = useNavigate();
  const toast = useToast();
  const [slug, setSlug] = useState<KnownSlug | ''>('');

  const templatesQ = useQuery({
    queryKey: qk.docTemplates.all(),
    queryFn: () => api.getJson<DocTemplateListResponse>('/api/v1/doc-templates'),
  });
  // Unpaginated (limit=200) — same rationale as Journal.tsx's counterparty
  // select: P1 organizations are small enough that this beats a
  // searchable combobox.
  const counterpartiesQ = useQuery({
    queryKey: qk.counterparties.all(),
    queryFn: () =>
      api.getJson<CounterpartyListResponse>('/api/v1/counterparties', {
        query: { limit: 200, offset: 0 },
      }),
  });
  const counterparties = counterpartiesQ.data?.data ?? [];

  const available = (templatesQ.data?.data ?? []).filter((t) =>
    (KNOWN_SLUGS as readonly string[]).includes(t.slug),
  );

  const generate = useApiMutation(
    (body: GenerateDocumentCreate) =>
      api.postJson<GenerateDocumentResponse>('/api/v1/documents/generate', { body }),
    {
      invalidate: [qk.documents.all()],
      onSuccess: (data) => {
        // The amber alert on /documents (queued=0) covers the failure case
        // in detail — a plain success toast here would be misleading when
        // render_queued came back false, so only fire it on the happy path.
        if (data.render_queued) toast.success('Документ поставлен в очередь на генерацию.');
        navigate(`/documents?focus=${data.document_id}&queued=${data.render_queued ? '1' : '0'}`);
      },
    },
  );
  useErrorToast(generate.error);

  return (
    <div className="container mx-auto max-w-4xl py-8 space-y-6">
      <PageHeader
        title="Создать документ"
        description="Выберите шаблон и заполните форму. Рендер выполняется в фоне — после отправки вы увидите статус документа на странице «Документы»."
      />

      <Card>
        <CardHeader>
          <CardTitle>Шаблон</CardTitle>
        </CardHeader>
        <CardContent className="space-y-2">
          <Label htmlFor="template-slug">Тип документа</Label>
          <select
            id="template-slug"
            className="flex h-10 w-full max-w-sm rounded-md border border-input bg-background px-3 py-2 text-sm"
            disabled={templatesQ.isLoading}
            value={slug}
            onChange={(e) => setSlug(e.target.value as KnownSlug | '')}
          >
            <option value="">{templatesQ.isLoading ? 'Загрузка…' : 'Выберите шаблон…'}</option>
            {available.map((t) => (
              <option key={t.slug} value={t.slug}>
                {TEMPLATE_LABELS[t.slug] ?? t.slug} ({t.version})
              </option>
            ))}
          </select>
          {templatesQ.data && available.length === 0 && (
            <p className="text-sm text-muted-foreground">
              Ни один из известных шаблонов не зарегистрирован на сервере.
            </p>
          )}
        </CardContent>
      </Card>

      {slug === 'invoice' && (
        <InvoiceForm
          counterparties={counterparties}
          submitting={generate.isPending}
          onSubmit={generate.mutate}
        />
      )}
      {slug === 'avr' && (
        <AvrForm
          counterparties={counterparties}
          submitting={generate.isPending}
          onSubmit={generate.mutate}
        />
      )}
      {slug === 'waybill' && (
        <WaybillForm
          counterparties={counterparties}
          submitting={generate.isPending}
          onSubmit={generate.mutate}
        />
      )}
      {slug === 'tax_invoice' && (
        <TaxInvoiceForm
          counterparties={counterparties}
          submitting={generate.isPending}
          onSubmit={generate.mutate}
        />
      )}
      {slug === 'reconciliation' && (
        <ReconciliationForm
          counterparties={counterparties}
          submitting={generate.isPending}
          onSubmit={generate.mutate}
        />
      )}
    </div>
  );
}

// ── Shared building blocks ──────────────────────────────────────────────────

/** name/identifier/address/iik/bik/bank/kbe (+ optional vat_certificate)
 *  editor for a manually-entered party — used for the seller ("my
 *  requisites") on every form. Fully controlled (value/onChange) so it
 *  drops into any form's Controller regardless of the enclosing type. */
function PartyFieldset({
  idPrefix,
  title,
  value,
  onChange,
  showVatCertificate,
}: {
  idPrefix: string;
  title: string;
  value: SellerDefaultsValues;
  onChange: (value: SellerDefaultsValues) => void;
  showVatCertificate?: boolean;
}) {
  const set = (patch: Partial<SellerDefaultsValues>) => onChange({ ...value, ...patch });
  return (
    <fieldset className="space-y-3 rounded-md border border-border p-3">
      <legend className="px-1 text-sm font-medium">{title}</legend>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        <FormField
          id={`${idPrefix}-name`}
          label="Наименование"
          value={value.name}
          onChange={(e) => set({ name: e.target.value })}
        />
        <FormField
          id={`${idPrefix}-identifier`}
          label="БИН/ИИН"
          value={value.identifier}
          onChange={(e) => set({ identifier: e.target.value })}
        />
      </div>
      <FormField
        id={`${idPrefix}-address`}
        label="Адрес"
        value={value.address}
        onChange={(e) => set({ address: e.target.value })}
      />
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
        <FormField
          id={`${idPrefix}-iik`}
          label="ИИК (IBAN)"
          value={value.iik}
          onChange={(e) => set({ iik: e.target.value })}
        />
        <FormField
          id={`${idPrefix}-bik`}
          label="БИК"
          value={value.bik}
          onChange={(e) => set({ bik: e.target.value })}
        />
        <FormField
          id={`${idPrefix}-kbe`}
          label="КБЕ"
          value={value.kbe}
          onChange={(e) => set({ kbe: e.target.value })}
        />
      </div>
      <FormField
        id={`${idPrefix}-bank`}
        label="Банк"
        value={value.bank}
        onChange={(e) => set({ bank: e.target.value })}
      />
      {showVatCertificate && (
        <FormField
          id={`${idPrefix}-vat-cert`}
          label="Свидетельство плательщика НДС"
          value={value.vat_certificate ?? ''}
          onChange={(e) => set({ vat_certificate: e.target.value })}
        />
      )}
    </fieldset>
  );
}

function CounterpartySelect({
  id,
  label,
  counterparties,
  value,
  onChange,
  error,
}: {
  id: string;
  label: string;
  counterparties: Counterparty[];
  value: string;
  onChange: (id: string) => void;
  error?: string;
}) {
  const sorted = useMemo(
    () => [...counterparties].sort((a, b) => a.name.localeCompare(b.name)),
    [counterparties],
  );
  return (
    <div className="space-y-2">
      <Label htmlFor={id}>{label}</Label>
      <select
        id={id}
        className="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
        value={value}
        onChange={(e) => onChange(e.target.value)}
      >
        <option value="">Выберите контрагента…</option>
        {sorted.map((c) => (
          <option key={c.id} value={c.id}>
            {c.name}
          </option>
        ))}
      </select>
      {error && (
        <p className="text-sm text-destructive" role="alert">
          {error}
        </p>
      )}
    </div>
  );
}

/** Money in tiyn for one name/qty/unit/price line — Math.round() on a
 *  single multiplication is the standard per-line kopeck rounding every
 *  real invoice uses; the running SUM below it is then pure integer
 *  addition, so it never drifts (same "no float arithmetic on the total"
 *  contract as lib/money.ts). */
function lineAmountTiyn(item: LineItemValues): number {
  const qty = Number.parseFloat(item.qty.replace(',', '.'));
  if (!Number.isFinite(qty)) return 0;
  return Math.round(toTiyn(item.price) * qty);
}

function sumLineAmounts(items: LineItemValues[] | undefined): number {
  return (items ?? []).reduce((sum, it) => sum + lineAmountTiyn(it), 0);
}

function buildLineItemsJson(items: LineItemValues[]) {
  return items.map((it) => ({
    name: it.name.trim(),
    qty: it.qty.trim(),
    unit: it.unit.trim(),
    price: formatTiynRu(toTiyn(it.price)),
    amount: formatTiynRu(lineAmountTiyn(it)),
  }));
}

function normalizeRatePercent(rate: string): string {
  const trimmed = rate.trim();
  return trimmed.endsWith('%') ? trimmed : `${trimmed}%`;
}

// ── Invoice ──────────────────────────────────────────────────────────────

function buildInvoiceInput(values: InvoiceFormValues, buyer: PartyValues): Record<string, unknown> {
  const subtotalTiyn = sumLineAmounts(values.items);
  const rate = parseVatRatePercent(values.vat_rate);
  const hasVat = values.vat_rate.trim() !== '' && rate > 0;
  const vatTiyn = hasVat ? Math.round((subtotalTiyn * rate) / 100) : 0;
  const input: Record<string, unknown> = {
    number: values.number.trim(),
    date: values.date.trim(),
    seller: buildPartyInput(values.seller),
    buyer: buildPartyInput(buyer),
    items: buildLineItemsJson(values.items),
    total: formatTiynRu(subtotalTiyn + vatTiyn),
    total_words: values.total_words.trim(),
  };
  if (values.contract.trim()) input.contract = values.contract.trim();
  if (hasVat) {
    input.vat_rate = normalizeRatePercent(values.vat_rate);
    input.vat_amount = formatTiynRu(vatTiyn);
  }
  return input;
}

function InvoiceForm({
  counterparties,
  submitting,
  onSubmit,
}: {
  counterparties: Counterparty[];
  submitting: boolean;
  onSubmit: (body: GenerateDocumentCreate) => void;
}) {
  const toast = useToast();
  const {
    control,
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<InvoiceFormValues>({
    resolver: zodResolver(invoiceFormSchema),
    defaultValues: {
      number: '',
      date: '',
      seller: getSellerDefaults(),
      buyerCounterpartyId: '',
      contract: '',
      items: [{ ...EMPTY_LINE_ITEM }],
      vat_rate: '',
      total_words: '',
    },
  });
  const { fields, append, remove } = useFieldArray({ control, name: 'items' });
  const watchedItems = useWatch({ control, name: 'items' });
  const watchedVatRate = useWatch({ control, name: 'vat_rate' });

  const subtotalTiyn = useMemo(() => sumLineAmounts(watchedItems), [watchedItems]);
  const vatTiyn = useMemo(() => {
    const rate = parseVatRatePercent(watchedVatRate ?? '');
    return watchedVatRate?.trim() && rate > 0 ? Math.round((subtotalTiyn * rate) / 100) : 0;
  }, [subtotalTiyn, watchedVatRate]);

  const submit = (values: InvoiceFormValues) => {
    const buyer = counterparties.find((c) => c.id === values.buyerCounterpartyId);
    if (!buyer) {
      toast.error('Выберите контрагента.');
      return;
    }
    onSubmit({
      template_slug: 'invoice',
      input: buildInvoiceInput(values, counterpartyToParty(buyer)),
      counterparty_id: buyer.id,
    });
  };

  const itemsError = errors.items?.message ?? errors.items?.root?.message;

  return (
    <Card>
      <CardHeader>
        <CardTitle>Счёт на оплату</CardTitle>
      </CardHeader>
      <CardContent>
        <form className="space-y-4" onSubmit={handleSubmit(submit)}>
          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
            <FormField
              id="inv-number"
              label="Номер"
              error={errors.number?.message}
              {...register('number')}
            />
            <FormField
              id="inv-date"
              label="Дата (ДД.ММ.ГГГГ)"
              placeholder="14.08.2026"
              error={errors.date?.message}
              {...register('date')}
            />
          </div>

          <Controller
            control={control}
            name="seller"
            render={({ field }) => (
              <PartyFieldset
                idPrefix="inv-seller"
                title="Продавец (мои реквизиты)"
                value={field.value}
                onChange={(v) => {
                  field.onChange(v);
                  setSellerDefaults(v);
                }}
              />
            )}
          />

          <Controller
            control={control}
            name="buyerCounterpartyId"
            render={({ field }) => (
              <CounterpartySelect
                id="inv-buyer"
                label="Покупатель"
                counterparties={counterparties}
                value={field.value}
                onChange={field.onChange}
                error={errors.buyerCounterpartyId?.message}
              />
            )}
          />

          <FormField
            id="inv-contract"
            label="Договор (необязательно)"
            error={errors.contract?.message}
            {...register('contract')}
          />

          <div className="space-y-3">
            <div className="flex items-center justify-between">
              <Label>Позиции</Label>
              <Button
                type="button"
                size="sm"
                variant="outline"
                onClick={() => append({ ...EMPTY_LINE_ITEM })}
              >
                Добавить строку
              </Button>
            </div>
            {fields.map((field, index) => (
              <div
                key={field.id}
                className="grid grid-cols-1 gap-2 rounded-md border border-border p-3 sm:grid-cols-[2fr_1fr_1fr_1fr_1fr_auto] sm:items-start"
              >
                <FormField
                  id={`inv-item-${index}-name`}
                  label="Наименование"
                  {...register(`items.${index}.name`)}
                />
                <FormField
                  id={`inv-item-${index}-qty`}
                  label="Кол-во"
                  {...register(`items.${index}.qty`)}
                />
                <FormField
                  id={`inv-item-${index}-unit`}
                  label="Ед."
                  {...register(`items.${index}.unit`)}
                />
                <FormField
                  id={`inv-item-${index}-price`}
                  label="Цена"
                  inputMode="decimal"
                  placeholder="0.00"
                  error={errors.items?.[index]?.price?.message}
                  {...register(`items.${index}.price`)}
                />
                <div className="space-y-1">
                  <Label>Сумма</Label>
                  <p className="flex h-10 items-center font-mono text-sm">
                    {formatTiynRu(lineAmountTiyn(watchedItems?.[index] ?? EMPTY_LINE_ITEM))}
                  </p>
                </div>
                <Button
                  type="button"
                  variant="ghost"
                  size="sm"
                  className="sm:mt-6"
                  disabled={fields.length <= 1}
                  onClick={() => remove(index)}
                >
                  Удалить
                </Button>
              </div>
            ))}
            {itemsError && (
              <p className="text-sm text-destructive" role="alert">
                {itemsError}
              </p>
            )}
          </div>

          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
            <FormField
              id="inv-vat-rate"
              label="Ставка НДС, % (необязательно)"
              placeholder="16"
              error={errors.vat_rate?.message}
              {...register('vat_rate')}
            />
            <div className="space-y-1 rounded-md border border-border p-3 text-sm">
              <p>
                Сумма позиций: <span className="font-mono">{formatTiynRu(subtotalTiyn)}</span>
              </p>
              <p>
                НДС: <span className="font-mono">{formatTiynRu(vatTiyn)}</span>
              </p>
              <p>
                Итого: <span className="font-mono">{formatTiynRu(subtotalTiyn + vatTiyn)}</span>
              </p>
            </div>
          </div>

          <FormField
            id="inv-total-words"
            label="Сумма прописью"
            error={errors.total_words?.message}
            {...register('total_words')}
          />

          <Button type="submit" disabled={submitting}>
            {submitting ? 'Создание…' : 'Создать документ'}
          </Button>
        </form>
      </CardContent>
    </Card>
  );
}

// ── AVR (акт выполненных работ) ─────────────────────────────────────────────

function buildAvrInput(values: AvrFormValues, buyer: PartyValues): Record<string, unknown> {
  return { ...buildInvoiceInput(values, buyer), act_period: values.act_period.trim() };
}

function AvrForm({
  counterparties,
  submitting,
  onSubmit,
}: {
  counterparties: Counterparty[];
  submitting: boolean;
  onSubmit: (body: GenerateDocumentCreate) => void;
}) {
  const toast = useToast();
  const {
    control,
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<AvrFormValues>({
    resolver: zodResolver(avrFormSchema),
    defaultValues: {
      number: '',
      date: '',
      seller: getSellerDefaults(),
      buyerCounterpartyId: '',
      contract: '',
      items: [{ ...EMPTY_LINE_ITEM }],
      vat_rate: '',
      total_words: '',
      act_period: '',
    },
  });
  const { fields, append, remove } = useFieldArray({ control, name: 'items' });
  const watchedItems = useWatch({ control, name: 'items' });
  const watchedVatRate = useWatch({ control, name: 'vat_rate' });

  const subtotalTiyn = useMemo(() => sumLineAmounts(watchedItems), [watchedItems]);
  const vatTiyn = useMemo(() => {
    const rate = parseVatRatePercent(watchedVatRate ?? '');
    return watchedVatRate?.trim() && rate > 0 ? Math.round((subtotalTiyn * rate) / 100) : 0;
  }, [subtotalTiyn, watchedVatRate]);

  const submit = (values: AvrFormValues) => {
    const buyer = counterparties.find((c) => c.id === values.buyerCounterpartyId);
    if (!buyer) {
      toast.error('Выберите контрагента.');
      return;
    }
    onSubmit({
      template_slug: 'avr',
      input: buildAvrInput(values, counterpartyToParty(buyer)),
      counterparty_id: buyer.id,
    });
  };

  const itemsError = errors.items?.message ?? errors.items?.root?.message;

  return (
    <Card>
      <CardHeader>
        <CardTitle>Акт выполненных работ</CardTitle>
      </CardHeader>
      <CardContent>
        <form className="space-y-4" onSubmit={handleSubmit(submit)}>
          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
            <FormField
              id="avr-number"
              label="Номер"
              error={errors.number?.message}
              {...register('number')}
            />
            <FormField
              id="avr-date"
              label="Дата (ДД.ММ.ГГГГ)"
              placeholder="14.08.2026"
              error={errors.date?.message}
              {...register('date')}
            />
          </div>
          <FormField
            id="avr-period"
            label="Период оказания услуг"
            placeholder="01.07.2026 – 31.07.2026"
            error={errors.act_period?.message}
            {...register('act_period')}
          />

          <Controller
            control={control}
            name="seller"
            render={({ field }) => (
              <PartyFieldset
                idPrefix="avr-seller"
                title="Исполнитель (мои реквизиты)"
                value={field.value}
                onChange={(v) => {
                  field.onChange(v);
                  setSellerDefaults(v);
                }}
              />
            )}
          />

          <Controller
            control={control}
            name="buyerCounterpartyId"
            render={({ field }) => (
              <CounterpartySelect
                id="avr-buyer"
                label="Заказчик"
                counterparties={counterparties}
                value={field.value}
                onChange={field.onChange}
                error={errors.buyerCounterpartyId?.message}
              />
            )}
          />

          <FormField
            id="avr-contract"
            label="Договор (необязательно)"
            error={errors.contract?.message}
            {...register('contract')}
          />

          <div className="space-y-3">
            <div className="flex items-center justify-between">
              <Label>Позиции</Label>
              <Button
                type="button"
                size="sm"
                variant="outline"
                onClick={() => append({ ...EMPTY_LINE_ITEM })}
              >
                Добавить строку
              </Button>
            </div>
            {fields.map((field, index) => (
              <div
                key={field.id}
                className="grid grid-cols-1 gap-2 rounded-md border border-border p-3 sm:grid-cols-[2fr_1fr_1fr_1fr_1fr_auto] sm:items-start"
              >
                <FormField
                  id={`avr-item-${index}-name`}
                  label="Наименование"
                  {...register(`items.${index}.name`)}
                />
                <FormField
                  id={`avr-item-${index}-qty`}
                  label="Кол-во"
                  {...register(`items.${index}.qty`)}
                />
                <FormField
                  id={`avr-item-${index}-unit`}
                  label="Ед."
                  {...register(`items.${index}.unit`)}
                />
                <FormField
                  id={`avr-item-${index}-price`}
                  label="Цена"
                  inputMode="decimal"
                  placeholder="0.00"
                  error={errors.items?.[index]?.price?.message}
                  {...register(`items.${index}.price`)}
                />
                <div className="space-y-1">
                  <Label>Сумма</Label>
                  <p className="flex h-10 items-center font-mono text-sm">
                    {formatTiynRu(lineAmountTiyn(watchedItems?.[index] ?? EMPTY_LINE_ITEM))}
                  </p>
                </div>
                <Button
                  type="button"
                  variant="ghost"
                  size="sm"
                  className="sm:mt-6"
                  disabled={fields.length <= 1}
                  onClick={() => remove(index)}
                >
                  Удалить
                </Button>
              </div>
            ))}
            {itemsError && (
              <p className="text-sm text-destructive" role="alert">
                {itemsError}
              </p>
            )}
          </div>

          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
            <FormField
              id="avr-vat-rate"
              label="Ставка НДС, % (необязательно)"
              placeholder="16"
              error={errors.vat_rate?.message}
              {...register('vat_rate')}
            />
            <div className="space-y-1 rounded-md border border-border p-3 text-sm">
              <p>
                Сумма позиций: <span className="font-mono">{formatTiynRu(subtotalTiyn)}</span>
              </p>
              <p>
                НДС: <span className="font-mono">{formatTiynRu(vatTiyn)}</span>
              </p>
              <p>
                Итого: <span className="font-mono">{formatTiynRu(subtotalTiyn + vatTiyn)}</span>
              </p>
            </div>
          </div>

          <FormField
            id="avr-total-words"
            label="Сумма прописью"
            error={errors.total_words?.message}
            {...register('total_words')}
          />

          <Button type="submit" disabled={submitting}>
            {submitting ? 'Создание…' : 'Создать документ'}
          </Button>
        </form>
      </CardContent>
    </Card>
  );
}

// ── Waybill (накладная) ─────────────────────────────────────────────────────

function buildWaybillInput(values: WaybillFormValues, buyer: PartyValues): Record<string, unknown> {
  const totalTiyn = sumLineAmounts(values.items);
  return {
    number: values.number.trim(),
    date: values.date.trim(),
    seller: buildPartyInput(values.seller),
    buyer: buildPartyInput(buyer),
    basis: values.basis.trim(),
    items: buildLineItemsJson(values.items),
    total: formatTiynRu(totalTiyn),
    total_words: values.total_words.trim(),
    released_by: values.released_by.trim(),
    received_by: values.received_by.trim(),
  };
}

function WaybillForm({
  counterparties,
  submitting,
  onSubmit,
}: {
  counterparties: Counterparty[];
  submitting: boolean;
  onSubmit: (body: GenerateDocumentCreate) => void;
}) {
  const toast = useToast();
  const {
    control,
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<WaybillFormValues>({
    resolver: zodResolver(waybillFormSchema),
    defaultValues: {
      number: '',
      date: '',
      seller: getSellerDefaults(),
      buyerCounterpartyId: '',
      basis: '',
      items: [{ ...EMPTY_LINE_ITEM }],
      total_words: '',
      released_by: '',
      received_by: '',
    },
  });
  const { fields, append, remove } = useFieldArray({ control, name: 'items' });
  const watchedItems = useWatch({ control, name: 'items' });
  const totalTiyn = useMemo(() => sumLineAmounts(watchedItems), [watchedItems]);

  const submit = (values: WaybillFormValues) => {
    const buyer = counterparties.find((c) => c.id === values.buyerCounterpartyId);
    if (!buyer) {
      toast.error('Выберите контрагента.');
      return;
    }
    onSubmit({
      template_slug: 'waybill',
      input: buildWaybillInput(values, counterpartyToParty(buyer)),
      counterparty_id: buyer.id,
    });
  };

  const itemsError = errors.items?.message ?? errors.items?.root?.message;

  return (
    <Card>
      <CardHeader>
        <CardTitle>Накладная на отпуск товара</CardTitle>
      </CardHeader>
      <CardContent>
        <form className="space-y-4" onSubmit={handleSubmit(submit)}>
          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
            <FormField
              id="wb-number"
              label="Номер"
              error={errors.number?.message}
              {...register('number')}
            />
            <FormField
              id="wb-date"
              label="Дата (ДД.ММ.ГГГГ)"
              placeholder="14.08.2026"
              error={errors.date?.message}
              {...register('date')}
            />
          </div>

          <Controller
            control={control}
            name="seller"
            render={({ field }) => (
              <PartyFieldset
                idPrefix="wb-seller"
                title="Поставщик (мои реквизиты)"
                value={field.value}
                onChange={(v) => {
                  field.onChange(v);
                  setSellerDefaults(v);
                }}
              />
            )}
          />

          <Controller
            control={control}
            name="buyerCounterpartyId"
            render={({ field }) => (
              <CounterpartySelect
                id="wb-buyer"
                label="Получатель"
                counterparties={counterparties}
                value={field.value}
                onChange={field.onChange}
                error={errors.buyerCounterpartyId?.message}
              />
            )}
          />

          <FormField
            id="wb-basis"
            label="Основание"
            placeholder="Требование-накладная № 5 от 10.08.2026"
            error={errors.basis?.message}
            {...register('basis')}
          />

          <div className="space-y-3">
            <div className="flex items-center justify-between">
              <Label>Позиции</Label>
              <Button
                type="button"
                size="sm"
                variant="outline"
                onClick={() => append({ ...EMPTY_LINE_ITEM })}
              >
                Добавить строку
              </Button>
            </div>
            {fields.map((field, index) => (
              <div
                key={field.id}
                className="grid grid-cols-1 gap-2 rounded-md border border-border p-3 sm:grid-cols-[2fr_1fr_1fr_1fr_1fr_auto] sm:items-start"
              >
                <FormField
                  id={`wb-item-${index}-name`}
                  label="Наименование"
                  {...register(`items.${index}.name`)}
                />
                <FormField
                  id={`wb-item-${index}-qty`}
                  label="Кол-во"
                  {...register(`items.${index}.qty`)}
                />
                <FormField
                  id={`wb-item-${index}-unit`}
                  label="Ед."
                  {...register(`items.${index}.unit`)}
                />
                <FormField
                  id={`wb-item-${index}-price`}
                  label="Цена"
                  inputMode="decimal"
                  placeholder="0.00"
                  error={errors.items?.[index]?.price?.message}
                  {...register(`items.${index}.price`)}
                />
                <div className="space-y-1">
                  <Label>Сумма</Label>
                  <p className="flex h-10 items-center font-mono text-sm">
                    {formatTiynRu(lineAmountTiyn(watchedItems?.[index] ?? EMPTY_LINE_ITEM))}
                  </p>
                </div>
                <Button
                  type="button"
                  variant="ghost"
                  size="sm"
                  className="sm:mt-6"
                  disabled={fields.length <= 1}
                  onClick={() => remove(index)}
                >
                  Удалить
                </Button>
              </div>
            ))}
            {itemsError && (
              <p className="text-sm text-destructive" role="alert">
                {itemsError}
              </p>
            )}
          </div>

          <p className="text-sm">
            Итого: <span className="font-mono">{formatTiynRu(totalTiyn)}</span>
          </p>

          <FormField
            id="wb-total-words"
            label="Сумма прописью"
            error={errors.total_words?.message}
            {...register('total_words')}
          />

          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
            <FormField
              id="wb-released-by"
              label="Отпустил"
              error={errors.released_by?.message}
              {...register('released_by')}
            />
            <FormField
              id="wb-received-by"
              label="Получил"
              error={errors.received_by?.message}
              {...register('received_by')}
            />
          </div>

          <Button type="submit" disabled={submitting}>
            {submitting ? 'Создание…' : 'Создать документ'}
          </Button>
        </form>
      </CardContent>
    </Card>
  );
}

// ── Tax invoice (счёт-фактура) — per-item VAT auto-calc ─────────────────────

function vatLineAmounts(item: VatLineItemValues) {
  const amountTiyn = lineAmountTiyn(item);
  const rate = parseVatRatePercent(item.vat_rate);
  const vatAmountTiyn = Math.round((amountTiyn * rate) / 100);
  return { amountTiyn, vatAmountTiyn, totalWithVatTiyn: amountTiyn + vatAmountTiyn };
}

function buildTaxInvoiceInput(
  values: TaxInvoiceFormValues,
  buyer: PartyValues,
): Record<string, unknown> {
  const lines = values.items.map((it) => ({ item: it, amounts: vatLineAmounts(it) }));
  const amount = lines.reduce((s, l) => s + l.amounts.amountTiyn, 0);
  const vat = lines.reduce((s, l) => s + l.amounts.vatAmountTiyn, 0);
  const withVat = lines.reduce((s, l) => s + l.amounts.totalWithVatTiyn, 0);
  return {
    number: values.number.trim(),
    date: values.date.trim(),
    seller: buildPartyInput(values.seller, { vat_certificate: values.seller.vat_certificate }),
    buyer: buildPartyInput(buyer, { vat_certificate: values.buyerVatCertificate }),
    items: lines.map(({ item, amounts }) => ({
      name: item.name.trim(),
      unit: item.unit.trim(),
      qty: item.qty.trim(),
      price: formatTiynRu(toTiyn(item.price)),
      amount: formatTiynRu(amounts.amountTiyn),
      vat_rate: normalizeRatePercent(item.vat_rate),
      vat_amount: formatTiynRu(amounts.vatAmountTiyn),
      total_with_vat: formatTiynRu(amounts.totalWithVatTiyn),
    })),
    totals: {
      amount: formatTiynRu(amount),
      vat: formatTiynRu(vat),
      with_vat: formatTiynRu(withVat),
    },
    total_words: values.total_words.trim(),
  };
}

function TaxInvoiceForm({
  counterparties,
  submitting,
  onSubmit,
}: {
  counterparties: Counterparty[];
  submitting: boolean;
  onSubmit: (body: GenerateDocumentCreate) => void;
}) {
  const toast = useToast();
  const {
    control,
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<TaxInvoiceFormValues>({
    resolver: zodResolver(taxInvoiceFormSchema),
    defaultValues: {
      number: '',
      date: '',
      seller: getSellerDefaults(),
      buyerCounterpartyId: '',
      buyerVatCertificate: '',
      items: [{ ...EMPTY_VAT_LINE_ITEM }],
      total_words: '',
    },
  });
  const { fields, append, remove } = useFieldArray({ control, name: 'items' });
  const watchedItems = useWatch({ control, name: 'items' });

  const totals = useMemo(() => {
    const lines = (watchedItems ?? []).map(vatLineAmounts);
    return {
      amount: lines.reduce((s, l) => s + l.amountTiyn, 0),
      vat: lines.reduce((s, l) => s + l.vatAmountTiyn, 0),
      withVat: lines.reduce((s, l) => s + l.totalWithVatTiyn, 0),
    };
  }, [watchedItems]);

  const submit = (values: TaxInvoiceFormValues) => {
    const buyer = counterparties.find((c) => c.id === values.buyerCounterpartyId);
    if (!buyer) {
      toast.error('Выберите контрагента.');
      return;
    }
    onSubmit({
      template_slug: 'tax_invoice',
      input: buildTaxInvoiceInput(values, counterpartyToParty(buyer)),
      counterparty_id: buyer.id,
    });
  };

  const itemsError = errors.items?.message ?? errors.items?.root?.message;

  return (
    <Card>
      <CardHeader>
        <CardTitle>Счёт-фактура</CardTitle>
      </CardHeader>
      <CardContent>
        <form className="space-y-4" onSubmit={handleSubmit(submit)}>
          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
            <FormField
              id="ti-number"
              label="Номер"
              error={errors.number?.message}
              {...register('number')}
            />
            <FormField
              id="ti-date"
              label="Дата (ДД.ММ.ГГГГ)"
              placeholder="14.08.2026"
              error={errors.date?.message}
              {...register('date')}
            />
          </div>

          <Controller
            control={control}
            name="seller"
            render={({ field }) => (
              <PartyFieldset
                idPrefix="ti-seller"
                title="Продавец (мои реквизиты)"
                value={field.value}
                onChange={(v) => {
                  field.onChange(v);
                  setSellerDefaults(v);
                }}
                showVatCertificate
              />
            )}
          />

          <Controller
            control={control}
            name="buyerCounterpartyId"
            render={({ field }) => (
              <CounterpartySelect
                id="ti-buyer"
                label="Покупатель"
                counterparties={counterparties}
                value={field.value}
                onChange={field.onChange}
                error={errors.buyerCounterpartyId?.message}
              />
            )}
          />
          <FormField
            id="ti-buyer-vat-cert"
            label="Свидетельство покупателя о постановке на учёт по НДС (необязательно)"
            error={errors.buyerVatCertificate?.message}
            {...register('buyerVatCertificate')}
          />

          <div className="space-y-3">
            <div className="flex items-center justify-between">
              <Label>Позиции</Label>
              <Button
                type="button"
                size="sm"
                variant="outline"
                onClick={() => append({ ...EMPTY_VAT_LINE_ITEM })}
              >
                Добавить строку
              </Button>
            </div>
            {fields.map((field, index) => {
              const amounts = vatLineAmounts(watchedItems?.[index] ?? EMPTY_VAT_LINE_ITEM);
              return (
                <div
                  key={field.id}
                  className="grid grid-cols-1 gap-2 rounded-md border border-border p-3 sm:grid-cols-[2fr_1fr_1fr_1fr_1fr_1fr_1fr_auto] sm:items-start"
                >
                  <FormField
                    id={`ti-item-${index}-name`}
                    label="Наименование"
                    {...register(`items.${index}.name`)}
                  />
                  <FormField
                    id={`ti-item-${index}-qty`}
                    label="Кол-во"
                    {...register(`items.${index}.qty`)}
                  />
                  <FormField
                    id={`ti-item-${index}-unit`}
                    label="Ед."
                    {...register(`items.${index}.unit`)}
                  />
                  <FormField
                    id={`ti-item-${index}-price`}
                    label="Цена"
                    inputMode="decimal"
                    placeholder="0.00"
                    error={errors.items?.[index]?.price?.message}
                    {...register(`items.${index}.price`)}
                  />
                  <FormField
                    id={`ti-item-${index}-vat-rate`}
                    label="НДС, %"
                    placeholder="16"
                    error={errors.items?.[index]?.vat_rate?.message}
                    {...register(`items.${index}.vat_rate`)}
                  />
                  <div className="space-y-1">
                    <Label>Сумма</Label>
                    <p className="flex h-10 items-center font-mono text-sm">
                      {formatTiynRu(amounts.amountTiyn)}
                    </p>
                  </div>
                  <div className="space-y-1">
                    <Label>Итого с НДС</Label>
                    <p className="flex h-10 items-center font-mono text-sm">
                      {formatTiynRu(amounts.totalWithVatTiyn)}
                    </p>
                  </div>
                  <Button
                    type="button"
                    variant="ghost"
                    size="sm"
                    className="sm:mt-6"
                    disabled={fields.length <= 1}
                    onClick={() => remove(index)}
                  >
                    Удалить
                  </Button>
                </div>
              );
            })}
            {itemsError && (
              <p className="text-sm text-destructive" role="alert">
                {itemsError}
              </p>
            )}
          </div>

          <div className="space-y-1 rounded-md border border-border p-3 text-sm">
            <p>
              Сумма без НДС: <span className="font-mono">{formatTiynRu(totals.amount)}</span>
            </p>
            <p>
              НДС: <span className="font-mono">{formatTiynRu(totals.vat)}</span>
            </p>
            <p>
              Всего с НДС: <span className="font-mono">{formatTiynRu(totals.withVat)}</span>
            </p>
          </div>

          <FormField
            id="ti-total-words"
            label="Сумма прописью"
            error={errors.total_words?.message}
            {...register('total_words')}
          />

          <Button type="submit" disabled={submitting}>
            {submitting ? 'Создание…' : 'Создать документ'}
          </Button>
        </form>
      </CardContent>
    </Card>
  );
}

// ── Reconciliation (акт сверки) ─────────────────────────────────────────────

function moneyOrEmpty(value: string): string {
  return value.trim() ? formatTiynRu(toTiyn(value)) : '';
}

function buildReconciliationInput(
  values: ReconciliationFormValues,
  partyB: PartyValues,
): Record<string, unknown> {
  const input: Record<string, unknown> = {
    period_from: values.period_from.trim(),
    period_to: values.period_to.trim(),
    party_a: buildPartyInput(values.partyA),
    party_b: buildPartyInput(partyB),
    rows: values.rows.map((r) => ({
      date: r.date.trim(),
      doc: r.doc.trim(),
      a_debit: moneyOrEmpty(r.a_debit),
      a_credit: moneyOrEmpty(r.a_credit),
      b_debit: moneyOrEmpty(r.b_debit),
      b_credit: moneyOrEmpty(r.b_credit),
    })),
    closing: { a_says: values.aSays.trim(), b_says: values.bSays.trim() },
  };
  if (values.openingADebit.trim() || values.openingACredit.trim()) {
    input.opening_balance = {
      a_debit: moneyOrEmpty(values.openingADebit),
      a_credit: moneyOrEmpty(values.openingACredit),
    };
  }
  return input;
}

function ReconciliationForm({
  counterparties,
  submitting,
  onSubmit,
}: {
  counterparties: Counterparty[];
  submitting: boolean;
  onSubmit: (body: GenerateDocumentCreate) => void;
}) {
  const toast = useToast();
  const {
    control,
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<ReconciliationFormValues>({
    resolver: zodResolver(reconciliationFormSchema),
    defaultValues: {
      period_from: '',
      period_to: '',
      partyA: getSellerDefaults(),
      counterpartyId: '',
      openingADebit: '',
      openingACredit: '',
      rows: [{ ...EMPTY_RECONCILIATION_ROW }],
      aSays: '',
      bSays: '',
    },
  });
  const { fields, append, remove } = useFieldArray({ control, name: 'rows' });

  const submit = (values: ReconciliationFormValues) => {
    const partyB = counterparties.find((c) => c.id === values.counterpartyId);
    if (!partyB) {
      toast.error('Выберите контрагента.');
      return;
    }
    onSubmit({
      template_slug: 'reconciliation',
      input: buildReconciliationInput(values, counterpartyToParty(partyB)),
      counterparty_id: partyB.id,
    });
  };

  const rowsError = errors.rows?.message ?? errors.rows?.root?.message;

  return (
    <Card>
      <CardHeader>
        <CardTitle>Акт сверки взаимных расчётов</CardTitle>
      </CardHeader>
      <CardContent>
        <form className="space-y-4" onSubmit={handleSubmit(submit)}>
          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
            <FormField
              id="rec-period-from"
              label="Период с (ДД.ММ.ГГГГ)"
              placeholder="01.07.2026"
              error={errors.period_from?.message}
              {...register('period_from')}
            />
            <FormField
              id="rec-period-to"
              label="Период по (ДД.ММ.ГГГГ)"
              placeholder="31.07.2026"
              error={errors.period_to?.message}
              {...register('period_to')}
            />
          </div>

          <Controller
            control={control}
            name="partyA"
            render={({ field }) => (
              <PartyFieldset
                idPrefix="rec-party-a"
                title="Сторона А (мои реквизиты)"
                value={field.value}
                onChange={(v) => {
                  field.onChange(v);
                  setSellerDefaults(v);
                }}
              />
            )}
          />

          <Controller
            control={control}
            name="counterpartyId"
            render={({ field }) => (
              <CounterpartySelect
                id="rec-party-b"
                label="Сторона Б (контрагент)"
                counterparties={counterparties}
                value={field.value}
                onChange={field.onChange}
                error={errors.counterpartyId?.message}
              />
            )}
          />

          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
            <FormField
              id="rec-opening-a-debit"
              label="Входящее сальдо, дебет А (необязательно)"
              inputMode="decimal"
              error={errors.openingADebit?.message}
              {...register('openingADebit')}
            />
            <FormField
              id="rec-opening-a-credit"
              label="Входящее сальдо, кредит А (необязательно)"
              inputMode="decimal"
              error={errors.openingACredit?.message}
              {...register('openingACredit')}
            />
          </div>

          <div className="space-y-3">
            <div className="flex items-center justify-between">
              <Label>Операции</Label>
              <Button
                type="button"
                size="sm"
                variant="outline"
                onClick={() => append({ ...EMPTY_RECONCILIATION_ROW })}
              >
                Добавить строку
              </Button>
            </div>
            {fields.map((field, index) => (
              <div
                key={field.id}
                className="grid grid-cols-1 gap-2 rounded-md border border-border p-3 sm:grid-cols-[1fr_2fr_1fr_1fr_1fr_1fr_auto] sm:items-start"
              >
                <FormField
                  id={`rec-row-${index}-date`}
                  label="Дата"
                  placeholder="05.07.2026"
                  error={errors.rows?.[index]?.date?.message}
                  {...register(`rows.${index}.date`)}
                />
                <FormField
                  id={`rec-row-${index}-doc`}
                  label="Документ"
                  error={errors.rows?.[index]?.doc?.message}
                  {...register(`rows.${index}.doc`)}
                />
                <FormField
                  id={`rec-row-${index}-a-debit`}
                  label="Дебет А"
                  inputMode="decimal"
                  error={errors.rows?.[index]?.a_debit?.message}
                  {...register(`rows.${index}.a_debit`)}
                />
                <FormField
                  id={`rec-row-${index}-a-credit`}
                  label="Кредит А"
                  inputMode="decimal"
                  error={errors.rows?.[index]?.a_credit?.message}
                  {...register(`rows.${index}.a_credit`)}
                />
                <FormField
                  id={`rec-row-${index}-b-debit`}
                  label="Дебет Б"
                  inputMode="decimal"
                  error={errors.rows?.[index]?.b_debit?.message}
                  {...register(`rows.${index}.b_debit`)}
                />
                <FormField
                  id={`rec-row-${index}-b-credit`}
                  label="Кредит Б"
                  inputMode="decimal"
                  error={errors.rows?.[index]?.b_credit?.message}
                  {...register(`rows.${index}.b_credit`)}
                />
                <Button
                  type="button"
                  variant="ghost"
                  size="sm"
                  className="sm:mt-6"
                  disabled={fields.length <= 1}
                  onClick={() => remove(index)}
                >
                  Удалить
                </Button>
              </div>
            ))}
            {rowsError && (
              <p className="text-sm text-destructive" role="alert">
                {rowsError}
              </p>
            )}
          </div>

          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
            <FormField
              id="rec-a-says"
              label="По данным стороны А"
              error={errors.aSays?.message}
              {...register('aSays')}
            />
            <FormField
              id="rec-b-says"
              label="По данным стороны Б"
              error={errors.bSays?.message}
              {...register('bSays')}
            />
          </div>

          <Button type="submit" disabled={submitting}>
            {submitting ? 'Создание…' : 'Создать документ'}
          </Button>
        </form>
      </CardContent>
    </Card>
  );
}
