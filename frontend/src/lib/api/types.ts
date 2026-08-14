/**
 * Flat domain-type aliases over the generated OpenAPI tree.
 *
 * openapi-typescript emits indexed types (`components['schemas']['User']`)
 * into schema.gen.ts. Call sites want flat names (`User`, `Job`, …), so
 * this module re-exports thin aliases — change a shape in
 * docs/openapi.yaml, run `npm run gen:api`, and every page picks it up.
 *
 * Everything here is now generated: the me / user-detail / roles / invite
 * response envelopes earned named `components/schemas` entries in
 * docs/openapi.yaml, so there are no hand-written envelope types left to
 * drift against the backend.
 */
import type { components } from './schema.gen';

type Schemas = components['schemas'];

export type Role = Schemas['Role'];
export type User = Schemas['User'];
export type Job = Schemas['Job'];
export type AuditEntry = Schemas['AuditEntry'];
export type AuditListResponse = Schemas['AuditListResponse'];
export type UserListResponse = Schemas['UserListResponse'];
export type JobListResponse = Schemas['JobListResponse'];
export type DlqListResponse = Schemas['DlqListResponse'];
export type JobCreate = Schemas['JobCreate'];
export type Organization = Schemas['Organization'];
export type OrganizationWithRole = Schemas['OrganizationWithRole'];
export type OrgMember = Schemas['OrgMember'];
export type MemberWithEmail = Schemas['MemberWithEmail'];
export type Counterparty = Schemas['Counterparty'];
export type CounterpartyWrite = Schemas['CounterpartyWrite'];
export type Account = Schemas['Account'];
export type AccountCreate = Schemas['AccountCreate'];
export type JournalLine = Schemas['JournalLine'];
export type JournalLineWrite = Schemas['JournalLineWrite'];
export type JournalEntry = Schemas['JournalEntry'];
export type JournalEntryCreate = Schemas['JournalEntryCreate'];
export type Document = Schemas['Document'];
export type DocTemplate = Schemas['DocTemplate'];
export type DocumentUploadCreate = Schemas['DocumentUploadCreate'];
export type DocumentConfirmUpload = Schemas['DocumentConfirmUpload'];
export type GenerateDocumentCreate = Schemas['GenerateDocumentCreate'];
export type Employee = Schemas['Employee'];
export type EmployeeCreate = Schemas['EmployeeCreate'];
export type EmployeeUpdate = Schemas['EmployeeUpdate'];
export type EmployeeDismiss = Schemas['EmployeeDismiss'];
export type HrOrder = Schemas['HrOrder'];
export type HrOrderCreate = Schemas['HrOrderCreate'];
export type LaborContract = Schemas['LaborContract'];
export type LaborContractCreate = Schemas['LaborContractCreate'];
export type Vacation = Schemas['Vacation'];
export type VacationCreate = Schemas['VacationCreate'];
/** Body of both …/generate-document endpoints — free-text fields only. */
export type GenerateHrDocumentExtra = Schemas['GenerateHrDocumentExtra'];

/** GET /api/auth/me, POST /api/auth/login, POST /api/auth/refresh — { user }. */
export type MeResponse = Schemas['MeResponse'];
/** GET/PATCH /api/admin/users/{id}, POST /api/admin/users — { data: User }. */
export type UserDetailResponse = Schemas['UserDetailResponse'];
/** GET /api/admin/roles — { data: Role[] }. */
export type RolesResponse = Schemas['RolesResponse'];
/** POST/PATCH /api/admin/roles[/{id}] — { data: Role }. */
export type RoleDetailResponse = Schemas['RoleDetailResponse'];
/** POST /api/admin/invite — { data: User, message? }. */
export type InviteResponse = Schemas['InviteResponse'];
/** Generic { message } envelope (logout, delete, …). */
export type MessageResponse = Schemas['MessageResponse'];
/** GET /api/v1/orgs — { data: Organization[], total, limit, offset }. */
export type OrganizationListResponse = Schemas['OrganizationListResponse'];
/** POST /api/v1/orgs — { data: Organization }. */
export type OrganizationDetailResponse = Schemas['OrganizationDetailResponse'];
/** GET /api/v1/orgs/mine — { data: OrganizationWithRole[] }. */
export type MineOrganizationsResponse = Schemas['MineOrganizationsResponse'];
/** POST/PATCH /api/v1/orgs/{id}/members[/{user_id}] — { data: OrgMember }. */
export type OrgMemberDetailResponse = Schemas['OrgMemberDetailResponse'];
/** GET /api/v1/orgs/{id}/members — { data: MemberWithEmail[] }. */
export type MembersListResponse = Schemas['MembersListResponse'];
/** POST /api/v1/orgs/{id}/switch — { access }. Cookie-mode clients never
 * read this body (the __Host-access cookie is HttpOnly); it exists for
 * Bearer-mode clients, which this template does not use. */
