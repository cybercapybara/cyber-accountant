import { useState } from 'react';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Link, useParams } from 'react-router-dom';
import type { z } from 'zod';

import { Button } from '@/components/ui/button';
import { FormField } from '@/components/FormField';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { useToast } from '@/components/ui/toaster';
import { api, apiErrorMessage } from '@/lib/api/client';
import { resetPasswordSchema } from '@/lib/schemas/auth';

type FormValues = z.infer<typeof resetPasswordSchema>;

/** Outcome of accepting an invite — drives the success vs. error branch. */
export type JoinResult = { ok: true } | { ok: false; error: string };

/**
 * Submits the new password for an invite token. Pure enough to unit-test
 * (the page just maps the result onto state) — no @testing-library needed.
 * Returns ok:true on 2xx, ok:false with a user-facing message otherwise.
 */
export async function submitJoinFromInvite(
  token: string,
  newPassword: string,
): Promise<JoinResult> {
  const { error } = await api.POST(
    '/api/v1/account/join-from-invite/' + encodeURIComponent(token),
    { body: { new_password: newPassword } },
  );
  if (error) {
    return {
      ok: false,
      error: apiErrorMessage(error, 'Эта ссылка-приглашение недействительна или истёк её срок.'),
    };
  }
  return { ok: true };
}

export function JoinFromInvitePage() {
  const { token = '' } = useParams<{ token: string }>();
  const toast = useToast();
  const [done, setDone] = useState(false);
  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<FormValues>({ resolver: zodResolver(resetPasswordSchema) });

  const onSubmit = handleSubmit(async (values) => {
    const result = await submitJoinFromInvite(token, values.new_password);
    if (!result.ok) {
      toast.error(result.error);
      return;
    }
    setDone(true);
  });

  return (
    <div className="container mx-auto max-w-md py-8">
      <Card>
        <CardHeader>
          <CardTitle>Придумайте пароль</CardTitle>
        </CardHeader>
        <CardContent>
          {done ? (
            <div className="space-y-4">
              <p className="text-sm text-muted-foreground">
                Аккаунт готов. Теперь вы можете войти.
              </p>
              <Button asChild className="w-full">
                <Link to="/login">Перейти ко входу</Link>
              </Button>
            </div>
          ) : (
            <form className="space-y-4" onSubmit={onSubmit}>
              <p className="text-sm text-muted-foreground">
                Примите приглашение, задав пароль для нового аккаунта.
              </p>
              <FormField
                id="new_password"
                type="password"
                label="Пароль"
                error={errors.new_password?.message}
                {...register('new_password')}
              />
              <FormField
                id="new_password_confirm"
                type="password"
                label="Подтверждение пароля"
                error={errors.new_password_confirm?.message}
                {...register('new_password_confirm')}
              />
              <Button type="submit" className="w-full" disabled={isSubmitting}>
                Задать пароль
              </Button>
            </form>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
