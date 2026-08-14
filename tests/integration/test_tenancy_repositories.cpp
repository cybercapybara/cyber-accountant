/**
 * @file test_tenancy_repositories.cpp
 * @brief Integration tests for OrganizationRepository and OrgMemberRepository
 *        against a real Postgres (migration 006). Exercises organization
 *        create/find, the unique-BIN conflict, and the full membership
 *        lifecycle (add -> find -> change role -> remove).
 */

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

class TenancyRepoTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // Fixed BINs and seed_user_id() emails below are only unique within a
        // single run; truncating up front (same pattern as
        // AdminFlowTest::SetUp) keeps the suite idempotent when re-run
        // against a persistent local Postgres instead of a fresh container.
        // CASCADE on either table also clears org_members.
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE organizations CASCADE");
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
    }

    /// Create a confirmed user with the default "User" role and return its
    /// id. Mirrors seed_user() in tests/integration/test_admin_flow.cpp,
    /// trimmed down to just the id since these tests don't need a principal.
    std::string seed_user_id(const std::string& email = "member@example.com") {
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
};

TEST_F(TenancyRepoTest, CreateFindOrganization) {
    Tenancy::OrganizationRepository repo;
    auto org = repo.create("111240000001", "Cyber Capybara LLP", "snr_simplified", false);
    auto found = repo.find(org.id, /*from_primary=*/true);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->bin, "111240000001");
}

TEST_F(TenancyRepoTest, DuplicateBinRejected) {
    Tenancy::OrganizationRepository repo;
    repo.create("111240000002", "First", "snr_simplified", false);
    EXPECT_THROW(repo.create("111240000002", "Second", "snr_simplified", false), Tenancy::DuplicateBin);
}

TEST_F(TenancyRepoTest, MembershipLifecycle) {
    Tenancy::OrganizationRepository orgs;
    Tenancy::OrgMemberRepository members;
    auto org = orgs.create("111240000003", "M LLP", "snr_simplified", false);
    auto user_id = seed_user_id();

    auto m = members.add(org.id, user_id, "accountant");
    EXPECT_EQ(m.org_id, org.id);
    EXPECT_EQ(m.user_id, user_id);

    auto found = members.find_membership(org.id, user_id);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->role, "accountant");

    EXPECT_TRUE(members.set_role(org.id, user_id, "owner"));
    EXPECT_EQ(members.find_membership(org.id, user_id)->role, "owner");

    EXPECT_TRUE(members.remove(org.id, user_id));
    EXPECT_FALSE(members.find_membership(org.id, user_id));
}

TEST_F(TenancyRepoTest, ListForUserAndListMembers) {
    Tenancy::OrganizationRepository orgs;
    Tenancy::OrgMemberRepository members;
    auto org_a = orgs.create("111240000004", "A LLP", "snr_simplified", false);
    auto org_b = orgs.create("111240000005", "B LLP", "standard", true);
    auto user_id = seed_user_id("multi@example.com");

    members.add(org_a.id, user_id, "owner");
    members.add(org_b.id, user_id, "viewer");

    auto for_user = members.list_for_user(user_id);
    EXPECT_EQ(for_user.size(), 2u);

    auto for_org = members.list_members(org_a.id);
    ASSERT_EQ(for_org.size(), 1u);
    EXPECT_EQ(for_org[0].user_id, user_id);
}

TEST_F(TenancyRepoTest, DuplicateMembershipRejected) {
    Tenancy::OrganizationRepository orgs;
    Tenancy::OrgMemberRepository members;
    auto org = orgs.create("111240000006", "Dup LLP", "snr_simplified", false);
    auto user_id = seed_user_id("dupmember@example.com");
    members.add(org.id, user_id, "viewer");
    EXPECT_THROW(members.add(org.id, user_id, "owner"), Tenancy::DuplicateMembership);
}

TEST_F(TenancyRepoTest, UpdateStatusTransitionsOrganization) {
    Tenancy::OrganizationRepository repo;
    auto org = repo.create("111240000007", "Status LLP", "snr_simplified", false);
    EXPECT_TRUE(repo.update_status(org.id, "suspended"));
    auto found = repo.find(org.id, /*from_primary=*/true);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->status, "suspended");
    EXPECT_FALSE(repo.update_status("11111111-1111-1111-1111-111111111111", "active"));
}

}  // namespace
