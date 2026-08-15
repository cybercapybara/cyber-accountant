/**
 * @file test_documents.cpp
 * @brief Integration tests for Ledger::DocumentRepository against a real
 *        Postgres (migration 010). Exercises create() (including the
 *        JSONB input_snapshot round-trip), link_entry()'s same-org success
 *        path and its cross-org FK rejection (document_entries' composite
 *        FKs — see migrations/010_documents.sql), the draft->final->sent
 *        happy path of set_status(), and that every CHECK-listed doc_type
 *        round-trips through create()/list_in_org().
 *
 *        P3 (migration 018) adds the document_versions half: create() makes
 *        version 1 without publishing it, add_version() appends without
 *        touching older rows, a document's file metadata is read from its
 *        CURRENT version, and none of it is reachable from another tenant.
 */

#include <atomic>
#include <chrono>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
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
    // template_version/input_snapshot moved onto the document's version
    // (migration 018) and the document reports the CURRENT version's copies —
    // there is none until a render publishes one, so the document's own
    // fields are empty and the values are asserted on version 1 below.
    EXPECT_FALSE(doc.current_version_id);
    EXPECT_FALSE(doc.template_version);
    EXPECT_FALSE(doc.input_snapshot);

    // Persisted — a fresh primary read confirms the same shape, including
    // the JSONB round-trip.
    auto found = repo.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->status, "draft");
    EXPECT_EQ(found->latest_version_no, 1);

    auto version = repo.latest_version(org_id, doc.id);
    ASSERT_TRUE(version);
    EXPECT_EQ(version->version_no, 1);
    ASSERT_TRUE(version->template_version);
    EXPECT_EQ(*version->template_version, "v1");
    ASSERT_TRUE(version->input_snapshot);
    EXPECT_EQ(*version->input_snapshot, snapshot);
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

    // Every value the doc_type CHECK allows (migrations/010_documents.sql,
    // widened with 'payroll' by migrations/023_payroll_doc_type.sql) — this
    // test doubles as a contract check that create() can insert each one,
    // not just an arbitrarily-chosen sample, and so it is what proves the
    // widened constraint actually reached the database.
    static const std::vector<std::string> kAllDocTypes = {"invoice",
                                                          "avr",
                                                          "waybill",
                                                          "tax_invoice",
                                                          "reconciliation",
                                                          "power_of_attorney",
                                                          "incoming",
                                                          "bank_statement",
                                                          "hr",
                                                          "payroll",
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

// ── document_versions (migration 018) ───────────────────────────────────────

TEST_F(DocumentsRepoTest, CreateMakesVersionOneAndLeavesTheCurrentPointerNull) {
    Ledger::DocumentRepository repo;
    auto org_id = make_org("111270000008");

    auto doc = repo.create(org_id,
                           "invoice",
                           "generated",
                           "draft",
                           /*counterparty_id=*/std::nullopt,
                           "invoice",
                           "v1",
                           std::optional<nlohmann::json>{nlohmann::json{{"number", "1"}}});

    EXPECT_EQ(doc.latest_version_no, 1);
    // No file exists yet, so nothing is published — the API must keep saying
    // "no file" rather than pointing at an empty version.
    EXPECT_FALSE(doc.current_version_id.has_value());
    EXPECT_FALSE(doc.s3_key.has_value());

    auto versions = repo.list_versions(org_id, doc.id);
    ASSERT_EQ(versions.size(), 1u);
    EXPECT_EQ(versions[0].version_no, 1);
    EXPECT_EQ(versions[0].org_id, org_id);
    EXPECT_EQ(versions[0].document_id, doc.id);
    ASSERT_TRUE(versions[0].template_version);
    EXPECT_EQ(*versions[0].template_version, "v1");
    ASSERT_TRUE(versions[0].input_snapshot);
    EXPECT_EQ((*versions[0].input_snapshot)["number"].get<std::string>(), "1");
}

TEST_F(DocumentsRepoTest, AddVersionIncrementsAndKeepsOlderRows) {
    Ledger::DocumentRepository repo;
    auto org_id = make_org("111270000009");

    auto doc = repo.create(org_id, "invoice", "generated", "draft");
    auto v2 = repo.add_version(org_id,
                               doc.id,
                               std::optional<nlohmann::json>{nlohmann::json{{"number", "2"}}},
                               std::string("v1"),
                               std::nullopt);

    EXPECT_EQ(v2.version_no, 2);
    // The point of the whole table: version 1 is still there, untouched.
    EXPECT_EQ(repo.list_versions(org_id, doc.id).size(), 2u);
    auto v1 = repo.find_version(org_id, doc.id, 1);
    ASSERT_TRUE(v1);
    EXPECT_EQ(v1->version_no, 1);

    auto newest = repo.latest_version(org_id, doc.id);
    ASSERT_TRUE(newest);
    EXPECT_EQ(newest->id, v2.id);

    // A new version does NOT publish itself.
    auto after = repo.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->latest_version_no, 2);
    EXPECT_FALSE(after->current_version_id.has_value());
}

