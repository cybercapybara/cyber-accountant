import { useEffect, useRef, useState } from 'react';
import { useForm, useWatch } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Link } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';
import { Calculator } from 'lucide-react';

import { DataTable, type Column } from '@/components/DataTable';
import { EmptyState } from '@/components/EmptyState';
import { FormField } from '@/components/FormField';
import { Money } from '@/components/Money';
import { PageHeader } from '@/components/PageHeader';
import { PaginationFooter } from '@/components/PaginationFooter';
import { StatusBadge, type BadgeTone } from '@/components/StatusBadge';
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Label } from '@/components/ui/label';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useDocumentRender } from '@/hooks/useDocumentRender';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { ApiClientError, api, apiErrorMessage } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type {
  FilingDownloadUrlResponse,
  TaxAlertListResponse,
  TaxCalculation,
  TaxCalculationDetailResponse,
  TaxCalculationListResponse,
  TaxDeadline,
  TaxDeadlineListResponse,
  TaxFiling,
  TaxFilingCreateResponse,
  TaxFilingDetailResponse,
  TaxFilingListResponse,
  TaxRatesResponse,
} from '@/lib/api/types';
import { formatIsoDateRu, formatIsoDateTimeRu } from '@/lib/dateFormat';
import {
  buildFno300DocumentInput,
  buildFno910DocumentInput,
  buildTaxCalculationCreate,
  buildTaxFilingCreate,
  calculationPeriodLabel,
  deadlineUrgency,
  filingKindFor,
  fno300DocumentSchema,
  fno910DocumentSchema,
  formatRateBpRu,
  snapshotTiyn,
  taxCalculationSchema,
  CALCULATION_KINDS,
  CALCULATION_KIND_LABELS,
  CALCULATION_PERIODS_PER_YEAR,
  DEADLINE_KIND_LABELS,
  FILING_KIND_LABELS,
  FILING_STATUS_LABELS,
  RATE_KIND_LABELS,
  TAX_ALERT_NOTES,
  TAX_ALERT_TITLES,
  TAX_CONSTANT_LABELS,
  type CalculationKind,
  type Fno300DocumentValues,
  type Fno910DocumentValues,
  type TaxCalculationValues,
} from '@/lib/schemas/tax';

const PER_PAGE = 20;

const TABS = [
  { id: 'deadlines', label: 'Сроки' },
  { id: 'calculations', label: 'Расчёты' },
  { id: 'filings', label: 'ФНО' },
] as const;
type TabId = (typeof TABS)[number]['id'];

const URGENCY_TONE: Record<ReturnType<typeof deadlineUrgency>, BadgeTone> = {
  overdue: 'danger',
  urgent: 'warning',
  normal: 'neutral',
};

const FILING_STATUS_TONE: Record<TaxFiling['status'], BadgeTone> = {
  draft: 'warning',
  generated: 'success',
  submitted_manually: 'info',
};

/** «дней» / «день» / «дня» — the ordinary Russian plural for a day count. */
function daysWord(n: number): string {
  const abs = Math.abs(n) % 100;
  const last = abs % 10;
  if (abs > 10 && abs < 20) return 'дней';
  if (last === 1) return 'день';
  if (last >= 2 && last <= 4) return 'дня';
  return 'дней';
}

/** Current year as a form default — a UTC slice, never a local getter. */
function currentYear(): string {
  return new Date().toISOString().slice(0, 4);
}

/**
 * TaxesPage — Task 14. Route: /taxes (guard: confirmed).
 *
 * Three sections behind one page, as tabs: сроки (GET /api/v1/tax/deadlines),
 * расчёты (POST/GET /api/v1/tax/calculations, plus the reference rates from
 * GET /api/v1/tax/rates) and ФНО (POST/GET /api/v1/tax/filings and the two
 * artifacts each filing has). The threshold alerts (GET /api/v1/tax/alerts)
 * sit ABOVE the tabs: "you are about to cross the НДС registration
 * threshold" is not information to hide behind a tab.
 *
 * NOTHING on this page hardcodes a rate, threshold, МРП or МЗП. Every such
 * number is fetched — from GET /tax/rates for the reference table, from the
 * calculation's own `result_snapshot` for its figures, and from the alert's
 * `current_tiyn`/`threshold_tiyn` for the warnings. All of them render as
 * integer tiyn through <Money> (DESIGN.md §7).
 *
 * The alerts' own `message` field is deliberately NOT shown: TaxService
 * composes it in English with raw tiyn integers inlined. The Russian titles
 * in lib/schemas/tax.ts plus the two amounts say the same thing correctly.
 *
 * A calculation selected in «Расчёты» is what «ФНО» files, so the selection
 * lives here rather than in either section — and the form code (910.00 /
 * 300.00) is DERIVED from the calculation's kind, never picked by hand: the
 * server answers a mismatched pair with a 422 `kind_mismatch`.
 */
