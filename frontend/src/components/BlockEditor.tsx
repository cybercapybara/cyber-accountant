import { useQuery } from '@tanstack/react-query';

import { Button } from '@/components/ui/button';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';

/**
 * Редактор блоков шаблона.
 *
 * Набор типов блоков ЗАКРЫТ на сервере, и здесь он повторён нарочно списком, а
 * не выведен из чего-либо: сервер отвергнет неизвестный тип с указанием номера
 * блока, поэтому расхождение проявится сразу и внятно, а не молча пропущенным
 * блоком в документе.
 *
 * Переменные подставляются ТОЛЬКО из каталога (GET /api/v1/template-variables).
 * Свободный ввод имени переменной не предусмотрен: опечатка тогда печатала бы
 * в документе пустоту, о которой никто не узнает, — поэтому на сервере
 * неизвестный идентификатор это отказ, а здесь его просто нельзя ввести.
 */

export type Block = Record<string, unknown>;

const BLOCK_TYPES: { type: string; label: string; hint: string }[] = [
  { type: 'header', label: 'Заголовок', hint: 'Название документа по центру' },
  { type: 'text', label: 'Абзац', hint: 'Произвольный текст' },
  { type: 'fields', label: 'Поля', hint: 'Пары «подпись: значение»' },
  { type: 'table', label: 'Таблица', hint: 'Позиции документа' },
  { type: 'totals', label: 'Итоги', hint: 'Сумма и сумма прописью' },
  { type: 'signatures', label: 'Подписи', hint: 'Линии для подписей сторон' },
  { type: 'pagebreak', label: 'Разрыв страницы', hint: '' },
];

interface Props {
  blocks: Block[];
  onChange?: (blocks: Block[]) => void;
  readOnly?: boolean;
}

export function BlockEditor({ blocks, onChange, readOnly = false }: Props) {
  const variables = useQuery({
    queryKey: qk.templates.variables(),
    queryFn: async () => {
      const { data, error } = await api.GET('/api/v1/template-variables');
      if (error) throw error;
      return (data as { data: { id: string; label_ru: string; kind: string }[] }).data;
    },
  });

  const set = (next: Block[]) => onChange?.(next);
  const patch = (i: number, changes: Block) =>
    set(blocks.map((b, idx) => (idx === i ? { ...b, ...changes } : b)));
  const move = (i: number, delta: number) => {
    const j = i + delta;
    if (j < 0 || j >= blocks.length) return;
    const next = [...blocks];
    [next[i], next[j]] = [next[j], next[i]];
    set(next);
  };

  const add = (type: string) => {
    const base: Block = { type };
    if (type === 'header') base.title = 'Счёт на оплату';
    if (type === 'text') base.text = '';
    if (type === 'fields') base.rows = [{ label: '', field: '' }];
    if (type === 'table')
      base.columns = [
        { title: 'Наименование', key: 'name' },
        { title: 'Кол-во', key: 'qty' },
        { title: 'Цена', key: 'price' },
      ];
    if (type === 'totals') base.label = 'Итого к оплате';
    if (type === 'signatures') base.parties = ['Поставщик', 'Покупатель'];
    set([...blocks, base]);
  };

  return (
    <div className="space-y-3">
      {blocks.length === 0 ? (
        <p className="text-sm text-muted-foreground">
          Блоков пока нет. Шаблон без единой статической подписи опубликовать нельзя — проверке было
          бы нечего искать в готовом PDF.
        </p>
      ) : null}

      <ol className="space-y-3">
        {blocks.map((b, i) => (
          <li key={i} className="rounded-md border p-3">
            <div className="mb-2 flex flex-wrap items-center gap-2 text-sm">
              <span className="font-medium">{labelFor(String(b.type))}</span>
              {!readOnly ? (
                <>
                  <Button type="button" variant="ghost" onClick={() => move(i, -1)}>
                    ↑
                  </Button>
                  <Button type="button" variant="ghost" onClick={() => move(i, 1)}>
                    ↓
                  </Button>
                  <Button
                    type="button"
                    variant="ghost"
                    onClick={() => set(blocks.filter((_, idx) => idx !== i))}
                  >
                    Удалить
                  </Button>
                </>
              ) : null}
            </div>

            {b.type === 'header' || b.type === 'text' ? (
              <textarea
                className="w-full rounded-md border px-3 py-2 text-sm"
                rows={b.type === 'text' ? 3 : 1}
                readOnly={readOnly}
                value={String((b.type === 'header' ? b.title : b.text) ?? '')}
                onChange={(e) =>
                  patch(
                    i,
                    b.type === 'header' ? { title: e.target.value } : { text: e.target.value },
                  )
                }
              />
            ) : null}

            {b.type === 'totals' ? (
              <div className="space-y-2 text-sm">
                <input
                  className="w-full rounded-md border px-3 py-2"
                  readOnly={readOnly}
                  value={String(b.label ?? '')}
                  onChange={(e) => patch(i, { label: e.target.value })}
                />
                <p className="text-xs text-muted-foreground">
                  Пользователь вводит только СУММУ ЧИСЛОМ. Печатаемую строку и сумму прописью
                  считает сервер — так цифра и текст в одном документе не могут разойтись.
                </p>
              </div>
            ) : null}

            {b.type === 'fields' ? (
              <FieldRows
                rows={(b.rows as FieldRow[]) ?? []}
                variables={variables.data ?? []}
                readOnly={readOnly}
                onChange={(rows) => patch(i, { rows })}
              />
            ) : null}

            {b.type === 'table' ? (
              <p className="text-sm text-muted-foreground">
                Колонки: {((b.columns as { title: string }[]) ?? []).map((c) => c.title).join(', ')}
              </p>
            ) : null}

            {b.type === 'signatures' ? (
              <input
                className="w-full rounded-md border px-3 py-2 text-sm"
                readOnly={readOnly}
                value={((b.parties as string[]) ?? []).join(', ')}
                onChange={(e) =>
                  patch(i, {
                    parties: e.target.value
                      .split(',')
                      .map((s) => s.trim())
                      .filter(Boolean),
                  })
                }
              />
            ) : null}
          </li>
        ))}
      </ol>

      {!readOnly ? (
        <div className="flex flex-wrap gap-2">
          {BLOCK_TYPES.map((t) => (
            <Button key={t.type} type="button" variant="ghost" onClick={() => add(t.type)}>
              + {t.label}
            </Button>
          ))}
        </div>
      ) : null}
    </div>
  );
}

