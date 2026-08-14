import { Link } from 'react-router-dom';
import { BookOpenText, Building2, CalendarClock, FileText, Users } from 'lucide-react';

import { EmptyState } from '@/components/EmptyState';
import { PageHeader } from '@/components/PageHeader';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useMe } from '@/hooks/useMe';

/**
 * HomePage — Task 16. The dashboard is the working entry point once signed
 * in (shortcut cards + quick actions), and a short landing pitch + a
 * "Войти" button when signed out (P0 guests never see the app shell's nav
 * shortcuts, so the page itself needs a way in).
 */
export function HomePage() {
  const user = useMe().data ?? null;

  if (!user) {
    return (
      <div className="container mx-auto max-w-xl py-16 text-center space-y-6">
        <div className="space-y-2">
          <h1 className="text-3xl font-bold">Кабинет бухгалтера ТОО</h1>
          <p className="text-muted-foreground">
            Контрагенты, журнал проводок и документы вашей организации — в одном месте.
          </p>
        </div>
        <div className="flex justify-center gap-3">
          <Button asChild size="lg">
            <Link to="/login">Войти</Link>
          </Button>
          <Button asChild size="lg" variant="outline">
            <Link to="/register">Регистрация</Link>
          </Button>
        </div>
      </div>
    );
  }

  return (
    <div className="container mx-auto max-w-5xl py-8 space-y-8">
      <PageHeader
        title={`Здравствуйте, ${user.full_name || user.email}`}
        description="Ваша рабочая панель: быстрый доступ к разделам и частым действиям."
      />

      <section aria-labelledby="home-shortcuts-heading" className="space-y-3">
        <h2 id="home-shortcuts-heading" className="text-lg font-semibold">
          Разделы
        </h2>
        <div className="grid gap-4 sm:grid-cols-2 lg:grid-cols-4">
          <Link
            to="/counterparties"
            className="rounded-md focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring"
          >
            <Card className="h-full transition-colors hover:bg-accent">
              <CardHeader>
                <Users className="mb-2 h-6 w-6 text-muted-foreground" aria-hidden="true" />
                <CardTitle>Контрагенты</CardTitle>
                <CardDescription>Поставщики, покупатели и другие стороны сделок.</CardDescription>
              </CardHeader>
            </Card>
          </Link>
          <Link
            to="/journal"
            className="rounded-md focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring"
          >
            <Card className="h-full transition-colors hover:bg-accent">
              <CardHeader>
                <BookOpenText className="mb-2 h-6 w-6 text-muted-foreground" aria-hidden="true" />
                <CardTitle>Журнал проводок</CardTitle>
                <CardDescription>Черновики, проведённые и сторнированные проводки.</CardDescription>
              </CardHeader>
            </Card>
          </Link>
          <Link
            to="/documents"
            className="rounded-md focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring"
          >
            <Card className="h-full transition-colors hover:bg-accent">
              <CardHeader>
                <FileText className="mb-2 h-6 w-6 text-muted-foreground" aria-hidden="true" />
                <CardTitle>Документы</CardTitle>
                <CardDescription>Сгенерированные и загруженные документы.</CardDescription>
              </CardHeader>
            </Card>
          </Link>
          <Link
            to="/organizations"
            className="rounded-md focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring"
          >
            <Card className="h-full transition-colors hover:bg-accent">
              <CardHeader>
                <Building2 className="mb-2 h-6 w-6 text-muted-foreground" aria-hidden="true" />
                <CardTitle>Организации</CardTitle>
                <CardDescription>Переключение организации и участники.</CardDescription>
              </CardHeader>
            </Card>
          </Link>
        </div>
      </section>

      <section aria-labelledby="home-quick-actions-heading" className="space-y-3">
        <h2 id="home-quick-actions-heading" className="text-lg font-semibold">
          Быстрые действия
        </h2>
        <div className="flex flex-wrap gap-3">
          <Button asChild>
            <Link to="/journal">Создать проводку</Link>
          </Button>
          <Button asChild variant="outline">
            <Link to="/documents/generate">Сгенерировать документ</Link>
          </Button>
        </div>
      </section>

      <section aria-labelledby="home-deadlines-heading" className="space-y-3">
        <h2 id="home-deadlines-heading" className="text-lg font-semibold">
          Ближайшие сроки
        </h2>
        <Card>
          <CardContent className="pt-6">
            {/* TODO(tax-deadlines): once GET /api/v1/tax/deadlines ships (this
                phase, tax module), replace this EmptyState with a real list —
                upcoming filing/payment deadlines for the active organization,
                sorted by due date, with an overdue/urgent visual treatment
                (see StatusBadge tones in DESIGN.md §5). */}
            <EmptyState
              icon={CalendarClock}
              title="Скоро здесь появятся сроки по налогам и отчётности"
              description="Мы работаем над блоком с ближайшими дедлайнами вашей организации."
            />
          </CardContent>
        </Card>
      </section>
    </div>
  );
}
