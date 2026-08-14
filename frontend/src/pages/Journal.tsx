import { useMemo, useState } from 'react';
import { useForm, useFieldArray, useWatch } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { useQuery, useQueryClient } from '@tanstack/react-query';

import { ConfirmDialog } from '@/components/ConfirmDialog';
import { DataTable, type Column } from '@/components/DataTable';
import { FormField } from '@/components/FormField';
import { Money } from '@/components/Money';
import { PageHeader } from '@/components/PageHeader';
import { PaginationFooter } from '@/components/PaginationFooter';
import { StatusBadge, type BadgeTone } from '@/components/StatusBadge';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type {
  Account,
  AccountListResponse,
  CounterpartyListResponse,
  JournalEntry,
  JournalEntryCreate,
  JournalEntryDetailResponse,
  JournalEntryListResponse,
} from '@/lib/api/types';
import { toTiyn } from '@/lib/money';
import { journalEntrySchema, type JournalEntryValues } from '@/lib/schemas/journal';

const PER_PAGE = 20;
const ACCOUNT_TYPES: Account['type'][] = ['asset', 'liability', 'equity', 'income', 'expense'];

// Shared StatusBadge tone family (DESIGN.md §5) — a "draft/posted/reversed"
// pill reads the same as a "pending/completed/failed" one elsewhere in the app.
const JOURNAL_STATUS: Record<JournalEntry['status'], { label: string; tone: BadgeTone }> = {
  draft: { label: 'Черновик', tone: 'warning' },
  posted: { label: 'Проведено', tone: 'success' },
  reversed: { label: 'Сторно', tone: 'neutral' },
};

/**
 * The list endpoint (GET /api/v1/journal-entries) returns header rows with
 * `lines` always `[]` (docs/openapi.yaml: "Header rows only... use GET
 * /journal-entries/{id} for lines") — so Σdebit and the line count both need
 * a per-row detail fetch. Two cells below share this one query per entry id
 * (same cache key, deduped by TanStack Query into a single request).
 */
function useEntryDetail(id: string) {
  return useQuery({
    queryKey: qk.journal.entry(id),
    queryFn: () => api.getJson<JournalEntryDetailResponse>(`/api/v1/journal-entries/${id}`),
  });
}

function DebitTotalCell({ id }: { id: string }) {
  const q = useEntryDetail(id);
  if (q.isLoading) return <span className="text-muted-foreground">…</span>;
  if (q.error || !q.data) return <span className="text-muted-foreground">—</span>;
  const tiyn = q.data.data.lines
    .filter((l) => l.side === 'debit')
    .reduce((acc, l) => acc + toTiyn(l.amount), 0);
  return <Money tiyn={tiyn} />;
}

function LineCountCell({ id }: { id: string }) {
  const q = useEntryDetail(id);
  if (q.isLoading) return <span className="text-muted-foreground">…</span>;
  if (q.error || !q.data) return <span className="text-muted-foreground">—</span>;
  return <span>{q.data.data.lines.length}</span>;
}

function buildJournalEntryCreate(values: JournalEntryValues): JournalEntryCreate {
  return {
    entry_date: values.entry_date,
    description: values.description,
    lines: values.lines.map((line) => ({
      account_code: line.account_code,
      side: line.side,
      amount: line.amount,
      ...(line.counterparty_id ? { counterparty_id: line.counterparty_id } : {}),
    })),
  };
}

/**
 * JournalPage — Task 14. Route: /journal (guard: confirmed).
 *
 * List (GET /api/v1/journal-entries, paginated, optional from/to filters)
 * plus a draft-entry create form (POST) with dynamic lines and a live
 * Σdebit−Σcredit balance indicator, and Post/Reverse row actions
 * (POST .../post, .../reverse) behind a confirm dialog.
 */
