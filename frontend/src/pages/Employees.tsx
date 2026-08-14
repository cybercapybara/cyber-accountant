import { useState } from 'react';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Link } from 'react-router-dom';

import { ConfirmDialog } from '@/components/ConfirmDialog';
import { DataTable, type Column } from '@/components/DataTable';
import { FormField } from '@/components/FormField';
import { Money } from '@/components/Money';
import { PageHeader } from '@/components/PageHeader';
import { PaginationFooter } from '@/components/PaginationFooter';
import { StatusBadge, type BadgeTone } from '@/components/StatusBadge';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Label } from '@/components/ui/label';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { Employee, EmployeeDetailResponse, EmployeeListResponse } from '@/lib/api/types';
import { formatIsoDateRu } from '@/lib/dateFormat';
import { formatTiyn } from '@/lib/money';
import {
  buildEmployeeCreate,
  buildEmployeeUpdate,
  employeeCreateSchema,
  employeeDismissSchema,
  employeeFullName,
  type EmployeeCreateValues,
  type EmployeeDismissValues,
} from '@/lib/schemas/hr';

const PER_PAGE = 20;

// Shared StatusBadge tone family (DESIGN.md §5).
const EMPLOYEE_STATUS: Record<Employee['status'], { label: string; tone: BadgeTone }> = {
  active: { label: 'Работает', tone: 'success' },
  dismissed: { label: 'Уволен', tone: 'neutral' },
};

/**
 * EmployeesPage — Task 13. Route: /employees (guard: confirmed).
 *
 * Paginated roster (GET /api/v1/employees — active AND dismissed, the
 * endpoint returns the whole roster) plus a create form (POST), an inline
 * edit form (PATCH) and a dismissal action.
 *
 * Dismissal is deliberately NOT a field on the edit form: PATCH answers a
 * 422 for `hired_on`, `status` and `dismissed_on` alike
 * (EmployeesController.hpp — "explicit failure over silent no-op"), and the
 * transition is owned by POST /employees/{id}/dismiss. So it is its own
 * action: a dismissal date panel, then a ConfirmDialog, because it cannot
 * be undone from this UI.
 *
 * As on CounterpartiesPage, the client only checks the cheap ИИН shape (12
 * digits) — the check-digit algorithm stays server-side (Ledger::
 * is_valid_bin_iin) and a shape-valid-but-invalid ИИН surfaces as a 422
 * error toast.
 */
