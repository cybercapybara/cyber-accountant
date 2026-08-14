import { Link } from 'react-router-dom';
import { Users, UserPlus, Shield, ListChecks, ScrollText } from 'lucide-react';

import { PageHeader } from '@/components/PageHeader';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useMe } from '@/hooks/useMe';
import { Permission, userCan } from '@/lib/auth/permissions';

export function AdminDashboardPage() {
  const me = useMe();
  const canAudit = userCan(me.data, Permission.AuditRead);
  return (
    <div className="container mx-auto py-8 space-y-6">
      <PageHeader title="Администрирование" />
      <div className="grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
        <Link to="/admin/users">
          <Card className="hover:bg-accent transition-colors h-full">
            <CardHeader>
              <Users className="h-6 w-6 mb-2 text-muted-foreground" />
              <CardTitle>Пользователи</CardTitle>
              <CardDescription>Список, редактирование и удаление пользователей.</CardDescription>
            </CardHeader>
          </Card>
        </Link>
        <Link to="/admin/invite">
          <Card className="hover:bg-accent transition-colors h-full">
            <CardHeader>
              <UserPlus className="h-6 w-6 mb-2 text-muted-foreground" />
              <CardTitle>Пригласить</CardTitle>
              <CardDescription>Отправить приглашение по email.</CardDescription>
            </CardHeader>
          </Card>
        </Link>
        <Link to="/admin/roles">
          <Card className="hover:bg-accent transition-colors h-full">
            <CardHeader>
              <Shield className="h-6 w-6 mb-2 text-muted-foreground" />
              <CardTitle>Роли</CardTitle>
              <CardDescription>Создание, редактирование и удаление ролей и прав.</CardDescription>
            </CardHeader>
            <CardContent className="text-sm text-muted-foreground">
              Каждая роль — это набор прав; назначайте их пользователям.
            </CardContent>
          </Card>
        </Link>
        <Link to="/admin/jobs">
          <Card className="hover:bg-accent transition-colors h-full">
            <CardHeader>
              <ListChecks className="h-6 w-6 mb-2 text-muted-foreground" />
              <CardTitle>Задачи</CardTitle>
              <CardDescription>
                Статусы очереди, данные задач, очередь недоставленных (DLQ).
              </CardDescription>
            </CardHeader>
          </Card>
        </Link>
        {canAudit && (
          <Link to="/admin/audit">
            <Card className="hover:bg-accent transition-colors h-full">
              <CardHeader>
                <ScrollText className="h-6 w-6 mb-2 text-muted-foreground" />
                <CardTitle>Журнал аудита</CardTitle>
                <CardDescription>Журнал действий администраторов (только чтение).</CardDescription>
              </CardHeader>
            </Card>
          </Link>
        )}
      </div>
    </div>
  );
}
