/**
 * @file OrgContext.hpp
 * @brief Resolves the caller's organization context (org id + role) from the
 *        access-token `org` claim, fail-closed against a live membership row.
 * @details The `org` claim is minted at login (AuthController::mint_session)
 *          only when the user belongs to exactly one organization — with zero
 *          or more than one membership, no claim is set and the client must
 *          pick via the switch flow (Task 7). Guard consumers use
 *          API_REQUIRE_ORG (src/api/Guards.hpp).
 */

#pragma once

#include <optional>
#include <string>

#include <drogon/HttpRequest.h>

#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"

namespace Tenancy {

struct OrgContext {
    std::string org_id;
    std::string role;  // 'owner' | 'accountant' | 'viewer'
    std::string user_id;
};

/**
 * @brief Fail-closed: a context exists only when the principal carries a
 *        non-empty `org` claim AND a live membership row backs it up. No
 *        principal, no claim, or a revoked/nonexistent membership all yield
 *        nullopt — callers must treat that as "no org access", never as
 *        "unscoped access".
 */
inline std::optional<OrgContext> org_context_of(const drogon::HttpRequestPtr& req) {
    auto p = Security::Auth::principal_of(req);
    if (!p || p->subject.empty() || p->org.empty())
        return std::nullopt;
    // Per-request membership read; add caching here if it shows up in profiles.
    OrgMemberRepository members;
    auto m = members.find_membership(p->org, p->subject);
    if (!m)
        return std::nullopt;
    return OrgContext{p->org, m->role, p->subject};
}

}  // namespace Tenancy