export function EmployeesPage() {
  const toast = useToast();
  const [creating, setCreating] = useState(false);
  const [editingId, setEditingId] = useState<string | null>(null);
  const [dismissingId, setDismissingId] = useState<string | null>(null);
  const [confirmDismiss, setConfirmDismiss] = useState<{
    employee: Employee;
    dismissed_on: string;
  } | null>(null);

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.employees.all(),
    queryFn: ({ limit, offset }) =>
      api.getJson<EmployeeListResponse>('/api/v1/employees', { query: { limit, offset } }),
    perPage: PER_PAGE,
  });

  const closeAllPanels = () => {
    setCreating(false);
    setEditingId(null);
    setDismissingId(null);
  };

  const create = useApiMutation(
    (values: EmployeeCreateValues) =>
      api.postJson<EmployeeDetailResponse>('/api/v1/employees', {
        body: buildEmployeeCreate(values),
      }),
    {
      invalidate: [qk.employees.all()],
      onSuccess: () => {
        setCreating(false);
        toast.success('Сотрудник добавлен.');
      },
    },
  );
  useErrorToast(create.error);

  const update = useApiMutation(
    (vars: { id: string; values: EmployeeCreateValues }) =>
      api.patchJson<EmployeeDetailResponse>(`/api/v1/employees/${vars.id}`, {
        body: buildEmployeeUpdate(vars.values),
      }),
    {
      invalidate: [qk.employees.all()],
      onSuccess: () => {
        setEditingId(null);
        toast.success('Данные сотрудника обновлены.');
      },
    },
  );
  useErrorToast(update.error);

  const dismiss = useApiMutation(
    (vars: { id: string; dismissed_on: string }) =>
      api.postJson<EmployeeDetailResponse>(`/api/v1/employees/${vars.id}/dismiss`, {
        body: { dismissed_on: vars.dismissed_on },
      }),
    {
      invalidate: [qk.employees.all()],
      onSuccess: () => {
        setConfirmDismiss(null);
        setDismissingId(null);
        toast.success('Сотрудник уволен.');
      },
      onError: () => setConfirmDismiss(null),
    },
  );
  useErrorToast(dismiss.error);

  const rows = data?.data;
  const editingRow = editingId ? rows?.find((e) => e.id === editingId) : undefined;
  const dismissingRow = dismissingId ? rows?.find((e) => e.id === dismissingId) : undefined;

  const columns: Column<Employee>[] = [
    { header: 'ФИО', className: 'font-medium', cell: (e) => employeeFullName(e) },
    { header: 'ИИН', className: 'font-mono', cell: (e) => e.iin },
    { header: 'Должность', cell: (e) => e.position },
    {
      header: 'Оклад',
      className: 'text-right',
      cell: (e) => <Money tiyn={e.salary_tiyn} />,
    },
    {
      header: 'Принят',
      className: 'whitespace-nowrap',
      cell: (e) => formatIsoDateRu(e.hired_on),
    },
    {
      header: 'Статус',
      cell: (e) => {
        const s = EMPLOYEE_STATUS[e.status];
        return (
          <span className="flex flex-col gap-0.5">
            <StatusBadge label={s.label} tone={s.tone} className="w-fit" />
            {e.dismissed_on && (
              <span className="text-xs text-muted-foreground">
                с {formatIsoDateRu(e.dismissed_on)}
              </span>
            )}
          </span>
        );
      },
    },
    {
      header: '',
      className: 'text-right',
      cell: (e) => (
        <div className="flex justify-end gap-2">
          <Button
            size="sm"
            variant="outline"
            onClick={() => {
              closeAllPanels();
              setEditingId(e.id);
            }}
          >
            Изменить
          </Button>
          {e.status === 'active' && (
            <Button
              size="sm"
              variant="outline"
              onClick={() => {
                closeAllPanels();
                setDismissingId(e.id);
              }}
            >
              Уволить
            </Button>
          )}
        </div>
      ),
    },
  ];

  return (
    <div className="container mx-auto max-w-6xl py-8 space-y-6">
      <PageHeader
        title="Сотрудники"
        description="Штат организации: приём, оклады и увольнение."
        actions={
          <Button asChild variant="outline">
            <Link to="/hr">Кадровые документы</Link>
          </Button>
        }
      />

      <Card>
        <CardHeader className="flex flex-row items-center justify-between space-y-0">
          <CardTitle>{data ? `Сотрудников: ${data.total}` : 'Сотрудники'}</CardTitle>
          <Button
            onClick={() => {
              const next = !creating;
              closeAllPanels();
              setCreating(next);
            }}
          >
            {creating ? 'Закрыть' : 'Новый сотрудник'}
          </Button>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={rows}
            rowKey={(e) => e.id}
            isLoading={isLoading}
            error={error}
            emptyText="Сотрудников пока нет."
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
            <EmployeeForm
              submitting={create.isPending}
              onSubmit={(values) => create.mutate(values)}
              onCancel={() => setCreating(false)}
            />
          </CardContent>
        )}

        {editingRow && (
          <CardContent className="border-t pt-6">
            <EmployeeForm
              employee={editingRow}
              submitting={update.isPending}
              onSubmit={(values) => update.mutate({ id: editingRow.id, values })}
              onCancel={() => setEditingId(null)}
              submitLabel="Сохранить изменения"
            />
          </CardContent>
        )}

        {dismissingRow && (
          <CardContent className="border-t pt-6">
            <DismissForm
              employee={dismissingRow}
              onSubmit={(values) =>
                setConfirmDismiss({ employee: dismissingRow, dismissed_on: values.dismissed_on })
              }
              onCancel={() => setDismissingId(null)}
            />
          </CardContent>
        )}
      </Card>

      {confirmDismiss && (
        <ConfirmDialog
          title="Уволить сотрудника"
          description={`Уволить ${employeeFullName(confirmDismiss.employee)} с ${formatIsoDateRu(
            confirmDismiss.dismissed_on,
          )}? Отменить увольнение через этот интерфейс нельзя.`}
          confirmLabel="Уволить"
          destructive
          busy={dismiss.isPending}
          onConfirm={() =>
            dismiss.mutate({
              id: confirmDismiss.employee.id,
              dismissed_on: confirmDismiss.dismissed_on,
            })
          }
          onClose={() => setConfirmDismiss(null)}
        />
      )}
    </div>
  );
}

/**
 * One form for both create and edit (same shape as CounterpartyForm). Edit
 * mode keeps `hired_on` in the form state — prefilled from the server, so
 * the schema is satisfied — but neither renders it nor sends it: the PATCH
 * body is assembled by `buildEmployeeUpdate`, whose allowlist has no
 * hired_on/status/dismissed_on at all.
 */
