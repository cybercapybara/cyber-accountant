import { useCallback, useEffect, useMemo, useState, type FormEvent, type ReactNode } from 'react';
import { Link, useSearchParams } from 'react-router-dom';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { useQuery, useQueryClient } from '@tanstack/react-query';

import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert';
import { ConfirmDialog } from '@/components/ConfirmDialog';
import { DataTable, type Column } from '@/components/DataTable';
import { FormField } from '@/components/FormField';
import { Modal } from '@/components/Modal';
import { PageHeader } from '@/components/PageHeader';
import { PaginationFooter } from '@/components/PaginationFooter';
import { StatusBadge, type BadgeTone } from '@/components/StatusBadge';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Label } from '@/components/ui/label';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import {
  useDocumentRender,
  RENDER_POLL_INTERVAL_MS,
  RENDER_POLL_TIMEOUT_MS,
} from '@/hooks/useDocumentRender';
import { useErrorToast } from '@/hooks/useErrorToast';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { ApiClientError, api, apiErrorMessage } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type {
  Counterparty,
  CounterpartyListResponse,
  CreateDocumentVersionResponse,
  Document,
  DocumentDetailResponse,
  DocumentListResponse,
  DocumentUploadResponse,
  DocumentVersion,
  DocumentVersionListResponse,
  DownloadUrlResponse,
  GenerateDocumentCreate,
} from '@/lib/api/types';
import { formatIsoDateTimeRu } from '@/lib/dateFormat';
import {
  documentActionAvailability,
  isAwaitingVersionRender,
  snapshotToAvrValues,
  snapshotToHrOrderValues,
  snapshotToInvoiceValues,
  snapshotToLaborContractValues,
  snapshotToReconciliationValues,
  snapshotToSignatories,
  snapshotToTaxInvoiceValues,
  snapshotToWaybillValues,
  voidDocumentSchema,
  type DeleteBlockCode,
  type VoidDocumentValues,
} from '@/lib/schemas/documents';
import {
  buildHrOrderDocumentExtra,
  buildLaborContractDocumentExtra,
  hrOrderDocumentSchema,
  laborContractDocumentSchema,
  type HrOrderDocumentValues,
  type LaborContractDocumentValues,
} from '@/lib/schemas/hr';
import {
  buildFno300DocumentInput,
  buildFno910DocumentInput,
  fno910DocumentSchema,
  type Fno910DocumentValues,
} from '@/lib/schemas/tax';
import {
  AvrForm,
  InvoiceForm,
  ReconciliationForm,
  TaxInvoiceForm,
  WaybillForm,
} from '@/pages/GenerateDocument';

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

const TEMPLATE_LABELS: Record<string, string> = {
  invoice: 'Счёт на оплату',
  avr: 'Акт выполненных работ',
  waybill: 'Накладная на отпуск товара',
  tax_invoice: 'Счёт-фактура',
  reconciliation: 'Акт сверки взаимных расчётов',
  fno_910: 'ФНО 910.00',
  fno_300: 'ФНО 300.00',
  hr_order: 'Кадровый приказ',
  labor_contract: 'Трудовой договор',
  payslip: 'Расчётный листок',
};

/**
 * The rule the page states BEFORE the user clicks anything, rather than
 * letting them discover it as a 409. Nothing in `Document` says whether a
 * document is the basis of a posted entry (that link lives in
 * document_entries, and the API does not expose it), so the honest interface
 * is: say the rule up front, and — once the server has refused a deletion —
 * replace the delete button with the reason and leave voiding as the action
 * that actually works. See documentActionAvailability in
 * lib/schemas/documents.ts.
 */
const DELETE_RULE_HINT =
  'Удалить можно только документ, который ещё не стал основанием проведённой проводки и на ' +
  'который не ссылаются кадровый приказ или налоговая отчётность. В остальных случаях документ ' +
  'аннулируется: запись, файл и вся история версий остаются, документ помечается недействительным.';

const ROLE_DENIED_MESSAGE = 'У вашей роли в организации нет прав на это действие.';

// The polling cadence and hard cap FocusedDocumentAlert used to own inline
// now live in useDocumentRender (hooks/useDocumentRender.ts) — four screens
// wait for a docgen render, and they all need the same two bounds.

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

/**
 * File size for the version history. Not money — no lib/money.ts contract
 * applies — but the same manual-formatting rule as everywhere else in this
 * app: a comma decimal separator and no `toLocaleString`.
 */
