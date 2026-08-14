/**
 * @file test_journal_schema.cpp
 * @brief Schema tests for the double-entry journal (migration 009):
 *        `journal_entries` / `journal_lines` plus the three invariant
 *        triggers — nothing here goes through a repository, since the
 *        invariants are meant to hold at the database layer regardless of
 *        which application code writes to these tables.
 *
 *        Covers: a balanced two-line entry commits cleanly
 *        (BalancedEntryCommits); an unbalanced entry is rejected — not on
 *        the first INSERT, but at COMMIT, because the balance trigger is a
 *        DEFERRABLE CONSTRAINT TRIGGER fired once per statement-set, after
 *        all lines of the write are in (UnbalancedEntryRejectedAtCommit);
 *        a posted entry cannot have its ordinary fields changed
 *        (PostedEntryImmutable) nor its lines' amounts changed
 *        (PostedLinesImmutable); a draft entry (and its lines) can be
 *        edited freely (DraftFreelyEditable); and posted -> reversed is the
 *        ONE legal UPDATE on a posted entry, but only when no other column
 *        changes alongside status — changing status alone succeeds,
 *        changing status together with e.g. description does not
 *        (PostedCanOnlyTransitionToReversed).
 *
 *        Fix round 1 (review + security scan on a live Postgres 16) added:
 *        deleting a still-draft entry with lines must succeed, both via a
 *        direct DELETE and via the organizations -> entries -> lines cascade
 *        (DeleteDraftWithLinesSucceeds) — trg_journal_lines_frozen's parent
 *        lookup returns NULL once the cascade has already removed the
 *        parent row, which must NOT read as "entry is posted/reversed";
 *        a line cannot be walked from one entry_id (or org_id) to another
 *        via UPDATE, regardless of either entry's status
 *        (LinesCannotMoveBetweenEntries); draft cannot jump straight to
 *        reversed without ever being posted (DraftCannotJumpToReversed); a
 *        draft with zero lines cannot be posted (CannotPostEmptyEntry); and
 *        a line's org_id must match its entry's real org_id at the
 *        constraint level, not just in application code
 *        (CrossOrgLineRejectedByCompositeFk).
 *
 *        Fixture idiom follows tests/integration/test_accounts.cpp: DELETE
 *        FROM organizations (not TRUNCATE ... CASCADE) in SetUp. journal_*
 *        rows are all org_id NOT NULL, so a plain per-row ON DELETE CASCADE
 *        clears them along with the deleted organizations — but TRUNCATE
 *        organizations CASCADE would ALSO blanket-truncate the org_id IS
 *        NULL system chart-of-accounts seed from migration 008, which this
 *        suite's fixtures don't own and must not erase for the rest of the
 *        binary's test run.
 */

#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

class JournalSchemaTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("DELETE FROM organizations");
            return 0;
        });
    }

    /// Create a tenant and return its id. Fixed BINs below are only unique
    /// within a single run; clearing organizations up front (SetUp) keeps
    /// the suite idempotent when re-run against a persistent local Postgres.
    std::string make_org(const std::string& bin) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "Journal Test Org " + bin, "snr_simplified", false).id;
    }

    /// Insert a draft journal_entries row and return its id.
    static std::string insert_draft_entry(auto& txn, const std::string& org_id, const std::string& description = "") {
        auto r = txn.exec_params(
            "INSERT INTO journal_entries (org_id, entry_date, description) "
            "VALUES ($1, '2026-08-14', $2) RETURNING id",
            org_id,
            description);
        return r[0]["id"].template as<std::string>();
    }

    /// Insert a journal_lines row for the given entry.
    static std::string insert_line(auto& txn,
                                   const std::string& org_id,
                                   const std::string& entry_id,
                                   const std::string& account_code,
                                   const std::string& side,
                                   const std::string& amount) {
        auto r = txn.exec_params(
            "INSERT INTO journal_lines (org_id, entry_id, account_code, side, amount) "
            "VALUES ($1, $2, $3, $4, $5) RETURNING id",
            org_id,
            entry_id,
            account_code,
            side,
            amount);
        return r[0]["id"].template as<std::string>();
    }

    /// Create a fully balanced two-line draft entry (100.00 debit / 100.00
    /// credit) and post it (draft -> posted is a free field change on a
    /// still-draft row, per journal_entries_immutability()). Returns
    /// {entry_id, line_id_of_the_debit_line}.
    std::pair<std::string, std::string> make_posted_entry(const std::string& org_id) {
        std::string entry_id;
        std::string line_id;
        Database::get().execute_write([&](auto& txn) {
            entry_id = insert_draft_entry(txn, org_id, "Posted fixture entry");
            line_id = insert_line(txn, org_id, entry_id, "1030", "debit", "100.00");
            insert_line(txn, org_id, entry_id, "6010", "credit", "100.00");
            return 0;
        });
        Database::get().execute_write([&](auto& txn) {
            txn.exec_params("UPDATE journal_entries SET status = 'posted' WHERE id = $1", entry_id);
            return 0;
        });
        return {entry_id, line_id};
    }
};

