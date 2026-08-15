/**
 * @file test_tenancy_schema.cpp
 * @brief Schema tests for the multitenancy foundation: `organizations` and
 *        `org_members` (migration 006). Every later repository/API task
 *        builds on these two tables, so this suite just pins the shape down:
 *        both tables exist, and org_members.role is constrained to the
 *        four tenancy roles ('owner' | 'accountant' | 'hr' | 'viewer' —
 *        'hr' added by migration 017). That the CHECK ACCEPTS 'hr' is
 *        proven end-to-end by OrganizationsApiTest.HrIsAnAcceptedMemberRole
 *        (tests/integration/test_organizations_api.cpp), which goes through
 *        a real membership row; here we only pin that it still REJECTS a
 *        value outside the roster.
 */

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "test_helpers.hpp"

namespace {

class TenancySchemaTest : public TestHelpers::CoreBackedTest {};

TEST_F(TenancySchemaTest, OrganizationsTableExists) {
    auto n = Database::get().execute_read([](auto& txn) {
        return txn
            .exec(
                "SELECT COUNT(*) FROM information_schema.tables "
                "WHERE table_name IN ('organizations','org_members')")
            .at(0)
            .at(0)
            .template as<int>();
    });
    EXPECT_EQ(n, 2);
}

TEST_F(TenancySchemaTest, MemberRoleIsConstrained) {
    EXPECT_THROW(Database::get().execute_write([](auto& txn) {
        txn.exec(
            "INSERT INTO org_members (org_id, user_id, role) "
            "VALUES (gen_random_uuid(), gen_random_uuid(), 'superuser')");
        return 0;
    }),
                 std::exception);
}

}  // namespace
