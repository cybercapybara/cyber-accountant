import { Link, useNavigate, useParams } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';

import { useState } from 'react';

import { ConfirmDialog } from '@/components/ConfirmDialog';
import { ErrorState } from '@/components/ErrorState';
import { FormField } from '@/components/FormField';
import { LoadingTable } from '@/components/LoadingTable';
import { PageHeader } from '@/components/PageHeader';
import { RoleSelect } from '@/components/RoleSelect';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Label } from '@/components/ui/label';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { useMe } from '@/hooks/useMe';
import { api, apiErrorMessage } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { UserDetailResponse } from '@/lib/api/types';

export function AdminUserDetailPage() {
  const { id = '' } = useParams<{ id: string }>();
  const navigate = useNavigate();
  const toast = useToast();
  const [confirmDelete, setConfirmDelete] = useState(false);
  // Query-backed via the TanStack Query cache: the cache is empty for one
  // paint after a hard reload, which would briefly disable the
  // self-protection UI.
  const me = useMe().data ?? null;

  const userQ = useQuery({
    queryKey: qk.admin.user(id),
    queryFn: () => api.getJson<UserDetailResponse>('/api/v1/admin/users/' + id),
  });

  const update = useApiMutation(
    (patch: Record<string, unknown>) =>
      api.patchJson<UserDetailResponse>('/api/v1/admin/users/' + id, { body: patch }),
    {
      invalidate: [qk.admin.user(id), qk.admin.users()],
      onSuccess: () => toast.success('Изменения сохранены.'),
    },
  );

  const remove = useApiMutation(() => api.deleteJson('/api/v1/admin/users/' + id), {
    invalidate: [qk.admin.users()],
    onSuccess: () => navigate('/admin/users'),
  });

  useErrorToast(update.error ?? remove.error);

  if (userQ.isLoading) {
    return (
      <div className="container mx-auto max-w-2xl py-8">
        <LoadingTable columns={2} rows={4} />
      </div>
    );
  }
  if (userQ.error || !userQ.data) {
    return (
      <div className="container mx-auto max-w-2xl py-8">
        <ErrorState
          message={apiErrorMessage(userQ.error, 'Пользователь не найден.')}
          onRetry={() => userQ.refetch()}
          retrying={userQ.isFetching}
        />
      </div>
    );
  }

  const user = userQ.data.data;
  const isSelf = me?.id === user.id;

  return (
    <div className="container mx-auto max-w-2xl py-8 space-y-6">
      <PageHeader
        title={user.email}
        actions={
          <Button variant="ghost" asChild>
            <Link to="/admin/users">← Назад</Link>
          </Button>
        }
      />
      <Card>
        <CardHeader>
          <CardTitle>Данные</CardTitle>
        </CardHeader>
        <CardContent className="space-y-4">
          <form
            onSubmit={(e) => {
              e.preventDefault();
              const fd = new FormData(e.currentTarget);
              const patch: Record<string, unknown> = {};
              const newEmail = String(fd.get('email') || '');
              const newRoleId = Number(fd.get('role_id'));
              const newFirst = String(fd.get('first_name') || '');
              const newLast = String(fd.get('last_name') || '');
              if (newEmail && newEmail !== user.email) patch.email = newEmail;
              if (newRoleId && newRoleId !== user.role_id) patch.role_id = newRoleId;
              if (newFirst !== (user.first_name ?? '')) patch.first_name = newFirst;
              if (newLast !== (user.last_name ?? '')) patch.last_name = newLast;
              if (Object.keys(patch).length === 0) return;
              update.mutate(patch);
            }}
            className="space-y-3"
          >
            <FormField id="email" name="email" label="Email" defaultValue={user.email} />
            <div className="grid grid-cols-1 sm:grid-cols-2 gap-3">
              <FormField
                id="first_name"
                name="first_name"
                label="Имя"
                defaultValue={user.first_name ?? ''}
              />
              <FormField
                id="last_name"
                name="last_name"
                label="Фамилия"
                defaultValue={user.last_name ?? ''}
              />
            </div>
            <div className="space-y-2">
              <Label htmlFor="role_id">Роль</Label>
              <RoleSelect
                id="role_id"
                name="role_id"
                defaultValue={user.role_id}
                disabled={isSelf}
              />
              {isSelf && (
                <p className="text-xs text-muted-foreground">
                  Вы не можете изменить роль своего собственного аккаунта.
                </p>
              )}
            </div>
            <Button type="submit" disabled={update.isPending}>
              {update.isPending ? 'Сохранение…' : 'Сохранить изменения'}
            </Button>
          </form>
        </CardContent>
      </Card>
      <Card>
        <CardHeader>
          <CardTitle className="text-destructive">Опасная зона</CardTitle>
        </CardHeader>
        <CardContent>
          <Button variant="destructive" disabled={isSelf} onClick={() => setConfirmDelete(true)}>
            Удалить пользователя
          </Button>
          {isSelf && (
            <p className="text-xs text-muted-foreground mt-2">
              Вы не можете удалить свой собственный аккаунт; обратитесь к другому администратору.
            </p>
          )}
        </CardContent>
      </Card>
      {confirmDelete && (
        <ConfirmDialog
          title="Удалить пользователя"
          description={`Удалить пользователя ${user.email}? Это действие необратимо.`}
          confirmLabel="Удалить пользователя"
          destructive
          busy={remove.isPending}
          onConfirm={() => remove.mutate()}
          onClose={() => setConfirmDelete(false)}
        />
      )}
    </div>
  );
}