export function JournalPage() {
  const toast = useToast();
  const qc = useQueryClient();

  const [draftFrom, setDraftFrom] = useState('');
  const [draftTo, setDraftTo] = useState('');
  const [appliedFrom, setAppliedFrom] = useState('');
  const [appliedTo, setAppliedTo] = useState('');
  const [creating, setCreating] = useState(false);
  const [confirmAction, setConfirmAction] = useState<{
    entry: JournalEntry;
    action: 'post' | 'reverse';
  } | null>(null);

  const filters = useMemo<Record<string, string>>(() => {
    const f: Record<string, string> = {};
    if (appliedFrom) f.from = appliedFrom;
    if (appliedTo) f.to = appliedTo;
    return f;
  }, [appliedFrom, appliedTo]);

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.journal.entries(filters),
    queryFn: ({ limit, offset }) =>
      api.getJson<JournalEntryListResponse>('/api/v1/journal-entries', {
        query: { limit, offset, ...filters },
      }),
    perPage: PER_PAGE,
  });

  const create = useApiMutation(
    (values: JournalEntryValues) =>
      api.postJson<JournalEntryDetailResponse>('/api/v1/journal-entries', {
        body: buildJournalEntryCreate(values),
      }),
    {
      invalidate: [qk.journal.entries()],
      onSuccess: () => {
        setCreating(false);
        toast.success('Draft journal entry created.');
      },
    },
  );
  useErrorToast(create.error);

  const postEntry = useApiMutation(
    (id: string) => api.postJson<JournalEntryDetailResponse>(`/api/v1/journal-entries/${id}/post`),
    {
      invalidate: [qk.journal.entries()],
      onSuccess: (_data, id) => {
        qc.invalidateQueries({ queryKey: qk.journal.entry(id) });
        toast.success('Journal entry posted.');
        setConfirmAction(null);
      },
    },
  );
  useErrorToast(postEntry.error);

  const reverseEntry = useApiMutation(
    (id: string) =>
      api.postJson<JournalEntryDetailResponse>(`/api/v1/journal-entries/${id}/reverse`),
    {
      invalidate: [qk.journal.entries()],
      onSuccess: (_data, id) => {
        qc.invalidateQueries({ queryKey: qk.journal.entry(id) });
        toast.success('Journal entry reversed.');
        setConfirmAction(null);
      },
    },
  );
  useErrorToast(reverseEntry.error);

  const columns: Column<JournalEntry>[] = [
    { header: 'Дата', className: 'whitespace-nowrap', cell: (e) => e.entry_date },
    { header: 'Описание', cell: (e) => e.description },
    {
      header: 'Статус',
      cell: (e) => {
        const s = JOURNAL_STATUS[e.status];
        return <StatusBadge label={s.label} tone={s.tone} />;
      },
    },
    {
      header: 'Σ Дебет',
      className: 'text-right',
      cell: (e) => <DebitTotalCell id={e.id} />,
    },
    { header: 'Строк', className: 'text-right', cell: (e) => <LineCountCell id={e.id} /> },
    {
      header: '',
      className: 'text-right',
      cell: (e) => (
        <div className="flex justify-end gap-2">
          {e.status === 'draft' && (
            <Button
              size="sm"
              variant="outline"
              onClick={() => setConfirmAction({ entry: e, action: 'post' })}
            >
              Провести
            </Button>
          )}
          {e.status === 'posted' && (
            <Button
              size="sm"
              variant="outline"
              onClick={() => setConfirmAction({ entry: e, action: 'reverse' })}
            >
              Сторно
            </Button>
          )}
        </div>
      ),
    },
  ];

  function applyFilters() {
    setAppliedFrom(draftFrom);
    setAppliedTo(draftTo);
    setPage(1);
  }

  function clearFilters() {
    setDraftFrom('');
    setDraftTo('');
    setAppliedFrom('');
    setAppliedTo('');
    setPage(1);
  }

  return (
    <div className="container mx-auto max-w-5xl py-8 space-y-6">
      <PageHeader
        title="Журнал проводок"
        description="Двойная запись — черновик, проведение и сторнирование."
      />

      <Card>
        <CardContent className="pt-6">
          <form
            className="grid gap-4 sm:grid-cols-2 lg:grid-cols-4"
            onSubmit={(ev) => {
              ev.preventDefault();
              applyFilters();
            }}
          >
            <div className="space-y-1">
              <Label htmlFor="f-from">С даты</Label>
              <Input
                id="f-from"
                type="date"
                value={draftFrom}
                onChange={(e) => setDraftFrom(e.target.value)}
              />
            </div>
            <div className="space-y-1">
              <Label htmlFor="f-to">По дату</Label>
              <Input
                id="f-to"
                type="date"
                value={draftTo}
                onChange={(e) => setDraftTo(e.target.value)}
              />
            </div>
            <div className="flex items-end gap-2 sm:col-span-2 lg:col-span-2">
              <Button type="submit">Применить</Button>
              <Button type="button" variant="ghost" onClick={clearFilters}>
                Сбросить
              </Button>
              {data && (
                <span className="ml-auto self-center text-sm text-muted-foreground">
                  Всего: {data.total}
                </span>
              )}
            </div>
          </form>
        </CardContent>
      </Card>

      <Card>
        <CardHeader className="flex flex-row items-center justify-between space-y-0">
          <CardTitle>Проводки</CardTitle>
          <Button onClick={() => setCreating((c) => !c)}>
            {creating ? 'Закрыть' : 'Новая проводка'}
          </Button>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(e) => e.id}
            isLoading={isLoading}
            error={error}
            emptyText="Проводок, соответствующих фильтрам, не найдено."
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
            <JournalEntryForm
              submitting={create.isPending}
              onSubmit={(values) => create.mutate(values)}
              onCancel={() => setCreating(false)}
            />
          </CardContent>
        )}
      </Card>

      {confirmAction && (
        <ConfirmDialog
          title={confirmAction.action === 'post' ? 'Провести черновик' : 'Сторнировать проводку'}
          description={
            confirmAction.action === 'post'
              ? `Провести «${confirmAction.entry.description}» (${confirmAction.entry.entry_date})? Проведённые проводки больше нельзя редактировать.`
              : `Сторнировать «${confirmAction.entry.description}» (${confirmAction.entry.entry_date})? Будет создана новая, уже проведённая сторно-проводка с обратными сторонами.`
          }
          confirmLabel={confirmAction.action === 'post' ? 'Провести' : 'Сторно'}
          destructive={confirmAction.action === 'reverse'}
          busy={postEntry.isPending || reverseEntry.isPending}
          onConfirm={() => {
            if (confirmAction.action === 'post') postEntry.mutate(confirmAction.entry.id);
            else reverseEntry.mutate(confirmAction.entry.id);
          }}
          onClose={() => setConfirmAction(null)}
        />
      )}
    </div>
  );
}