function formatBytesRu(bytes: number | null): string {
  if (bytes === null || !Number.isFinite(bytes)) return '—';
  if (bytes < 1024) return `${bytes} Б`;
  const units = ['КБ', 'МБ', 'ГБ'];
  let value = bytes / 1024;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value.toFixed(1).replace('.', ',')} ${units[unit]}`;
}

async function sha256Hex(file: File): Promise<string> {
  const digest = await crypto.subtle.digest('SHA-256', await file.arrayBuffer());
  return Array.from(new Uint8Array(digest))
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
}

/** Was this 409/403 a delete the interface should stop offering at all? */
function deleteBlockFromError(error: unknown): DeleteBlockCode | null {
  if (!(error instanceof ApiClientError) || error.status !== 409) return null;
  if (error.code === 'document_has_posted_entries') return 'document_has_posted_entries';
  if (error.code === 'document_referenced') return 'document_referenced';
  return null;
}

/**
 * Server error → a sentence a Kazakh accountant can act on. The codes are
 * LedgerDocumentsController's own (`not_editable`, `document_voided`,
 * `not_allowed_override`, `schema_validation_failed`, `org_role_denied`);
 * anything else falls through to apiErrorMessage.
 *
 * `org_role_denied` is handled here rather than by hiding the buttons: the
 * SPA does not know the caller's ORGANIZATION role yet (it arrives with
 * /auth/me in task 14), and inventing a second source for it now would only
 * create a divergence that task then has to remove. The server is the source
 * of truth; the interface merely never lies about the outcome.
 *
 * `action` exists because ONE server code, 409 `document_voided`, answers two
 * different requests: editing a voided document and deleting one. Both are
 * only reachable through a concurrent void (the buttons disappear as soon as
 * `voided_at` is set), and a message that names the wrong action is worse
 * than a generic one — so each caller says which request it made.
 */
type DocumentAction = 'edit' | 'delete' | 'void';

const VOIDED_BLOCK_MESSAGE: Record<DocumentAction, string> = {
  edit: 'Аннулированный документ изменить нельзя.',
  delete: 'Аннулированный документ удалить нельзя: он остаётся в реестре как след решения.',
  void: 'Документ уже аннулирован.',
};

function documentErrorMessage(
  error: unknown,
  fallback: string,
  action: DocumentAction = 'edit',
): string {
  if (error instanceof ApiClientError) {
    if (error.code === 'org_role_denied') return ROLE_DENIED_MESSAGE;
    if (error.code === 'not_editable')
      return 'Загруженные и присланные почтой документы не редактируются.';
    if (error.code === 'document_voided' && error.status === 409)
      return VOIDED_BLOCK_MESSAGE[action];
    if (error.code === 'already_voided') return 'Документ уже аннулирован.';
    const override = error.fields?.find((f) => f.code === 'not_allowed_override');
    if (override)
      return `Поле ${override.field ?? 'документа'} вычисляется сервером и не может быть задано вручную.`;
    const schema = error.fields?.find((f) => f.code === 'schema_validation_failed');
    if (schema?.message) return schema.message;
  }
  return apiErrorMessage(error, fallback);
}

/**
 * DocumentsPage — Task 15, extended in P3 task 13. Route: /documents
 * (guard: confirmed).
 *
 * List (GET /api/v1/documents, paginated, optional type/status filters),
 * a client-driven upload flow (uploads → PUT presigned → confirm-upload),
 * a link to /documents/generate, and a per-document card — reached by
 * clicking a row, or automatically via `?focus=<id>&queued=<0|1>` after a
 * successful generate call — that holds the document's whole lifecycle:
 * version history, editing (a new version + a new render), deletion and
 * voiding.
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
  // small enough that this beats a per-row detail fetch. The edit dialog
  // reuses the very same list for its buyer/party select.
  const counterpartiesQ = useQuery({
    queryKey: qk.counterparties.all(),
    queryFn: () =>
      api.getJson<CounterpartyListResponse>('/api/v1/counterparties', {
        query: { limit: 200, offset: 0 },
      }),
  });
  const counterparties = useMemo(() => counterpartiesQ.data?.data ?? [], [counterpartiesQ.data]);
  const counterpartyName = useMemo(() => {
    const map = new Map<string, string>();
    for (const c of counterparties) map.set(c.id, c.name);
    return (id: string | null) => (id ? (map.get(id) ?? '—') : '—');
  }, [counterparties]);

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
      invalidate: [qk.documents.root()],
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

  const clearFocus = useCallback(() => {
    const next = new URLSearchParams(searchParams);
    next.delete('focus');
    next.delete('queued');
    setSearchParams(next, { replace: true });
  }, [searchParams, setSearchParams]);

  const focusDocument = (id: string) => {
    const next = new URLSearchParams(searchParams);
    next.set('focus', id);
    // `queued` describes ONE generate call and must not survive into a
    // hand-picked document, or the card would claim a failed enqueue that
    // never happened.
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
      // The status pill keeps saying what the document WAS (final/sent) —
      // voiding never touches `status`, because an audit needs to know which
      // of the two a void cancelled. The void is a second, unmistakable pill
      // next to it.
      cell: (d) => (
        <span className="flex flex-wrap items-center gap-1">
          <StatusBadge
            label={STATUS_LABELS[d.status] ?? d.status}
            tone={STATUS_TONE[d.status] ?? 'neutral'}
          />
          {d.voided_at && <StatusBadge label="Аннулирован" tone="danger" />}
        </span>
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
          onClick={(e) => {
            // The row itself opens the card; the button must not do both.
            e.stopPropagation();
            download.mutate(d.id);
          }}
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
        <DocumentCard
          key={focusId}
          id={focusId}
          queuedFailed={queuedFailed}
          counterparties={counterparties}
          onDismiss={clearFocus}
        />
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
            // Row-select idiom of DataTable: tabIndex/Enter/Space and the
            // focus ring come from the table itself, the aria-label is ours
            // — and it says out loud that a row is voided, which is
            // otherwise a purely visual pill.
            rowProps={(d) => ({
              onClick: () => focusDocument(d.id),
              'aria-label': `Открыть документ: ${DOC_TYPE_LABELS[d.doc_type] ?? d.doc_type} от ${formatDate(d.created_at)}${d.voided_at ? ', аннулирован' : ''}`,
              className: d.id === focusId ? 'bg-primary/5' : '',
            })}
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
 * The one document's card: status, the void plaque, the version history and
 * the three lifecycle actions.
 *
 * Polling is bounded twice over and split by WHICH wait it is:
 *   - the FIRST render of a freshly created document is a status wait
 *     ('draft' → 'final'), and useDocumentRender owns it, exactly as before
 *     — including `queued=0`, which means no job was ever enqueued and there
 *     is nothing to wait for at all;
 *   - an EDIT does not change `status`; what moves is the current-version
 *     pointer, so the wait is `latest_version_no > (номер текущей версии)`.
 *     That one is polled here, and only here. The two conditions are
 *     mutually exclusive, and that is asserted rather than assumed:
 *     isAwaitingVersionRender (lib/schemas/documents.ts) refuses to fire on
 *     a 'draft' document, and documents.test.ts pins it — so the two
 *     mechanisms never both hammer the same endpoint.
 * Both stop after RENDER_POLL_TIMEOUT_MS: a render that has not finished in
 * two minutes has failed, and a spinner that never stops is a lie.
 */
function DocumentCard({
  id,
  queuedFailed,
  counterparties,
  onDismiss,
}: {
  id: string;
  queuedFailed: boolean;
  counterparties: Counterparty[];
  onDismiss: () => void;
}) {
  const toast = useToast();
  const queryClient = useQueryClient();
  const render = useDocumentRender(id, !queuedFailed);
  const doc = render.document;

  const [editing, setEditing] = useState(false);
  const [confirmingDelete, setConfirmingDelete] = useState(false);
  const [voiding, setVoiding] = useState(false);
  const [deleteBlock, setDeleteBlock] = useState<DeleteBlockCode | null>(null);

  const versionsQ = useQuery({
    queryKey: qk.documents.versions(id),
    queryFn: () => api.getJson<DocumentVersionListResponse>(`/api/v1/documents/${id}/versions`),
  });
  const versions = useMemo(() => versionsQ.data?.data ?? [], [versionsQ.data]);

  const currentVersionNo = useMemo(() => {
    if (!doc?.current_version_id) return null;
    return versions.find((v) => v.id === doc.current_version_id)?.version_no ?? null;
  }, [versions, doc?.current_version_id]);

  const pendingVersionNo =
    doc && currentVersionNo !== null && doc.latest_version_no > currentVersionNo
      ? doc.latest_version_no
      : null;
  // A voided document is never re-rendered, and a still-'draft' one is the
  // status poll's business — the exclusion lives in isAwaitingVersionRender
  // (lib/schemas/documents.ts) and is pinned by a test there.
  const awaitingRender = doc ? isAwaitingVersionRender(doc, currentVersionNo) : false;

  const refreshDocument = useCallback(() => {
    void queryClient.invalidateQueries({ queryKey: qk.documents.detail(id) });
    void queryClient.invalidateQueries({ queryKey: qk.documents.versions(id) });
  }, [queryClient, id]);

  const [versionPollTimedOut, setVersionPollTimedOut] = useState(false);
  useEffect(() => {
    if (!awaitingRender) {
      setVersionPollTimedOut(false);
      return;
    }
    setVersionPollTimedOut(false);
    const interval = setInterval(refreshDocument, RENDER_POLL_INTERVAL_MS);
    const timer = setTimeout(() => {
      clearInterval(interval);
      setVersionPollTimedOut(true);
    }, RENDER_POLL_TIMEOUT_MS);
    return () => {
      clearInterval(interval);
      clearTimeout(timer);
    };
  }, [awaitingRender, refreshDocument]);

  const availability = documentActionAvailability(
    doc ?? { source: 'generated', template_slug: null, voided_at: null },
    deleteBlock,
  );

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

  const downloadVersion = useApiMutation(
    (versionNo: number) =>
      api.postJson<DownloadUrlResponse>(
        `/api/v1/documents/${id}/versions/${versionNo}/download-url`,
      ),
    {
      onSuccess: (data) => window.open(data.url, '_blank', 'noopener,noreferrer'),
      onError: (message, err) => {
        const isConflict = err instanceof ApiClientError && err.status === 409;
        toast.error(isConflict ? 'Файл этой версии ещё не готов.' : message);
      },
    },
  );

  const edit = useApiMutation(
    (input: Record<string, unknown>) =>
      api.postJson<CreateDocumentVersionResponse>(`/api/v1/documents/${id}/versions`, {
        body: { input },
      }),
    {
      invalidate: [qk.documents.root()],
      onSuccess: (data) => {
        setEditing(false);
        toast.success(
          data.render_queued
            ? `Создана версия ${data.version_no}, идёт рендер.`
            : `Создана версия ${data.version_no}, но фоновый рендер не был поставлен в очередь.`,
        );
      },
      onError: (_message, err) => {
        toast.error(documentErrorMessage(err, 'Не удалось сохранить изменения.', 'edit'));
      },
    },
  );

  // api.DELETE, not api.deleteJson: a successful delete is 204 WITH NO BODY,
  // and the *Json helpers reject an empty body as a failure (see fetchJson in
  // lib/api/client.ts). Going through the { data, error } pair instead is the
  // difference between "удалён" and a spurious error toast over a document
  // that is already gone.
  const remove = useApiMutation(
    async () => {
      const { error } = await api.DELETE(`/api/v1/documents/${id}`);
      if (error) throw error;
    },
    {
      invalidate: [qk.documents.root()],
      onSuccess: () => {
        setConfirmingDelete(false);
        toast.success('Документ удалён.');
        onDismiss();
      },
      onError: (_message, err) => {
        const block = deleteBlockFromError(err);
        setConfirmingDelete(false);
        if (block) {
          // The honest affordance: the server has just said this document can
          // only be voided, so stop offering deletion at all and open the
          // dialog that will actually work.
          setDeleteBlock(block);
          toast.error(
            block === 'document_has_posted_entries'
              ? 'Документ связан с проведённой проводкой — его можно только аннулировать.'
              : 'На документ ссылается кадровый приказ или налоговая отчётность — доступно только аннулирование.',
          );
          setVoiding(true);
          return;
        }
        toast.error(documentErrorMessage(err, 'Не удалось удалить документ.', 'delete'));
      },
    },
  );

  const voidDocument = useApiMutation(
    (values: VoidDocumentValues) =>
      api.postJson<DocumentDetailResponse>(`/api/v1/documents/${id}/void`, {
        body: { reason: values.reason },
      }),
    {
      invalidate: [qk.documents.root()],
      onSuccess: () => {
        setVoiding(false);
        toast.success('Документ аннулирован.');
      },
      onError: (_message, err) => {
        toast.error(documentErrorMessage(err, 'Не удалось аннулировать документ.', 'void'));
      },
    },
  );

  const versionColumns: Column<DocumentVersion>[] = [
    {
      header: 'Версия',
      className: 'whitespace-nowrap',
      cell: (v) => (
        <span className="flex items-center gap-2">
          <span className="font-mono">№ {v.version_no}</span>
          {doc?.current_version_id === v.id && <StatusBadge label="Текущая" tone="success" />}
        </span>
      ),
    },
    { header: 'Создана', className: 'whitespace-nowrap', cell: (v) => formatDate(v.created_at) },
    { header: 'Размер', className: 'whitespace-nowrap', cell: (v) => formatBytesRu(v.size_bytes) },
    {
      header: '',
      className: 'text-right',
      cell: (v) =>
        v.s3_key ? (
          <Button
            size="sm"
            variant="outline"
            disabled={downloadVersion.isPending}
            onClick={() => downloadVersion.mutate(v.version_no)}
          >
            Скачать
          </Button>
        ) : (
          <span className="text-muted-foreground">Файл ещё не готов</span>
        ),
    },
  ];

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

      {doc?.voided_at && (
        <Alert variant="destructive">
          <AlertTitle>Документ аннулирован</AlertTitle>
          <AlertDescription>
            {formatDate(doc.voided_at)}
            {doc.void_reason ? ` — ${doc.void_reason}` : ''}. Документ остаётся в реестре вместе с
            файлом и историей версий, но больше не изменяется и не перерендеривается.
          </AlertDescription>
        </Alert>
      )}

      <Card>
        <CardHeader className="flex flex-row items-center justify-between space-y-0">
          <CardTitle>{doc?.voided_at ? 'Документ (аннулирован)' : 'Документ'}</CardTitle>
          <Button variant="ghost" size="sm" onClick={onDismiss}>
            Скрыть
          </Button>
        </CardHeader>
        <CardContent className="space-y-4 text-sm">
          {render.isLoading && <p className="text-muted-foreground">Загрузка статуса…</p>}
          {doc && (
            <>
              <p className="flex flex-wrap items-center gap-2">
                <span>{DOC_TYPE_LABELS[doc.doc_type] ?? doc.doc_type}</span>
                <StatusBadge
                  label={STATUS_LABELS[doc.status] ?? doc.status}
                  tone={STATUS_TONE[doc.status] ?? 'neutral'}
                />
                {doc.voided_at && <StatusBadge label="Аннулирован" tone="danger" />}
                <span className="text-muted-foreground">
                  {SOURCE_LABELS[doc.source] ?? doc.source}
                  {doc.template_slug
                    ? ` · ${TEMPLATE_LABELS[doc.template_slug] ?? doc.template_slug}`
                    : ''}
                </span>
              </p>

              {queuedFailed && doc.status === 'draft' && (
                <p className="text-muted-foreground">
                  Задача рендера не поставлена. Попробуйте сгенерировать документ заново.
                </p>
              )}
              {render.rendering && (
                <p className="text-muted-foreground">
                  Рендер выполняется, страница обновится автоматически…
                </p>
              )}
              {render.stuck && (
                <div className="space-y-2">
                  <p className="text-muted-foreground">
                    Рендер занимает дольше обычного — обновите страницу позже.
                  </p>
                  <Button
                    size="sm"
                    variant="outline"
                    onClick={() => render.refetch()}
                    disabled={render.isFetching}
                  >
                    Обновить
                  </Button>
                </div>
              )}

              <div className="flex flex-wrap items-center gap-2">
                {render.ready && !doc.voided_at && (
                  <Button size="sm" onClick={() => download.mutate()} disabled={download.isPending}>
                    Скачать
                  </Button>
                )}
                {doc.voided_at && doc.s3_key && (
                  <Button
                    size="sm"
                    variant="outline"
                    onClick={() => download.mutate()}
                    disabled={download.isPending}
                  >
                    Скачать
                  </Button>
                )}
                {availability.canEdit && (
                  <Button size="sm" variant="outline" onClick={() => setEditing(true)}>
                    Изменить
                  </Button>
                )}
                {availability.canDelete && (
                  <Button
                    size="sm"
                    variant="destructive"
                    onClick={() => setConfirmingDelete(true)}
                    disabled={remove.isPending}
                  >
                    Удалить
                  </Button>
                )}
                {availability.canVoid && (
                  <Button size="sm" variant="outline" onClick={() => setVoiding(true)}>
                    Аннулировать
                  </Button>
                )}
              </div>

              <div className="space-y-1 text-muted-foreground">
                {availability.editBlockReason && <p>{availability.editBlockReason}</p>}
                {availability.deleteBlockReason && <p>{availability.deleteBlockReason}</p>}
                {!doc.voided_at && <p>{DELETE_RULE_HINT}</p>}
              </div>
            </>
          )}
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>История версий</CardTitle>
        </CardHeader>
        <CardContent className="space-y-3 overflow-x-auto">
          {pendingVersionNo !== null && !doc?.voided_at && (
            <p className="text-sm text-muted-foreground">
              Версия {pendingVersionNo} готовится — файл появится после рендера.
              {versionPollTimedOut && ' Рендер занимает дольше обычного.'}
            </p>
          )}
          {versionPollTimedOut && (
            <Button
              size="sm"
              variant="outline"
              onClick={refreshDocument}
              disabled={versionsQ.isFetching}
            >
              Обновить
            </Button>
          )}
          <DataTable
            columns={versionColumns}
            rows={versionsQ.data?.data}
            rowKey={(v) => v.id}
            isLoading={versionsQ.isLoading}
            error={versionsQ.error}
            emptyText="Версий пока нет."
          />
        </CardContent>
      </Card>

      {editing && doc && (
        <EditDocumentModal
          doc={doc}
          counterparties={counterparties}
          submitting={edit.isPending}
          onSubmit={(input) => edit.mutate(input)}
          onClose={() => setEditing(false)}
        />
      )}

      {confirmingDelete && (
        <ConfirmDialog
          title="Удалить документ?"
          description="Документ будет удалён безвозвратно вместе со всеми версиями. Файлы в хранилище останутся."
          confirmLabel="Удалить"
          destructive
          busy={remove.isPending}
          onConfirm={() => remove.mutate()}
          onClose={() => setConfirmingDelete(false)}
        />
      )}

      {voiding && (
        <VoidDocumentModal
          submitting={voidDocument.isPending}
          onSubmit={(values) => voidDocument.mutate(values)}
          onClose={() => setVoiding(false)}
        />
      )}
    </div>
  );
}

/**
 * Reason-for-voiding dialog. A separate modal rather than ConfirmDialog
 * because the reason is a required FIELD, not a confirmation: the server
 * rejects a blank one with a 422, and it is the only thing the audit trail
 * will have to explain the decision later.
 */
function VoidDocumentModal({
  submitting,
  onSubmit,
  onClose,
}: {
  submitting: boolean;
  onSubmit: (values: VoidDocumentValues) => void;
  onClose: () => void;
}) {
  const {
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<VoidDocumentValues>({
    resolver: zodResolver(voidDocumentSchema),
    defaultValues: { reason: '' },
  });

  return (
    <Modal onClose={onClose} className="max-w-lg">
      <Card>
        <CardHeader>
          <CardTitle>Аннулировать документ</CardTitle>
        </CardHeader>
        <CardContent>
          <form className="space-y-4" onSubmit={handleSubmit(onSubmit)}>
            <p className="text-sm text-muted-foreground">
              Документ останется в реестре вместе с файлом и историей версий и будет помечен как
              недействительный. Отменить аннулирование нельзя.
            </p>
            <FormField
              id="void-reason"
              label="Причина аннулирования"
              placeholder="например: ошибка в реквизитах покупателя"
              error={errors.reason?.message}
              {...register('reason')}
            />
            <div className="flex gap-2">
              <Button type="submit" variant="destructive" disabled={submitting}>
                {submitting ? 'Аннулирование…' : 'Аннулировать'}
              </Button>
              <Button type="button" variant="ghost" onClick={onClose} disabled={submitting}>
                Отмена
              </Button>
            </div>
          </form>
        </CardContent>
      </Card>
    </Modal>
  );
}

/**
 * The edit dialog. It does NOT own a single form of its own for the five
 * caller-authored templates — those are the very components
 * /documents/generate renders, imported and prefilled from the stored render
 * input, because the edit endpoint takes exactly the same `input` object
 * through exactly the same allowlist. A second copy of them would be a
 * second place for the money contract to drift.
 *
 * The server-built forms (ФНО, кадровый приказ, трудовой договор) have no
 * counterpart on the generation page — they are created by the endpoints
 * that own their data — so their few allowlisted fields get a small local
 * form each, built on the SAME zod schemas and the SAME builders those pages
 * already use (lib/schemas/tax.ts, lib/schemas/hr.ts).
 */
function EditDocumentModal({
  doc,
  counterparties,
  submitting,
  onSubmit,
  onClose,
}: {
  doc: Document;
  counterparties: Counterparty[];
  submitting: boolean;
  onSubmit: (input: Record<string, unknown>) => void;
  onClose: () => void;
}) {
  const slug = doc.template_slug;
  const snapshot = doc.input_snapshot;
  const counterpartyId = doc.counterparty_id;
  // The generation forms hand back a whole GenerateDocumentCreate; an edit
  // wants only its `input` — the endpoint already knows the template and the
  // counterparty from the row it is versioning.
  const shared = {
    counterparties,
    submitting,
    // `input` is optional on the wire (an empty edit is a faithful re-render
    // of the stored input), and every builder above always fills it.
    onSubmit: (body: GenerateDocumentCreate) => onSubmit(body.input ?? {}),
    submitLabel: 'Сохранить и создать версию',
    busyLabel: 'Сохранение…',
  };

  return (
    <Modal onClose={onClose} className="max-w-4xl">
      <div className="space-y-3">
        <Alert>
          <AlertTitle>Правка создаёт новую версию</AlertTitle>
          <AlertDescription>
            Предыдущий файл остаётся в истории версий и по-прежнему скачивается. Итоговые суммы и
            суммы прописью считает сервер — их в форме нет.
          </AlertDescription>
        </Alert>

        {slug === 'invoice' && (
          <InvoiceForm
            {...shared}
            defaultValues={snapshotToInvoiceValues(snapshot, counterpartyId)}
          />
        )}
        {slug === 'avr' && (
          <AvrForm {...shared} defaultValues={snapshotToAvrValues(snapshot, counterpartyId)} />
        )}
        {slug === 'waybill' && (
          <WaybillForm
            {...shared}
            defaultValues={snapshotToWaybillValues(snapshot, counterpartyId)}
          />
        )}
        {slug === 'tax_invoice' && (
          <TaxInvoiceForm
            {...shared}
            defaultValues={snapshotToTaxInvoiceValues(snapshot, counterpartyId)}
          />
        )}
        {slug === 'reconciliation' && (
          <ReconciliationForm
            {...shared}
            defaultValues={snapshotToReconciliationValues(snapshot, counterpartyId)}
          />
        )}
        {(slug === 'fno_910' || slug === 'fno_300') && (
          <SignatoriesEditForm
            title={TEMPLATE_LABELS[slug]}
            defaultValues={snapshotToSignatories(snapshot)}
            submitting={submitting}
            onSubmit={(values) =>
              onSubmit(
                slug === 'fno_910'
                  ? buildFno910DocumentInput(values)
                  : buildFno300DocumentInput(values),
              )
            }
            onCancel={onClose}
          />
        )}
        {slug === 'hr_order' && (
          <HrOrderEditForm
            defaultValues={snapshotToHrOrderValues(snapshot)}
            submitting={submitting}
            onSubmit={(values) => onSubmit(buildHrOrderDocumentExtra(values))}
            onCancel={onClose}
          />
        )}
        {slug === 'labor_contract' && (
          <LaborContractEditForm
            defaultValues={snapshotToLaborContractValues(snapshot)}
            submitting={submitting}
            onSubmit={(values) => onSubmit(buildLaborContractDocumentExtra(values))}
            onCancel={onClose}
          />
        )}

        <div className="flex justify-end">
          <Button variant="ghost" onClick={onClose} disabled={submitting}>
            Закрыть
          </Button>
        </div>
      </div>
    </Modal>
  );
}

/**
 * ФНО 910.00 / 300.00 — the two signatories are the entire allowlist
 * (Docgen::InputPolicy::editable_fields). Every figure on the form, the
 * amount in words included, is derived from the stored calculation and a
 * client-supplied one is a 422 `not_allowed_override`. Both forms share one
 * schema (they always had the same shape), hence one component.
 */
function SignatoriesEditForm({
  title,
  defaultValues,
  submitting,
  onSubmit,
  onCancel,
}: {
  title: string;
  defaultValues: Fno910DocumentValues;
  submitting: boolean;
  onSubmit: (values: Fno910DocumentValues) => void;
  onCancel: () => void;
}) {
  const {
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<Fno910DocumentValues>({
    resolver: zodResolver(fno910DocumentSchema),
    defaultValues,
  });

  return (
    <EditFormCard
      title={title}
      submitting={submitting}
      onCancel={onCancel}
      onSubmit={handleSubmit(onSubmit)}
    >
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        <FormField
          id="edit-fno-director"
          label="Руководитель"
          error={errors.director?.message}
          {...register('director')}
        />
        <FormField
          id="edit-fno-accountant"
          label="Бухгалтер"
          error={errors.accountant?.message}
          {...register('accountant')}
        />
      </div>
    </EditFormCard>
  );
}

function HrOrderEditForm({
  defaultValues,
  submitting,
  onSubmit,
  onCancel,
}: {
  defaultValues: HrOrderDocumentValues;
  submitting: boolean;
  onSubmit: (values: HrOrderDocumentValues) => void;
  onCancel: () => void;
}) {
  const {
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<HrOrderDocumentValues>({
    resolver: zodResolver(hrOrderDocumentSchema),
    defaultValues,
  });

  return (
    <EditFormCard
      title="Кадровый приказ"
      submitting={submitting}
      onCancel={onCancel}
      onSubmit={handleSubmit(onSubmit)}
    >
      <FormField
        id="edit-hr-director"
        label="Руководитель"
        error={errors.director?.message}
        {...register('director')}
      />
      <FormField
        id="edit-hr-reason"
        label="Основание"
        error={errors.reason?.message}
        {...register('reason')}
      />
      <FormField
        id="edit-hr-details"
        label="Детали"
        error={errors.details?.message}
        {...register('details')}
      />
    </EditFormCard>
  );
}

function LaborContractEditForm({
  defaultValues,
  submitting,
  onSubmit,
  onCancel,
}: {
  defaultValues: LaborContractDocumentValues;
  submitting: boolean;
  onSubmit: (values: LaborContractDocumentValues) => void;
  onCancel: () => void;
}) {
  const {
    register,
    handleSubmit,
    formState: { errors },
  } = useForm<LaborContractDocumentValues>({
    resolver: zodResolver(laborContractDocumentSchema),
    defaultValues,
  });

  return (
    <EditFormCard
      title="Трудовой договор"
      submitting={submitting}
      onCancel={onCancel}
      onSubmit={handleSubmit(onSubmit)}
    >
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        <FormField
          id="edit-lc-schedule"
          label="Режим работы"
          error={errors.work_schedule?.message}
          {...register('work_schedule')}
        />
        <FormField
          id="edit-lc-probation"
          label="Испытательный срок, мес."
          inputMode="numeric"
          error={errors.probation_months?.message}
          {...register('probation_months')}
        />
      </div>
      <FormField
        id="edit-lc-director"
        label="Директор (работодатель)"
        error={errors.director?.message}
        {...register('director')}
      />
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        <FormField
          id="edit-lc-employer-address"
          label="Адрес работодателя"
          error={errors.employer_address?.message}
          {...register('employer_address')}
        />
        <FormField
          id="edit-lc-employee-address"
          label="Адрес работника"
          error={errors.employee_address?.message}
          {...register('employee_address')}
        />
      </div>
    </EditFormCard>
  );
}

/** Card + submit/cancel row shared by the three server-built edit forms. */
function EditFormCard({
  title,
  submitting,
  onSubmit,
  onCancel,
  children,
}: {
  title: string;
  submitting: boolean;
  onSubmit: (e: FormEvent) => void;
  onCancel: () => void;
  children: ReactNode;
}) {
  return (
    <Card>
      <CardHeader>
        <CardTitle>{title}</CardTitle>
      </CardHeader>
      <CardContent>
        <form className="space-y-4" onSubmit={onSubmit}>
          {children}
          <p className="text-sm text-muted-foreground">
            Остальные поля документа сервер переносит из предыдущей версии без изменений.
          </p>
          <div className="flex gap-2">
            <Button type="submit" disabled={submitting}>
              {submitting ? 'Сохранение…' : 'Сохранить и создать версию'}
            </Button>
            <Button type="button" variant="ghost" onClick={onCancel} disabled={submitting}>
              Отмена
            </Button>
          </div>
        </form>
      </CardContent>
    </Card>
  );
}
