import type { ReactNode } from 'react';

import { cn } from '@/lib/utils';

interface PageHeaderProps {
  title: string;
  description?: ReactNode;
  /** Buttons/links rendered to the right of the title (wraps under it on mobile). */
  actions?: ReactNode;
  className?: string;
}

/**
 * Standard page title block — title + optional description + optional
 * action buttons. Replaces the ad-hoc `<h1>` + `<p>` pair every page was
 * hand-rolling (see DESIGN.md §4).
 */
export function PageHeader({ title, description, actions, className }: PageHeaderProps) {
  return (
    <div className={cn('flex flex-wrap items-start justify-between gap-3', className)}>
      <div>
        <h1 className="text-3xl font-bold">{title}</h1>
        {description && <p className="mt-1 text-sm text-muted-foreground">{description}</p>}
      </div>
      {actions && <div className="flex flex-wrap items-center gap-2">{actions}</div>}
    </div>
  );
}
