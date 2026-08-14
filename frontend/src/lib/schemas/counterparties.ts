import { z } from 'zod';

/**
 * Counterparty form schema. zod is the source of truth on the client; the
 * backend re-validates on every request (Ledger::is_valid_bin_iin) and a
 * shape-valid-but-checksum-invalid identifier comes back as a 422 that
 * error-toasts via useErrorToast — the client only checks the cheap shape
 * (12 digits), never re-implements the check-digit algorithm.
 * Mirrors CounterpartyWrite in docs/openapi.yaml.
 */
export const counterpartySchema = z.object({
  identifier: z
    .string()
    .trim()
    .regex(/^[0-9]{12}$/, 'БИН/ИИН должен состоять ровно из 12 цифр'),
  name: z.string().trim().min(1, 'Укажите название'),
  address: z.string().trim().default(''),
  iik: z.string().trim().default(''),
  bik: z.string().trim().default(''),
  kbe: z.string().trim().default(''),
  is_resident: z.boolean().default(true),
  vat_payer: z.boolean().default(false),
  contact_email: z.string().trim().default(''),
});

export type CounterpartyValues = z.infer<typeof counterpartySchema>;
