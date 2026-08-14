import type { ReactNode } from 'react';

import { StatusBadge, type BadgeTone } from '@/components/StatusBadge';
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useDocumentRender } from '@/hooks/useDocumentRender';
import { ApiClientError, api } from '@/lib/api/client';
import type { DownloadUrlResponse } from '@/lib/api/types';

/**
 * Result of a successful …/generate-document call — what every caller of
 * this card holds on to.
 */
export interface GeneratedDocument {
  documentId: string;
  renderQueued: boolean;
}

const DOC_STATUS_LABELS: Record<string, string> = {
  draft: 'Черновик',
  final: 'Готов',
  sent: 'Отправлен',
};

const DOC_STATUS_TONE: Record<string, BadgeTone> = {
  draft: 'warning',
  final: 'success',
  sent: 'info',
};

/**
 * The "your document is being generated" card, shared by every screen that
 * enqueues a docgen render (Кадры, Зарплата, Налоги). Polls the document
 * until it leaves 'draft' — bounded by `useDocumentRender`, see that hook —
 * and then offers the download.
 *
 * `renderQueued === false` is a real, reachable outcome: enqueueing is
 * best-effort everywhere in this codebase (the document row already exists,
 * so a Redis blip must not become a 500 the client retries into an orphan),
 * and the endpoint still answers 202. The card says so instead of spinning
 * forever on a job that was never submitted.
 */
export function GeneratedDocumentCard({
  documentId,
  renderQueued,
  title = 'Сформированный документ',
  onDismiss,
  children,
}: {
  documentId: string;
  renderQueued: boolean;
  /** Card heading — the calling screen names its own artifact. */
  title?: string;
  onDismiss: () => void;
  /** Extra content under the status block (e.g. a link to Документы). */
  children?: ReactNode;
}) {
  const toast = useToast();
  const render = useDocumentRender(documentId, renderQueued);

  const download = useApiMutation(
    () => api.postJson<DownloadUrlResponse>(`/api/v1/documents/${documentId}/download-url`),
    {
      onSuccess: (data) => window.open(data.url, '_blank', 'noopener,noreferrer'),
      onError: (message, err) => {
        const isConflict = err instanceof ApiClientError && err.status === 409;
        toast.error(isConflict ? 'Файл ещё не готов.' : message);
      },
    },
  );

  const doc = render.document;

  return (
    <div className="space-y-3">
      {!renderQueued && (
        <Alert variant="warning">
          <AlertTitle>Постановка в очередь не удалась</AlertTitle>
          <AlertDescription>
            Документ создан, но фоновая генерация не была поставлена в очередь. Попробуйте
            сформировать документ заново.
          </AlertDescription>
        </Alert>
      )}
      <Card>
        <CardHeader className="flex flex-row items-center justify-between space-y-0">
          <CardTitle>{title}</CardTitle>
          <Button variant="ghost" size="sm" onClick={onDismiss}>
            Скрыть
          </Button>
        </CardHeader>
        <CardContent className="space-y-2 text-sm">
          {render.isLoading && <p className="text-muted-foreground">Загрузка статуса…</p>}
          {doc && (
            <>
              <p>
                Статус:{' '}
                <StatusBadge
                  label={DOC_STATUS_LABELS[doc.status] ?? doc.status}
                  tone={DOC_STATUS_TONE[doc.status] ?? 'neutral'}
                />
              </p>
              {render.rendering && (
                <p className="text-muted-foreground">
                  Рендер выполняется, статус обновится автоматически…
                </p>
              )}
              {render.stuck && (
                <div className="space-y-2">
                  <p className="text-muted-foreground">
                    Рендер занимает дольше обычного — проверьте статус позже.
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
              {render.ready && (
                <Button size="sm" onClick={() => download.mutate()} disabled={download.isPending}>
                  Скачать
                </Button>
              )}
              {children}
            </>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
