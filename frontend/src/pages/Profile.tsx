import { Link } from 'react-router-dom';

import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useMe } from '@/hooks/useMe';

export function ProfilePage() {
  const user = useMe().data ?? null;
  if (!user) return null;
  return (
    <div className="container mx-auto max-w-2xl py-8 space-y-6">
      <Card>
        <CardHeader>
          <CardTitle>Мой аккаунт</CardTitle>
          <CardDescription>{user.email}</CardDescription>
        </CardHeader>
        <CardContent className="space-y-1 text-sm">
          <div>
            <span className="text-muted-foreground">Имя: </span>
            {user.full_name || '(не задано)'}
          </div>
          <div>
            <span className="text-muted-foreground">Роль: </span>
            {user.role?.name ?? user.role_id}
          </div>
          <div>
            <span className="text-muted-foreground">Email подтверждён: </span>
            {user.confirmed ? 'да' : 'нет'}
          </div>
        </CardContent>
      </Card>
      <div className="grid gap-3 sm:grid-cols-3">
        <Button variant="outline" asChild>
          <Link to="/account/change-password">Сменить пароль</Link>
        </Button>
        <Button variant="outline" asChild>
          <Link to="/account/change-email">Сменить email</Link>
        </Button>
      </div>
    </div>
  );
}
