import { formatTiynRu } from '@/lib/money';
import { cn } from '@/lib/utils';

interface MoneyProps {
  /** Signed integer count of tiyn (1/100 tenge) — see lib/money.ts. Never a float amount of tenge. */
  tiyn: number;
  /** Omit the trailing "₸" — for a cell right next to an explicit currency label. */
  hideCurrency?: boolean;
  className?: string;
}

/**
 * The one place amounts get rendered as text (DESIGN.md §7): monospaced,
 * tabular-nums, "12 345,67 ₸". Right-alignment in a table is the column's
 * job (`className: 'text-right'` on the Column), not this component's.
 */
export function Money({ tiyn, hideCurrency, className }: MoneyProps) {
  return (
    <span className={cn('whitespace-nowrap font-mono tabular-nums', className)}>
      {formatTiynRu(tiyn)}
      {!hideCurrency && ' ₸'}
    </span>
  );
}
