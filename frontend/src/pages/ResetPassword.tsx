import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Link, useParams } from 'react-router-dom';
import type { z } from 'zod';

import { Button } from '@/components/ui/button';
import { FormField } from '@/components/FormField';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api } from '@/lib/api/client';
import { resetPasswordSchema } from '@/lib/schemas/auth';

type FormValues = z.infer<typeof resetPasswordSchema>;

export function ResetPasswordPage() {
  const { token = '' } = useParams<{ token: string }>();
  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<FormValues>({ resolver: zodResolver(resetPasswordSchema) });

  const reset = useApiMutation((values: FormValues) =>
    api.postJson('/api/v1/account/reset-password/' + encodeURIComponent(token), {
      body: { new_password: values.new_password },
    }),
  );
  useErrorToast(reset.error);

  const onSubmit = handleSubmit((values) => reset.mutate(values));

  return (
    <div className="container mx-auto max-w-md py-8">
      <Card>
        <CardHeader>
          <CardTitle>Новый пароль</CardTitle>
        </CardHeader>
        <CardContent>
          {reset.isSuccess ? (
            <div className="space-y-4">
              <p className="text-sm text-muted-foreground">
                Пароль обновлён. Теперь вы можете войти.
              </p>
              <Button asChild className="w-full">
                <Link to="/login">Перейти ко входу</Link>
              </Button>
            </div>
          ) : (
            <form className="space-y-4" onSubmit={onSubmit}>
              <FormField
                id="new_password"
                type="password"
                label="Новый пароль"
                error={errors.new_password?.message}
                {...register('new_password')}
              />
              <FormField
                id="new_password_confirm"
                type="password"
                label="Подтверждение нового пароля"
                error={errors.new_password_confirm?.message}
                {...register('new_password_confirm')}
              />
              <Button type="submit" className="w-full" disabled={isSubmitting || reset.isPending}>
                Обновить пароль
              </Button>
            </form>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
