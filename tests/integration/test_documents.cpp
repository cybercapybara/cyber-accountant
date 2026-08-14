/**
 * @file test_documents.cpp
 * @brief Integration tests for Ledger::DocumentRepository against a real
 *        Postgres (migration 010). Exercises create() (including the
 *        JSONB input_snapshot round-trip), link_entry()'s same-org success
 *        path and its cross-org FK rejection (document_entries' composite
 *        FKs — see migrations/010_documents.sql), the draft->final->sent
 *        happy path of set_status(), and that every CHECK-listed doc_type
 *        round-trips through create()/list_in_org().
 */

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "ledger/DocumentRepository.hpp"
#include "ledger/JournalEntry.hpp"
#include "ledger/JournalService.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

class DocumentsRepoTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // Centralized org-data wipe (TestHelpers::wipe_org_data(), in
        // test_helpers.hpp) — see its Doxygen comment for the full rationale
        // (in short: journal_entries_immutability() forbids DELETEing a
        // posted/reversed row, so the journal/document tables are TRUNCATEd,
        // bypassing that row-level trigger, before organizations is plain
        // DELETEd). TRUNCATE users CASCADE stays local to this fixture
        // (untouched by the centralization).
        TestHelpers::wipe_org_data();
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
    }

    /// Create a tenant and return its id. Fixed BINs below are only unique
    /// within a single run; clearing organizations up front (SetUp) keeps
    /// the suite idempotent when re-run against a persistent local Postgres.
    std::string make_org(const std::string& bin) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "Document Test Org " + bin, "snr_simplified", false).id;
    }

    /// Confirmed "User"-role user — journal_entries.created_by_user_id needs
    /// a real row, same as test_journal_service.cpp's seed_user().
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

    /// A minimal balanced draft entry (cash in against sales income), left
    /// as 'draft' — these tests never post/reverse, so DELETE FROM
    /// organizations in the NEXT SetUp can clean them up without tripping
    /// journal_entries_immutability().
    std::string make_draft_entry(const std::string& org_id, const std::string& user_id) {
        Ledger::JournalService svc;
        Ledger::JournalLine debit;
        debit.account_code = "1030";
        debit.side = "debit";
        debit.amount = "1000.00";
        Ledger::JournalLine credit;
        credit.account_code = "6010";
        credit.side = "credit";
        credit.amount = "1000.00";
        auto entry = svc.create_draft(org_id, user_id, "2026-01-15", "Doc link fixture", {debit, credit});
        return entry.id;
    }
};

TEST_F(DocumentsRepoTest, CreateGeneratedDraft) {
    Ledger::DocumentRepository repo;
    auto org_id = make_org("111270000001");

    nlohmann::json snapshot = {{"number", "1"}, {"date", "15.01.2026"}, {"total", "1000.00"}};

    auto doc = repo.create(org_id,
                           "invoice",
                           "generated",
                           "draft",
                           /*counterparty_id=*/std::nullopt,
                           "invoice",
                           "v1",
                           snapshot);

    EXPECT_FALSE(doc.id.empty());
    EXPECT_EQ(doc.org_id, org_id);
    EXPECT_EQ(doc.doc_type, "invoice");
    EXPECT_EQ(doc.source, "generated");
    EXPECT_EQ(doc.status, "draft");
    EXPECT_FALSE(doc.counterparty_id);
    EXPECT_FALSE(doc.s3_key);
    EXPECT_FALSE(doc.checksum_sha256);
    EXPECT_FALSE(doc.mime);
    EXPECT_FALSE(doc.size_bytes);
    ASSERT_TRUE(doc.template_slug);
    EXPECT_EQ(*doc.template_slug, "invoice");
    ASSERT_TRUE(doc.template_version);
    EXPECT_EQ(*doc.template_version, "v1");
    ASSERT_TRUE(doc.input_snapshot);
    EXPECT_EQ(*doc.input_snapshot, snapshot);

    // Persisted — a fresh primary read confirms the same shape, including
    // the JSONB round-trip.
    auto found = repo.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->status, "draft");
    ASSERT_TRUE(found->input_snapshot);
    EXPECT_EQ(*found->input_snapshot, snapshot);
}

