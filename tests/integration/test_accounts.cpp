/**
 * @file test_accounts.cpp
 * @brief Integration tests for Ledger::AccountRepository against a real
 *        Postgres (migration 008). Exercises the system-seed visibility
 *        (org_id IS NULL rows shared across every tenant), creating a
 *        tenant subaccount under a system parent, the subaccount-code /
 *        parent-code validation (InvalidSubaccount), cross-org isolation of
 *        tenant subaccounts, and the reserved-system-code conflict
 *        (DuplicateAccount).
 */

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "ledger/AccountRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

class AccountsRepoTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // Centralized org-data wipe (TestHelpers::wipe_org_data(), in
        // test_helpers.hpp). It ends in a plain "DELETE FROM organizations"
        // (never TRUNCATE ... CASCADE): accounts.org_id references
        // organizations too, and TRUNCATE CASCADE truncates the WHOLE
        // referencing table — including the org_id IS NULL system seed rows
        // from migration 008, not just the tenant rows that actually
        // reference a deleted org. Plain DELETE respects the FK's per-row
        // ON DELETE CASCADE instead, clearing org-scoped subaccounts while
        // leaving org_id IS NULL rows untouched (they reference no org row
        // in the first place) — no separate "DELETE FROM accounts WHERE
        // org_id IS NOT NULL" needed ahead of it.
        TestHelpers::wipe_org_data();
    }

    /// Create a tenant and return its id. Fixed BINs below are only unique
    /// within a single run; clearing organizations up front (SetUp) keeps
    /// the suite idempotent when re-run against a persistent local Postgres.
    std::string make_org(const std::string& bin) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "Accounts Test Org " + bin, "snr_simplified", false).id;
    }
};

TEST_F(AccountsRepoTest, SeedVisibleToAnyOrg) {
    Ledger::AccountRepository repo;
    auto org_id = make_org("111250000010");

    auto accounts = repo.list_visible(org_id);
    // migration 008 seeds 36 system rows; a brand-new org has no
    // subaccounts of its own yet, so this is also the exact system count.
    EXPECT_GE(accounts.size(), 36u);

    bool has_1030 = false;
    bool has_6010 = false;
    for (const auto& a : accounts) {
        if (a.code == "1030")
            has_1030 = true;
        if (a.code == "6010")
            has_6010 = true;
        // Every seed row is a system row (org_id IS NULL) — a fresh org
        // hasn't created any subaccounts yet.
        EXPECT_FALSE(a.org_id);
    }
    EXPECT_TRUE(has_1030);
    EXPECT_TRUE(has_6010);

    for (std::size_t i = 1; i < accounts.size(); ++i) {
        EXPECT_LE(accounts[i - 1].code, accounts[i].code) << "list_visible must be ORDER BY code";
    }
}

TEST_F(AccountsRepoTest, CreateSubaccountUnderSystemParent) {
    Ledger::AccountRepository repo;
    auto org_id = make_org("111250000011");

    auto sub = repo.create_subaccount(org_id, "1030.1", "Текущий счет в KZT", "", "1030");
    ASSERT_TRUE(sub.org_id);
    EXPECT_EQ(*sub.org_id, org_id);
    EXPECT_EQ(sub.code, "1030.1");
    EXPECT_EQ(sub.name_ru, "Текущий счет в KZT");
    ASSERT_TRUE(sub.parent_code);
    EXPECT_EQ(*sub.parent_code, "1030");
    // Inherited from the parent (1030 is an asset, not currency-tracked).
    EXPECT_EQ(sub.type, "asset");
    EXPECT_FALSE(sub.currency_tracked);

    auto found = repo.find_visible(org_id, "1030.1");
    ASSERT_TRUE(found);
    EXPECT_EQ(found->id, sub.id);
}

TEST_F(AccountsRepoTest, SubaccountCodeMustExtendParent) {
    Ledger::AccountRepository repo;
    auto org_id = make_org("111250000012");

    EXPECT_THROW(repo.create_subaccount(org_id, "9999", "Not a child of 1030", "", "1030"), Ledger::InvalidSubaccount);
}

TEST_F(AccountsRepoTest, SubaccountsIsolatedBetweenOrgs) {
    Ledger::AccountRepository repo;
    auto org_a = make_org("111250000013");
    auto org_b = make_org("111250000014");

    repo.create_subaccount(org_a, "1030.1", "A's KZT account", "", "1030");

    EXPECT_FALSE(repo.find_visible(org_b, "1030.1"));
    ASSERT_TRUE(repo.find_visible(org_a, "1030.1"));

    for (const auto& a : repo.list_visible(org_b))
        EXPECT_NE(a.code, "1030.1");
}

TEST_F(AccountsRepoTest, DuplicateSystemCodeForbidden) {
    Ledger::AccountRepository repo;
    auto org_id = make_org("111250000015");

    // "1030" is already a system code — attempting to shadow it as a
    // subaccount under itself must be rejected as a conflict, not silently
    // create a second, org-scoped row with the same code.
    EXPECT_THROW(repo.create_subaccount(org_id, "1030", "Shadow attempt", "", "1030"), Ledger::DuplicateAccount);
}

}  // namespace
