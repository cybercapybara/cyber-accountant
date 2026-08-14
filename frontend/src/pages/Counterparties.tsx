import { useState } from 'react';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';

import { DataTable, type Column } from '@/components/DataTable';
import { FormField } from '@/components/FormField';
import { PageHeader } from '@/components/PageHeader';
import { PaginationFooter } from '@/components/PaginationFooter';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Label } from '@/components/ui/label';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type {
  Counterparty,
  CounterpartyDetailResponse,
  CounterpartyListResponse,
} from '@/lib/api/types';
import { counterpartySchema, type CounterpartyValues } from '@/lib/schemas/counterparties';

const PER_PAGE = 20;

/**
 * CounterpartiesPage — Task 14. Route: /counterparties (guard: confirmed).
 *
 * Paginated list (GET /api/v1/counterparties) plus a create form
 * (POST) and an inline edit form (PATCH) reusing the same
 * CounterpartyForm. There is no search in P1 (task brief) — just
 * pagination, mirroring AdminOrganizationsSection's shape in
 * pages/Organizations.tsx.
 *
 * The server re-validates `identifier` against the BIN/IIN check-digit
 * algorithm (Ledger::is_valid_bin_iin); the client only enforces the
 * cheap shape (12 digits) via zod, so a shape-valid-but-checksum-invalid
 * value surfaces as a 422 error toast rather than a client-side message.
 */
export function CounterpartiesPage() {
  const toast = useToast();
  const [creating, setCreating] = useState(false);
  const [editingId, setEditingId] = useState<string | null>(null);

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.counterparties.all(),
    queryFn: ({ limit, offset }) =>
      api.getJson<CounterpartyListResponse>('/api/v1/counterparties', {
        query: { limit, offset },
      }),
    perPage: PER_PAGE,
  });

  const create = useApiMutation(
    (values: CounterpartyValues) =>
      api.postJson<CounterpartyDetailResponse>('/api/v1/counterparties', { body: values }),
    {
      invalidate: [qk.counterparties.all()],
      onSuccess: () => {
        setCreating(false);
        toast.success('Контрагент создан.');
      },
    },
  );
  useErrorToast(create.error);

  const update = useApiMutation(
    (vars: { id: string; values: CounterpartyValues }) =>
      api.patchJson<CounterpartyDetailResponse>(`/api/v1/counterparties/${vars.id}`, {
        body: vars.values,
      }),
    {
      invalidate: [qk.counterparties.all()],
      onSuccess: () => {
        setEditingId(null);
        toast.success('Контрагент обновлён.');
      },
    },
  );
  useErrorToast(update.error);

  const editingRow = editingId ? data?.data.find((c) => c.id === editingId) : undefined;

  const columns: Column<Counterparty>[] = [
    { header: 'Название', className: 'font-medium', cell: (c) => c.name },
    { header: 'БИН/ИИН', className: 'font-mono', cell: (c) => c.identifier },
    {
      header: 'Плательщик НДС',
      cell: (c) => (
        <span aria-label={c.vat_payer ? 'Плательщик НДС' : 'Не плательщик НДС'}>
          <span aria-hidden="true">{c.vat_payer ? '✓' : '—'}</span>
        </span>
      ),
    },
    { header: 'ИИК', className: 'font-mono', cell: (c) => c.iik || '—' },
    {
      header: '',
      className: 'text-right',
      cell: (c) => (
        <Button
          size="sm"
          variant="outline"
          onClick={() => {
            setCreating(false);
            setEditingId(c.id);
          }}
        >
          Изменить
        </Button>
      ),
    },
  ];

  return (
    <div className="container mx-auto max-w-4xl py-8 space-y-6">
      <PageHeader
        title="Контрагенты"
        description="Поставщики, покупатели и другие стороны сделок вашей организации."
      />

      <Card>
        <CardHeader className="flex flex-row items-center justify-between space-y-0">
          <CardTitle>{data ? `Контрагентов: ${data.total}` : 'Контрагенты'}</CardTitle>
          <Button
            onClick={() => {
              setEditingId(null);
              setCreating(true);
            }}
          >
            Новый контрагент
          </Button>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(c) => c.id}
            isLoading={isLoading}
            error={error}
            emptyText="Контрагентов пока нет."
            isPlaceholder={isPlaceholderData}
          />
          {data && (
            <PaginationFooter
              page={page}
              totalPages={totalPages}
              isPlaceholderData={isPlaceholderData}
              onPageChange={setPage}
            />
          )}
        </CardContent>
        {creating && (
          <CardContent className="border-t pt-6">
            <CounterpartyForm
              submitting={create.isPending}
              onSubmit={(values) => create.mutate(values)}
              onCancel={() => setCreating(false)}
            />
          </CardContent>
        )}
        {editingRow && (
          <CardContent className="border-t pt-6">
            <CounterpartyForm
              defaultValues={editingRow}
              submitting={update.isPending}
              onSubmit={(values) => update.mutate({ id: editingRow.id, values })}
              onCancel={() => setEditingId(null)}
              submitLabel="Сохранить изменения"
            />
          </CardContent>
        )}
      </Card>
    </div>
  );
}

