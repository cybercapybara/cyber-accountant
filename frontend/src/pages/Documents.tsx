import { useEffect, useMemo, useRef, useState, type FormEvent } from 'react';
import { Link, useSearchParams } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';

import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert';
import { DataTable, type Column } from '@/components/DataTable';
import { PageHeader } from '@/components/PageHeader';
import { PaginationFooter } from '@/components/PaginationFooter';
import { StatusBadge, type BadgeTone } from '@/components/StatusBadge';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Label } from '@/components/ui/label';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { ApiClientError, api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type {
  CounterpartyListResponse,
  Document,
  DocumentDetailResponse,
  DocumentListResponse,
  DocumentUploadResponse,
  DownloadUrlResponse,
} from '@/lib/api/types';
import { formatIsoDateTimeRu } from '@/lib/dateFormat';

const PER_PAGE = 20;

// GET /api/v1/documents' `type`/`status` filters must be one of the
// CHECK-listed values (Document.doc_type / Document.status enums,
// docs/openapi.yaml) — these mirror that allowlist exactly.
const DOC_TYPES = [
  'invoice',
  'avr',
  'waybill',
  'tax_invoice',
  'reconciliation',
  'power_of_attorney',
  'incoming',
  'bank_statement',
  'hr',
  'fno',
  'other',
] as const;
type DocType = (typeof DOC_TYPES)[number];

const DOC_STATUSES = [
  'inbox',
  'recognized',
  'linked',
  'archived',
  'draft',
  'final',
  'sent',
] as const;

const DOC_TYPE_LABELS: Record<string, string> = {
  invoice: 'Счёт',
  avr: 'Акт выполненных работ',
  waybill: 'Накладная',
  tax_invoice: 'Счёт-фактура',
  reconciliation: 'Акт сверки',
  power_of_attorney: 'Доверенность',
  incoming: 'Входящий документ',
  bank_statement: 'Банковская выписка',
  hr: 'Кадровый документ',
  fno: 'ФНО',
  other: 'Другое',
};

const STATUS_LABELS: Record<string, string> = {
  inbox: 'Входящие',
  recognized: 'Распознан',
  linked: 'Связан',
  archived: 'В архиве',
  draft: 'Черновик',
  final: 'Готов',
  sent: 'Отправлен',
};

const SOURCE_LABELS: Record<string, string> = {
  generated: 'Сгенерирован',
  uploaded: 'Загружен',
  email: 'Почта',
};

// FocusedDocumentAlert's polling cadence and hard cap. Fix round 1
// (controller review): the previous version polled every 2s forever while
// status stayed 'draft' — a LaTeX render failure or exhausted job retries
// leave a document in 'draft' permanently, so that was an unbounded
// background poll with no stop condition. Two minutes is generous for a
// docgen render (seconds in practice); past that we stop and tell the user
// plainly rather than keep claiming "рендер выполняется".
const FOCUS_POLL_INTERVAL_MS = 2000;
const FOCUS_POLL_TIMEOUT_MS = 120_000;

// Shared StatusBadge tone family (DESIGN.md §5) — a document's
// "draft/final/sent/…" pill reads the same as any other status pill in the app.
const STATUS_TONE: Record<string, BadgeTone> = {
  draft: 'warning',
  final: 'success',
  sent: 'info',
  recognized: 'info',
  linked: 'info',
  inbox: 'neutral',
  archived: 'neutral',
};

const formatDate = formatIsoDateTimeRu;

async function sha256Hex(file: File): Promise<string> {
  const digest = await crypto.subtle.digest('SHA-256', await file.arrayBuffer());
  return Array.from(new Uint8Array(digest))
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
}

/**
 * DocumentsPage — Task 15. Route: /documents (guard: confirmed).
 *
 * List (GET /api/v1/documents, paginated, optional type/status filters),
 * a client-driven upload flow (uploads → PUT presigned → confirm-upload),
 * a link to /documents/generate, and — when arriving from a successful
 * generate call via `?focus=<id>&queued=<0|1>` — a banner that polls that
 * one document until it leaves 'draft' (bounded — see FocusedDocumentAlert).
 *
 * The table itself does NOT poll. An earlier version had the list refetch
 * on an interval whenever the current page held a generated+draft row, but
 * that has no natural stop condition of its own (a stuck draft — failed
 * LaTeX render, exhausted job retries — would poll forever with no
 * per-document timeout to anchor against, unlike the single focused
 * document below). One bounded poll source is simpler and safer than two;
 * a user who wants a fresher view of someone else's still-rendering draft
 * can just reload the page.
 */
export function DocumentsPage() {
  const toast = useToast();
  const [searchParams, setSearchParams] = useSearchParams();
  const focusId = searchParams.get('focus');
  const queuedFailed = searchParams.get('queued') === '0';

  const [typeFilter, setTypeFilter] = useState('');
  const [statusFilter, setStatusFilter] = useState('');
  const [uploading, setUploading] = useState(false);
  const [uploadDocType, setUploadDocType] = useState<DocType>('other');
  const [uploadFile, setUploadFile] = useState<File | null>(null);

  const filters = useMemo<Record<string, string>>(() => {
    const f: Record<string, string> = {};
    if (typeFilter) f.type = typeFilter;
    if (statusFilter) f.status = statusFilter;
    return f;
  }, [typeFilter, statusFilter]);

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.documents.all(filters),
    queryFn: ({ limit, offset }) =>
      api.getJson<DocumentListResponse>('/api/v1/documents', {
        query: { limit, offset, ...filters },
      }),
    perPage: PER_PAGE,
  });

  // Unpaginated (limit=200) counterparty lookup for the "Counterparty"
  // column — same rationale as Journal.tsx's select: P1 organizations are
  // small enough that this beats a per-row detail fetch.
  const counterpartiesQ = useQuery({
    queryKey: qk.counterparties.all(),
    queryFn: () =>
      api.getJson<CounterpartyListResponse>('/api/v1/counterparties', {
        query: { limit: 200, offset: 0 },
      }),
  });
  const counterpartyName = useMemo(() => {
    const map = new Map<string, string>();
    for (const c of counterpartiesQ.data?.data ?? []) map.set(c.id, c.name);
    return (id: string | null) => (id ? (map.get(id) ?? '—') : '—');
  }, [counterpartiesQ.data]);

  const download = useApiMutation(
    (id: string) => api.postJson<DownloadUrlResponse>(`/api/v1/documents/${id}/download-url`),
    {
      onSuccess: (res) => {
        window.open(res.url, '_blank', 'noopener,noreferrer');
      },
      onError: (message, err) => {
        const isConflict = err instanceof ApiClientError && err.status === 409;
        toast.error(isConflict ? 'Файл ещё не готов.' : message);
      },
    },
  );

  const upload = useApiMutation(
    async (vars: { file: File; docType: DocType }) => {
      const mime = vars.file.type || 'application/octet-stream';
      const created = await api.postJson<DocumentUploadResponse>('/api/v1/documents/uploads', {
        body: { filename: vars.file.name, mime, doc_type: vars.docType },
      });
      const putResponse = await fetch(created.upload_url, {
        method: 'PUT',
        headers: { 'Content-Type': mime },
        body: vars.file,
      });
      if (!putResponse.ok) {
        throw new Error(`Не удалось загрузить файл в хранилище: HTTP ${putResponse.status}`);
      }
      const checksum_sha256 = await sha256Hex(vars.file);
      return api.postJson<DocumentDetailResponse>(
        `/api/v1/documents/${created.data.id}/confirm-upload`,
        {
          body: { size_bytes: vars.file.size, checksum_sha256 },
        },
      );
    },
    {
      invalidate: [qk.documents.all()],
      onSuccess: () => {
        setUploading(false);
        setUploadFile(null);
        toast.success('Файл загружен.');
      },
    },
  );
  useErrorToast(upload.error);

  const handleUpload = (e: FormEvent) => {
    e.preventDefault();
    if (!uploadFile) return;
    upload.mutate({ file: uploadFile, docType: uploadDocType });
  };

  const clearFocus = () => {
    const next = new URLSearchParams(searchParams);
    next.delete('focus');
    next.delete('queued');
    setSearchParams(next, { replace: true });
  };

  const columns: Column<Document>[] = [
    {
      header: 'Тип',
      cell: (d) => <StatusBadge label={DOC_TYPE_LABELS[d.doc_type] ?? d.doc_type} />,
    },
    {
      header: 'Статус',
      cell: (d) => (
        <StatusBadge
          label={STATUS_LABELS[d.status] ?? d.status}
          tone={STATUS_TONE[d.status] ?? 'neutral'}
        />
      ),
    },
    { header: 'Контрагент', cell: (d) => counterpartyName(d.counterparty_id) },
    { header: 'Создан', className: 'whitespace-nowrap', cell: (d) => formatDate(d.created_at) },
    { header: 'Источник', cell: (d) => SOURCE_LABELS[d.source] ?? d.source },
    {
      header: '',
      className: 'text-right',
      cell: (d) => (
        <Button
          size="sm"
          variant="outline"
          disabled={download.isPending}
          onClick={() => download.mutate(d.id)}
        >
          Скачать
        </Button>
      ),
    },
  ];

  function applyFilters() {
    setPage(1);
  }

  function clearFilters() {
    setTypeFilter('');
    setStatusFilter('');
    setPage(1);
  }

  return (
    <div className="container mx-auto max-w-6xl py-8 space-y-6">
      <PageHeader
        title="Документы"
        description="Сгенерированные и загруженные документы организации."
        actions={
          <>
            <Button variant="outline" onClick={() => setUploading((v) => !v)}>
              {uploading ? 'Закрыть' : 'Загрузить'}
            </Button>
            <Button asChild>
              <Link to="/documents/generate">Создать документ</Link>
            </Button>
          </>
        }
      />

      {focusId && (
        <FocusedDocumentAlert id={focusId} queuedFailed={queuedFailed} onDismiss={clearFocus} />
      )}

      {uploading && (
        <Card>
          <CardHeader>
            <CardTitle>Загрузить файл</CardTitle>
          </CardHeader>
          <CardContent>
            <form className="flex flex-wrap items-end gap-3" onSubmit={handleUpload}>
              <div className="space-y-2">
                <Label htmlFor="upload-doc-type">Тип документа</Label>
                <select
                  id="upload-doc-type"
                  className="flex h-10 w-56 rounded-md border border-input bg-background px-3 py-2 text-sm"
                  value={uploadDocType}
                  onChange={(e) => setUploadDocType(e.target.value as DocType)}
                >
                  {DOC_TYPES.map((t) => (
                    <option key={t} value={t}>
                      {DOC_TYPE_LABELS[t]}
                    </option>
                  ))}
                </select>
              </div>
              <div className="space-y-2">
                <Label htmlFor="upload-file">Файл</Label>
                <input
                  id="upload-file"
                  type="file"
                  className="block text-sm"
                  onChange={(e) => setUploadFile(e.target.files?.[0] ?? null)}
                />
              </div>
              <Button type="submit" disabled={!uploadFile || upload.isPending}>
                {upload.isPending ? 'Загрузка…' : 'Загрузить'}
              </Button>
            </form>
          </CardContent>
        </Card>
      )}

      <Card>
        <CardContent className="pt-6">
          <form
            className="grid gap-4 sm:grid-cols-2 lg:grid-cols-4"
            onSubmit={(ev) => {
              ev.preventDefault();
              applyFilters();
            }}
          >
            <div className="space-y-1">
              <Label htmlFor="f-type">Тип</Label>
              <select
                id="f-type"
                className="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
                value={typeFilter}
                onChange={(e) => setTypeFilter(e.target.value)}
              >
                <option value="">Все</option>
                {DOC_TYPES.map((t) => (
                  <option key={t} value={t}>
                    {DOC_TYPE_LABELS[t]}
                  </option>
                ))}
              </select>
            </div>
            <div className="space-y-1">
              <Label htmlFor="f-status">Статус</Label>
              <select
                id="f-status"
                className="flex h-10 w-full rounded-md border border-input bg-background px-3 py-2 text-sm"
                value={statusFilter}
                onChange={(e) => setStatusFilter(e.target.value)}
              >
                <option value="">Все</option>
                {DOC_STATUSES.map((s) => (
                  <option key={s} value={s}>
                    {STATUS_LABELS[s]}
                  </option>
                ))}
              </select>
            </div>
            <div className="flex items-end gap-2 sm:col-span-2 lg:col-span-2">
              <Button type="submit">Применить</Button>
              <Button type="button" variant="ghost" onClick={clearFilters}>
                Сбросить
              </Button>
              {data && (
                <span className="ml-auto self-center text-sm text-muted-foreground">
                  Всего: {data.total}
                </span>
              )}
            </div>
          </form>
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>{data ? `${data.total} документ(ов)` : 'Документы'}</CardTitle>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(d) => d.id}
            isLoading={isLoading}
            error={error}
            emptyText="Документов пока нет."
            isPlaceholder={isPlaceholderData}
            rowProps={(d) => (d.id === focusId ? { className: 'bg-primary/5' } : {})}
          />
          {data && (
            <PaginationFooter
              page={page}
              totalPages={totalPages}
              isPlaceholderData={isPlaceholderData}
              onPageChange={setPage}
            />
          )}
        </CardContent>
      </Card>
    </div>
  );
}

