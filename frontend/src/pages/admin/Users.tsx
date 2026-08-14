import { Link } from 'react-router-dom';

import { DataTable, type Column } from '@/components/DataTable';
import { PageHeader } from '@/components/PageHeader';
import { PaginationFooter } from '@/components/PaginationFooter';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { User } from '@/lib/api/types';

const PER_PAGE = 20;

const columns: Column<User>[] = [
  { header: 'Email', cell: (u) => <span className="font-mono">{u.email}</span> },
  { header: 'Имя', cell: (u) => u.full_name },
  { header: 'Роль', cell: (u) => u.role?.name ?? u.role_id },
  {
    header: 'Подтверждён',
    cell: (u) => (
      <span aria-label={u.confirmed ? 'Подтверждён' : 'Не подтверждён'}>
        <span aria-hidden="true">{u.confirmed ? '✓' : '—'}</span>
      </span>
    ),
  },
  {
    header: '',
    className: 'text-right',
    cell: (u) => (
      <Button variant="ghost" size="sm" asChild>
        <Link to={`/admin/users/${u.id}`}>Изменить</Link>
      </Button>
    ),
  },
];

export function AdminUsersPage() {
  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.admin.users(),
    queryFn: ({ limit, offset }) =>
      api.getJson('/api/v1/admin/users', { query: { limit, offset } }),
    perPage: PER_PAGE,
  });

  return (
    <div className="container mx-auto py-8 space-y-6">
      <PageHeader
        title="Пользователи"
        actions={
          <Button asChild>
            <Link to="/admin/invite">Пригласить пользователя</Link>
          </Button>
        }
      />
      <Card>
        <CardHeader>
          <CardTitle>{data ? `Всего: ${data.total}` : 'Пользователи'}</CardTitle>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(u) => u.id}
            isLoading={isLoading}
            error={error}
            emptyText="Пользователей пока нет."
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
      </Card>
    </div>
  );
}
