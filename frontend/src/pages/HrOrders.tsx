import { useEffect, useMemo, useRef, useState } from 'react';
import { useForm, useWatch } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Link } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';
import { UsersRound } from 'lucide-react';

import { DataTable, type Column } from '@/components/DataTable';
import { EmptyState } from '@/components/EmptyState';
import { FormField } from '@/components/FormField';
import { GeneratedDocumentCard, type GeneratedDocument } from '@/components/GeneratedDocumentCard';
import { PageHeader } from '@/components/PageHeader';
import { PaginationFooter } from '@/components/PaginationFooter';
import { StatusBadge } from '@/components/StatusBadge';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Label } from '@/components/ui/label';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { ApiClientError, api, apiErrorMessage } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type {
  Employee,
  EmployeeListResponse,
  GenerateDocumentResponse,
  GenerateHrDocumentExtra,
  HrOrder,
  HrOrderDetailResponse,
  HrOrderListResponse,
  LaborContract,
  LaborContractDetailResponse,
  LaborContractListResponse,
  Vacation,
  VacationDetailResponse,
  VacationListResponse,
} from '@/lib/api/types';
import { formatIsoDateRu } from '@/lib/dateFormat';
import {
  buildHrOrderCreate,
  buildHrOrderDocumentExtra,
  buildLaborContractCreate,
  buildLaborContractDocumentExtra,
  buildVacationCreate,
  employeeFullName,
  hrOrderDocumentSchema,
  hrOrderSchema,
  laborContractDocumentSchema,
  laborContractSchema,
  vacationSchema,
  HR_ORDER_KINDS,
  HR_ORDER_KIND_LABELS,
  HR_ORDER_PAYLOAD_FIELDS,
  VACATION_KINDS,
  VACATION_KIND_LABELS,
  type HrOrderDocumentValues,
  type HrOrderValues,
  type LaborContractDocumentValues,
  type LaborContractValues,
  type VacationValues,
} from '@/lib/schemas/hr';

// Unpaginated employee roster for the filter/selects — same rationale as
// Journal.tsx's counterparty select: a P2 organization's staff list is
// small enough that one 200-row fetch beats a searchable combobox.
const EMPLOYEE_FETCH_LIMIT = 200;

// Приказы and отпуска are paginated (GET .../hr-orders and .../vacations,
// both parse_page_params(50, 200)) — same PER_PAGE as every other paginated
// list page (Employees.tsx, Journal.tsx, Documents.tsx, ...).
const PER_PAGE = 20;

const TABS = [
  { id: 'orders', label: 'Приказы' },
  { id: 'contracts', label: 'Трудовые договоры' },
  { id: 'vacations', label: 'Отпуска' },
] as const;
type TabId = (typeof TABS)[number]['id'];

/**
 * HrOrdersPage — Task 13. Route: /hr (guard: confirmed).
 *
 * Three кадровые sections behind one employee filter, as tabs on a single
 * page: приказы (GET/POST /api/v1/hr-orders), трудовые договоры
 * (GET/POST /api/v1/labor-contracts) and отпуска (GET/POST
 * /api/v1/vacations). Приказы and отпуска are paginated
 * (parse_page_params(50, 200)) and use the same usePagedQuery +
 * PaginationFooter pattern as every other list page. Трудовые договоры
 * stays unpaginated — GET /labor-contracts always scopes to one employee_id,
 * so its result set is small by construction (docs/openapi.yaml).
 *
 * The employee filter is optional for приказы and отпуска but REQUIRED for
 * трудовые договоры — `GET /labor-contracts` has no "every employee" mode
 * (HrController.hpp) — so that tab shows an EmptyState prompting for an
 * employee instead of firing a request that would 400.
 *
 * «Сформировать документ» asks only for the free-text fields the LaTeX
 * template needs and that the database has no column for (руководитель,
 * режим работы, адреса). Everything else — ФИО, ИИН, должность, оклад и
 * оклад прописью, реквизиты организации — is derived server-side and merged
 * underneath, so this form never echoes a server-owned value back (see
 * lib/schemas/hr.ts). A 422 from that endpoint is a validation failure of
 * these fields, so it is rendered inline under the form rather than thrown
 * at the user as an anonymous toast.
 */
