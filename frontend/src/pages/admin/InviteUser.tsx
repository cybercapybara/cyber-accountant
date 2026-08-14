import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { useNavigate } from 'react-router-dom';

import { FormField } from '@/components/FormField';
import { RoleSelect } from '@/components/RoleSelect';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Label } from '@/components/ui/label';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { InviteResponse } from '@/lib/api/types';
import { inviteUserSchema, type InviteUserValues } from '@/lib/schemas/admin';

export function AdminInviteUserPage() {
  const navigate = useNavigate();
  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<InviteUserValues>({ resolver: zodResolver(inviteUserSchema) });

  // useApiMutation owns the error banner + invalidation: a fresh invite
  // creates a user row, so the admin users list must be re-fetched (any
  // paged variant is prefix-matched by qk.admin.users()).
  const invite = useApiMutation(
    (values: InviteUserValues) =>
      api.postJson<InviteResponse>('/api/v1/admin/invite', {
        body: {
          email: values.email,
          first_name: values.first_name || undefined,
          last_name: values.last_name || undefined,
          role_id: values.role_id ? Number(values.role_id) : undefined,
        },
      }),
    {
      invalidate: [qk.admin.users()],
      onSuccess: () => navigate('/admin/users'),
    },
  );
  useErrorToast(invite.error);

  const onSubmit = handleSubmit((values) => invite.mutate(values));

  return (
    <div className="container mx-auto max-w-md py-8">
      <Card>
        <CardHeader>
          <CardTitle>Пригласить пользователя</CardTitle>
          <CardDescription>
            Он получит письмо со ссылкой для установки собственного пароля.
          </CardDescription>
        </CardHeader>
        <CardContent>
          <form className="space-y-4" onSubmit={onSubmit}>
            <FormField
              id="email"
              type="email"
              label="Email"
              error={errors.email?.message}
              {...register('email')}
            />
            <div className="grid grid-cols-1 sm:grid-cols-2 gap-3">
              <FormField
                id="first_name"
                label="Имя"
                error={errors.first_name?.message}
                {...register('first_name')}
              />
              <FormField
                id="last_name"
                label="Фамилия"
                error={errors.last_name?.message}
                {...register('last_name')}
              />
            </div>
            <div className="space-y-2">
              <Label htmlFor="role_id">Роль</Label>
              <RoleSelect id="role_id" includeDefaultOption {...register('role_id')} />
            </div>
            <Button type="submit" className="w-full" disabled={isSubmitting || invite.isPending}>
              Отправить приглашение
            </Button>
          </form>
        </CardContent>
      </Card>
    </div>
  );
}
