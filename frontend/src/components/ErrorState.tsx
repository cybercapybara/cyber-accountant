import { AlertCircle } from 'lucide-react';

import { Button } from '@/components/ui/button';
import { cn } from '@/lib/utils';

interface ErrorStateProps {
  message: string;
  /** Shown as a "Повторить" button when provided. */
  onRetry?: () => void;
  retrying?: boolean;
  className?: string;
}

/**
 * Single error-state look for the whole app (DESIGN.md §9) — a failed
 * list load, a failed detail fetch. Pair with `apiErrorMessage(error, '…')`
 * for the message.
 */
export function ErrorState({ message, onRetry, retrying, className }: ErrorStateProps) {
  return (
    <div
      role="alert"
      className={cn(
        'flex flex-col items-center gap-3 rounded-md border border-destructive/30 bg-destructive/5 px-6 py-8 text-center',
        className,
      )}
    >
      <AlertCircle className="h-6 w-6 text-destructive" aria-hidden="true" />
      <p className="text-sm text-destructive">{message}</p>
      {onRetry && (
        <Button size="sm" variant="outline" onClick={onRetry} disabled={retrying}>
          {retrying ? 'Повтор…' : 'Повторить'}
        </Button>
      )}
    </div>
  );
}