export function HrOrdersPage() {
  const [tab, setTab] = useState<TabId>('orders');
  const [employeeId, setEmployeeId] = useState('');
  const tabRefs = useRef<(HTMLButtonElement | null)[]>([]);

  const employeesQ = useQuery({
    queryKey: qk.employees.all(),
    queryFn: () =>
      api.getJson<EmployeeListResponse>('/api/v1/employees', {
        query: { limit: EMPLOYEE_FETCH_LIMIT, offset: 0 },
      }),
  });
  const employees = useMemo(
    () =>
      [...(employeesQ.data?.data ?? [])].sort((a, b) =>
        employeeFullName(a).localeCompare(employeeFullName(b)),
      ),
    [employeesQ.data],
  );
  const employeeName = useMemo(() => {
    const map = new Map(employees.map((e) => [e.id, employeeFullName(e)]));
    return (id: string) => map.get(id) ?? '—';
  }, [employees]);

  // Roving tabindex + arrow-key navigation — the keyboard contract a
  // role="tablist" promises.
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
        title="Кадры"
        description="Приказы, трудовые договоры и отпуска сотрудников организации."
        actions={
          <Button asChild variant="outline">
            <Link to="/employees">Сотрудники</Link>
          </Button>
        }
      />

      <Card>
        <CardContent className="space-y-4 pt-6">
          <div className="space-y-1">
            <Label htmlFor="hr-employee">Сотрудник</Label>
            <select
              id="hr-employee"
              className="flex h-10 w-full max-w-md rounded-md border border-input bg-background px-3 py-2 text-sm"
              disabled={employeesQ.isLoading}
              value={employeeId}
              onChange={(e) => setEmployeeId(e.target.value)}
            >
              <option value="">{employeesQ.isLoading ? 'Загрузка…' : 'Все сотрудники'}</option>
              {employees.map((e) => (
                <option key={e.id} value={e.id}>
                  {employeeFullName(e)}
                </option>
              ))}
            </select>
            <p className="text-sm text-muted-foreground">
              Для трудовых договоров выбор сотрудника обязателен.
            </p>
          </div>

          <div role="tablist" aria-label="Разделы кадрового учёта" className="flex flex-wrap gap-2">
            {TABS.map((t, index) => (
              <Button
                key={t.id}
                ref={(node) => {
                  tabRefs.current[index] = node;
                }}
                role="tab"
                id={`hr-tab-${t.id}`}
                aria-selected={tab === t.id}
                aria-controls={`hr-panel-${t.id}`}
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
        id={`hr-panel-${tab}`}
        aria-labelledby={`hr-tab-${tab}`}
        className="space-y-6"
      >
        {tab === 'orders' && (
          <OrdersSection
            employees={employees}
            employeeId={employeeId}
            employeeName={employeeName}
          />
        )}
        {tab === 'contracts' && <ContractsSection employees={employees} employeeId={employeeId} />}
        {tab === 'vacations' && (
          <VacationsSection
            employees={employees}
            employeeId={employeeId}
            employeeName={employeeName}
          />
        )}
      </div>
    </div>
  );
}

// ── Shared building blocks ──────────────────────────────────────────────────

function EmployeeSelect({
  id,
  employees,
  value,
  onChange,
  error,
  label = 'Сотрудник',
}: {
  id: string;
  employees: Employee[];
  value: string;
  onChange: (id: string) => void;
  error?: string;
  label?: string;
}) {
  return (
    <div className="space-y-2">
      <Label htmlFor={id}>{label}</Label>
      <select
        id={id}
        className="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
        value={value}
        onChange={(e) => onChange(e.target.value)}
        aria-invalid={!!error}
        aria-describedby={error ? `${id}-error` : undefined}
      >
        <option value="">Выберите сотрудника…</option>
        {employees.map((e) => (
          <option key={e.id} value={e.id}>
            {employeeFullName(e)}
          </option>
        ))}
      </select>
      {error && (
        <p id={`${id}-error`} className="text-sm text-destructive" role="alert">
          {error}
        </p>
      )}
    </div>
  );
}

/**
 * "Документ также доступен в разделе Документы" — the same footer both
 * кадровые sections put under the shared GeneratedDocumentCard.
 */
function DocumentsSectionHint({ documentId }: { documentId: string }) {
  return (
    <p className="text-muted-foreground">
      Документ также доступен в разделе{' '}
      <Link to={`/documents?focus=${documentId}&queued=1`} className="underline">
        Документы
      </Link>
      .
    </p>
  );
}

// ── Приказы ─────────────────────────────────────────────────────────────────

function OrdersSection({
  employees,
  employeeId,
  employeeName,
}: {
  employees: Employee[];
  employeeId: string;
  employeeName: (id: string) => string;
}) {
  const toast = useToast();
  const [creating, setCreating] = useState(false);
  const [generatingFor, setGeneratingFor] = useState<HrOrder | null>(null);
  const [generated, setGenerated] = useState<GeneratedDocument | null>(null);
  const [generateError, setGenerateError] = useState<string | null>(null);

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.hr.orders(employeeId),
    queryFn: ({ limit, offset }) =>
      api.getJson<HrOrderListResponse>('/api/v1/hr-orders', {
        query: employeeId ? { employee_id: employeeId, limit, offset } : { limit, offset },
      }),
    perPage: PER_PAGE,
  });

  // Switching the employee filter changes the result set — land back on
  // page 1 rather than risk requesting an offset past the new total.
  useEffect(() => {
    setPage(1);
  }, [employeeId, setPage]);

  const create = useApiMutation(
    (values: HrOrderValues) =>
      api.postJson<HrOrderDetailResponse>('/api/v1/hr-orders', {
        body: buildHrOrderCreate(values),
      }),
    {
      invalidate: [qk.hr.orders()],
      onSuccess: () => {
        setCreating(false);
        toast.success('Приказ создан.');
      },
    },
  );
  useErrorToast(create.error);

  const generate = useApiMutation(
    (vars: { id: string; extra: GenerateHrDocumentExtra }) =>
      api.postJson<GenerateDocumentResponse>(`/api/v1/hr-orders/${vars.id}/generate-document`, {
        body: vars.extra,
      }),
    {
      invalidate: [qk.hr.orders(), qk.documents.all()],
      onSuccess: (data) => {
        setGeneratingFor(null);
        setGenerateError(null);
        setGenerated({ documentId: data.document_id, renderQueued: data.render_queued });
        if (data.render_queued) toast.success('Документ поставлен в очередь на генерацию.');
      },
      onError: (message, err) => {
        // A 422 here is a field-level failure of the free-text fields above
        // (a missing required one, or — under the strict allowlist — a field
        // the template does not accept), so it belongs next to the form.
        if (err instanceof ApiClientError && err.status === 422) {
          setGenerateError(apiErrorMessage(err, 'Не удалось сформировать документ.'));
          return;
        }
        toast.error(message);
      },
    },
  );

  const columns: Column<HrOrder>[] = [
    {
      header: 'Вид приказа',
      cell: (o) => <StatusBadge label={HR_ORDER_KIND_LABELS[o.kind] ?? o.kind} />,
    },
    { header: 'Номер', className: 'font-mono', cell: (o) => o.number },
    { header: 'Сотрудник', cell: (o) => employeeName(o.employee_id) },
    { header: 'Издан', className: 'whitespace-nowrap', cell: (o) => formatIsoDateRu(o.issued_on) },
    {
      header: 'Действует с',
      className: 'whitespace-nowrap',
      cell: (o) => formatIsoDateRu(o.effective_from),
    },
    {
      header: 'по',
      className: 'whitespace-nowrap',
      cell: (o) => formatIsoDateRu(o.effective_to),
    },
    {
      header: '',
      className: 'text-right',
      cell: (o) => (
        <Button
          size="sm"
          variant="outline"
          onClick={() => {
            setGenerateError(null);
            setGenerated(null);
            setGeneratingFor(o);
          }}
        >
          Сформировать документ
        </Button>
      ),
    },
  ];

  return (
    <>
      <Card>
        <CardHeader className="flex flex-row items-center justify-between space-y-0">
          <CardTitle>Приказы</CardTitle>
          <Button onClick={() => setCreating((v) => !v)}>
            {creating ? 'Закрыть' : 'Новый приказ'}
          </Button>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(o) => o.id}
            isLoading={isLoading}
            error={error}
            emptyText="Приказов пока нет."
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
            <HrOrderForm
              employees={employees}
              defaultEmployeeId={employeeId}
              submitting={create.isPending}
              onSubmit={(values) => create.mutate(values)}
              onCancel={() => setCreating(false)}
            />
          </CardContent>
        )}
      </Card>

      {generatingFor && (
        <Card>
          <CardHeader>
            <CardTitle>
              Документ по приказу № {generatingFor.number} —{' '}
              {HR_ORDER_KIND_LABELS[generatingFor.kind] ?? generatingFor.kind}
            </CardTitle>
          </CardHeader>
          <CardContent>
            <HrOrderDocumentForm
              submitting={generate.isPending}
              serverError={generateError}
              onSubmit={(values) =>
                generate.mutate({
                  id: generatingFor.id,
                  extra: buildHrOrderDocumentExtra(values),
                })
              }
              onCancel={() => {
                setGeneratingFor(null);
                setGenerateError(null);
              }}
            />
          </CardContent>
        </Card>
      )}

      {generated && (
        <GeneratedDocumentCard
          documentId={generated.documentId}
          renderQueued={generated.renderQueued}
          onDismiss={() => setGenerated(null)}
        >
          <DocumentsSectionHint documentId={generated.documentId} />
        </GeneratedDocumentCard>
      )}
    </>
  );
}

