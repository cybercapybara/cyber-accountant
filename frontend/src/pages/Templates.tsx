import { useState } from 'react';
import { useQuery } from '@tanstack/react-query';

import { PageHeader } from '@/components/PageHeader';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { DocumentTemplate, DocumentTemplateListResponse } from '@/lib/api/types';
import { BlockEditor, type Block } from '@/components/BlockEditor';

/**
 * TemplatesPage — конструктор шаблонов документов. Маршрут: /templates.
 *
 * ПУБЛИКАЦИЯ АСИНХРОННА, и интерфейс это не скрывает. Гейт рендерит шаблон
 * настоящим движком, а движок есть только в воркере — поэтому кнопка
 * «Опубликовать» ставит задачу и честно говорит, что вердикт придёт позже, а
 * шаблон пока остаётся черновиком. Врать «опубликовано» до проверки нельзя:
 * ровно из этого выросли бы документы по шаблону, который не собирается.
 *
 * Опубликованную версию править невозможно — сервер отвечает 409, а не молча
 * создаёт расхождение с уже выпущенными документами. В интерфейсе она поэтому
 * открывается только на чтение, а кнопка предлагает создать НОВУЮ версию.
 */
export function TemplatesPage() {
  const toast = useToast();
  const [editing, setEditing] = useState<DocumentTemplate | null>(null);
  const [draftBlocks, setDraftBlocks] = useState<Block[]>([]);
  const [draftSlug, setDraftSlug] = useState('');

  const templates = useQuery({
    queryKey: qk.templates.all(),
    queryFn: async () => {
      const { data, error } = await api.GET('/api/v1/org-templates');
      if (error) throw error;
      return (data as DocumentTemplateListResponse).data;
    },
  });

  const create = useApiMutation(
    (vars: { slug: string; blocks: Block[] }) =>
      api.postJson('/api/v1/org-templates', { body: { slug: vars.slug, blocks: vars.blocks } }),
    {
      invalidate: [qk.templates.all()],
      onSuccess: () => {
        toast.success('Черновик создан');
        setDraftBlocks([]);
        setDraftSlug('');
      },
    },
  );
  useErrorToast(create.error);

  const save = useApiMutation(
    (vars: { id: string; blocks: Block[] }) =>
      api.patchJson(`/api/v1/org-templates/${vars.id}`, { body: { blocks: vars.blocks } }),
    {
      invalidate: [qk.templates.all()],
      onSuccess: () => toast.success('Черновик сохранён'),
    },
  );
  useErrorToast(save.error);

  const publish = useApiMutation(
    (id: string) => api.postJson(`/api/v1/org-templates/${id}/publish`),
    {
      invalidate: [qk.templates.all()],
      onSuccess: () =>
        toast.info(
          'Проверка запущена. Шаблон станет опубликованным, только если он собирается и печатает все свои подписи.',
        ),
    },
  );
  useErrorToast(publish.error);

  const archive = useApiMutation((id: string) => api.deleteJson(`/api/v1/org-templates/${id}`), {
    invalidate: [qk.templates.all()],
    onSuccess: () => {
      toast.success('Шаблон архивирован');
      setEditing(null);
    },
  });
  useErrorToast(archive.error);

  const rows = templates.data ?? [];

  return (
    <div className="space-y-6">
      <PageHeader
        title="Шаблоны документов"
        description="Собираются из блоков. Опубликованная версия неизменяема: правка создаёт новую, чтобы выпущенные документы остались воспроизводимыми."
      />

      <Card>
        <CardHeader>
          <CardTitle>Мои шаблоны</CardTitle>
        </CardHeader>
        <CardContent>
          {templates.isPending ? (
            <p className="text-sm text-muted-foreground">Загружается…</p>
          ) : rows.length === 0 ? (
            <p className="text-sm text-muted-foreground">
              Своих шаблонов пока нет — документы печатаются встроенными.
            </p>
          ) : (
            <ul className="divide-y">
              {rows.map((t) => (
                <li key={t.id} className="flex flex-wrap items-center gap-3 py-2 text-sm">
                  <span className="font-medium">{t.slug}</span>
                  <span className="text-muted-foreground">версия {t.version}</span>
                  <span className="rounded bg-muted px-2 py-0.5">{statusLabel(t.status)}</span>
                  {t.org_id === null ? (
                    <span className="text-muted-foreground">шаблон площадки</span>
                  ) : null}
                  <Button
                    type="button"
                    variant="ghost"
                    onClick={() => {
                      setEditing(t);
                      setDraftBlocks(((t.blocks ?? []) as Block[]) ?? []);
                    }}
                  >
                    {t.status === 'draft' && t.org_id !== null ? 'Править' : 'Посмотреть'}
                  </Button>
                  {t.status === 'draft' && t.org_id !== null ? (
                    <>
                      <Button type="button" variant="ghost" onClick={() => publish.mutate(t.id)}>
                        Опубликовать
                      </Button>
                      <Button type="button" variant="ghost" onClick={() => archive.mutate(t.id)}>
                        Архивировать
                      </Button>
                    </>
                  ) : null}
                </li>
              ))}
            </ul>
          )}
        </CardContent>
      </Card>

      {editing ? (
        <Card>
          <CardHeader>
            <CardTitle>
              {editing.slug} — версия {editing.version} ({statusLabel(editing.status)})
            </CardTitle>
          </CardHeader>
          <CardContent className="space-y-4">
            {editing.status !== 'draft' ? (
              <p className="text-sm">
                Эта версия опубликована и неизменяема: документы, выпущенные по ней, обязаны
                остаться воспроизводимыми. Чтобы что-то поменять, создайте новую версию ниже.
              </p>
            ) : null}
            <BlockEditor
              blocks={draftBlocks}
              onChange={setDraftBlocks}
              readOnly={editing.status !== 'draft' || editing.org_id === null}
            />
            {editing.status === 'draft' && editing.org_id !== null ? (
              <Button
                type="button"
                disabled={save.isPending}
                onClick={() => save.mutate({ id: editing.id, blocks: draftBlocks })}
              >
                {save.isPending ? 'Сохранение…' : 'Сохранить черновик'}
              </Button>
            ) : null}
            <Button type="button" variant="ghost" onClick={() => setEditing(null)}>
              Закрыть
            </Button>
          </CardContent>
        </Card>
      ) : null}

      <Card>
        <CardHeader>
          <CardTitle>Новый шаблон</CardTitle>
        </CardHeader>
        <CardContent className="space-y-4">
          <label className="block text-sm">
            Код шаблона
            <input
              className="mt-1 w-full rounded-md border px-3 py-2"
              placeholder="my_invoice"
              value={draftSlug}
              onChange={(e) => setDraftSlug(e.target.value)}
            />
            <span className="mt-1 block text-xs text-muted-foreground">
              Строчные латинские буквы, цифры и подчёркивание: код становится и типом документа.
            </span>
          </label>
          <BlockEditor blocks={draftBlocks} onChange={setDraftBlocks} />
          <Button
            type="button"
            disabled={create.isPending || !draftSlug || draftBlocks.length === 0}
            onClick={() => create.mutate({ slug: draftSlug, blocks: draftBlocks })}
          >
            {create.isPending ? 'Создание…' : 'Создать черновик'}
          </Button>
        </CardContent>
      </Card>
    </div>
  );
}

function statusLabel(status: string): string {
  if (status === 'draft') return 'черновик';
  if (status === 'published') return 'опубликован';
  return 'в архиве';
}