TEST_F(DocumentsRepoTest, LinkEntrySameOrg) {
    Ledger::DocumentRepository repo;
    auto org_id = make_org("111270000002");
    auto user_id = seed_user("doclink1@example.com");
    auto entry_id = make_draft_entry(org_id, user_id);

    auto doc = repo.create(org_id, "incoming", "uploaded", "inbox");

    EXPECT_TRUE(repo.link_entry(org_id, doc.id, entry_id));

    auto linked = repo.list_for_entry(org_id, entry_id);
    ASSERT_EQ(linked.size(), 1u);
    EXPECT_EQ(linked[0].id, doc.id);
    EXPECT_EQ(linked[0].doc_type, "incoming");
}

TEST_F(DocumentsRepoTest, LinkEntryForeignOrgRejected) {
    Ledger::DocumentRepository repo;
    auto org_a = make_org("111270000003");
    auto org_b = make_org("111270000004");
    auto user_id = seed_user("doclink2@example.com");

    // Entry lives in org_b; document lives in org_a.
    auto entry_b = make_draft_entry(org_b, user_id);
    auto doc_a = repo.create(org_a, "incoming", "uploaded", "inbox");

    // document_entries.org_id = org_a would need BOTH
    // (doc_a.id, org_a) -> documents(id, org_id)      [satisfied]
    // (entry_b, org_a)  -> journal_entries(id, org_id) [NOT satisfied —
    //                                                     entry_b belongs to
    //                                                     org_b]
    // so the INSERT trips the composite FK (23503) and link_entry() returns
    // false rather than throwing — no row is created.
    EXPECT_FALSE(repo.link_entry(org_a, doc_a.id, entry_b));

    EXPECT_TRUE(repo.list_for_entry(org_a, entry_b).empty());
    EXPECT_TRUE(repo.list_for_entry(org_b, entry_b).empty());
}

TEST_F(DocumentsRepoTest, StatusTransitions) {
    Ledger::DocumentRepository repo;
    auto org_id = make_org("111270000005");
    auto other_org_id = make_org("111270000006");

    auto doc = repo.create(org_id, "invoice", "generated", "draft");
    ASSERT_EQ(doc.status, "draft");

    // Cross-org write is rejected the same way OrgCrudBase reads are: no
    // matching (id, org_id) row, so false rather than a 403/exception.
    EXPECT_FALSE(repo.set_status(other_org_id, doc.id, "final"));

    ASSERT_TRUE(repo.set_status(org_id, doc.id, "final"));
    auto after_final = repo.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(after_final);
    EXPECT_EQ(after_final->status, "final");

    ASSERT_TRUE(repo.set_status(org_id, doc.id, "sent"));
    auto after_sent = repo.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(after_sent);
    EXPECT_EQ(after_sent->status, "sent");
}

TEST_F(DocumentsRepoTest, ListByType) {
    Ledger::DocumentRepository repo;
    auto org_id = make_org("111270000007");

    // Every value migrations/010_documents.sql's doc_type CHECK allows —
    // this test doubles as a contract check that create() can insert each
    // one, not just an arbitrarily-chosen sample.
    static const std::vector<std::string> kAllDocTypes = {"invoice",
                                                          "avr",
                                                          "waybill",
                                                          "tax_invoice",
                                                          "reconciliation",
                                                          "power_of_attorney",
                                                          "incoming",
                                                          "bank_statement",
                                                          "hr",
                                                          "fno",
                                                          "other"};

    for (const auto& doc_type : kAllDocTypes)
        repo.create(org_id, doc_type, "uploaded", "inbox");

    EXPECT_EQ(repo.count_in_org(org_id), static_cast<long>(kAllDocTypes.size()));

    auto listed = repo.list_in_org(org_id, /*limit=*/100, /*offset=*/0);
    ASSERT_EQ(listed.size(), kAllDocTypes.size());

    std::set<std::string> seen_types;
    for (const auto& doc : listed) {
        EXPECT_EQ(doc.org_id, org_id);
        seen_types.insert(doc.doc_type);
    }
    EXPECT_EQ(seen_types, std::set<std::string>(kAllDocTypes.begin(), kAllDocTypes.end()));
}

}  // namespace