function CounterpartyForm({
  defaultValues,
  submitting,
  onSubmit,
  onCancel,
  submitLabel = 'Создать контрагента',
}: {
  defaultValues?: Counterparty;
  submitting: boolean;
  onSubmit: (values: CounterpartyValues) => void;
  onCancel: () => void;
  submitLabel?: string;
}) {
  const {
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<CounterpartyValues>({
    resolver: zodResolver(counterpartySchema),
    defaultValues: {
      identifier: defaultValues?.identifier ?? '',
      name: defaultValues?.name ?? '',
      address: defaultValues?.address ?? '',
      iik: defaultValues?.iik ?? '',
      bik: defaultValues?.bik ?? '',
      kbe: defaultValues?.kbe ?? '',
      is_resident: defaultValues?.is_resident ?? true,
      vat_payer: defaultValues?.vat_payer ?? false,
      contact_email: defaultValues?.contact_email ?? '',
    },
  });

  return (
    <form className="space-y-4" onSubmit={handleSubmit(onSubmit)}>
      <div className="grid grid-cols-1 sm:grid-cols-2 gap-3">
        <FormField
          id="cp-identifier"
          label="БИН/ИИН (12 цифр)"
          inputMode="numeric"
          maxLength={12}
          error={errors.identifier?.message}
          {...register('identifier')}
        />
        <FormField
          id="cp-name"
          label="Название"
          error={errors.name?.message}
          {...register('name')}
        />
      </div>
      <div className="grid grid-cols-1 sm:grid-cols-2 gap-3">
        <FormField
          id="cp-address"
          label="Адрес"
          error={errors.address?.message}
          {...register('address')}
        />
        <FormField
          id="cp-contact-email"
          label="Email для связи"
          type="email"
          error={errors.contact_email?.message}
          {...register('contact_email')}
        />
      </div>
      <div className="grid grid-cols-1 sm:grid-cols-3 gap-3">
        <FormField
          id="cp-iik"
          label="ИИК (IBAN)"
          error={errors.iik?.message}
          {...register('iik')}
        />
        <FormField id="cp-bik" label="БИК" error={errors.bik?.message} {...register('bik')} />
        <FormField id="cp-kbe" label="КБЕ" error={errors.kbe?.message} {...register('kbe')} />
      </div>
      <div className="flex flex-wrap gap-6">
        <div className="flex items-center gap-2">
          <input id="cp-is-resident" type="checkbox" {...register('is_resident')} />
          <Label htmlFor="cp-is-resident">Резидент</Label>
        </div>
        <div className="flex items-center gap-2">
          <input id="cp-vat-payer" type="checkbox" {...register('vat_payer')} />
          <Label htmlFor="cp-vat-payer">Плательщик НДС</Label>
        </div>
      </div>
      <div className="flex gap-2">
        <Button type="submit" disabled={submitting}>
          {submitting ? 'Сохранение…' : submitLabel}
        </Button>
        <Button type="button" variant="ghost" onClick={onCancel}>
          Отмена
        </Button>
      </div>
    </form>
  );
}