export function TaxesPage() {
  const [tab, setTab] = useState<TabId>('deadlines');
  const [selectedCalculation, setSelectedCalculation] = useState<TaxCalculation | null>(null);
  const tabRefs = useRef<(HTMLButtonElement | null)[]>([]);

  // Roving tabindex + arrow-key navigation, the same keyboard contract
  // HrOrders' tablist keeps.
  const onTabKeyDown = (event: React.KeyboardEvent, index: number) => {
    if (event.key !== 'ArrowRight' && event.key !== 'ArrowLeft') return;
    event.preventDefault();
    const delta = event.key === 'ArrowRight' ? 1 : -1;
    const next = (index + delta + TABS.length) % TABS.length;
    setTab(TABS[next].id);
    tabRefs.current[next]?.focus();
  };

  return (
    <div className="container mx-auto max-w-6xl py-8 space-y-6">
      <PageHeader
        title="Налоги"
        description="Сроки сдачи и уплаты, расчёты по упрощёнке и НДС, формы налоговой отчётности (ФНО)."
        actions={
          <Button asChild variant="outline">
            <Link to="/journal">Журнал проводок</Link>
          </Button>
        }
      />

      <AlertsSection />

      <Card>
        <CardContent className="pt-6">
          <div role="tablist" aria-label="Разделы налогов" className="flex flex-wrap gap-2">
            {TABS.map((t, index) => (
              <Button
                key={t.id}
                ref={(node) => {
                  tabRefs.current[index] = node;
                }}
                role="tab"
                id={`tax-tab-${t.id}`}
                aria-selected={tab === t.id}
                aria-controls={`tax-panel-${t.id}`}
                tabIndex={tab === t.id ? 0 : -1}
                variant={tab === t.id ? 'default' : 'outline'}
                onClick={() => setTab(t.id)}
                onKeyDown={(event) => onTabKeyDown(event, index)}
              >
                {t.label}
              </Button>
            ))}
          </div>
        </CardContent>
      </Card>

      <div
        role="tabpanel"
        id={`tax-panel-${tab}`}
        aria-labelledby={`tax-tab-${tab}`}
        className="space-y-6"
      >
        {tab === 'deadlines' && <DeadlinesSection />}
        {tab === 'calculations' && (
          <CalculationsSection selected={selectedCalculation} onSelect={setSelectedCalculation} />
        )}
        {tab === 'filings' && (
          <FilingsSection
            calculation={selectedCalculation}
            onOpenCalculations={() => setTab('calculations')}
          />
        )}
      </div>
    </div>
  );
}

// ── Пороговые алерты ────────────────────────────────────────────────────────

function AlertsSection() {
  const alertsQ = useQuery({
    queryKey: qk.tax.alerts(),
    queryFn: () => api.getJson<TaxAlertListResponse>('/api/v1/tax/alerts'),
  });

  const alerts = alertsQ.data?.data ?? [];
  if (alerts.length === 0) return null;

  return (
    <div className="space-y-3">
      {alerts.map((alert) => (
        <Alert key={alert.kind} variant="warning">
          <AlertTitle>{TAX_ALERT_TITLES[alert.kind]}</AlertTitle>
          <AlertDescription>
            <p>
              Доход за последние 12 месяцев — <Money tiyn={alert.current_tiyn} />, порог —{' '}
              <Money tiyn={alert.threshold_tiyn} />.
            </p>
            <p className="text-sm text-muted-foreground">{TAX_ALERT_NOTES[alert.kind]}</p>
          </AlertDescription>
        </Alert>
      ))}
    </div>
  );
}

// ── Сроки ───────────────────────────────────────────────────────────────────

