/**
 * @file test_org_context.cpp
 * @brief Integration tests for Tenancy::org_context_of(): the fail-closed
 *        resolver behind the API_REQUIRE_ORG guard.
 *
 * Mints real HS256 access tokens (Security::Auth::issue_hs256_jwt) and runs
 * them through the same Security::Auth::Authenticator::verify_jwt() path the
 * auth middleware uses in production — this exercises the new `org` claim
 * parsing in src/security/Auth.hpp, not just OrgContext's own logic. The
 * resulting principal is stamped onto a bare request (TestHelpers::authed,
 * mirroring what the middleware does after verification) before calling
 * org_context_of(req) directly.
 *
 * Coverage (per task-6-brief.md):
 *   - a member of the claimed org gets a context carrying their role
 *   - a claimed org the caller has no membership in yields nullopt
 *   - a token with no `org` claim at all yields nullopt
 */

#include <optional>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-org-context-padding";

class OrgContextTest : public TestHelpers::CoreBackedTest {
protected:
    std::string config_file_name() const override { return "org_context_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // CASCADE on either table also clears org_members (FK to both),
        // same pattern as TenancyRepoTest::SetUp.
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE organizations CASCADE");
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
    }

    /// Confirmed "User"-role user; mirrors seed_user_id() in
    /// test_tenancy_repositories.cpp — these tests don't need a principal
    /// built ahead of time, just the id to reference in claims/memberships.
    std::string seed_user(const std::string& email) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name("User");
        if (!role) {
            ADD_FAILURE() << "role 'User' missing — seed migration?";
            throw std::runtime_error("seed role missing: User");
        }
        auto created = users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
        return created.id;
    }

    Tenancy::Organization seed_org(const std::string& bin, const std::string& name) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, name, "snr_simplified", false);
    }

    /// Mint an HS256 access token with the given claims and run it through
    /// Authenticator::verify_jwt() exactly like the auth middleware would —
    /// this is what actually exercises the new `p.org = claims.value("org",
    /// "")` line in Auth.hpp, rather than hand-constructing an AuthPrincipal.
    static std::optional<Security::Auth::AuthPrincipal> mint_principal(const json& claims) {
        const std::string token = Security::Auth::issue_hs256_jwt(claims, kSecret);
        std::string err;
        return Security::Auth::get().verify_jwt(token, err);
    }
};

TEST_F(OrgContextTest, MemberGetsContextWithRole) {
    auto user_id = seed_user("member@example.com");
    auto org = seed_org("111240000101", "Member Org LLP");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, user_id, "accountant");

    auto principal = mint_principal({{"sub", user_id}, {"org", org.id}});
    ASSERT_TRUE(principal.has_value());
    EXPECT_EQ(principal->org, org.id);

    auto req = TestHelpers::authed(*principal);
    auto ctx = Tenancy::org_context_of(req);
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->org_id, org.id);
    EXPECT_EQ(ctx->role, "accountant");
    EXPECT_EQ(ctx->user_id, user_id);
}

TEST_F(OrgContextTest, NonMemberGetsNoContext) {
    auto user_id = seed_user("outsider@example.com");
    // The claimed org exists, but this user has no membership row in it —
    // e.g. a stale claim after removal, or a forged/foreign org id.
    auto foreign_org = seed_org("111240000102", "Foreign Org LLP");

    auto principal = mint_principal({{"sub", user_id}, {"org", foreign_org.id}});
    ASSERT_TRUE(principal.has_value());
    EXPECT_EQ(principal->org, foreign_org.id);

    auto req = TestHelpers::authed(*principal);
    EXPECT_FALSE(Tenancy::org_context_of(req).has_value());
}

TEST_F(OrgContextTest, MissingOrgClaimGetsNoContext) {
    auto user_id = seed_user("noorg@example.com");
    // Even with a real membership on file, a token minted without an `org`
    // claim (0/>1 memberships at login, or a pre-multitenancy token) must
    // fail closed rather than silently guessing which org to scope to.
    auto org = seed_org("111240000103", "Unclaimed Org LLP");
    Tenancy::OrgMemberRepository members;
    members.add(org.id, user_id, "owner");

    auto principal = mint_principal({{"sub", user_id}});
    ASSERT_TRUE(principal.has_value());
    EXPECT_TRUE(principal->org.empty());

    auto req = TestHelpers::authed(*principal);
    EXPECT_FALSE(Tenancy::org_context_of(req).has_value());
}

}  // namespace