TEST_F(DocumentsRepoTest, DocumentReadsFileMetadataFromTheCurrentVersion) {
    Ledger::DocumentRepository repo;
    auto org_id = make_org("111270000010");

    auto doc = repo.create(org_id, "invoice", "generated", "draft");
    auto v1 = repo.latest_version(org_id, doc.id);
    ASSERT_TRUE(v1);
    ASSERT_TRUE(
        repo.set_version_file(org_id, v1->id, "org/x/generated/a.pdf", std::string(64, 'a'), "application/pdf", 1234));

    // Until the pointer is moved, the document has "no file" — a stored but
    // unpublished render must not become downloadable.
    auto before = repo.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(before);
    EXPECT_FALSE(before->s3_key.has_value());
    EXPECT_FALSE(before->checksum_sha256.has_value());

    ASSERT_TRUE(repo.set_current_version(org_id, doc.id, v1->id));
    auto after = repo.find_in_org(doc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(after);
    ASSERT_TRUE(after->s3_key);
    EXPECT_EQ(*after->s3_key, "org/x/generated/a.pdf");
    EXPECT_EQ(after->size_bytes.value_or(0), 1234);
    EXPECT_EQ(after->mime.value_or(""), "application/pdf");
    ASSERT_TRUE(after->current_version_id);
    EXPECT_EQ(*after->current_version_id, v1->id);
}

TEST_F(DocumentsRepoTest, VersionsAreOrgIsolated) {
    Ledger::DocumentRepository repo;
    auto org_id = make_org("111270000011");
    auto other_org_id = make_org("111270000012");

    auto doc = repo.create(org_id, "invoice", "generated", "draft");

    // Same "wrong org is indistinguishable from missing" contract the reads
    // on documents itself follow — no leak of existence, no exception.
    EXPECT_TRUE(repo.list_versions(other_org_id, doc.id).empty());
    EXPECT_FALSE(repo.find_version(other_org_id, doc.id, 1).has_value());
    EXPECT_FALSE(repo.latest_version(other_org_id, doc.id).has_value());
    EXPECT_FALSE(repo.set_current_version(other_org_id, doc.id, "00000000-0000-0000-0000-000000000000"));

    // …and the document's own version survived every one of those calls.
    EXPECT_EQ(repo.list_versions(org_id, doc.id).size(), 1u);
}

TEST_F(DocumentsRepoTest, MigrationBackfilledExistingDocuments) {
    // Каждый документ обязан иметь хотя бы одну версию — иначе бэкфилл
    // 018 пропустил строки, и старые PDF стали недостижимы. Проверяется
    // по ВСЕЙ таблице, а не по одной организации: строка без версии
    // недостижима из любого тенанта.
    Ledger::DocumentRepository repo;
    auto org_id = make_org("111270000013");
    repo.create(org_id, "invoice", "generated", "draft");

    auto orphans = Database::get().execute_read([](auto& txn) {
        auto r = txn.exec(
            "SELECT COUNT(*) FROM documents d "
            " WHERE NOT EXISTS (SELECT 1 FROM document_versions v WHERE v.document_id = d.id)");
        return r.at(0).at(0).template as<long>();
    });
    EXPECT_EQ(orphans, 0);
}

// ── P3 task 11: remove() must LOCK, not merely look ──────────────────────────
//
// Being inside one transaction is not enough. execute_write runs at READ
// COMMITTED, so an unlocked "is anything posted?" look is stale the instant it
// returns: a concurrent session can post a linked draft entry and commit
// between that look and the DELETE, and because document_entries cascades, the
// link then vanishes silently — leaving a POSTED entry with no basis, which is
// precisely the corruption this task exists to prevent. Reproduced on
// PostgreSQL 16 before the fix: the check returned false, the other session
// posted and committed, the DELETE succeeded.
//
// This test pins the fix without needing to hit that microsecond window. It
// takes the conflicting lock FROM THE OUTSIDE — an exclusive FOR UPDATE on the
// linked journal entry — and asserts that remove() WAITS for it. An
// implementation that only reads (no FOR SHARE) never blocks on a row lock
// under MVCC and returns in single-digit milliseconds, so this fails loudly if
// the locking is dropped or if the status predicate is "simplified" back into
// the locking query, where the planner pushes it below LockRows and the draft
// row ends up never locked at all.
TEST_F(DocumentsRepoTest, RemoveWaitsForALockOnTheLinkedEntryBeforeDeciding) {
    Ledger::DocumentRepository repo;
    auto org_id = make_org("111270000014");
    auto user_id = seed_user("lockrace@example.com");
    auto entry_id = make_draft_entry(org_id, user_id);
    auto doc = repo.create(org_id, "incoming", "uploaded", "inbox");
    ASSERT_TRUE(repo.link_entry(org_id, doc.id, entry_id));

    constexpr int kHoldMs = 600;
    std::atomic<bool> holding{false};
    std::thread holder([&] {
        Database::get().execute_write([&](auto& txn) {
            // Exactly the lock a concurrent JournalService::post() would take
            // on this row before flipping its status.
            txn.exec_params(
                "SELECT id FROM journal_entries WHERE id = $1 AND org_id = $2 FOR UPDATE", entry_id, org_id);
            holding = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(kHoldMs));
            return 0;
        });
    });
    while (!holding)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    const auto started = std::chrono::steady_clock::now();
    const auto outcome = repo.remove(org_id, doc.id);
    const auto waited_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    holder.join();

    // Generous threshold (the holder keeps the lock for 600 ms): the point is
    // the difference between "waited" and "did not wait at all", which was
    // measured live at 1.5 s versus 4 ms.
    EXPECT_GE(waited_ms, 250) << "remove() returned in " << waited_ms
                              << " ms while another session held FOR UPDATE on the linked journal entry — it is not "
                                 "locking, so a concurrent post can still slip between its check and its DELETE";
    // Once the holder let go, the entry was still a draft, so the delete is
    // the correct outcome — the lock delays the decision, it does not change it.
    EXPECT_EQ(outcome, Ledger::DeleteOutcome::kDeleted);
    EXPECT_FALSE(repo.find_in_org(doc.id, org_id, /*from_primary=*/true).has_value());
}

}  // namespace
