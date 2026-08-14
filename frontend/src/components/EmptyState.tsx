import type { ComponentType, ReactNode } from 'react';

import { cn } from '@/lib/utils';

interface EmptyStateProps {
  /** A lucide icon component, e.g. `Inbox`. Purely decorative (aria-hidden). */
  icon?: ComponentType<{ className?: string }>;
  title: string;
  description?: string;
  /** Usually a <Button> that starts the "add the first thing" flow. */
  action?: ReactNode;
  className?: string;
}

/**
 * Single empty-state look for the whole app (DESIGN.md §9) — an empty
 * table, an empty roster, a not-yet-built section. Dashed border keeps it
 * visually distinct from a real content card.
 */
export function EmptyState({ icon: Icon, title, description, action, className }: EmptyStateProps) {
  return (
    <div
      className={cn(
        'flex flex-col items-center gap-2 rounded-md border border-dashed border-border px-6 py-10 text-center',
        className,
      )}
    >
      {Icon && <Icon className="h-8 w-8 text-muted-foreground" aria-hidden="true" />}
      <p className="font-medium text-foreground">{title}</p>
      {description && <p className="max-w-sm text-sm text-muted-foreground">{description}</p>}
      {action && <div className="mt-2">{action}</div>}
    </div>
  );
}