/** Accounts grouped by type, each group sorted by code — the select's optgroups. */
function groupAccountsByType(accounts: Account[]): Record<Account['type'], Account[]> {
  const grouped = Object.fromEntries(ACCOUNT_TYPES.map((t) => [t, [] as Account[]])) as Record<
    Account['type'],
    Account[]
  >;
  for (const a of accounts) grouped[a.type]?.push(a);
  for (const type of ACCOUNT_TYPES) grouped[type].sort((a, b) => a.code.localeCompare(b.code));
  return grouped;
}

const EMPTY_LINE = { account_code: '', side: 'debit' as const, amount: '', counterparty_id: '' };

function JournalEntryForm({
  submitting,
  onSubmit,
  onCancel,
}: {
  submitting: boolean;
  onSubmit: (values: JournalEntryValues) => void;
  onCancel: () => void;
}) {
  const accountsQ = useQuery({
    queryKey: qk.accounts.all(),
    queryFn: () => api.getJson<AccountListResponse>('/api/v1/accounts'),
  });
  // Unpaginated fetch (limit=200) for the select — P1 organizations are
  // small enough that this beats building a searchable-combobox for a
  // handful of counterparties. Same cache prefix as CounterpartiesPage's
  // paged query, so a create/edit there invalidates this too.
  const counterpartiesQ = useQuery({
    queryKey: qk.counterparties.all(),
    queryFn: () =>
      api.getJson<CounterpartyListResponse>('/api/v1/counterparties', {
        query: { limit: 200, offset: 0 },
      }),
  });

  const {
    register,
    control,
    handleSubmit,
    formState: { errors },
  } = useForm<JournalEntryValues>({
    resolver: zodResolver(journalEntrySchema),
    defaultValues: {
      entry_date: new Date().toISOString().slice(0, 10),
      description: '',
      lines: [
        { ...EMPTY_LINE, side: 'debit' },
        { ...EMPTY_LINE, side: 'credit' },
      ],
    },
  });

  const { fields, append, remove } = useFieldArray({ control, name: 'lines' });
  const watchedLines = useWatch({ control, name: 'lines' });

  // Live balance, computed in integer tiyn (parseInt on the string's
  // whole/fractional parts — lib/money.ts's toTiyn) so a still-being-typed
  // amount never goes through float arithmetic. An unparsable amount
  // (mid-edit, e.g. "12.") contributes 0 rather than throwing.
  const { debitTiyn, creditTiyn } = useMemo(() => {
    let debit = 0;
    let credit = 0;
    for (const line of watchedLines ?? []) {
      const tiyn = toTiyn(line?.amount ?? '');
      if (line?.side === 'credit') credit += tiyn;
      else debit += tiyn;
    }
    return { debitTiyn: debit, creditTiyn: credit };
  }, [watchedLines]);
  const balanced = debitTiyn === creditTiyn && debitTiyn > 0;

  const accountsByType = useMemo(
    () => groupAccountsByType(accountsQ.data?.data ?? []),
    [accountsQ.data],
  );
  const counterparties = useMemo(
    () => [...(counterpartiesQ.data?.data ?? [])].sort((a, b) => a.name.localeCompare(b.name)),
    [counterpartiesQ.data],
  );

  const lineErrorMessage = errors.lines?.message ?? errors.lines?.root?.message;

  return (
    <form className="space-y-4" onSubmit={handleSubmit(onSubmit)}>
      <div className="grid grid-cols-1 sm:grid-cols-2 gap-3">
        <FormField
          id="je-date"
          label="Дата"
          type="date"
          error={errors.entry_date?.message}
          {...register('entry_date')}
        />
        <FormField
          id="je-description"
          label="Описание"
          error={errors.description?.message}
          {...register('description')}
        />
      </div>

      <div className="space-y-3">
        <div className="flex items-center justify-between">
          <Label>Строки</Label>
          <Button type="button" size="sm" variant="outline" onClick={() => append(EMPTY_LINE)}>
            Добавить строку
          </Button>
        </div>

        {fields.map((field, index) => (
          <div
            key={field.id}
            className="grid grid-cols-1 gap-2 rounded-md border border-border p-3 sm:grid-cols-[2fr_1fr_1fr_1.5fr_auto] sm:items-start"
          >
            <div className="space-y-1">
              <Label htmlFor={`je-account-${index}`}>Счёт</Label>
              <select
                id={`je-account-${index}`}
                className="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
                disabled={accountsQ.isLoading}
                {...register(`lines.${index}.account_code`)}
              >
                <option value="">{accountsQ.isLoading ? 'Загрузка…' : 'Выберите счёт…'}</option>
                {ACCOUNT_TYPES.map((type) =>
                  accountsByType[type].length > 0 ? (
                    <optgroup key={type} label={type}>
                      {accountsByType[type].map((a) => (
                        <option key={a.id} value={a.code}>
                          {a.code} — {a.name_ru}
                        </option>
                      ))}
                    </optgroup>
                  ) : null,
                )}
              </select>
              {errors.lines?.[index]?.account_code && (
                <p className="text-sm text-destructive" role="alert">
                  {errors.lines[index]?.account_code?.message}
                </p>
              )}
            </div>

            <div className="space-y-1">
              <Label htmlFor={`je-side-${index}`}>Сторона</Label>
              <select
                id={`je-side-${index}`}
                className="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
                {...register(`lines.${index}.side`)}
              >
                <option value="debit">Дебет</option>
                <option value="credit">Кредит</option>
              </select>
            </div>

            <FormField
              id={`je-amount-${index}`}
              label="Сумма"
              inputMode="decimal"
              placeholder="0.00"
              error={errors.lines?.[index]?.amount?.message}
              {...register(`lines.${index}.amount`)}
            />

            <div className="space-y-1">
              <Label htmlFor={`je-cp-${index}`}>Контрагент</Label>
              <select
                id={`je-cp-${index}`}
                className="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
                {...register(`lines.${index}.counterparty_id`)}
              >
                <option value="">—</option>
                {counterparties.map((c) => (
                  <option key={c.id} value={c.id}>
                    {c.name}
                  </option>
                ))}
              </select>
            </div>

            <Button
              type="button"
              variant="ghost"
              size="sm"
              className="sm:mt-6"
              disabled={fields.length <= 2}
              onClick={() => remove(index)}
            >
              Удалить
            </Button>
          </div>
        ))}
        {lineErrorMessage && (
          <p className="text-sm text-destructive" role="alert">
            {lineErrorMessage}
          </p>
        )}
      </div>

      <div
        className={`rounded-md border p-3 text-sm ${
          balanced
            ? 'border-emerald-300 bg-emerald-50 dark:border-emerald-500/30 dark:bg-emerald-500/10'
            : 'border-amber-300 bg-amber-50 dark:border-amber-500/30 dark:bg-amber-500/10'
        }`}
      >
        <div className="flex flex-wrap gap-4">
          <span>
            Σ Дебет: <Money tiyn={debitTiyn} className="font-semibold" />
          </span>
          <span>
            Σ Кредит: <Money tiyn={creditTiyn} className="font-semibold" />
          </span>
          <span>
            Разница: <Money tiyn={debitTiyn - creditTiyn} className="font-semibold" />
          </span>
        </div>
        {!balanced && (
          <p className="mt-1 text-muted-foreground">
            Суммы дебета и кредита должны совпадать (и быть больше нуля), прежде чем проводку можно
            будет сохранить.
          </p>
        )}
      </div>

      <div className="flex gap-2">
        <Button type="submit" disabled={submitting || !balanced}>
          {submitting ? 'Сохранение…' : 'Создать черновик проводки'}
        </Button>
        <Button type="button" variant="ghost" onClick={onCancel}>
          Отмена
        </Button>
      </div>
    </form>
  );
}