TEST_F(JournalSchemaTest, BalancedEntryCommits) {
    auto org_id = make_org("111260000030");

    EXPECT_NO_THROW(Database::get().execute_write([&](auto& txn) {
        auto entry_id = insert_draft_entry(txn, org_id, "Balanced entry");
        insert_line(txn, org_id, entry_id, "1030", "debit", "100.00");
        insert_line(txn, org_id, entry_id, "6010", "credit", "100.00");
        return 0;
    }));

    auto count = Database::get().execute_read([&](auto& txn) {
        return txn.exec_params("SELECT COUNT(*) FROM journal_lines WHERE org_id = $1", org_id)
            .at(0)
            .at(0)
            .template as<int>();
    });
    EXPECT_EQ(count, 2);
}

TEST_F(JournalSchemaTest, UnbalancedEntryRejectedAtCommit) {
    auto org_id = make_org("111260000031");

    // A single 100.00 debit line with no offsetting credit: the deferred
    // constraint trigger fires once, at COMMIT of this whole execute_write
    // transaction — not on the INSERT itself — so the exception surfaces
    // from execute_write, not from the individual txn.exec_params call.
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        auto entry_id = insert_draft_entry(txn, org_id, "Unbalanced entry");
        insert_line(txn, org_id, entry_id, "1030", "debit", "100.00");
        return 0;
    }),
                 std::exception);
}

TEST_F(JournalSchemaTest, PostedEntryImmutable) {
    auto org_id = make_org("111260000032");
    auto [entry_id, line_id] = make_posted_entry(org_id);
    (void)line_id;

    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("UPDATE journal_entries SET description = 'tampered' WHERE id = $1", entry_id);
        return 0;
    }),
                 std::exception);
}

TEST_F(JournalSchemaTest, PostedLinesImmutable) {
    auto org_id = make_org("111260000033");
    auto [entry_id, line_id] = make_posted_entry(org_id);
    (void)entry_id;

    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("UPDATE journal_lines SET amount = '200.00' WHERE id = $1", line_id);
        return 0;
    }),
                 std::exception);
}

TEST_F(JournalSchemaTest, DraftFreelyEditable) {
    auto org_id = make_org("111260000034");
    std::string entry_id;
    std::string line_id;
    Database::get().execute_write([&](auto& txn) {
        entry_id = insert_draft_entry(txn, org_id, "Original description");
        line_id = insert_line(txn, org_id, entry_id, "1030", "debit", "100.00");
        insert_line(txn, org_id, entry_id, "6010", "credit", "100.00");
        return 0;
    });

    // Still draft: header field and a line's non-amount field both change
    // freely in the same transaction, and the balance is untouched (no
    // amount changed), so the deferred balance trigger is happy too.
    EXPECT_NO_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("UPDATE journal_entries SET description = 'Edited description' WHERE id = $1", entry_id);
        txn.exec_params("UPDATE journal_lines SET account_code = '1010' WHERE id = $1", line_id);
        return 0;
    }));

    auto description = Database::get().execute_read([&](auto& txn) {
        return txn.exec_params("SELECT description FROM journal_entries WHERE id = $1", entry_id)
            .at(0)
            .at(0)
            .template as<std::string>();
    });
    EXPECT_EQ(description, "Edited description");
}

