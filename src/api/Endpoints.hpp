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
        {"POST", "/api/v1/orgs/{id}/members", "Add a member to an organization (admin or owner)"},
        {"PATCH", "/api/v1/orgs/{id}/members/{user_id}", "Change a member's role (admin or owner)"},
        {"DELETE", "/api/v1/orgs/{id}/members/{user_id}", "Remove a member from an organization (admin or owner)"},
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
