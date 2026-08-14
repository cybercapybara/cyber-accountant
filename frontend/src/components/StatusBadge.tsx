import { cn } from '@/lib/utils';

/**
 * Semantic tone, not a color — the same tone reads the same everywhere
 * (DESIGN.md §5). Pages own the status→{label,tone} dictionary for their
 * own domain (journal status, document status, job status, …); this
 * component only renders the resulting pill.
 */
export type BadgeTone = 'neutral' | 'warning' | 'info' | 'success' | 'danger';

const TONE_STYLES: Record<BadgeTone, string> = {
  neutral:
    'border-slate-300 bg-slate-50 text-slate-700 dark:border-slate-500/30 dark:bg-slate-500/10 dark:text-slate-300',
  warning:
    'border-amber-300 bg-amber-50 text-amber-700 dark:border-amber-500/30 dark:bg-amber-500/10 dark:text-amber-300',
  info: 'border-sky-300 bg-sky-50 text-sky-700 dark:border-sky-500/30 dark:bg-sky-500/10 dark:text-sky-300',
  success:
    'border-emerald-300 bg-emerald-50 text-emerald-700 dark:border-emerald-500/30 dark:bg-emerald-500/10 dark:text-emerald-300',
  danger:
    'border-red-300 bg-red-50 text-red-700 dark:border-red-500/30 dark:bg-red-500/10 dark:text-red-300',
};

interface StatusBadgeProps {
  label: string;
  tone?: BadgeTone;
  className?: string;
}

export function StatusBadge({ label, tone = 'neutral', className }: StatusBadgeProps) {
  return (
    <span
      className={cn(
        'inline-flex items-center rounded border px-2 py-0.5 text-xs font-medium',
        TONE_STYLES[tone],
        className,
      )}
    >
      {label}
    </span>
  );
}