TEST_F(JournalSchemaTest, PostedCanOnlyTransitionToReversed) {
    auto org_id = make_org("111260000035");

    // Case 1: status -> 'reversed' alone (no other column touched) is the
    // one legal UPDATE on a posted entry.
    auto [entry_a, line_a] = make_posted_entry(org_id);
    (void)line_a;
    EXPECT_NO_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("UPDATE journal_entries SET status = 'reversed' WHERE id = $1", entry_a);
        return 0;
    }));
    auto status_a = Database::get().execute_read([&](auto& txn) {
        return txn.exec_params("SELECT status FROM journal_entries WHERE id = $1", entry_a)
            .at(0)
            .at(0)
            .template as<std::string>();
    });
    EXPECT_EQ(status_a, "reversed");

    // Case 2: status -> 'reversed' bundled with a description change is
    // NOT legal — the immutability trigger requires every other column to
    // stay byte-for-byte equal to OLD.
    auto [entry_b, line_b] = make_posted_entry(org_id);
    (void)line_b;
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("UPDATE journal_entries SET status = 'reversed', description = 'sneaky edit' WHERE id = $1",
                        entry_b);
        return 0;
    }),
                 std::exception);

    // Storno nuance: reversing must still be able to INSERT lines into a
    // brand-new DRAFT entry created for the storno — trg_journal_lines_frozen
    // looks at the status of the entry the NEW line belongs to, not at
    // whether some other entry references it via reverses_entry_id, so this
    // is unaffected by entry_a/entry_b above already being reversed/posted.
    EXPECT_NO_THROW(Database::get().execute_write([&](auto& txn) {
        auto storno_id = insert_draft_entry(txn, org_id, "Storno of entry_a");
        txn.exec_params("UPDATE journal_entries SET reverses_entry_id = $2 WHERE id = $1", storno_id, entry_a);
        insert_line(txn, org_id, storno_id, "1030", "credit", "100.00");
        insert_line(txn, org_id, storno_id, "6010", "debit", "100.00");
        return 0;
    }));
}

TEST_F(JournalSchemaTest, DeleteDraftWithLinesSucceeds) {
    auto org_id = make_org("111260000036");

    // Case 1: a direct DELETE of a still-draft entry cascades its own two
    // lines away via journal_lines' FK ON DELETE CASCADE. Before the
    // CRITICAL-2 fix, trg_journal_lines_frozen's parent lookup found no row
    // (the parent was already gone earlier in the same cascade) and
    // misread that as "entry is posted/reversed", blocking the very
    // deletion the FK was trying to perform.
    std::string entry_id;
    Database::get().execute_write([&](auto& txn) {
        entry_id = insert_draft_entry(txn, org_id, "Direct delete of a draft with lines");
        insert_line(txn, org_id, entry_id, "1030", "debit", "100.00");
        insert_line(txn, org_id, entry_id, "6010", "credit", "100.00");
        return 0;
    });
    EXPECT_NO_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("DELETE FROM journal_entries WHERE id = $1", entry_id);
        return 0;
    }));
    auto remaining_after_direct_delete = Database::get().execute_read([&](auto& txn) {
        return txn.exec_params("SELECT COUNT(*) FROM journal_lines WHERE entry_id = $1", entry_id)
            .at(0)
            .at(0)
            .template as<int>();
    });
    EXPECT_EQ(remaining_after_direct_delete, 0);

    // Case 2: the same cascade triggered two levels up, exactly like this
    // suite's own SetUp() does between tests: organizations -> (FK)
    // journal_entries -> (FK) journal_lines.
    std::string entry_id_2;
    Database::get().execute_write([&](auto& txn) {
        entry_id_2 = insert_draft_entry(txn, org_id, "Deleted via organizations cascade");
        insert_line(txn, org_id, entry_id_2, "1030", "debit", "50.00");
        insert_line(txn, org_id, entry_id_2, "6010", "credit", "50.00");
        return 0;
    });
    EXPECT_NO_THROW(Database::get().execute_write([](auto& txn) {
        txn.exec("DELETE FROM organizations");
        return 0;
    }));
}

