import { Link } from 'react-router-dom';

import { Button } from '@/components/ui/button';

export function NotFoundPage() {
  return (
    <div className="container mx-auto flex max-w-md flex-col items-center gap-4 py-24 text-center">
      <p className="text-6xl font-bold text-muted-foreground">404</p>
      <h1 className="text-2xl font-semibold">Страница не найдена</h1>
      <p className="text-muted-foreground">
        Такой страницы не существует или она была перемещена. Проверьте адрес или вернитесь на
        главную.
      </p>
      <Button asChild>
        <Link to="/">На главную</Link>
      </Button>
    </div>
  );
}
