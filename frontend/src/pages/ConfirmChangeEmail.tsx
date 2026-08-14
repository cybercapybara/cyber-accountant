import { Link, useParams } from 'react-router-dom';

import { Alert, AlertDescription } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useApiMutation } from '@/hooks/useApiMutation';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';

/**
 * Hit by the link mailed to the NEW address during an email change
 * (backend: POST /api/account/change-email/{token}). Same pattern as
 * ConfirmEmailPage: the POST is behind an explicit button so email
 * scanners can't burn the one-shot token and StrictMode can't double
 * fire it from an effect.
 *
 * Invalidates qk.me() on success: the account's email just changed, so a
 * signed-in user's cached `email` is stale — refetch /me so the nav and
 * profile reflect the new address.
 */
export function ConfirmChangeEmailPage() {
  const { token = '' } = useParams<{ token: string }>();

  const confirm = useApiMutation(
    () => api.postJson('/api/v1/account/change-email/' + encodeURIComponent(token)),
    { invalidate: [qk.me()] },
  );

  return (
    <div className="container mx-auto max-w-md py-8">
      <Card>
        <CardHeader>
          <CardTitle>Подтверждение нового email</CardTitle>
          <CardDescription>
            Нажмите кнопку ниже, чтобы привязать аккаунт к этому адресу.
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-4">
          {confirm.isSuccess && (
            <Alert variant="success">
              <AlertDescription>
                Email адрес обновлён. Используйте новый адрес для входа.
              </AlertDescription>
            </Alert>
          )}
          {confirm.isError && (
            <Alert variant="destructive">
              <AlertDescription>
                {confirm.error ?? 'Эта ссылка недействительна или истёк её срок.'}
              </AlertDescription>
            </Alert>
          )}
          {confirm.isSuccess ? (
            <Button asChild className="w-full">
              <Link to="/login">Перейти ко входу</Link>
            </Button>
          ) : (
            <Button
              className="w-full"
              disabled={confirm.isPending}
              onClick={() => confirm.mutate()}
            >
              {confirm.isPending ? 'Подтверждение…' : 'Подтвердить новый email'}
            </Button>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
