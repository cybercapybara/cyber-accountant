import { Link, useLocation } from 'react-router-dom';

import { Alert, AlertDescription } from '@/components/ui/alert';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';

/**
 * Static page shown right after Register. The backend has fired the
 * confirmation email but we don't auto-log-in (flask-base parity).
 */
export function CheckEmailPage() {
  const location = useLocation();
  const email = (location.state as { email?: string } | null)?.email;
  return (
    <div className="container mx-auto max-w-md py-8">
      <Card>
        <CardHeader>
          <CardTitle>Проверьте почту</CardTitle>
          <CardDescription>
            {email
              ? `Мы отправили ссылку для подтверждения на ${email}.`
              : 'Мы отправили ссылку для подтверждения.'}
          </CardDescription>
        </CardHeader>
        <CardContent>
          <Alert>
            <AlertDescription>
              Не пришло письмо? Проверьте папку «Спам» или{' '}
              <Link to="/login" className="underline">
                войдите
              </Link>{' '}
              и воспользуйтесь кнопкой «Отправить письмо повторно».
            </AlertDescription>
          </Alert>
        </CardContent>
      </Card>
    </div>
  );
}
