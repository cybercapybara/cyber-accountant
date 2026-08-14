import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Link } from 'react-router-dom';
import type { z } from 'zod';

import { Alert, AlertDescription } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { FormField } from '@/components/FormField';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useApiMutation } from '@/hooks/useApiMutation';
import { api } from '@/lib/api/client';
import { requestResetSchema } from '@/lib/schemas/auth';

type FormValues = z.infer<typeof requestResetSchema>;

export function RequestResetPage() {
  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<FormValues>({ resolver: zodResolver(requestResetSchema) });

  // The backend always returns 200 (no enumeration). We surface a generic
  // confirmation once the request has settled — success or failure — so the
  // UI never reveals whether the address is registered.
  const request = useApiMutation((values: FormValues) =>
    api.postJson('/api/v1/account/reset-password-request', { body: values }),
  );
  const sent = request.isSuccess || request.isError;

  const onSubmit = handleSubmit((values) => request.mutate(values));

  return (
    <div className="container mx-auto max-w-md py-8">
      <Card>
        <CardHeader>
          <CardTitle>Сброс пароля</CardTitle>
          <CardDescription>
            Если такой email зарегистрирован, мы отправим на него ссылку для сброса пароля.
          </CardDescription>
        </CardHeader>
        <CardContent>
          {sent ? (
            <Alert>
              <AlertDescription>
                Если этот email зарегистрирован, ссылка для сброса пароля уже отправлена.
              </AlertDescription>
            </Alert>
          ) : (
            <form className="space-y-4" onSubmit={onSubmit}>
              <FormField
                id="email"
                type="email"
                label="Email"
                error={errors.email?.message}
                {...register('email')}
              />
              <Button type="submit" className="w-full" disabled={isSubmitting || request.isPending}>
                Отправить ссылку
              </Button>
              <div className="text-sm text-muted-foreground text-center">
                <Link to="/login" className="hover:underline">
                  Назад ко входу
                </Link>
              </div>
            </form>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
