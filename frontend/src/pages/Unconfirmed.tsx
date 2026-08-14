import { useState } from 'react';

import { Alert, AlertDescription } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useMe } from '@/hooks/useMe';
import { api } from '@/lib/api/client';

/**
 * Shown when the user is logged in but the access JWT carries
 * confirmed=false. flask-base parity: app/account/views.py
 * before_request blocks unconfirmed users from non-account routes
 * and redirects them to /unconfirmed.
 */
export function UnconfirmedPage() {
  const user = useMe().data ?? null;
  const [resent, setResent] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const resend = async () => {
    setError(null);
    const { error: e } = await api.POST('/api/v1/account/confirm-resend');
    if (e) {
      setError('Не удалось отправить письмо повторно. Попробуйте позже.');
      return;
    }
    setResent(true);
  };

  return (
    <div className="container mx-auto max-w-md py-8">
      <Card>
        <CardHeader>
          <CardTitle>Подтвердите email</CardTitle>
          <CardDescription>
            Мы отправили ссылку для подтверждения на {user?.email ?? 'ваш email'}. Перейдите по ней,
            чтобы открыть доступ ко всему приложению.
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-4">
          {resent && (
            <Alert variant="success">
              <AlertDescription>Новая ссылка для подтверждения уже отправлена.</AlertDescription>
            </Alert>
          )}
          {error && (
            <Alert variant="destructive">
              <AlertDescription>{error}</AlertDescription>
            </Alert>
          )}
          <Button onClick={resend} className="w-full" variant="outline">
            Отправить письмо повторно
          </Button>
        </CardContent>
      </Card>
    </div>
  );
}
