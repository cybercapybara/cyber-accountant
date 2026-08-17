import type { DocumentTemplate } from '@/lib/api/types';

/**
 * Описание формы, которое сервер порождает ИЗ БЛОКОВ шаблона.
 *
 * ЗАЧЕМ ЭТО, А НЕ ФОРМА ПО СХЕМЕ. Схемы шаблонов не содержат ни заголовков
 * полей, ни языков: в `templates/docs/invoice/v1/schema.json` у поля объявлен
 * только тип. Форма, построенная напрямую по схеме, показала бы бухгалтеру
 * `total_tiyn`. Поэтому источник подписей — блоки, а схема остаётся тем, чем
 * была: контрактом валидации.
 */
export interface TemplateFormField {
  field: string;
  label_ru: string;
  label_kk: string;
  /** `money` — целое в тиынах; `table` — массив строк; иначе текст. */
  widget: 'text' | 'money' | 'table';
  columns?: { title: string; key: string }[];
}

export interface TemplateForm {
  fields: TemplateFormField[];
}

/** Пустое описание — законный случай: шаблон может не иметь полей ввода вовсе
 *  (например, из одних статических подписей). */
export function templateForm(tpl: DocumentTemplate): TemplateForm {
  const form = (tpl.form ?? {}) as { fields?: unknown };
  const fields = Array.isArray(form.fields) ? (form.fields as TemplateFormField[]) : [];
  return { fields };
}

/**
 * Поля, которые ВВОДИТ пользователь. Производные сюда не попадают: `total` и
 * `total_words` пишет сервер из целого в тиынах, и присланная клиентом строка
 * отвергается как not_allowed_override (P3 §3). Список фильтруется по описанию
 * формы, а не по схеме, именно поэтому: в схеме производные поля объявлены,
 * ведь они обязательны в готовом документе.
 */
export function inputFields(tpl: DocumentTemplate): TemplateFormField[] {
  return templateForm(tpl).fields;
}

/** Значения формы -> тело запроса. Денежные поля уходят ЦЕЛЫМИ. */
export function buildTemplateInput(
  tpl: DocumentTemplate,
  values: Record<string, unknown>,
): Record<string, unknown> {
  const input: Record<string, unknown> = {};
  for (const f of inputFields(tpl)) {
    const raw = values[f.field];
    if (f.widget === 'money') {
      input[f.field] = typeof raw === 'number' ? raw : Number(raw ?? 0);
    } else if (f.widget === 'table') {
      input[f.field] = Array.isArray(raw) ? raw : [];
    } else {
      input[f.field] = String(raw ?? '');
    }
  }
  return input;
}