/**
 * Standalone poll for the just-generated document, independent of the
 * table's own paging/filters (it may well not be on the current page or
 * may not match the active filters at all).
 *
 * Two things bound the polling, both fixed in controller review round 1
 * (an earlier version polled every 2s forever while status stayed
 * 'draft', which is indistinguishable from "stuck forever" for a failed
 * render or exhausted job retries):
 *   - `queued=0` (render_queued was false) means no job was ever enqueued
 *     — the document simply stays 'draft' until an operator re-enqueues
 *     it by hand, so this component never polls at all in that case, and
 *     says so plainly instead of the generic "rendering…" text.
 *   - Otherwise, polling stops after FOCUS_POLL_TIMEOUT_MS (2 minutes) even
 *     if status is still 'draft' — a real docgen render takes seconds, so
 *     two minutes stuck at 'draft' means something failed server-side
 *     (LaTeX error, exhausted retries). Past the cap the UI stops
 *     claiming a render is in progress and offers a manual refetch
 *     instead of an indefinite background poll.
 */
function FocusedDocumentAlert({
  id,
  queuedFailed,
  onDismiss,
}: {
  id: string;
  queuedFailed: boolean;
  onDismiss: () => void;
}) {
  const toast = useToast();
  const startedAtRef = useRef(Date.now());
  const [timedOut, setTimedOut] = useState(false);

  useEffect(() => {
    if (queuedFailed) return; // never polling in this case — nothing to time out.
    const timer = setTimeout(() => setTimedOut(true), FOCUS_POLL_TIMEOUT_MS);
    return () => clearTimeout(timer);
  }, [queuedFailed]);

  const docQ = useQuery({
    queryKey: qk.documents.detail(id),
    queryFn: () => api.getJson<DocumentDetailResponse>(`/api/v1/documents/${id}`),
    refetchInterval: (query) => {
      if (queuedFailed) return false;
      if (query.state.data?.data.status !== 'draft') return false;
      if (Date.now() - startedAtRef.current > FOCUS_POLL_TIMEOUT_MS) return false;
      return FOCUS_POLL_INTERVAL_MS;
    },
  });

  const download = useApiMutation(
    () => api.postJson<DownloadUrlResponse>(`/api/v1/documents/${id}/download-url`),
    {
      onSuccess: (data) => window.open(data.url, '_blank', 'noopener,noreferrer'),
      onError: (message, err) => {
        const isConflict = err instanceof ApiClientError && err.status === 409;
        toast.error(isConflict ? 'Файл ещё не готов.' : message);
      },
    },
  );

  const doc = docQ.data?.data;
  const stillRenderingHonestly = !queuedFailed && !timedOut && doc?.status === 'draft';
  const stuck = !queuedFailed && timedOut && doc?.status === 'draft';

  return (
    <div className="space-y-3">
      {queuedFailed && (
        <Alert variant="warning">
          <AlertTitle>Постановка в очередь не удалась</AlertTitle>
          <AlertDescription>
            Документ создан, но фоновая генерация не была поставлена в очередь. Обратитесь позже —
            оператору нужно повторно поставить задачу вручную.
          </AlertDescription>
        </Alert>
      )}
      <Card>
        <CardHeader className="flex flex-row items-center justify-between space-y-0">
          <CardTitle>Новый документ</CardTitle>
          <Button variant="ghost" size="sm" onClick={onDismiss}>
            Скрыть
          </Button>
        </CardHeader>
        <CardContent className="space-y-2 text-sm">
          {docQ.isLoading && <p className="text-muted-foreground">Загрузка статуса…</p>}
          {doc && (
            <>
              <p>
                {DOC_TYPE_LABELS[doc.doc_type] ?? doc.doc_type} — статус:{' '}
                <StatusBadge
                  label={STATUS_LABELS[doc.status] ?? doc.status}
                  tone={STATUS_TONE[doc.status] ?? 'neutral'}
                />
              </p>
              {queuedFailed && doc.status === 'draft' && (
                <p className="text-muted-foreground">
                  Задача рендера не поставлена. Попробуйте сгенерировать документ заново.
                </p>
              )}
              {stillRenderingHonestly && (
                <p className="text-muted-foreground">
                  Рендер выполняется, страница обновится автоматически…
                </p>
              )}
              {stuck && (
                <div className="space-y-2">
                  <p className="text-muted-foreground">
                    Рендер занимает дольше обычного — обновите страницу позже.
                  </p>
                  <Button
                    size="sm"
                    variant="outline"
                    onClick={() => docQ.refetch()}
                    disabled={docQ.isFetching}
                  >
                    Обновить
                  </Button>
                </div>
              )}
              {doc.status !== 'draft' && (
                <Button size="sm" onClick={() => download.mutate()} disabled={download.isPending}>
                  Скачать
                </Button>
              )}
            </>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
