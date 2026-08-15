import { useMemo, useState } from 'react';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Link } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';

import { ConfirmDialog } from '@/components/ConfirmDialog';
import { DataTable, type Column } from '@/components/DataTable';
import { FormField } from '@/components/FormField';
import { GeneratedDocumentCard, type GeneratedDocument } from '@/components/GeneratedDocumentCard';
import { Money } from '@/components/Money';
import { PageHeader } from '@/components/PageHeader';
import { PaginationFooter } from '@/components/PaginationFooter';
import { StatusBadge, type BadgeTone } from '@/components/StatusBadge';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Label } from '@/components/ui/label';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { ApiClientError, api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type {
  EmployeeListResponse,
  GenerateDocumentResponse,
  PayrollRun,
  PayrollRunDetailResponse,
  PayrollRunListResponse,
  Payslip,
  PayslipDocumentExtra,
  PayslipListResponse,
  PostToJournalResponse,
} from '@/lib/api/types';
import { formatIsoDateTimeRu } from '@/lib/dateFormat';
import { employeeFullName } from '@/lib/schemas/hr';
import {
  buildPayrollRunCreate,
  buildPayslipDocumentExtra,
  MONTHS_RU,
  payrollPeriodLabel,
  payrollPeriodSchema,
  payrollRunActions,
  payrollRunStage,
  PAYSLIP_AMOUNT_FIELDS,
  sumPayslips,
  type PayrollPeriodValues,
  type PayrollRunStage,
  type PayslipAmountField,
  type PayslipAmounts,
} from '@/lib/schemas/payroll';

const PER_PAGE = 20;

// Unpaginated employee roster for the ФИО column — same rationale as
// HrOrders.tsx: a P2 organization's staff fits in one 200-row fetch, and a
// payroll run has exactly one payslip per active employee anyway.
const EMPLOYEE_FETCH_LIMIT = 200;

// Shared StatusBadge tone family (DESIGN.md §5).
const STAGE_BADGE: Record<PayrollRunStage, { label: string; tone: BadgeTone }> = {
  draft: { label: 'Черновик', tone: 'warning' },
  approved: { label: 'Утверждён', tone: 'info' },
  posted: { label: 'Проведён в учёт', tone: 'success' },
};

/** Headers for the nine money columns, in PAYSLIP_AMOUNT_FIELDS order. */
const AMOUNT_HEADERS: Record<PayslipAmountField, string> = {
  gross_tiyn: 'Оклад',
  opv: 'ОПВ',
  vosms: 'ВОСМС',
  ipn: 'ИПН',
  net: 'К выплате',
  opvr: 'ОПВР',
  so: 'СО',
  osms: 'ОСМС',
  social_tax: 'Соцналог',
};

/** Today's year/month as the form's defaults. UTC slices of the ISO string
 *  rather than local-timezone getters — the same idiom Journal.tsx uses for
 *  its default entry date. */
function currentPeriodDefaults(): PayrollPeriodValues {
  const iso = new Date().toISOString();
  return { year: iso.slice(0, 4), month: String(Number.parseInt(iso.slice(5, 7), 10)) };
}

/** One line of the payslip table — the «Итого» row carries no payslip. */
interface PayslipRow {
  key: string;
  label: string;
  amounts: PayslipAmounts;
  payslip?: Payslip;
}

/**
 * PayrollPage — Task 14. Route: /payroll (guard: confirmed).
 *
 * A period picker that calculates (or RECALCULATES) one month's payroll
 * (POST /api/v1/payroll-runs), the list of runs (GET, paginated), and — for
 * the selected run — its payslips with a totals row plus the one lifecycle
 * action that is actually available.
 *
 * The lifecycle is the whole point of this screen and it is NOT a single
 * column: a run is 'draft' until approved, stays 'approved' forever after,
 * and counts as posted only once `journal_entry_id` is non-null (the
 * compare-and-swap PayrollService guards a double post with). `payrollRunStage`
 * / `payrollRunActions` (lib/schemas/payroll.ts) fold the two columns into
 * one stage, so exactly one of «Пересчитать»/«Утвердить»/«Провести в учёт»
 * is offered and a posted run offers none.
 *
 * Every 409 from those routes is a STATE conflict, not a failure of the
 * request: the period was approved while this tab was open, or the run was
 * already posted from another tab. Each is answered with its own sentence
 * plus a refetch, so the screen catches up instead of showing a bare
 * "invalid_run_state". Recalculating a DRAFT period, by contrast, is a
 * normal 200 — POST /payroll-runs upserts.
 */
export function PayrollPage() {
  const toast = useToast();
  const [selected, setSelected] = useState<PayrollRun | null>(null);

  const {
    data,
    isLoading,
    error,
    isPlaceholderData,
    page,
    setPage,
    totalPages,
    refetch: refetchRuns,
  } = usePagedQuery({
    queryKey: qk.payroll.runs(),
    queryFn: ({ limit, offset }) =>
      api.getJson<PayrollRunListResponse>('/api/v1/payroll-runs', { query: { limit, offset } }),
    perPage: PER_PAGE,
  });

  const rows = data?.data;
  // Prefer the list's copy (it is refetched after every mutation); fall back
  // to the run this tab last received, because there is no
  // GET /payroll-runs/{id} — the list IS the only source of a run header, and
  // a period calculated for an old month may not be on the current page.
  const selectedRun = selected ? (rows?.find((r) => r.id === selected.id) ?? selected) : null;
  const calculate = useApiMutation(
    (values: PayrollPeriodValues) =>
      api.postJson<PayrollRunDetailResponse>('/api/v1/payroll-runs', {
        body: buildPayrollRunCreate(values),
      }),
    {
      invalidate: [qk.payroll.runs(), qk.payroll.payslips()],
      onSuccess: (res) => {
        setSelected(res.data);
        setPage(1);
        toast.success(
          `Расчёт за ${payrollPeriodLabel(res.data.period_year, res.data.period_month)} выполнен.`,
        );
      },
      onError: (message, err) => {
        if (err instanceof ApiClientError && err.status === 409) {
          toast.error(
            'Этот период уже утверждён — пересчитать можно только черновик. Снимите утверждение через бухгалтера или выберите другой период.',
          );
          void refetchRuns();
          return;
        }
        toast.error(message);
      },
    },
  );

  const approve = useApiMutation(
    (id: string) => api.postJson<PayrollRunDetailResponse>(`/api/v1/payroll-runs/${id}/approve`),
    {
      invalidate: [qk.payroll.runs()],
      onSuccess: (res) => {
        setSelected(res.data);
        toast.success('Расчёт утверждён.');
      },
      onError: (message, err) => {
        if (err instanceof ApiClientError && err.status === 409) {
          toast.error('Этот расчёт уже утверждён — повторное утверждение не требуется.');
          void refetchRuns();
          return;
        }
        toast.error(message);
      },
    },
  );

  const postToJournal = useApiMutation(
    (id: string) =>
      api.postJson<PostToJournalResponse>(`/api/v1/payroll-runs/${id}/post-to-journal`),
    {
      invalidate: [qk.payroll.runs(), qk.journal.entries()],
      onSuccess: (res) => {
        // The run is now posted with exactly this entry — record it locally
        // so the card switches to the posted state even before the list
        // refetch lands (`posted_at` arrives with that refetch).
        setSelected((prev) => (prev ? { ...prev, journal_entry_id: res.entry_id } : prev));
        toast.success('Расчёт проведён в учёт.');
      },
      onError: (message, err) => {
        if (err instanceof ApiClientError && err.status === 409) {
          toast.error(
            err.code === 'empty_run'
              ? 'В расчёте нет ни одного расчётного листка — сначала выполните расчёт за период.'
              : 'Расчёт уже проведён в учёт или ещё не утверждён — состояние обновлено.',
          );
          void refetchRuns();
          return;
        }
        toast.error(message);
      },
    },
  );

  const [confirming, setConfirming] = useState<'approve' | 'post' | null>(null);

  const columns: Column<PayrollRun>[] = [
    {
      header: 'Период',
      className: 'font-medium whitespace-nowrap',
      cell: (r) => payrollPeriodLabel(r.period_year, r.period_month),
    },
    {
      header: 'Статус',
      cell: (r) => {
        const badge = STAGE_BADGE[payrollRunStage(r)];
        return <StatusBadge label={badge.label} tone={badge.tone} />;
      },
    },
    {
      header: 'Рассчитан',
      className: 'whitespace-nowrap',
      cell: (r) => formatIsoDateTimeRu(r.calculated_at),
    },
    {
      header: 'Проведён',
      className: 'whitespace-nowrap',
      cell: (r) => (r.posted_at ? formatIsoDateTimeRu(r.posted_at) : '—'),
    },
  ];

  return (
    <div className="container mx-auto max-w-6xl py-8 space-y-6">
      <PageHeader
        title="Зарплата"
        description="Расчёт зарплаты, удержаний и взносов по сотрудникам организации."
        actions={
          <Button asChild variant="outline">
            <Link to="/employees">Сотрудники</Link>
          </Button>
        }
      />

      <Card>
        <CardHeader>
          <CardTitle>Расчёт за период</CardTitle>
        </CardHeader>
        <CardContent>
          <PeriodForm
            submitting={calculate.isPending}
            onSubmit={(values) => calculate.mutate(values)}
          />
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>{data ? `Расчётов: ${data.total}` : 'Расчёты'}</CardTitle>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={rows}
            rowKey={(r) => r.id}
            isLoading={isLoading}
            error={error}
            emptyText="Расчётов зарплаты пока нет."
            isPlaceholder={isPlaceholderData}
            rowProps={(r) => ({
              onClick: () => setSelected(r),
              'aria-label': `Открыть расчёт за ${payrollPeriodLabel(r.period_year, r.period_month)}`,
              className: r.id === selectedRun?.id ? 'bg-muted/60' : '',
            })}
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
      </Card>

      {selectedRun && (
        <RunDetail
          run={selectedRun}
          onApprove={() => setConfirming('approve')}
          onPost={() => setConfirming('post')}
          busy={approve.isPending || postToJournal.isPending}
        />
      )}

      {selectedRun && confirming === 'approve' && (
        <ConfirmDialog
          title="Утвердить расчёт"
          description={`Утвердить расчёт за ${payrollPeriodLabel(
            selectedRun.period_year,
            selectedRun.period_month,
          )}? После утверждения пересчитать период будет нельзя.`}
          confirmLabel="Утвердить"
          busy={approve.isPending}
          onConfirm={() => {
            approve.mutate(selectedRun.id);
            setConfirming(null);
          }}
          onClose={() => setConfirming(null)}
        />
      )}

      {selectedRun && confirming === 'post' && (
        <ConfirmDialog
          title="Провести в учёт"
          description={`Провести расчёт за ${payrollPeriodLabel(
            selectedRun.period_year,
            selectedRun.period_month,
          )} в журнал проводок? Будет создана одна проводка на все начисления и удержания; провести повторно нельзя.`}
          confirmLabel="Провести"
          busy={postToJournal.isPending}
          onConfirm={() => {
            postToJournal.mutate(selectedRun.id);
            setConfirming(null);
          }}
          onClose={() => setConfirming(null)}
        />
      )}

      {selectedRun && <PayslipsSection run={selectedRun} />}
    </div>
  );
}

function PeriodForm({
  submitting,
  onSubmit,
}: {
  submitting: boolean;
  onSubmit: (values: PayrollPeriodValues) => void;
}) {
  const {
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<PayrollPeriodValues>({
    resolver: zodResolver(payrollPeriodSchema),
    defaultValues: currentPeriodDefaults(),
  });

  return (
    <form className="space-y-4" onSubmit={handleSubmit(onSubmit)}>
      <p className="text-sm text-muted-foreground">
        Расчёт выполняется по всем работающим сотрудникам. Повторный расчёт того же периода заменяет
        черновик — это нормально; утверждённый период пересчитать нельзя.
      </p>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
        <div className="space-y-2">
          <Label htmlFor="payroll-month">Месяц</Label>
          <select
            id="payroll-month"
            className="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
            {...register('month')}
          >
            {MONTHS_RU.slice(1).map((name, index) => (
              <option key={name} value={index + 1}>
                {name}
              </option>
            ))}
          </select>
          {errors.month?.message && (
            <p className="text-sm text-destructive" role="alert">
              {errors.month.message}
            </p>
          )}
        </div>
        <FormField
          id="payroll-year"
          label="Год"
          inputMode="numeric"
          maxLength={4}
          error={errors.year?.message}
          {...register('year')}
        />
      </div>
      <Button type="submit" disabled={submitting}>
        {submitting ? 'Расчёт…' : 'Рассчитать'}
      </Button>
    </form>
  );
}

/**
 * The selected run's header: its stage, and the ONE action that stage
 * allows. A posted run shows the journal entry it became instead of a
 * button — «Провести в учёт» a second time is a 409, never a second entry.
 */
function RunDetail({
  run,
  onApprove,
  onPost,
  busy,
}: {
  run: PayrollRun;
  onApprove: () => void;
  onPost: () => void;
  busy: boolean;
}) {
  const stage = payrollRunStage(run);
  const actions = payrollRunActions(run);
  const badge = STAGE_BADGE[stage];

  return (
    <Card>
      <CardHeader className="flex flex-row flex-wrap items-center justify-between gap-3 space-y-0">
        <CardTitle>
          Расчёт за {payrollPeriodLabel(run.period_year, run.period_month)}{' '}
          <StatusBadge label={badge.label} tone={badge.tone} className="ml-2 align-middle" />
        </CardTitle>
        <div className="flex flex-wrap gap-2">
          {actions.canApprove && (
            <Button onClick={onApprove} disabled={busy}>
              Утвердить
            </Button>
          )}
          {actions.canPost && (
            <Button onClick={onPost} disabled={busy}>
              Провести в учёт
            </Button>
          )}
        </div>
      </CardHeader>
      <CardContent className="space-y-2 text-sm">
        {stage === 'draft' && (
          <p className="text-muted-foreground">
            Черновик: период можно пересчитать сколько угодно раз. Проводка в учёт станет доступна
            после утверждения.
          </p>
        )}
        {stage === 'approved' && (
          <p className="text-muted-foreground">
            Расчёт утверждён и больше не пересчитывается. Осталось провести его в журнал проводок.
          </p>
        )}
        {stage === 'posted' && (
          <p className="text-muted-foreground">
            Расчёт проведён в учёт{run.posted_at ? ` ${formatIsoDateTimeRu(run.posted_at)}` : ''}.
            Проводка: <span className="font-mono">{run.journal_entry_id}</span> —{' '}
            <Link to="/journal" className="underline">
              открыть журнал проводок
            </Link>
            .
          </p>
        )}
      </CardContent>
    </Card>
  );
}

/**
 * The run's payslips plus an «Итого» row, and the per-employee «Расчётный
 * листок» document flow. The totals row is a synthetic row of the same
 * table rather than a separate block, so every sum stays under its own
 * column (DESIGN.md §7).
 */
function PayslipsSection({ run }: { run: PayrollRun }) {
  const toast = useToast();
  // Employee id of the payslip whose document is being rendered right now —
  // only that row's button says «Формирование…».
  const [generatingFor, setGeneratingFor] = useState<string | null>(null);
  const [generated, setGenerated] = useState<GeneratedDocument | null>(null);

  const payslipsQ = useQuery({
    queryKey: qk.payroll.payslips(run.id),
    queryFn: () => api.getJson<PayslipListResponse>(`/api/v1/payroll-runs/${run.id}/payslips`),
  });

  const employeesQ = useQuery({
    queryKey: qk.employees.all(),
    queryFn: () =>
      api.getJson<EmployeeListResponse>('/api/v1/employees', {
        query: { limit: EMPLOYEE_FETCH_LIMIT, offset: 0 },
      }),
  });
  const employeeName = useMemo(() => {
    const map = new Map(
      (employeesQ.data?.data ?? []).map((e) => [e.id, employeeFullName(e)] as const),
    );
    return (id: string) => map.get(id) ?? '—';
  }, [employeesQ.data]);

  // The body is empty: every figure on the листок, the net amount in words
  // included, is derived from the stored payslip (buildPayslipDocumentExtra).
  // There is nothing for the user to fill in, so the row's button generates
  // the document directly instead of opening a form.
  const generate = useApiMutation(
    (vars: { employeeId: string; extra: PayslipDocumentExtra }) =>
      api.postJson<GenerateDocumentResponse>(
        `/api/v1/payroll-runs/${run.id}/payslips/${vars.employeeId}/generate-document`,
        { body: vars.extra },
      ),
    {
      invalidate: [qk.documents.all()],
      onSuccess: (res) => {
        setGeneratingFor(null);
        setGenerated({ documentId: res.document_id, renderQueued: res.render_queued });
        if (res.render_queued) toast.success('Расчётный листок поставлен в очередь на генерацию.');
      },
      onError: (message) => {
        // With no caller-supplied field left there is no input to correct,
        // so even a 422 is a server-side failure — a toast, not an inline
        // field error.
        setGeneratingFor(null);
        toast.error(message);
      },
    },
  );

  const payslips = payslipsQ.data?.data;
  const tableRows = useMemo<PayslipRow[] | undefined>(() => {
    if (!payslips) return undefined;
    if (payslips.length === 0) return [];
    const rows: PayslipRow[] = payslips.map((p) => ({
      key: p.id,
      label: employeeName(p.employee_id),
      amounts: p,
      payslip: p,
    }));
    rows.push({ key: '__total__', label: 'Итого', amounts: sumPayslips(payslips) });
    return rows;
  }, [payslips, employeeName]);

  const columns: Column<PayslipRow>[] = [
    { header: 'Сотрудник', className: 'font-medium', cell: (r) => r.label },
    ...PAYSLIP_AMOUNT_FIELDS.map<Column<PayslipRow>>((field) => ({
      header: AMOUNT_HEADERS[field],
      className: 'text-right',
      cell: (r) => <Money tiyn={r.amounts[field]} hideCurrency />,
    })),
    {
      header: '',
      className: 'text-right',
      cell: (r) =>
        r.payslip ? (
          <Button
            size="sm"
            variant="outline"
            disabled={generate.isPending}
            onClick={() => {
              const employeeId = r.payslip?.employee_id;
              if (!employeeId) return;
              setGenerated(null);
              setGeneratingFor(employeeId);
              generate.mutate({ employeeId, extra: buildPayslipDocumentExtra() });
            }}
          >
            {generatingFor === r.payslip.employee_id ? 'Формирование…' : 'Расчётный листок'}
          </Button>
        ) : null,
    },
  ];

  return (
    <>
      <Card>
        <CardHeader>
          <CardTitle>Расчётные листки</CardTitle>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <p className="mb-3 text-sm text-muted-foreground">
            Все суммы в тенге (₸). Расчётный листок формируется сразу: все данные, включая сумму к
            выплате прописью, берутся из расчёта — вводить ничего не нужно.
          </p>
          <DataTable
            columns={columns}
            rows={tableRows}
            rowKey={(r) => r.key}
            isLoading={payslipsQ.isLoading}
            error={payslipsQ.error}
            emptyText="В этом расчёте нет расчётных листков."
            rowProps={(r) => (r.payslip ? {} : { className: 'font-medium bg-muted/40' })}
          />
        </CardContent>
      </Card>

      {generated && (
        <GeneratedDocumentCard
          documentId={generated.documentId}
          renderQueued={generated.renderQueued}
          title="Расчётный листок"
          onDismiss={() => setGenerated(null)}
        >
          <p className="text-muted-foreground">
            Документ также доступен в разделе{' '}
            <Link to={`/documents?focus=${generated.documentId}&queued=1`} className="underline">
              Документы
            </Link>
            .
          </p>
        </GeneratedDocumentCard>
      )}
    </>
  );
}
