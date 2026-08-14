import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';

export function AboutPage() {
  return (
    <div className="container mx-auto py-8 max-w-3xl">
      <Card>
        <CardHeader>
          <CardTitle>О сервисе</CardTitle>
          <CardDescription>
            Кабинет бухгалтера ТОО — контрагенты, журнал проводок, документы и организации в одном
            месте.
          </CardDescription>
        </CardHeader>
        <CardContent className="text-sm text-muted-foreground space-y-2">
          <p>Бэкенд: Drogon, libpqxx, redis-plus-plus, libsodium argon2id, JWT в HttpOnly-куках.</p>
          <p>
            Фронтенд: Vite + React 18 + TanStack Query + react-hook-form + zod + Tailwind +
            примитивы shadcn/ui.
          </p>
        </CardContent>
      </Card>
    </div>
  );
}