interface FieldRow {
  label?: string;
  field?: string;
  variable?: string;
}

function FieldRows({
  rows,
  variables,
  readOnly,
  onChange,
}: {
  rows: FieldRow[];
  variables: { id: string; label_ru: string }[];
  readOnly: boolean;
  onChange: (rows: FieldRow[]) => void;
}) {
  const patch = (i: number, changes: FieldRow) =>
    onChange(rows.map((r, idx) => (idx === i ? { ...r, ...changes } : r)));

  return (
    <div className="space-y-2">
      {rows.map((r, i) => (
        <div key={i} className="grid gap-2 sm:grid-cols-[1fr_1fr_auto]">
          <input
            className="rounded-md border px-3 py-2 text-sm"
            placeholder="Подпись"
            readOnly={readOnly}
            value={r.label ?? ''}
            onChange={(e) => patch(i, { label: e.target.value })}
          />
          <select
            className="rounded-md border px-3 py-2 text-sm"
            disabled={readOnly}
            value={r.variable ?? ''}
            onChange={(e) =>
              patch(
                i,
                e.target.value
                  ? { variable: e.target.value, field: undefined }
                  : { variable: undefined },
              )
            }
          >
            <option value="">— вводится в форме —</option>
            {variables.map((v) => (
              <option key={v.id} value={v.id}>
                {v.label_ru}
              </option>
            ))}
          </select>
          {!r.variable ? (
            <input
              className="rounded-md border px-3 py-2 text-sm"
              placeholder="имя_поля"
              readOnly={readOnly}
              value={r.field ?? ''}
              onChange={(e) => patch(i, { field: e.target.value })}
            />
          ) : (
            <span className="self-center text-xs text-muted-foreground">подставит сервер</span>
          )}
        </div>
      ))}
      {!readOnly ? (
        <Button
          type="button"
          variant="ghost"
          onClick={() => onChange([...rows, { label: '', field: '' }])}
        >
          + строка
        </Button>
      ) : null}
    </div>
  );
}

function labelFor(type: string): string {
  return BLOCK_TYPES.find((t) => t.type === type)?.label ?? type;
}