function HrOrderForm({
  employees,
  defaultEmployeeId,
  submitting,
  onSubmit,
  onCancel,
}: {
  employees: Employee[];
  defaultEmployeeId: string;
  submitting: boolean;
  onSubmit: (values: HrOrderValues) => void;
  onCancel: () => void;
}) {
  const {
    register,
    control,
    setValue,
    handleSubmit,
    formState: { errors },
  } = useForm<HrOrderValues>({
    resolver: zodResolver(hrOrderSchema),
    defaultValues: {
      employee_id: defaultEmployeeId,
      kind: 'hire',
      number: '',
      issued_on: '',
      effective_from: '',
      effective_to: '',
      payload: {},
    },
  });
  const kind = useWatch({ control, name: 'kind' }) ?? 'hire';
  const selectedEmployeeId = useWatch({ control, name: 'employee_id' }) ?? '';

  return (
    <form className="space-y-4" onSubmit={handleSubmit(onSubmit)}>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        <EmployeeSelect
          id="order-employee"
          employees={employees}
          value={selectedEmployeeId}
          onChange={(id) => setValue('employee_id', id, { shouldValidate: true })}
          error={errors.employee_id?.message}
        />
        <div className="space-y-2">
          <Label htmlFor="order-kind">Вид приказа</Label>
          <select
            id="order-kind"
            className="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
            {...register('kind')}
          >
            {HR_ORDER_KINDS.map((k) => (
              <option key={k} value={k}>
                {HR_ORDER_KIND_LABELS[k]}
              </option>
            ))}
          </select>
        </div>
      </div>

      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        <FormField
          id="order-number"
          label="Номер приказа"
          error={errors.number?.message}
          {...register('number')}
        />
        <FormField
          id="order-issued-on"
          label="Дата издания"
          type="date"
          error={errors.issued_on?.message}
          {...register('issued_on')}
        />
      </div>

      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        <FormField
          id="order-effective-from"
          label="Действует с"
          type="date"
          error={errors.effective_from?.message}
          {...register('effective_from')}
        />
        <FormField
          id="order-effective-to"
          label="Действует по (необязательно)"
          type="date"
          error={errors.effective_to?.message}
          {...register('effective_to')}
        />
      </div>

      <fieldset className="space-y-3 rounded-md border border-border p-3">
        <legend className="px-1 text-sm font-medium">
          Данные приказа: {HR_ORDER_KIND_LABELS[kind]}
        </legend>
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
          {HR_ORDER_PAYLOAD_FIELDS[kind].map((field) => (
            <FormField
              key={`${kind}-${field.name}`}
              id={`order-payload-${field.name}`}
              label={field.label}
              placeholder={field.placeholder}
              {...register(`payload.${field.name}`)}
            />
          ))}
        </div>
      </fieldset>

      <div className="flex gap-2">
        <Button type="submit" disabled={submitting}>
          {submitting ? 'Сохранение…' : 'Создать приказ'}
        </Button>
        <Button type="button" variant="ghost" onClick={onCancel}>
          Отмена
        </Button>
      </div>
    </form>
  );
}