function DeadlinesSection() {
  const deadlinesQ = useQuery({
    queryKey: qk.tax.deadlines(),
    queryFn: () => api.getJson<TaxDeadlineListResponse>('/api/v1/tax/deadlines'),
  });

  const columns: Column<TaxDeadline>[] = [
    { header: 'Форма', className: 'font-mono whitespace-nowrap', cell: (d) => d.form },
    { header: 'Вид', cell: (d) => DEADLINE_KIND_LABELS[d.kind] ?? d.kind },
    { header: 'Период', cell: (d) => d.period_label },
    {
      header: 'Срок',
      className: 'whitespace-nowrap',
      cell: (d) => formatIsoDateRu(d.due_date),
    },
    {
      header: 'Осталось',
      className: 'whitespace-nowrap',
      cell: (d) => {
        const urgency = deadlineUrgency(d.days_left);
        return (
          <StatusBadge
            label={
              d.days_left < 0
                ? `просрочено на ${Math.abs(d.days_left)} ${daysWord(d.days_left)}`
                : d.days_left === 0
                  ? 'сегодня'
                  : `${d.days_left} ${daysWord(d.days_left)}`
            }
            tone={URGENCY_TONE[urgency]}
          />
        );
      },
    },
  ];

  return (
    <Card>
      <CardHeader>
        <CardTitle>Ближайшие сроки</CardTitle>
      </CardHeader>
      <CardContent className="overflow-x-auto">
        <p className="mb-3 text-sm text-muted-foreground">
          Сроки на ближайшие 90 дней. Даты уже перенесены с выходных на ближайший рабочий день;
          праздничные дни не учитываются.
        </p>
        <DataTable
          columns={columns}
          rows={deadlinesQ.data?.data}
          rowKey={(d) => `${d.form}-${d.kind}-${d.due_date}`}
          isLoading={deadlinesQ.isLoading}
          error={deadlinesQ.error}
          emptyText="Ближайших сроков нет."
          rowProps={(d) =>
            deadlineUrgency(d.days_left) === 'normal' ? {} : { className: 'bg-amber-500/5' }
          }
        />
      </CardContent>
    </Card>
  );
}

// ── Расчёты ─────────────────────────────────────────────────────────────────

