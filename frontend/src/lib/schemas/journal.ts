import { z } from 'zod';

import { toTiyn } from '@/lib/money';

/**
 * Draft journal-entry form schema. Mirrors JournalEntryCreate /
 * JournalLineWrite in docs/openapi.yaml.
 *
 * Money stays a string end-to-end: no float arithmetic anywhere on the
 * client (the running debit/credit balance in JournalPage is computed in
 * тиыны — integer minor units — from these same strings, see toTiyn() in
 * pages/Journal.tsx). The regex below is the full client-side contract for
 * an amount: an integer or up-to-two-decimal-place string. The backend
 * separately enforces amount > 0 and rejects an unbalanced entry with a 422
 * (Ledger::JournalService), which surfaces as an error toast.
 */
const amountSchema = z
  .string()
  .trim()
  .regex(/^\d+(\.\d{1,2})?$/, 'Не более 2 знаков после запятой, например 1234.56')
  .refine((v) => toTiyn(v) > 0, 'Сумма должна быть больше нуля');

export const journalLineSchema = z.object({
  account_code: z.string().min(1, 'Выберите счёт'),
  side: z.enum(['debit', 'credit']),
  amount: amountSchema,
  // '' means "no counterparty" — stripped before the request body is built
  // (see buildJournalEntryCreate in pages/Journal.tsx).
  counterparty_id: z.string().trim(),
});

export const journalEntrySchema = z
  .object({
    entry_date: z
      .string()
      .trim()
      .regex(/^\d{4}-\d{2}-\d{2}$/, 'Формат: ГГГГ-ММ-ДД'),
    description: z.string().trim().min(1, 'Укажите описание'),
    lines: z.array(journalLineSchema).min(2, 'В проводке должно быть минимум 2 строки'),
  })
  // Cross-field invariant: Σdebit === Σcredit, in integer tiyn — the same
  // check JournalPage's live indicator runs (lib/money.ts's toTiyn), so a
  // balanced-looking form can never slip past submit disabled and land here
  // unbalanced, and vice versa a truly balanced form is never blocked.
  .refine(
    (entry) => {
      let debit = 0;
      let credit = 0;
      for (const line of entry.lines) {
        const tiyn = toTiyn(line.amount);
        if (line.side === 'debit') debit += tiyn;
        else credit += tiyn;
      }
      return debit === credit;
    },
    { message: 'Суммы дебета и кредита должны совпадать', path: ['lines'] },
  );

export type JournalLineValues = z.infer<typeof journalLineSchema>;
export type JournalEntryValues = z.infer<typeof journalEntrySchema>;