function HrOrderDocumentForm({
  submitting,
  serverError,
  onSubmit,
  onCancel,
}: {
  submitting: boolean;
  serverError: string | null;
  onSubmit: (values: HrOrderDocumentValues) => void;
  onCancel: () => void;
}) {
  const {
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<HrOrderDocumentValues>({
    resolver: zodResolver(hrOrderDocumentSchema),
    defaultValues: { director: '', reason: '', details: '' },
  });

  return (
    <form className="space-y-4" onSubmit={handleSubmit(onSubmit)}>
      <p className="text-sm text-muted-foreground">
        ФИО и ИИН сотрудника, должность и реквизиты организации подставляются автоматически —
        заполните только то, чего нет в системе.
      </p>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        <FormField
          id="order-doc-director"
          label="Руководитель (ФИО, должность)"
          placeholder="Директор Смирнов С.С."
          error={errors.director?.message}
          {...register('director')}
        />
        <FormField
          id="order-doc-reason"
          label="Основание (необязательно)"
          error={errors.reason?.message}
          {...register('reason')}
        />
      </div>
      <FormField
        id="order-doc-details"
        label="Дополнительные сведения (необязательно)"
        error={errors.details?.message}
        {...register('details')}
      />
      {serverError && (
        <p className="text-sm text-destructive" role="alert">
          {serverError}
        </p>
      )}
      <div className="flex gap-2">
        <Button type="submit" disabled={submitting}>
          {submitting ? 'Формирование…' : 'Сформировать документ'}
        </Button>
        <Button type="button" variant="ghost" onClick={onCancel}>
          Отмена
        </Button>
      </div>
    </form>
  );
}

// ── Трудовые договоры ───────────────────────────────────────────────────────

function ContractsSection({
  employees,
  employeeId,
}: {
  employees: Employee[];
  employeeId: string;
}) {
  const toast = useToast();
  const [creating, setCreating] = useState(false);
  const [generatingFor, setGeneratingFor] = useState<LaborContract | null>(null);
  const [generated, setGenerated] = useState<GeneratedDocument | null>(null);
  const [generateError, setGenerateError] = useState<string | null>(null);

  const contractsQ = useQuery({
    queryKey: qk.hr.contracts(employeeId),
    // GET /labor-contracts REQUIRES employee_id (HrController.hpp), so the
    // query simply does not run until one is chosen.
    enabled: employeeId !== '',
    queryFn: () =>
      api.getJson<LaborContractListResponse>('/api/v1/labor-contracts', {
        query: { employee_id: employeeId },
      }),
  });

  const create = useApiMutation(
    (values: LaborContractValues) =>
      api.postJson<LaborContractDetailResponse>('/api/v1/labor-contracts', {
        body: buildLaborContractCreate(values),
      }),
    {
      invalidate: [qk.hr.contracts()],
      onSuccess: () => {
        setCreating(false);
        toast.success('Трудовой договор создан.');
      },
    },
  );
  useErrorToast(create.error);

  const generate = useApiMutation(
    (vars: { id: string; extra: GenerateHrDocumentExtra }) =>
      api.postJson<GenerateDocumentResponse>(
        `/api/v1/labor-contracts/${vars.id}/generate-document`,
        { body: vars.extra },
      ),
    {
      invalidate: [qk.documents.all()],
      onSuccess: (data) => {
        setGeneratingFor(null);
        setGenerateError(null);
        setGenerated({ documentId: data.document_id, renderQueued: data.render_queued });
        if (data.render_queued) toast.success('Документ поставлен в очередь на генерацию.');
      },
      onError: (message, err) => {
        if (err instanceof ApiClientError && err.status === 422) {
          setGenerateError(apiErrorMessage(err, 'Не удалось сформировать документ.'));
          return;
        }
        toast.error(message);
      },
    },
  );

  const columns: Column<LaborContract>[] = [
    { header: 'Номер', className: 'font-mono', cell: (c) => c.number },
    {
      header: 'Подписан',
      className: 'whitespace-nowrap',
      cell: (c) => formatIsoDateRu(c.signed_on),
    },
    {
      header: 'Действует с',
      className: 'whitespace-nowrap',
      cell: (c) => formatIsoDateRu(c.starts_on),
    },
    {
      header: 'по',
      className: 'whitespace-nowrap',
      cell: (c) => (c.ends_on ? formatIsoDateRu(c.ends_on) : 'бессрочно'),
    },
    {
      header: '',
      className: 'text-right',
      cell: (c) => (
        <Button
          size="sm"
          variant="outline"
          onClick={() => {
            setGenerateError(null);
            setGenerated(null);
            setGeneratingFor(c);
          }}
        >
          Сформировать документ
        </Button>
      ),
    },
  ];

  if (!employeeId) {
    return (
      <Card>
        <CardContent className="pt-6">
          <EmptyState
            icon={UsersRound}
            title="Выберите сотрудника"
            description="Трудовые договоры показываются по одному сотруднику — выберите его в фильтре выше."
          />
        </CardContent>
      </Card>
    );
  }

  return (
    <>
      <Card>
        <CardHeader className="flex flex-row items-center justify-between space-y-0">
          <CardTitle>Трудовые договоры</CardTitle>
          <Button onClick={() => setCreating((v) => !v)}>
            {creating ? 'Закрыть' : 'Новый договор'}
          </Button>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={contractsQ.data?.data}
            rowKey={(c) => c.id}
            isLoading={contractsQ.isLoading}
            error={contractsQ.error}
            emptyText="Трудовых договоров у этого сотрудника пока нет."
          />
        </CardContent>
        {creating && (
          <CardContent className="border-t pt-6">
            <LaborContractForm
              employees={employees}
              defaultEmployeeId={employeeId}
              submitting={create.isPending}
              onSubmit={(values) => create.mutate(values)}
              onCancel={() => setCreating(false)}
            />
          </CardContent>
        )}
      </Card>

      {generatingFor && (
        <Card>
          <CardHeader>
            <CardTitle>Документ по договору № {generatingFor.number}</CardTitle>
          </CardHeader>
          <CardContent>
            <LaborContractDocumentForm
              submitting={generate.isPending}
              serverError={generateError}
              onSubmit={(values) =>
                generate.mutate({
                  id: generatingFor.id,
                  extra: buildLaborContractDocumentExtra(values),
                })
              }
              onCancel={() => {
                setGeneratingFor(null);
                setGenerateError(null);
              }}
            />
          </CardContent>
        </Card>
      )}

      {generated && (
        <GeneratedDocumentCard
          documentId={generated.documentId}
          renderQueued={generated.renderQueued}
          onDismiss={() => setGenerated(null)}
        >
          <DocumentsSectionHint documentId={generated.documentId} />
        </GeneratedDocumentCard>
      )}
    </>
  );
}

function LaborContractForm({
  employees,
  defaultEmployeeId,
  submitting,
  onSubmit,
  onCancel,
}: {
  employees: Employee[];
  defaultEmployeeId: string;
  submitting: boolean;
  onSubmit: (values: LaborContractValues) => void;
  onCancel: () => void;
}) {
  const {
    register,
    control,
    setValue,
    handleSubmit,
    formState: { errors },
  } = useForm<LaborContractValues>({
    resolver: zodResolver(laborContractSchema),
    defaultValues: {
      employee_id: defaultEmployeeId,
      number: '',
      signed_on: '',
      starts_on: '',
      ends_on: '',
    },
  });
  const selectedEmployeeId = useWatch({ control, name: 'employee_id' }) ?? '';

  return (
    <form className="space-y-4" onSubmit={handleSubmit(onSubmit)}>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        <EmployeeSelect
          id="contract-employee"
          employees={employees}
          value={selectedEmployeeId}
          onChange={(id) => setValue('employee_id', id, { shouldValidate: true })}
          error={errors.employee_id?.message}
        />
        <FormField
          id="contract-number"
          label="Номер договора"
          error={errors.number?.message}
          {...register('number')}
        />
      </div>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
        <FormField
          id="contract-signed-on"
          label="Дата подписания"
          type="date"
          error={errors.signed_on?.message}
          {...register('signed_on')}
        />
        <FormField
          id="contract-starts-on"
          label="Действует с"
          type="date"
          error={errors.starts_on?.message}
          {...register('starts_on')}
        />
        <FormField
          id="contract-ends-on"
          label="Действует по (необязательно)"
          type="date"
          error={errors.ends_on?.message}
          {...register('ends_on')}
        />
      </div>
      <div className="flex gap-2">
        <Button type="submit" disabled={submitting}>
          {submitting ? 'Сохранение…' : 'Создать договор'}
        </Button>
        <Button type="button" variant="ghost" onClick={onCancel}>
          Отмена
        </Button>
      </div>
    </form>
  );
}

function LaborContractDocumentForm({
  submitting,
  serverError,
  onSubmit,
  onCancel,
}: {
  submitting: boolean;
  serverError: string | null;
  onSubmit: (values: LaborContractDocumentValues) => void;
  onCancel: () => void;
}) {
  const {
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<LaborContractDocumentValues>({
    resolver: zodResolver(laborContractDocumentSchema),
    defaultValues: {
      director: '',
      work_schedule: '',
      probation_months: '',
      employer_address: '',
      employee_address: '',
    },
  });

  return (
    <form className="space-y-4" onSubmit={handleSubmit(onSubmit)}>
      <p className="text-sm text-muted-foreground">
        ФИО и ИИН сотрудника, должность, оклад (в том числе прописью) и реквизиты организации
        подставляются автоматически — заполните только то, чего нет в системе.
      </p>
      <FormField
        id="contract-doc-director"
        label="Руководитель (ФИО, должность)"
        placeholder="Директор Смирнов С.С."
        error={errors.director?.message}
        {...register('director')}
      />
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        <FormField
          id="contract-doc-schedule"
          label="Режим рабочего времени"
          placeholder="пятидневная рабочая неделя, 40 часов"
          error={errors.work_schedule?.message}
          {...register('work_schedule')}
        />
        <FormField
          id="contract-doc-probation"
          label="Испытательный срок, мес. (необязательно)"
          placeholder="3"
          error={errors.probation_months?.message}
          {...register('probation_months')}
        />
      </div>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        <FormField
          id="contract-doc-employer-address"
          label="Адрес работодателя (необязательно)"
          error={errors.employer_address?.message}
          {...register('employer_address')}
        />
        <FormField
          id="contract-doc-employee-address"
          label="Адрес сотрудника (необязательно)"
          error={errors.employee_address?.message}
          {...register('employee_address')}
        />
      </div>
      {serverError && (
        <p className="text-sm text-destructive" role="alert">
          {serverError}
        </p>
      )}
      <div className="flex gap-2">
        <Button type="submit" disabled={submitting}>
          {submitting ? 'Формирование…' : 'Сформировать документ'}
        </Button>
        <Button type="button" variant="ghost" onClick={onCancel}>
          Отмена
        </Button>
      </div>
    </form>
  );
}

// ── Отпуска ─────────────────────────────────────────────────────────────────

function VacationsSection({
  employees,
  employeeId,
  employeeName,
}: {
  employees: Employee[];
  employeeId: string;
  employeeName: (id: string) => string;
}) {
  const toast = useToast();
  const [creating, setCreating] = useState(false);

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.hr.vacations(employeeId),
    queryFn: ({ limit, offset }) =>
      api.getJson<VacationListResponse>('/api/v1/vacations', {
        query: employeeId ? { employee_id: employeeId, limit, offset } : { limit, offset },
      }),
    perPage: PER_PAGE,
  });

  // Switching the employee filter changes the result set — land back on
  // page 1 rather than risk requesting an offset past the new total.
  useEffect(() => {
    setPage(1);
  }, [employeeId, setPage]);

  const create = useApiMutation(
    (values: VacationValues) =>
      api.postJson<VacationDetailResponse>('/api/v1/vacations', {
        body: buildVacationCreate(values),
      }),
    {
      invalidate: [qk.hr.vacations()],
      onSuccess: () => {
        setCreating(false);
        toast.success('Отпуск добавлен.');
      },
    },
  );
  useErrorToast(create.error);

  const columns: Column<Vacation>[] = [
    { header: 'Сотрудник', cell: (v) => employeeName(v.employee_id) },
    {
      header: 'Вид',
      cell: (v) => <StatusBadge label={VACATION_KIND_LABELS[v.kind] ?? v.kind} />,
    },
    { header: 'С', className: 'whitespace-nowrap', cell: (v) => formatIsoDateRu(v.starts_on) },
    { header: 'По', className: 'whitespace-nowrap', cell: (v) => formatIsoDateRu(v.ends_on) },
    { header: 'Дней', className: 'text-right', cell: (v) => v.days },
  ];

  return (
    <Card>
      <CardHeader className="flex flex-row items-center justify-between space-y-0">
        <CardTitle>Отпуска</CardTitle>
        <Button onClick={() => setCreating((v) => !v)}>
          {creating ? 'Закрыть' : 'Новый отпуск'}
        </Button>
      </CardHeader>
      <CardContent className="overflow-x-auto">
        <DataTable
          columns={columns}
          rows={data?.data}
          rowKey={(v) => v.id}
          isLoading={isLoading}
          error={error}
          emptyText="Отпусков пока нет."
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
          <VacationForm
            employees={employees}
            defaultEmployeeId={employeeId}
            submitting={create.isPending}
            onSubmit={(values) => create.mutate(values)}
            onCancel={() => setCreating(false)}
          />
        </CardContent>
      )}
    </Card>
  );
}

