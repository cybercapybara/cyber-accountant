import { z } from 'zod';

/**
 * Organizations form schemas. zod is the source of truth on the client;
 * the backend re-validates (Api::Validation::*) on every request. Mirrors
 * the POST /api/v1/orgs request body in docs/openapi.yaml.
 */

export const createOrganizationSchema = z.object({
  bin: z
    .string()
    .trim()
    .regex(/^[0-9]{12}$/, 'BIN/IIN must be exactly 12 digits'),
  name: z.string().trim().min(1, 'Name is required'),
  tax_regime: z.enum(['snr_simplified', 'standard']),
  vat_payer: z.boolean(),
});

export type CreateOrganizationValues = z.infer<typeof createOrganizationSchema>;
