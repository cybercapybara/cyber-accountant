/**
 * @file Endpoints.hpp
 * @brief Endpoint registry — the single source of truth for routes.
 * @details `docs/openapi.yaml` is checked against this list in CI via
 *          scripts/check-openapi-drift.sh; `--print-routes` prints it.
 *          Add a line here for every new ADD_METHOD_TO.
 */

#pragma once

#include <string>
#include <vector>

namespace Api {

/**
 * @brief Endpoint metadata: method, path, description
 */
struct EndpointInfo {
    std::string method;
    std::string path;
    std::string description;
};

/**
 * @brief Single source of truth for all registered API endpoints
 */
inline const std::vector<EndpointInfo>& get_endpoints() {
    static const std::vector<EndpointInfo> endpoints = {
        {"GET", "/", "Endpoint discovery"},
        {"GET", "/healthz", "Liveness probe"},
        {"GET", "/ready", "Readiness probe"},
        {"GET", "/health", "Detailed health check"},
        {"POST", "/api/v1/auth/register", "Register a new user"},
        {"POST", "/api/v1/auth/login", "Log in (issues access + refresh cookies)"},
        {"POST", "/api/v1/auth/logout", "Log out (clears cookies + revokes refresh)"},
        {"POST", "/api/v1/auth/refresh", "Rotate access + refresh cookies"},
        {"GET", "/api/v1/auth/me", "Get the authenticated user"},
        {"POST", "/api/v1/account/confirm-resend", "Resend email confirmation link"},
        {"POST", "/api/v1/account/confirm/{token}", "Confirm an account from token"},
        {"POST", "/api/v1/account/reset-password-request", "Request a password-reset email"},
        {"POST", "/api/v1/account/reset-password/{token}", "Reset password using a token"},
        {"POST", "/api/v1/account/change-email-request", "Request a confirm-email link for a new address"},
        {"POST", "/api/v1/account/change-email/{token}", "Apply a pending email change from token"},
        {"POST", "/api/v1/account/join-from-invite/{token}", "Set password and confirm account from an invite token"},
        {"POST", "/api/v1/account/change-password", "Change password while logged in"},
        {"GET", "/api/v1/account/api-keys", "List your API keys"},
        {"POST", "/api/v1/account/api-keys", "Create an API key (secret shown once)"},
        {"DELETE", "/api/v1/account/api-keys/{id}", "Revoke an API key"},
        {"GET", "/api/v1/admin/users", "Admin: list users"},
        {"POST", "/api/v1/admin/users", "Admin: create a user"},
        {"POST", "/api/v1/admin/invite", "Admin: invite a user via email"},
        {"GET", "/api/v1/admin/users/{id}", "Admin: user detail"},
        {"PATCH", "/api/v1/admin/users/{id}", "Admin: update user (email, role, name)"},
        {"DELETE", "/api/v1/admin/users/{id}", "Admin: delete user"},
        {"GET", "/api/v1/admin/roles", "Admin: list roles"},
        {"POST", "/api/v1/admin/roles", "Admin: create role"},
        {"PATCH", "/api/v1/admin/roles/{id}", "Admin: update role (name, permissions, is_default)"},
        {"DELETE", "/api/v1/admin/roles/{id}", "Admin: delete role"},
        {"GET", "/api/v1/admin/audit", "Admin: list the audit trail (requires audit-read permission)"},
        {"POST", "/api/v1/orgs", "Admin: create an organization (tenant)"},
        {"GET", "/api/v1/orgs", "Admin: list organizations"},
        {"GET", "/api/v1/orgs/mine", "List the caller's organizations with their role in each"},
        {"POST", "/api/v1/orgs/{id}/switch", "Mint an access token scoped to the given organization"},
        {"GET", "/api/v1/orgs/{id}/members", "List an organization's members with email (admin or owner)"},
        {"POST", "/api/v1/orgs/{id}/members", "Add a member to an organization (admin or owner)"},
        {"PATCH", "/api/v1/orgs/{id}/members/{user_id}", "Change a member's role (admin or owner)"},
        {"DELETE", "/api/v1/orgs/{id}/members/{user_id}", "Remove a member from an organization (admin or owner)"},
        {"GET", "/api/v1/counterparties", "List counterparties (paginated)"},
        {"POST", "/api/v1/counterparties", "Create a counterparty (accountant/owner)"},
        {"GET", "/api/v1/counterparties/{id}", "Get a counterparty"},
        {"PATCH", "/api/v1/counterparties/{id}", "Replace a counterparty (accountant/owner)"},
        {"GET", "/api/v1/accounts", "List visible accounts (system chart + this org's subaccounts)"},
        {"POST", "/api/v1/accounts", "Create a subaccount (accountant/owner)"},
        {"GET", "/api/v1/documents", "List documents (paginated, optional type/status filters)"},
        {"GET", "/api/v1/documents/{id}", "Get a document"},
        {"POST", "/api/v1/documents/{id}/download-url", "Mint a presigned download URL (GET, TTL 300s)"},
        {"POST", "/api/v1/documents/uploads", "Start a client-driven upload (presigned PUT, TTL 600s)"},
        {"POST", "/api/v1/documents/{id}/confirm-upload", "Verify an uploaded object and finalize the document"},
        {"GET", "/api/v1/documents/{id}/versions", "List a document's versions (oldest first)"},
        {"POST", "/api/v1/documents/{id}/versions", "Edit a document: create a new version and queue its render"},
        {"POST", "/api/v1/documents/{id}/versions/{version_no}/download-url", "Presigned URL for one version"},
        {"GET", "/api/v1/journal-entries", "List journal entries (paginated, optional from/to entry_date filters)"},
        {"POST", "/api/v1/journal-entries", "Create a draft journal entry (accountant/owner)"},
        {"GET", "/api/v1/journal-entries/{id}", "Get a journal entry with its lines"},
        {"POST", "/api/v1/journal-entries/{id}/post", "Post a draft journal entry (accountant/owner)"},
        {"POST", "/api/v1/journal-entries/{id}/reverse", "Reverse (storno) a posted journal entry (accountant/owner)"},
        {"GET", "/api/v1/doc-templates", "List registered docgen templates (slug, version, schema)"},
        {"POST", "/api/v1/documents/generate", "Generate a document from a template (accountant/owner)"},
        {"GET", "/api/v1/employees", "List employees (paginated)"},
        {"POST", "/api/v1/employees", "Create an employee (accountant/owner)"},
        {"GET", "/api/v1/employees/{id}", "Get an employee"},
        {"PATCH", "/api/v1/employees/{id}", "Patch an employee's editable fields (accountant/owner)"},
        {"POST", "/api/v1/employees/{id}/dismiss", "Dismiss an employee (accountant/owner)"},
        {"GET", "/api/v1/hr-orders", "List HR orders (paginated, optional ?employee_id filter)"},
        {"POST", "/api/v1/hr-orders", "Create an HR order (accountant/owner)"},
        {"POST", "/api/v1/hr-orders/{id}/generate-document", "Generate the hr_order document"},
        {"GET", "/api/v1/labor-contracts", "List labor contracts for one employee (?employee_id required)"},
        {"POST", "/api/v1/labor-contracts", "Create a labor contract (accountant/owner)"},
        {"POST", "/api/v1/labor-contracts/{id}/generate-document", "Generate the labor_contract document"},
        {"GET", "/api/v1/vacations", "List vacations (paginated, optional ?employee_id filter)"},
        {"POST", "/api/v1/vacations", "Create a vacation record (accountant/owner)"},
        {"GET", "/api/v1/payroll-runs", "List payroll runs (paginated, optional ?year filter)"},
        {"POST", "/api/v1/payroll-runs", "Calculate or recalculate a period's payroll run (accountant/owner)"},
        {"POST", "/api/v1/payroll-runs/{id}/approve", "Approve a draft payroll run (accountant/owner)"},
        {"POST", "/api/v1/payroll-runs/{id}/post-to-journal", "Post an approved payroll run to the journal"},
        {"GET", "/api/v1/payroll-runs/{id}/payslips", "List a payroll run's payslips"},
        {"POST", "/api/v1/payroll-runs/{id}/payslips/{employee_id}/generate-document", "Generate a payslip doc"},
        {"GET", "/api/v1/tax/rates", "Tax rates and constants in force on a date (?on=YYYY-MM-DD)"},
        {"POST", "/api/v1/tax/calculations", "Run a tax calculation for a period (accountant/owner)"},
        {"GET", "/api/v1/tax/calculations", "List stored tax calculations (paginated, optional ?kind/?year)"},
        {"GET", "/api/v1/tax/alerts", "Registration-threshold alerts for this organization (?on=YYYY-MM-DD)"},
        {"GET", "/api/v1/tax/deadlines", "Upcoming tax deadlines (?on=YYYY-MM-DD, ?horizon_days=90)"},
        {"POST", "/api/v1/tax/filings", "Generate a FNO filing: XML into storage + printable form (accountant/owner)"},
        {"GET", "/api/v1/tax/filings", "List tax filings (paginated, optional ?kind filter)"},
        {"GET", "/api/v1/tax/filings/{id}", "Get a tax filing"},
        {"POST", "/api/v1/tax/filings/{id}/download-url", "Presigned download URL (?artifact=xml|pdf)"},
        {"GET", "/api/v1/jobs", "List jobs"},
        {"POST", "/api/v1/jobs", "Submit job"},
        {"GET", "/api/v1/jobs/dlq", "List dead-letter queue"},
        {"POST", "/api/v1/jobs/dlq/{id}/requeue", "Requeue a DLQ job"},
        {"GET", "/api/v1/jobs/{id}", "Get job status"},
        {"DELETE", "/api/v1/jobs/{id}", "Cancel job"},
    };
    return endpoints;
}

}  // namespace Api