function VacationForm({
  employees,
  defaultEmployeeId,
  submitting,
  onSubmit,
  onCancel,
}: {
  employees: Employee[];
  defaultEmployeeId: string;
  submitting: boolean;
  onSubmit: (values: VacationValues) => void;
  onCancel: () => void;
}) {
  const {
    register,
    control,
    setValue,
    handleSubmit,
    formState: { errors },
  } = useForm<VacationValues>({
    resolver: zodResolver(vacationSchema),
    defaultValues: {
      employee_id: defaultEmployeeId,
      kind: 'annual',
      starts_on: '',
      ends_on: '',
      days: '',
    },
  });
  const selectedEmployeeId = useWatch({ control, name: 'employee_id' }) ?? '';

  return (
    <form className="space-y-4" onSubmit={handleSubmit(onSubmit)}>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        <EmployeeSelect
          id="vacation-employee"
          employees={employees}
          value={selectedEmployeeId}
          onChange={(id) => setValue('employee_id', id, { shouldValidate: true })}
          error={errors.employee_id?.message}
        />
        <div className="space-y-2">
          <Label htmlFor="vacation-kind">Вид отпуска</Label>
          <select
            id="vacation-kind"
            className="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
            {...register('kind')}
          >
            {VACATION_KINDS.map((k) => (
              <option key={k} value={k}>
                {VACATION_KIND_LABELS[k]}
              </option>
            ))}
          </select>
        </div>
      </div>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
        <FormField
          id="vacation-starts-on"
          label="Дата начала"
          type="date"
          error={errors.starts_on?.message}
          {...register('starts_on')}
        />
        <FormField
          id="vacation-ends-on"
          label="Дата окончания"
          type="date"
          error={errors.ends_on?.message}
          {...register('ends_on')}
        />
        <FormField
          id="vacation-days"
          label="Календарных дней"
          inputMode="numeric"
          placeholder="24"
          error={errors.days?.message}
          {...register('days')}
        />
      </div>
      <div className="flex gap-2">
        <Button type="submit" disabled={submitting}>
          {submitting ? 'Сохранение…' : 'Добавить отпуск'}
        </Button>
        <Button type="button" variant="ghost" onClick={onCancel}>
          Отмена
        </Button>
      </div>
    </form>
  );
}