export type SwitchResponse = Schemas['SwitchResponse'];
/** GET/POST /api/v1/counterparties — { data: Counterparty[], total, limit, offset }. */
export type CounterpartyListResponse = Schemas['CounterpartyListResponse'];
/** GET/POST/PATCH /api/v1/counterparties[/{id}] — { data: Counterparty }. */
export type CounterpartyDetailResponse = Schemas['CounterpartyDetailResponse'];
/** GET /api/v1/accounts — { data: Account[] }, unpaginated. */
export type AccountListResponse = Schemas['AccountListResponse'];
/** POST /api/v1/accounts — { data: Account }. */
export type AccountDetailResponse = Schemas['AccountDetailResponse'];
/** GET/POST /api/v1/journal-entries — { data: JournalEntry[], total, limit, offset }. */
export type JournalEntryListResponse = Schemas['JournalEntryListResponse'];
/** GET/POST /api/v1/journal-entries[/{id}[/post|/reverse]] — { data: JournalEntry }. */
export type JournalEntryDetailResponse = Schemas['JournalEntryDetailResponse'];
/** GET /api/v1/documents — { data: Document[], total, limit, offset }. */
export type DocumentListResponse = Schemas['DocumentListResponse'];
/** GET /api/v1/documents/{id}, POST .../confirm-upload — { data: Document }. */
export type DocumentDetailResponse = Schemas['DocumentDetailResponse'];
/** POST /api/v1/documents/uploads — { data: Document, upload_url }. */
export type DocumentUploadResponse = Schemas['DocumentUploadResponse'];
/** POST /api/v1/documents/{id}/download-url — { url }. */
export type DownloadUrlResponse = Schemas['DownloadUrlResponse'];
/** GET /api/v1/doc-templates — { data: DocTemplate[] }. */
export type DocTemplateListResponse = Schemas['DocTemplateListResponse'];
/** POST /api/v1/documents/generate — 202 { document_id, render_queued }. */
export type GenerateDocumentResponse = Schemas['GenerateDocumentResponse'];
/** GET /api/v1/employees — { data: Employee[], total, limit, offset }. */
export type EmployeeListResponse = Schemas['EmployeeListResponse'];
/** GET/POST/PATCH /api/v1/employees[/{id}[/dismiss]] — { data: Employee }. */
export type EmployeeDetailResponse = Schemas['EmployeeDetailResponse'];
/** GET /api/v1/hr-orders — { data: HrOrder[] }, unpaginated. */
export type HrOrderListResponse = Schemas['HrOrderListResponse'];
/** POST /api/v1/hr-orders — { data: HrOrder }. */
export type HrOrderDetailResponse = Schemas['HrOrderDetailResponse'];
/** GET /api/v1/labor-contracts — { data: LaborContract[] }, unpaginated. */
export type LaborContractListResponse = Schemas['LaborContractListResponse'];
/** POST /api/v1/labor-contracts — { data: LaborContract }. */
export type LaborContractDetailResponse = Schemas['LaborContractDetailResponse'];
/** GET /api/v1/vacations — { data: Vacation[] }, unpaginated. */
export type VacationListResponse = Schemas['VacationListResponse'];
/** POST /api/v1/vacations — { data: Vacation }. */
export type VacationDetailResponse = Schemas['VacationDetailResponse'];
