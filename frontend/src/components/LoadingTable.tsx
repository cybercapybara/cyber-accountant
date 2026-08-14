import { Skeleton } from '@/components/ui/skeleton';

interface LoadingTableProps {
  /** Number of skeleton columns. */
  columns?: number;
  /** Number of skeleton rows. */
  rows?: number;
}

function loadingRows(rows: number, columns: number) {
  return Array.from({ length: rows }).map((_, r) => (
    <tr key={r} className="border-b border-border last:border-0">
      {Array.from({ length: columns }).map((_, c) => (
        <td key={c} className="py-1.5 pr-4">
          <Skeleton className="h-4 w-full" />
        </td>
      ))}
    </tr>
  ));
}

/**
 * Standalone table skeleton (DESIGN.md §4/§9) — a full `<table>` for a
 * detail view that hasn't been wrapped in DataTable yet.
 */
export function LoadingTable({ columns = 4, rows = 5 }: LoadingTableProps) {
  return (
    <table className="w-full text-sm" aria-hidden="true">
      <tbody>{loadingRows(rows, columns)}</tbody>
    </table>
  );
}

/**
 * Just the `<tr>` skeletons, for a caller (DataTable) that already owns
 * its own `<thead>` and needs the rows to line up under real column
 * headers instead of a second nested `<table>`.
 */
export function LoadingTableRows({ columns = 4, rows = 5 }: LoadingTableProps) {
  return <>{loadingRows(rows, columns)}</>;
}