TEST_F(JournalSchemaTest, LinesCannotMoveBetweenEntries) {
    auto org_id = make_org("111260000037");

    std::string entry_a;
    std::string entry_b;
    std::string line_id;
    Database::get().execute_write([&](auto& txn) {
        entry_a = insert_draft_entry(txn, org_id, "Entry A");
        line_id = insert_line(txn, org_id, entry_a, "1030", "debit", "100.00");
        insert_line(txn, org_id, entry_a, "6010", "credit", "100.00");
        entry_b = insert_draft_entry(txn, org_id, "Entry B");
        insert_line(txn, org_id, entry_b, "1030", "debit", "50.00");
        insert_line(txn, org_id, entry_b, "6010", "credit", "50.00");
        return 0;
    });

    // Both entries are draft, so this LOOKS harmless at first glance — but
    // journal_lines_frozen_after_post() only ever inspects the status of
    // the TARGET (NEW) entry, never the source (OLD) one. Without an
    // explicit entry_id/org_id-change guard, a line could be walked out of
    // a POSTED entry into a draft one completely undetected, silently
    // corrupting the posted entry's already-verified balance. The move is
    // therefore rejected unconditionally, regardless of either side's
    // status.
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("UPDATE journal_lines SET entry_id = $2 WHERE id = $1", line_id, entry_b);
        return 0;
    }),
                 std::exception);
}

TEST_F(JournalSchemaTest, DraftCannotJumpToReversed) {
    auto org_id = make_org("111260000038");

    std::string entry_id;
    Database::get().execute_write([&](auto& txn) {
        entry_id = insert_draft_entry(txn, org_id, "Never posted");
        insert_line(txn, org_id, entry_id, "1030", "debit", "100.00");
        insert_line(txn, org_id, entry_id, "6010", "credit", "100.00");
        return 0;
    });

    // posted -> reversed is the one legal transition; draft -> reversed
    // skips posting entirely and must be rejected — storno reverses a
    // POSTED entry, there is nothing to storno on a draft.
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("UPDATE journal_entries SET status = 'reversed' WHERE id = $1", entry_id);
        return 0;
    }),
                 std::exception);
}

TEST_F(JournalSchemaTest, CannotPostEmptyEntry) {
    auto org_id = make_org("111260000039");

    std::string entry_id;
    Database::get().execute_write([&](auto& txn) {
        entry_id = insert_draft_entry(txn, org_id, "No lines at all");
        return 0;
    });

    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("UPDATE journal_entries SET status = 'posted' WHERE id = $1", entry_id);
        return 0;
    }),
                 std::exception);
}

TEST_F(JournalSchemaTest, CrossOrgLineRejectedByCompositeFk) {
    auto org_a = make_org("111260000040");
    auto org_b = make_org("111260000041");

    std::string entry_id;
    Database::get().execute_write([&](auto& txn) {
        entry_id = insert_draft_entry(txn, org_a, "Belongs to org_a");
        return 0;
    });

    // A line claiming org_b while pointing at an org_a entry has no
    // matching (entry_id, org_id) pair in journal_entries to satisfy the
    // composite FK — rejected at the constraint level (foreign_key_violation),
    // not by application-level org-scoping logic that a repository could
    // forget to check.
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        insert_line(txn, org_b, entry_id, "1030", "debit", "100.00");
        return 0;
    }),
                 std::exception);
}

}  // namespace
