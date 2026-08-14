import { useEffect, useRef, useState } from 'react';
import { useQuery } from '@tanstack/react-query';

import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { Document, DocumentDetailResponse } from '@/lib/api/types';

/**
 * Bounded polling for a just-enqueued docgen document.
 *
 * Four screens now enqueue a `docgen.render` job and then wait for the file:
 * Documents (`?focus=`), Кадры (приказ / трудовой договор), Зарплата
 * (расчётный листок) and Налоги (печатная форма ФНО). They had copied the
 * same `refetchInterval` + timeout dance twice already; this hook is the
 * single implementation.
 *
 * Bounded TWICE over, and both bounds matter:
 *   - `renderQueued === false` means no job was ever enqueued (a Redis blip;
 *     every enqueue in this codebase is best-effort and still answers 202),
 *     so there is nothing to wait for and the hook never polls at all;
 *   - otherwise polling stops after RENDER_POLL_TIMEOUT_MS even if the
 *     document is still 'draft'. A failed LaTeX render or exhausted job
 *     retries leave a document in 'draft' FOREVER, so an unbounded poll
 *     would be indistinguishable from "stuck" and would keep hammering the
 *     API from an idle tab.
 */
export const RENDER_POLL_INTERVAL_MS = 2000;
export const RENDER_POLL_TIMEOUT_MS = 120_000;

export interface DocumentRenderState {
  document: Document | undefined;
  isLoading: boolean;
  isFetching: boolean;
  /** The poll is live and the document has not left 'draft' yet. */
  rendering: boolean;
  /** The poll gave up and the document is STILL 'draft' — say so plainly. */
  stuck: boolean;
  /** The render finished: the file can be downloaded. */
  ready: boolean;
  refetch: () => void;
}

export function useDocumentRender(
  documentId: string | null | undefined,
  renderQueued: boolean,
): DocumentRenderState {
  const startedAtRef = useRef(Date.now());
  const [timedOut, setTimedOut] = useState(false);

  useEffect(() => {
    // Never polling in this case — nothing to time out.
    if (!renderQueued || !documentId) return;
    startedAtRef.current = Date.now();
    setTimedOut(false);
    const timer = setTimeout(() => setTimedOut(true), RENDER_POLL_TIMEOUT_MS);
    return () => clearTimeout(timer);
  }, [renderQueued, documentId]);

  const query = useQuery({
    queryKey: qk.documents.detail(documentId ?? ''),
    enabled: !!documentId,
    queryFn: () => api.getJson<DocumentDetailResponse>(`/api/v1/documents/${documentId}`),
    refetchInterval: (q) => {
      if (!renderQueued) return false;
      if (q.state.data?.data.status !== 'draft') return false;
      if (Date.now() - startedAtRef.current > RENDER_POLL_TIMEOUT_MS) return false;
      return RENDER_POLL_INTERVAL_MS;
    },
  });

  const document = query.data?.data;
  return {
    document,
    isLoading: query.isLoading,
    isFetching: query.isFetching,
    rendering: renderQueued && !timedOut && document?.status === 'draft',
    stuck: renderQueued && timedOut && document?.status === 'draft',
    ready: !!document && document.status !== 'draft',
    refetch: () => void query.refetch(),
  };
}