function EmployeeForm({
  employee,
  submitting,
  onSubmit,
  onCancel,
  submitLabel = 'Добавить сотрудника',
}: {
  employee?: Employee;
  submitting: boolean;
  onSubmit: (values: EmployeeCreateValues) => void;
  onCancel: () => void;
  submitLabel?: string;
}) {
  const idPrefix = employee ? `emp-edit-${employee.id}` : 'emp-new';
  const {
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<EmployeeCreateValues>({
    resolver: zodResolver(employeeCreateSchema),
    defaultValues: {
      iin: employee?.iin ?? '',
      last_name: employee?.last_name ?? '',
      first_name: employee?.first_name ?? '',
      middle_name: employee?.middle_name ?? '',
      position: employee?.position ?? '',
      // Integer тиын back to the "300000.00" decimal string the API takes —
      // never a float round-trip (lib/money.ts).
      salary: employee ? formatTiyn(employee.salary_tiyn) : '',
      hired_on: employee?.hired_on ?? '',
      ipn_deduction_claimed: employee?.ipn_deduction_claimed ?? false,
      opvr_exempt: employee?.opvr_exempt ?? false,
      payout_iik: employee?.payout_iik ?? '',
    },
  });

  return (
    <form className="space-y-4" onSubmit={handleSubmit(onSubmit)}>
      {employee && (
        <p className="text-sm text-muted-foreground">
          Дата приёма ({formatIsoDateRu(employee.hired_on)}) не редактируется. Чтобы прекратить
          трудовые отношения, воспользуйтесь действием «Уволить».
        </p>
      )}
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
        <FormField
          id={`${idPrefix}-last-name`}
          label="Фамилия"
          error={errors.last_name?.message}
          {...register('last_name')}
        />
        <FormField
          id={`${idPrefix}-first-name`}
          label="Имя"
          error={errors.first_name?.message}
          {...register('first_name')}
        />
        <FormField
          id={`${idPrefix}-middle-name`}
          label="Отчество (необязательно)"
          error={errors.middle_name?.message}
          {...register('middle_name')}
        />
      </div>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
        <FormField
          id={`${idPrefix}-iin`}
          label="ИИН (12 цифр)"
          inputMode="numeric"
          maxLength={12}
          error={errors.iin?.message}
          {...register('iin')}
        />
        <FormField
          id={`${idPrefix}-position`}
          label="Должность"
          error={errors.position?.message}
          {...register('position')}
        />
        <FormField
          id={`${idPrefix}-salary`}
          label="Оклад, ₸"
          inputMode="decimal"
          placeholder="300000.00"
          error={errors.salary?.message}
          {...register('salary')}
        />
      </div>
      <FormField
        id={`${idPrefix}-payout-iik`}
        label="ИИК для выплаты (необязательно)"
        error={errors.payout_iik?.message}
        {...register('payout_iik')}
      />
      <div className="flex flex-wrap gap-6">
        <div className="flex items-center gap-2">
          <input id={`${idPrefix}-ipn`} type="checkbox" {...register('ipn_deduction_claimed')} />
          <Label htmlFor={`${idPrefix}-ipn`}>Заявлен вычет по ИПН (14 МРП)</Label>
        </div>
        <div className="flex items-center gap-2">
          <input id={`${idPrefix}-opvr`} type="checkbox" {...register('opvr_exempt')} />
          <Label htmlFor={`${idPrefix}-opvr`}>Освобождён от ОПВР</Label>
        </div>
      </div>
      {!employee && (
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
          <FormField
            id={`${idPrefix}-hired-on`}
            label="Дата приёма"
            type="date"
            error={errors.hired_on?.message}
            {...register('hired_on')}
          />
        </div>
      )}
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

function DismissForm({
  employee,
  onSubmit,
  onCancel,
}: {
  employee: Employee;
  onSubmit: (values: EmployeeDismissValues) => void;
  onCancel: () => void;
}) {
  const {
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<EmployeeDismissValues>({
    resolver: zodResolver(employeeDismissSchema),
    defaultValues: { dismissed_on: '' },
  });

  return (
    <form className="space-y-4" onSubmit={handleSubmit(onSubmit)}>
      <p className="text-sm">
        Увольнение сотрудника <span className="font-medium">{employeeFullName(employee)}</span>.
      </p>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-3">
        <FormField
          id={`emp-dismiss-${employee.id}`}
          label="Дата увольнения"
          type="date"
          error={errors.dismissed_on?.message}
          {...register('dismissed_on')}
        />
      </div>
      <div className="flex gap-2">
        <Button type="submit" variant="destructive">
          Уволить…
        </Button>
        <Button type="button" variant="ghost" onClick={onCancel}>
          Отмена
        </Button>
      </div>
    </form>
  );
}
