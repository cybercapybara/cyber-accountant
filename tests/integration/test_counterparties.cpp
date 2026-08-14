/**
 * @file test_counterparties.cpp
 * @brief Integration tests for Ledger::CounterpartyRepository against a real
 *        Postgres (migration 007). Exercises create/find-by-identifier, the
 *        per-org UNIQUE(org_id, identifier) conflict, the same identifier
 *        being allowed across two different organizations, patching via
 *        update, and cross-org read isolation (Tenancy::OrgCrudBase).
 */

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "ledger/CounterpartyRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

class CounterpartiesRepoTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // Centralized org-data wipe — see TestHelpers::wipe_org_data()'s
        // Doxygen comment in test_helpers.hpp for why it TRUNCATEs the
        // journal/document tables before a plain DELETE on organizations
        // (accounts.org_id system-seed rows must survive; journal_entries'
        // immutability trigger must not fire on a leftover posted row from
        // another suite sharing this Postgres).
        TestHelpers::wipe_org_data();
    }

    /// Create a tenant and return its id. Fixed BINs below are only unique
    /// within a single run; truncating up front (SetUp) keeps the suite
    /// idempotent when re-run against a persistent local Postgres.
    std::string make_org(const std::string& bin) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "Counterparty Test Org " + bin, "snr_simplified", false).id;
    }

    static Ledger::Counterparty draft(const std::string& identifier, const std::string& name = "Acme LLP") {
        Ledger::Counterparty c;
        c.identifier = identifier;
        c.name = name;
        c.address = "Almaty, Abay 1";
        c.iik = "KZ00000000000000000";
        c.bik = "AAAAKZKA";
        c.kbe = "17";
        c.is_resident = true;
        c.vat_payer = false;
        c.contact_email = "ap@acme.example";
        return c;
    }
};

TEST_F(CounterpartiesRepoTest, CreateFindByIdentifier) {
    Ledger::CounterpartyRepository repo;
    auto org_id = make_org("111240000020");

    auto created = repo.create(org_id, draft("111240000099"));
    EXPECT_EQ(created.org_id, org_id);
    EXPECT_EQ(created.identifier, "111240000099");
    EXPECT_EQ(created.name, "Acme LLP");

    auto found = repo.find_by_identifier(org_id, "111240000099");
    ASSERT_TRUE(found);
    EXPECT_EQ(found->id, created.id);
    EXPECT_EQ(found->contact_email, "ap@acme.example");
}

TEST_F(CounterpartiesRepoTest, DuplicateIdentifierSameOrgRejected) {
    Ledger::CounterpartyRepository repo;
    auto org_id = make_org("111240000021");

    repo.create(org_id, draft("111240000100"));
    EXPECT_THROW(repo.create(org_id, draft("111240000100", "Other LLP")), Ledger::DuplicateCounterparty);
}

TEST_F(CounterpartiesRepoTest, SameIdentifierDifferentOrgsAllowed) {
    Ledger::CounterpartyRepository repo;
    auto org_a = make_org("111240000022");
    auto org_b = make_org("111240000023");

    auto in_a = repo.create(org_a, draft("111240000101"));
    auto in_b = repo.create(org_b, draft("111240000101"));

    EXPECT_NE(in_a.id, in_b.id);
    EXPECT_EQ(in_a.identifier, in_b.identifier);
    ASSERT_TRUE(repo.find_by_identifier(org_a, "111240000101"));
    ASSERT_TRUE(repo.find_by_identifier(org_b, "111240000101"));
}

TEST_F(CounterpartiesRepoTest, UpdatePatchesFields) {
    Ledger::CounterpartyRepository repo;
    auto org_id = make_org("111240000024");
    auto created = repo.create(org_id, draft("111240000102"));

    auto patch = draft("111240000102", "Renamed LLP");
    patch.address = "Astana, Turan 5";
    patch.iik = "KZ99999999999999999";
    patch.bik = "BBBBKZKA";
    patch.kbe = "19";
    patch.is_resident = false;
    patch.vat_payer = true;
    patch.contact_email = "new@acme.example";

    auto updated = repo.update(org_id, created.id, patch);
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated->name, "Renamed LLP");
    EXPECT_EQ(updated->address, "Astana, Turan 5");
    EXPECT_EQ(updated->iik, "KZ99999999999999999");
    EXPECT_EQ(updated->bik, "BBBBKZKA");
    EXPECT_EQ(updated->kbe, "19");
    EXPECT_FALSE(updated->is_resident);
    EXPECT_TRUE(updated->vat_payer);
    EXPECT_EQ(updated->contact_email, "new@acme.example");

    auto refetched = repo.find_in_org(created.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(refetched);
    EXPECT_EQ(refetched->name, "Renamed LLP");
}

TEST_F(CounterpartiesRepoTest, CrossOrgReadIsolated) {
    Ledger::CounterpartyRepository repo;
    auto org_a = make_org("111240000025");
    auto org_b = make_org("111240000026");

    auto created = repo.create(org_a, draft("111240000103"));

    EXPECT_FALSE(repo.find_in_org(created.id, org_b));
    ASSERT_TRUE(repo.find_in_org(created.id, org_a));
}

}  // namespace