function CalculationsSection({
  selected,
  onSelect,
}: {
  selected: TaxCalculation | null;
  onSelect: (calculation: TaxCalculation) => void;
}) {
  const toast = useToast();

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.tax.calculations(),
    queryFn: ({ limit, offset }) =>
      api.getJson<TaxCalculationListResponse>('/api/v1/tax/calculations', {
        query: { limit, offset },
      }),
    perPage: PER_PAGE,
  });

  const calculate = useApiMutation(
    (values: TaxCalculationValues) =>
      api.postJson<TaxCalculationDetailResponse>('/api/v1/tax/calculations', {
        body: buildTaxCalculationCreate(values),
      }),
    {
      invalidate: [qk.tax.calculations()],
      onSuccess: (res) => {
        onSelect(res.data);
        setPage(1);
        toast.success('Расчёт выполнен.');
      },
      onError: (message) => toast.error(message),
    },
  );

  const columns: Column<TaxCalculation>[] = [
    { header: 'Вид', cell: (c) => CALCULATION_KIND_LABELS[c.kind] },
    {
      header: 'Период',
      className: 'whitespace-nowrap',
      cell: (c) => `${formatIsoDateRu(c.period_from)} — ${formatIsoDateRu(c.period_to)}`,
    },
    {
      header: 'Рассчитан',
      className: 'whitespace-nowrap',
      cell: (c) => formatIsoDateTimeRu(c.computed_at),
    },
    {
      header: 'Итого',
      className: 'text-right',
      cell: (c) => <Money tiyn={c.total_tiyn} />,
    },
  ];

  return (
    <>
      <Card>
        <CardHeader>
          <CardTitle>Новый расчёт</CardTitle>
        </CardHeader>
        <CardContent>
          <CalculationForm
            submitting={calculate.isPending}
            onSubmit={(values) => calculate.mutate(values)}
          />
        </CardContent>
      </Card>

      {selected && <CalculationResultCard calculation={selected} />}

      <Card>
        <CardHeader>
          <CardTitle>{data ? `Расчётов: ${data.total}` : 'Расчёты'}</CardTitle>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(c) => c.id}
            isLoading={isLoading}
            error={error}
            emptyText="Расчётов пока нет."
            isPlaceholder={isPlaceholderData}
            rowProps={(c) => ({
              onClick: () => onSelect(c),
              'aria-label': `Открыть расчёт: ${CALCULATION_KIND_LABELS[c.kind]}, ${formatIsoDateRu(
                c.period_from,
              )} — ${formatIsoDateRu(c.period_to)}`,
              className: c.id === selected?.id ? 'bg-muted/60' : '',
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

      <RatesCard />
    </>
  );
}

function CalculationForm({
  submitting,
  onSubmit,
}: {
  submitting: boolean;
  onSubmit: (values: TaxCalculationValues) => void;
}) {
  const {
    register,
    control,
    setValue,
    handleSubmit,
    formState: { errors },
  } = useForm<TaxCalculationValues>({
    resolver: zodResolver(taxCalculationSchema),
    defaultValues: { kind: 'snr_simplified', year: currentYear(), ordinal: '1' },
  });
  const kind = (useWatch({ control, name: 'kind' }) ?? 'snr_simplified') as CalculationKind;
  const ordinal = useWatch({ control, name: 'ordinal' }) ?? '1';
  const maxOrdinal = CALCULATION_PERIODS_PER_YEAR[kind];
  const ordinals = Array.from({ length: maxOrdinal }, (_, index) => index + 1);

  // Switching «НДС, IV квартал» to «Упрощёнка» leaves an ordinal the new
  // kind has no option for — the select would then DISPLAY «I полугодие»
  // while the form still held '4'. Snap it back so what is shown is what is
  // submitted (buildTaxCalculationCreate clamps too, but a form must not
  // rely on its builder to be honest).
  useEffect(() => {
    if (Number.parseInt(ordinal, 10) > maxOrdinal) {
      setValue('ordinal', String(maxOrdinal), { shouldValidate: true });
    }
  }, [ordinal, maxOrdinal, setValue]);

  return (
    <form className="space-y-4" onSubmit={handleSubmit(onSubmit)}>
      <p className="text-sm text-muted-foreground">
        Период задаётся отчётным: полугодие для упрощёнки (ФНО 910.00) и квартал для НДС (ФНО
        300.00). Повторный расчёт того же периода заменяет предыдущий результат.
      </p>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
        <div className="space-y-2">
          <Label htmlFor="calc-kind">Вид расчёта</Label>
          <select
            id="calc-kind"
            className="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
            {...register('kind')}
          >
            {CALCULATION_KINDS.map((k) => (
              <option key={k} value={k}>
                {CALCULATION_KIND_LABELS[k]}
              </option>
            ))}
          </select>
        </div>
        <div className="space-y-2">
          <Label htmlFor="calc-ordinal">Отчётный период</Label>
          <select
            id="calc-ordinal"
            className="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
            {...register('ordinal')}
          >
            {ordinals.map((ordinal) => (
              <option key={ordinal} value={ordinal}>
                {calculationPeriodLabel(kind, ordinal)}
              </option>
            ))}
          </select>
          {errors.ordinal?.message && (
            <p className="text-sm text-destructive" role="alert">
              {errors.ordinal.message}
            </p>
          )}
        </div>
        <FormField
          id="calc-year"
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

/** One labelled figure of a calculation snapshot. */
function Figure({ label, tiyn, note }: { label: string; tiyn: number | null; note?: string }) {
  return (
    <div className="space-y-1">
      <p className="text-sm text-muted-foreground">{label}</p>
      <p className="text-base">{tiyn === null ? '—' : <Money tiyn={tiyn} />}</p>
      {note && <p className="text-xs text-muted-foreground">{note}</p>}
    </div>
  );
}

/**
 * The stored `result_snapshot` of the selected calculation, laid out per
 * kind. Every figure is read out of the snapshot rather than recomputed —
 * that snapshot IS the reproducibility record (design spec §7.2), and a key
 * a kind does not record renders as «—» instead of a fabricated 0 ₸.
 */
function CalculationResultCard({ calculation }: { calculation: TaxCalculation }) {
  const snapshot = calculation.result_snapshot;
  const rateBp = snapshotTiyn(snapshot, 'rate_bp');
  const balance = snapshotTiyn(snapshot, 'balance_tiyn');

  return (
    <Card>
      <CardHeader>
        <CardTitle>
          {CALCULATION_KIND_LABELS[calculation.kind]}: {formatIsoDateRu(calculation.period_from)} —{' '}
          {formatIsoDateRu(calculation.period_to)}
        </CardTitle>
      </CardHeader>
      <CardContent className="space-y-4">
        <div className="grid grid-cols-1 gap-4 sm:grid-cols-3">
          {calculation.kind === 'snr_simplified' ? (
            <>
              <Figure label="Доход за период" tiyn={snapshotTiyn(snapshot, 'income_tiyn')} />
              <div className="space-y-1">
                <p className="text-sm text-muted-foreground">Ставка</p>
                <p className="text-base font-mono tabular-nums">
                  {rateBp === null ? '—' : formatRateBpRu(rateBp)}
                </p>
                <p className="text-xs text-muted-foreground">
                  Ставка взята из справочника на дату окончания периода.
                </p>
              </div>
              <Figure label="Налог к уплате" tiyn={snapshotTiyn(snapshot, 'tax_tiyn')} />
            </>
          ) : (
            <>
              <Figure label="НДС начисленный" tiyn={snapshotTiyn(snapshot, 'accrued_tiyn')} />
              <Figure label="НДС к зачёту" tiyn={snapshotTiyn(snapshot, 'deductible_tiyn')} />
              <Figure
                label={balance !== null && balance < 0 ? 'Сальдо: к возврату' : 'Сальдо: к уплате'}
                tiyn={balance === null ? null : Math.abs(balance)}
                note={
                  balance !== null && balance < 0
                    ? 'Отрицательное сальдо — это нормальная позиция к возврату.'
                    : undefined
                }
              />
            </>
          )}
        </div>
        <p className="text-sm text-muted-foreground">
          Рассчитан {formatIsoDateTimeRu(calculation.computed_at)}. Форма отчётности:{' '}
          {FILING_KIND_LABELS[filingKindFor(calculation.kind)]}.
        </p>
      </CardContent>
    </Card>
  );
}

/**
 * The rates and constants in force TODAY, straight from
 * GET /api/v1/tax/rates. This card exists so the screen can show a rate at
 * all: rates, thresholds, МРП and МЗП are versioned reference data and are
 * never written into this codebase.
 */
function RatesCard() {
  const ratesQ = useQuery({
    queryKey: qk.tax.rates(),
    queryFn: () => api.getJson<TaxRatesResponse>('/api/v1/tax/rates'),
  });

  const data = ratesQ.data?.data;

  const rateColumns: Column<NonNullable<typeof data>['rates'][number]>[] = [
    { header: 'Показатель', cell: (r) => RATE_KIND_LABELS[r.kind] ?? r.kind },
    {
      header: 'Ставка',
      className: 'text-right font-mono tabular-nums',
      cell: (r) => formatRateBpRu(r.rate_bp),
    },
    { header: 'Регион', cell: (r) => r.region ?? 'вся страна' },
    {
      header: 'Действует с',
      className: 'whitespace-nowrap',
      cell: (r) => formatIsoDateRu(r.effective_from),
    },
    {
      header: 'по',
      className: 'whitespace-nowrap',
      cell: (r) => (r.effective_to ? formatIsoDateRu(r.effective_to) : 'бессрочно'),
    },
    { header: 'Основание', cell: (r) => r.source_note },
  ];

  const constantColumns: Column<NonNullable<typeof data>['constants'][number]>[] = [
    { header: 'Показатель', cell: (c) => TAX_CONSTANT_LABELS[c.key] ?? c.key },
    {
      header: 'Значение',
      className: 'text-right',
      cell: (c) => <Money tiyn={c.value_tiyn} />,
    },
    {
      header: 'Кратность',
      className: 'text-right font-mono tabular-nums',
      cell: (c) => c.value_units ?? '—',
    },
    {
      header: 'Действует с',
      className: 'whitespace-nowrap',
      cell: (c) => formatIsoDateRu(c.effective_from),
    },
    {
      header: 'по',
      className: 'whitespace-nowrap',
      cell: (c) => (c.effective_to ? formatIsoDateRu(c.effective_to) : 'бессрочно'),
    },
    { header: 'Основание', cell: (c) => c.source_note },
  ];

  return (
    <Card>
      <CardHeader>
        <CardTitle>Ставки и константы{data ? ` на ${formatIsoDateRu(data.on)}` : ''}</CardTitle>
      </CardHeader>
      <CardContent className="space-y-6 overflow-x-auto">
        <DataTable
          columns={rateColumns}
          rows={data?.rates}
          rowKey={(r) => r.id}
          isLoading={ratesQ.isLoading}
          error={ratesQ.error}
          emptyText="Ставок на эту дату нет."
        />
        <DataTable
          columns={constantColumns}
          rows={data?.constants}
          rowKey={(c) => c.id}
          isLoading={ratesQ.isLoading}
          error={ratesQ.error}
          emptyText="Констант на эту дату нет."
        />
      </CardContent>
    </Card>
  );
}

// ── ФНО ─────────────────────────────────────────────────────────────────────

/** A filing this tab is currently showing artifacts for. */
interface FocusedFiling {
  id: string;
  /** Only a filing created in THIS tab knows whether a render was enqueued. */
  renderQueued: boolean;
}

function FilingsSection({
  calculation,
  onOpenCalculations,
}: {
  calculation: TaxCalculation | null;
  onOpenCalculations: () => void;
}) {
  const toast = useToast();
  const [focused, setFocused] = useState<FocusedFiling | null>(null);
  const [formError, setFormError] = useState<string | null>(null);

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.tax.filings(),
    queryFn: ({ limit, offset }) =>
      api.getJson<TaxFilingListResponse>('/api/v1/tax/filings', { query: { limit, offset } }),
    perPage: PER_PAGE,
  });

  const create = useApiMutation(
    (vars: { calculation: TaxCalculation; documentInput: Record<string, unknown> }) =>
      api.postJson<TaxFilingCreateResponse>('/api/v1/tax/filings', {
        body: buildTaxFilingCreate(vars.calculation, vars.documentInput),
      }),
    {
      invalidate: [qk.tax.filings(), qk.documents.all()],
      onSuccess: (res) => {
        setFormError(null);
        setPage(1);
        setFocused({ id: res.filing_id, renderQueued: res.render_queued });
        if (!res.xml_ready) {
          toast.error('Форма создана, но XML не удалось сохранить в хранилище.');
        } else {
          toast.success('ФНО сформирована.');
        }
      },
      onError: (message, err) => {
        // A 422 here is the ФНО template's own schema check over the
        // free-text fields below (or a kind/organization mismatch) — it
        // belongs next to the form, not in an anonymous toast.
        if (err instanceof ApiClientError && err.status === 422) {
          setFormError(apiErrorMessage(err, 'Не удалось сформировать ФНО.'));
          return;
        }
        toast.error(message);
      },
    },
  );

  const columns: Column<TaxFiling>[] = [
    { header: 'Форма', className: 'font-mono whitespace-nowrap', cell: (f) => f.kind },
    {
      header: 'Период',
      className: 'whitespace-nowrap',
      cell: (f) => `${formatIsoDateRu(f.period_from)} — ${formatIsoDateRu(f.period_to)}`,
    },
    {
      header: 'Статус',
      cell: (f) => (
        <StatusBadge label={FILING_STATUS_LABELS[f.status]} tone={FILING_STATUS_TONE[f.status]} />
      ),
    },
    {
      header: 'Сформирована',
      className: 'whitespace-nowrap',
      cell: (f) => formatIsoDateTimeRu(f.created_at),
    },
  ];

  return (
    <>
      <Card>
        <CardHeader>
          <CardTitle>Сформировать ФНО</CardTitle>
        </CardHeader>
        <CardContent>
          {calculation ? (
            <FilingForm
              calculation={calculation}
              submitting={create.isPending}
              serverError={formError}
              onSubmit={(documentInput) => create.mutate({ calculation, documentInput })}
            />
          ) : (
            <EmptyState
              icon={Calculator}
              title="Сначала выберите расчёт"
              description="ФНО формируется из сохранённого расчёта: 910.00 — из упрощёнки, 300.00 — из НДС."
              action={
                <Button variant="outline" onClick={onOpenCalculations}>
                  Перейти к расчётам
                </Button>
              }
            />
          )}
        </CardContent>
      </Card>

      {focused && (
        <FilingArtifactsCard
          filingId={focused.id}
          renderQueued={focused.renderQueued}
          onDismiss={() => setFocused(null)}
        />
      )}

      <Card>
        <CardHeader>
          <CardTitle>{data ? `Форм ФНО: ${data.total}` : 'Сформированные ФНО'}</CardTitle>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(f) => f.id}
            isLoading={isLoading}
            error={error}
            emptyText="Форм ФНО пока нет."
            isPlaceholder={isPlaceholderData}
            rowProps={(f) => ({
              // A filing opened from the list was created earlier, possibly
              // in another session, so nothing here knows whether its render
              // job was enqueued — renderQueued: false means "do not poll",
              // and the PDF download says so plainly if it is not ready.
              onClick: () => setFocused({ id: f.id, renderQueued: false }),
              'aria-label': `Открыть файлы формы ${f.kind} за ${formatIsoDateRu(
                f.period_from,
              )} — ${formatIsoDateRu(f.period_to)}`,
              className: f.id === focused?.id ? 'bg-muted/60' : '',
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
    </>
  );
}

/**
 * The free-text fields the ФНО print templates require and no column holds
 * — see lib/schemas/tax.ts. Which set is asked for follows from the
 * calculation's kind, exactly as the form code does.
 */
function FilingForm({
  calculation,
  submitting,
  serverError,
  onSubmit,
}: {
  calculation: TaxCalculation;
  submitting: boolean;
  serverError: string | null;
  onSubmit: (documentInput: Record<string, unknown>) => void;
}) {
  const filingKind = filingKindFor(calculation.kind);
  const period = `${formatIsoDateRu(calculation.period_from)} — ${formatIsoDateRu(
    calculation.period_to,
  )}`;

  const form910 = useForm<Fno910DocumentValues>({
    resolver: zodResolver(fno910DocumentSchema),
    defaultValues: { director: '', accountant: '', tax_words: '' },
  });
  const form300 = useForm<Fno300DocumentValues>({
    resolver: zodResolver(fno300DocumentSchema),
    defaultValues: { director: '', accountant: '', balance_words: '' },
  });

  const intro = (
    <>
      <p className="text-sm">
        <span className="font-medium">{FILING_KIND_LABELS[filingKind]}</span> за {period}.
      </p>
      <p className="text-sm text-muted-foreground">
        БИН и наименование организации, период и все суммы подставляются из расчёта — заполните
        только то, чего нет в системе.
      </p>
    </>
  );

  const errorBlock = serverError && (
    <p className="text-sm text-destructive" role="alert">
      {serverError}
    </p>
  );

  const buttons = (
    <Button type="submit" disabled={submitting}>
      {submitting ? 'Формирование…' : 'Сформировать'}
    </Button>
  );

  if (calculation.kind === 'snr_simplified') {
    return (
      <form
        className="space-y-4"
        onSubmit={form910.handleSubmit((values) => onSubmit(buildFno910DocumentInput(values)))}
      >
        {intro}
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
          <FormField
            id="fno910-director"
            label="Руководитель (ФИО)"
            error={form910.formState.errors.director?.message}
            {...form910.register('director')}
          />
          <FormField
            id="fno910-accountant"
            label="Главный бухгалтер (ФИО)"
            error={form910.formState.errors.accountant?.message}
            {...form910.register('accountant')}
          />
        </div>
        <FormField
          id="fno910-tax-words"
          label="Сумма налога прописью"
          placeholder="сто двадцать тысяч тенге 00 тиын"
          error={form910.formState.errors.tax_words?.message}
          {...form910.register('tax_words')}
        />
        {errorBlock}
        {buttons}
      </form>
    );
  }

  return (
    <form
      className="space-y-4"
      onSubmit={form300.handleSubmit((values) => onSubmit(buildFno300DocumentInput(values)))}
    >
      {intro}
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        <FormField
          id="fno300-director"
          label="Руководитель (ФИО)"
          error={form300.formState.errors.director?.message}
          {...form300.register('director')}
        />
        <FormField
          id="fno300-accountant"
          label="Главный бухгалтер (ФИО)"
          error={form300.formState.errors.accountant?.message}
          {...form300.register('accountant')}
        />
      </div>
      <FormField
        id="fno300-balance-words"
        label="Сумма к уплате (возврату) прописью"
        placeholder="сто двадцать тысяч тенге 00 тиын"
        error={form300.formState.errors.balance_words?.message}
        {...form300.register('balance_words')}
      />
      {errorBlock}
      {buttons}
    </form>
  );
}

/**
 * A filing's two artifacts. They live in DIFFERENT places and are ready at
 * DIFFERENT times: the XML is written into object storage synchronously by
 * POST /tax/filings, while the printable form is a docgen document rendered
 * by a background job — so the PDF is polled (bounded, useDocumentRender)
 * and both downloads go through the filing's own presigned-URL endpoint.
 * The URL it returns is used as-is; storage keys are never assembled here.
 *
 * `schema_validated` is false for every filing KGD publishes no content XSD
 * for — which today is all of them — so the banner is unconditional in
 * practice and honest about what was and was not checked.
 */
function FilingArtifactsCard({
  filingId,
  renderQueued,
  onDismiss,
}: {
  filingId: string;
  renderQueued: boolean;
  onDismiss: () => void;
}) {
  const toast = useToast();
  const filingQ = useQuery({
    queryKey: [...qk.tax.filings(), 'detail', filingId],
    queryFn: () => api.getJson<TaxFilingDetailResponse>(`/api/v1/tax/filings/${filingId}`),
  });
  const filing = filingQ.data?.data;
  const render = useDocumentRender(filing?.document_id, renderQueued);

  const download = useApiMutation(
    (artifact: 'xml' | 'pdf') =>
      api.postJson<FilingDownloadUrlResponse>(`/api/v1/tax/filings/${filingId}/download-url`, {
        query: { artifact },
      }),
    {
      onSuccess: (res) => window.open(res.url, '_blank', 'noopener,noreferrer'),
      onError: (message, err) => {
        if (err instanceof ApiClientError && err.status === 409) {
          toast.error(
            err.code === 'no_xml'
              ? 'XML этой формы не сохранён в хранилище — сформируйте её заново.'
              : 'Печатная форма ещё не готова — попробуйте через несколько секунд.',
          );
          return;
        }
        toast.error(message);
      },
    },
  );

  return (
    <div className="space-y-3">
      {filing && !filing.schema_validated && (
        <Alert variant="warning">
          <AlertTitle>Схема не валидирована</AlertTitle>
          <AlertDescription>
            КГД не публикует XSD-схему содержимого ни для одной формы ФНО, поэтому сформированный
            XML не проверен по схеме. Перед отправкой загрузите его в кабинет налогоплательщика и
            проверьте там.
          </AlertDescription>
        </Alert>
      )}
      {filing && !filing.xml_s3_key && (
        <Alert variant="warning">
          <AlertTitle>XML не сохранён</AlertTitle>
          <AlertDescription>
            Форма зарегистрирована, но её XML не попал в хранилище. Сформируйте ФНО заново, чтобы
            получить файл.
          </AlertDescription>
        </Alert>
      )}

      <Card>
        <CardHeader className="flex flex-row items-center justify-between space-y-0">
          <CardTitle>{filing ? `${FILING_KIND_LABELS[filing.kind]}` : 'Файлы формы ФНО'}</CardTitle>
          <Button variant="ghost" size="sm" onClick={onDismiss}>
            Скрыть
          </Button>
        </CardHeader>
        <CardContent className="space-y-3 text-sm">
          {filingQ.isLoading && <p className="text-muted-foreground">Загрузка формы…</p>}
          {filing && (
            <>
              <p>
                Период: {formatIsoDateRu(filing.period_from)} — {formatIsoDateRu(filing.period_to)}.
                Статус:{' '}
                <StatusBadge
                  label={FILING_STATUS_LABELS[filing.status]}
                  tone={FILING_STATUS_TONE[filing.status]}
                />
              </p>
              {render.rendering && (
                <p className="text-muted-foreground">
                  Печатная форма формируется, статус обновится автоматически…
                </p>
              )}
              {render.stuck && (
                <div className="space-y-2">
                  <p className="text-muted-foreground">
                    Формирование печатной формы занимает дольше обычного — проверьте статус позже.
                  </p>
                  <Button
                    size="sm"
                    variant="outline"
                    onClick={() => render.refetch()}
                    disabled={render.isFetching}
                  >
                    Обновить
                  </Button>
                </div>
              )}
              {!renderQueued && filing.document_id && (
                <p className="text-muted-foreground">
                  Статус печатной формы не отслеживается для ранее созданных форм — просто
                  попробуйте её скачать.
                </p>
              )}
              <div className="flex flex-wrap gap-2">
                <Button
                  size="sm"
                  onClick={() => download.mutate('xml')}
                  disabled={download.isPending || !filing.xml_s3_key}
                >
                  Скачать XML
                </Button>
                <Button
                  size="sm"
                  variant="outline"
                  onClick={() => download.mutate('pdf')}
                  disabled={download.isPending || !filing.document_id || render.rendering}
                >
                  Скачать печатную форму
                </Button>
              </div>
              {filing.document_id && (
                <p className="text-muted-foreground">
                  Печатная форма также доступна в разделе{' '}
                  <Link
                    to={`/documents?focus=${filing.document_id}&queued=1`}
                    className="underline"
                  >
                    Документы
                  </Link>
                  .
                </p>
              )}
            </>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
