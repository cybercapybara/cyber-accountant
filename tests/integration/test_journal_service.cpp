/**
 * @file test_journal_service.cpp
 * @brief Integration tests for Ledger::JournalService against a real
 *        Postgres (migration 009) — the draft/post/storno lifecycle, its
 *        pre-DB validations (line count, account visibility, counterparty
 *        ownership, Σdebit=Σcredit), the cross-org invisibility
 *        OrgCrudBase::find_in_org guarantees, and Ledger::parse_tiyn's
 *        decimal-string parsing.
 */

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "ledger/AccountRepository.hpp"
#include "ledger/CounterpartyRepository.hpp"
#include "ledger/JournalEntry.hpp"
#include "ledger/JournalRepository.hpp"
#include "ledger/JournalService.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

class JournalServiceTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // journal_entries_immutability() (migration 009) forbids DELETEing a
        // posted/reversed row — PostTransitions/ReverseCreatesMirror... below
        // deliberately leave rows in exactly those statuses. A plain
        // "DELETE FROM organizations" cascade (the idiom test_counterparties.cpp
        // and test_accounts.cpp use) would fire that trigger once per
        // cascaded journal_entries row and abort the whole SetUp from the
        // second test onward. TRUNCATE bypasses row-level triggers entirely,
        // so clear the journal tables that way FIRST — before anything else
        // that would otherwise try to cascade into them.
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE journal_lines, journal_entries CASCADE");
            txn.exec("DELETE FROM organizations");
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
    }

    /// Create a tenant and return its id. Fixed BINs below are only unique
    /// within a single run; clearing organizations up front (SetUp) keeps
    /// the suite idempotent when re-run against a persistent local Postgres.
    std::string make_org(const std::string& bin, const std::string& name) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, name, "snr_simplified", false).id;
    }

    /// Confirmed "User"-role user; mirrors seed_user() in test_org_context.cpp
    /// — journal_entries.created_by_user_id is a (nullable) FK to users(id),
    /// so create_draft/reverse need a real row to satisfy it.
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

    static Ledger::Counterparty draft_counterparty(const std::string& identifier) {
        Ledger::Counterparty c;
        c.identifier = identifier;
        c.name = "Foreign LLP";
        c.address = "Almaty, Abay 1";
        c.iik = "KZ00000000000000000";
        c.bik = "AAAAKZKA";
        c.kbe = "17";
        c.is_resident = true;
        c.vat_payer = false;
        c.contact_email = "ap@foreign.example";
        return c;
    }

    static Ledger::JournalLine line(const std::string& account_code,
                                    const std::string& side,
                                    const std::string& amount,
                                    std::optional<std::string> counterparty_id = std::nullopt,
                                    std::optional<std::string> vat_amount = std::nullopt) {
        Ledger::JournalLine l;
        l.account_code = account_code;
        l.side = side;
        l.amount = amount;
        l.counterparty_id = std::move(counterparty_id);
        l.vat_amount = std::move(vat_amount);
        return l;
    }

    /// A minimal balanced 2-line entry: cash in (1030) against sales income
    /// (6010), 1000.00 each side, no counterparty/VAT.
    static std::vector<Ledger::JournalLine> balanced_lines() {
        return {line("1030", "debit", "1000.00"), line("6010", "credit", "1000.00")};
    }
};

TEST_F(JournalServiceTest, CreateDraftBalanced) {
    Ledger::JournalService svc;
    auto org_id = make_org("111260000001", "Journal Test Org 1");
    auto user_id = seed_user("draft1@example.com");

    auto entry = svc.create_draft(org_id, user_id, "2026-01-15", "Cash sale", balanced_lines());

    EXPECT_FALSE(entry.id.empty());
    EXPECT_EQ(entry.org_id, org_id);
    EXPECT_EQ(entry.entry_date, "2026-01-15");
    EXPECT_EQ(entry.description, "Cash sale");
    EXPECT_EQ(entry.status, "draft");
    ASSERT_TRUE(entry.created_by_user_id);
    EXPECT_EQ(*entry.created_by_user_id, user_id);
    EXPECT_FALSE(entry.reverses_entry_id);
    ASSERT_EQ(entry.lines.size(), 2u);
    EXPECT_EQ(entry.lines[0].account_code, "1030");
    EXPECT_EQ(entry.lines[0].side, "debit");
    EXPECT_EQ(entry.lines[0].amount, "1000.00");
    EXPECT_EQ(entry.lines[1].account_code, "6010");
    EXPECT_EQ(entry.lines[1].side, "credit");
    EXPECT_EQ(entry.lines[1].amount, "1000.00");

    // Persisted — a fresh repository read confirms the same shape.
    Ledger::JournalRepository repo;
    auto found = repo.find_in_org(entry.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->status, "draft");
    EXPECT_EQ(repo.load_lines(*found).size(), 2u);
}

TEST_F(JournalServiceTest, CreateDraftUnbalancedRejected422Path) {
    Ledger::JournalService svc;
    auto org_id = make_org("111260000002", "Journal Test Org 2");
    auto user_id = seed_user("draft2@example.com");

    std::vector<Ledger::JournalLine> lines = {line("1030", "debit", "1000.00"), line("6010", "credit", "900.00")};

    EXPECT_THROW(svc.create_draft(org_id, user_id, "2026-01-15", "Unbalanced", lines), Ledger::UnbalancedEntry);

    // Rejected BEFORE any SQL runs — no row exists to map to a 500 later.
    Ledger::JournalRepository repo;
    EXPECT_EQ(repo.count_in_org(org_id), 0);
}

TEST_F(JournalServiceTest, UnknownAccountRejected) {
    Ledger::JournalService svc;
    auto org_id = make_org("111260000003", "Journal Test Org 3");
    auto user_id = seed_user("draft3@example.com");

    // Balanced by amount (500.00 = 500.00) so only the account check can
    // possibly reject it — proves UnknownAccount fires independently of the
    // balance check, not as a side effect of it.
    std::vector<Ledger::JournalLine> lines = {line("9999.notreal", "debit", "500.00"),
                                              line("6010", "credit", "500.00")};

    EXPECT_THROW(svc.create_draft(org_id, user_id, "2026-01-15", "Bad account", lines), Ledger::UnknownAccount);

    Ledger::JournalRepository repo;
    EXPECT_EQ(repo.count_in_org(org_id), 0);
}

TEST_F(JournalServiceTest, ForeignCounterpartyRejected) {
    Ledger::JournalService svc;
    auto org_a = make_org("111260000004", "Journal Test Org 4A");
    auto org_b = make_org("111260000005", "Journal Test Org 4B");
    auto user_id = seed_user("draft4@example.com");

    Ledger::CounterpartyRepository counterparties;
    auto foreign_cp = counterparties.create(org_b, draft_counterparty("111240000200"));

    // Balanced (300.00 = 300.00), valid accounts — only the counterparty
    // belongs to a different org.
    std::vector<Ledger::JournalLine> lines = {line("1210", "debit", "300.00", foreign_cp.id),
                                              line("6010", "credit", "300.00")};

    EXPECT_THROW(svc.create_draft(org_a, user_id, "2026-01-15", "Foreign counterparty", lines),
                 Ledger::ForeignCounterparty);

    Ledger::JournalRepository repo;
    EXPECT_EQ(repo.count_in_org(org_a), 0);
}

TEST_F(JournalServiceTest, PostTransitions) {
    Ledger::JournalService svc;
    auto org_id = make_org("111260000006", "Journal Test Org 5");
    auto user_id = seed_user("post1@example.com");

    auto entry = svc.create_draft(org_id, user_id, "2026-01-15", "To be posted", balanced_lines());
    ASSERT_EQ(entry.status, "draft");

    auto posted = svc.post(org_id, entry.id);
    ASSERT_TRUE(posted);
    EXPECT_EQ(posted->id, entry.id);
    EXPECT_EQ(posted->status, "posted");
    ASSERT_EQ(posted->lines.size(), 2u);

    Ledger::JournalRepository repo;
    auto reloaded = repo.find_in_org(entry.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(reloaded->status, "posted");
}

TEST_F(JournalServiceTest, PostIdempotentRejected) {
    Ledger::JournalService svc;
    auto org_id = make_org("111260000007", "Journal Test Org 6");
    auto user_id = seed_user("post2@example.com");

    auto entry = svc.create_draft(org_id, user_id, "2026-01-15", "Post twice", balanced_lines());
    ASSERT_TRUE(svc.post(org_id, entry.id));

    EXPECT_THROW(svc.post(org_id, entry.id), Ledger::InvalidEntryState);
}

TEST_F(JournalServiceTest, ReverseCreatesMirrorAndMarksOriginal) {
    Ledger::JournalService svc;
    auto org_id = make_org("111260000008", "Journal Test Org 7");
    auto user_id = seed_user("storno1@example.com");

    Ledger::CounterpartyRepository counterparties;
    auto cp = counterparties.create(org_id, draft_counterparty("111260000201"));

    // One line carries a counterparty AND VAT so the mirror-fidelity check
    // below exercises every field the brief calls out ("суммы/счета/
    // контрагенты/vat те же"), not just account+amount.
    std::vector<Ledger::JournalLine> lines = {line("1210", "debit", "1120.00", cp.id, "120.00"),
                                              line("6010", "credit", "1120.00")};
    auto entry = svc.create_draft(org_id, user_id, "2026-01-15", "Original entry", lines);
    auto posted = svc.post(org_id, entry.id);
    ASSERT_TRUE(posted);

    auto storno = svc.reverse(org_id, entry.id, user_id);
    ASSERT_TRUE(storno);
    EXPECT_NE(storno->id, entry.id);
    EXPECT_EQ(storno->status, "posted");
    EXPECT_EQ(storno->description, "Сторно: Original entry");
    EXPECT_EQ(storno->entry_date, entry.entry_date);
    EXPECT_EQ(storno->org_id, org_id);
    ASSERT_TRUE(storno->reverses_entry_id);
    EXPECT_EQ(*storno->reverses_entry_id, entry.id);

    ASSERT_EQ(storno->lines.size(), posted->lines.size());
    for (std::size_t i = 0; i < storno->lines.size(); ++i) {
        const auto& original_line = posted->lines[i];
        const auto& mirror_line = storno->lines[i];
        EXPECT_EQ(mirror_line.account_code, original_line.account_code);
        EXPECT_EQ(mirror_line.amount, original_line.amount);
        EXPECT_EQ(mirror_line.counterparty_id, original_line.counterparty_id);
        EXPECT_EQ(mirror_line.vat_amount, original_line.vat_amount);
        // Sides are flipped, everything else is identical.
        EXPECT_NE(mirror_line.side, original_line.side);
        EXPECT_EQ(mirror_line.side, original_line.side == "debit" ? "credit" : "debit");
    }

    // Original is reversed; the storno itself is posted — both reloaded
    // fresh from the DB, not just the in-memory return values.
    Ledger::JournalRepository repo;
    auto original_reloaded = repo.find_in_org(entry.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(original_reloaded);
    EXPECT_EQ(original_reloaded->status, "reversed");

    auto storno_reloaded = repo.find_in_org(storno->id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(storno_reloaded);
    EXPECT_EQ(storno_reloaded->status, "posted");
    EXPECT_EQ(repo.load_lines(*storno_reloaded).size(), 2u);
}

TEST_F(JournalServiceTest, ReverseDraftRejected) {
    Ledger::JournalService svc;
    auto org_id = make_org("111260000009", "Journal Test Org 8");
    auto user_id = seed_user("storno2@example.com");

    auto entry = svc.create_draft(org_id, user_id, "2026-01-15", "Still draft", balanced_lines());

    EXPECT_THROW(svc.reverse(org_id, entry.id, user_id), Ledger::InvalidEntryState);
}

TEST_F(JournalServiceTest, CrossOrgInvisible) {
    Ledger::JournalService svc;
    auto org_a = make_org("111260000010", "Journal Test Org 9A");
    auto org_b = make_org("111260000011", "Journal Test Org 9B");
    auto user_id = seed_user("cross1@example.com");

    auto entry = svc.create_draft(org_a, user_id, "2026-01-15", "Org A only", balanced_lines());

    // A different org can neither post nor reverse it — both come back
    // std::nullopt, not an exception, mirroring OrgCrudBase::find_in_org's
    // "wrong org is indistinguishable from missing" contract.
    EXPECT_FALSE(svc.post(org_b, entry.id));
    EXPECT_FALSE(svc.reverse(org_b, entry.id, user_id));

    Ledger::JournalRepository repo;
    EXPECT_FALSE(repo.find_in_org(entry.id, org_b));
    ASSERT_TRUE(repo.find_in_org(entry.id, org_a));
}

// ---------------------------------------------------------------------------
// Ledger::parse_tiyn — pure function, no DB needed, plain TEST() alongside
// the fixture-based suite above per the brief ("юнит-подобные кейсы parse_tiyn
// в том же файле").
// ---------------------------------------------------------------------------

TEST(ParseTiyn, WholeAmountAssumesZeroCents) {
    EXPECT_EQ(Ledger::parse_tiyn("100"), 10000);
}

TEST(ParseTiyn, TwoDecimalDigits) {
    EXPECT_EQ(Ledger::parse_tiyn("1234.56"), 123456);
}

TEST(ParseTiyn, SingleDecimalDigitIsRightPadded) {
    EXPECT_EQ(Ledger::parse_tiyn("10.5"), 1050);
}

TEST(ParseTiyn, RejectsMoreThanTwoDecimalDigits) {
    EXPECT_THROW(Ledger::parse_tiyn("10.555"), std::invalid_argument);
}

TEST(ParseTiyn, RejectsMultipleDecimalPoints) {
    EXPECT_THROW(Ledger::parse_tiyn("1.2.3"), std::invalid_argument);
}

// Fix round 1: `whole * 100 + frac` overflows `long long` (UB, not an
// exception) once the integer part reaches 17+ digits — 16 nines is the
// largest integer part that stays safe (see parse_tiyn's doc comment for
// the exact derivation). These two cases pin the boundary on both sides.
TEST(ParseTiyn, RejectsIntegerPartLongerThan16Digits) {
    // 17 digits — one past the NUMERIC(18,2)-derived limit that keeps
    // `whole * 100` itself from overflowing.
    EXPECT_THROW(Ledger::parse_tiyn("99999999999999999.00"), std::invalid_argument);
}

TEST(ParseTiyn, Parses16DigitIntegerPartAtTheBoundary) {
    // 16 nines, worst-case fractional part: 9999999999999999 * 100 + 99 =
    // 999999999999999999, comfortably under LLONG_MAX (~9.22e18).
    EXPECT_EQ(Ledger::parse_tiyn("9999999999999999.99"), 999999999999999999LL);
}

TEST(ParseTiyn, RejectsNegativeSign) {
    EXPECT_THROW(Ledger::parse_tiyn("-5.00"), std::invalid_argument);
}

TEST(ParseTiyn, RejectsZero) {
    EXPECT_THROW(Ledger::parse_tiyn("0.00"), std::invalid_argument);
}

TEST(ParseTiyn, RejectsNonNumericGarbage) {
    EXPECT_THROW(Ledger::parse_tiyn("abc"), std::invalid_argument);
}

TEST(ParseTiyn, RejectsEmptyString) {
    EXPECT_THROW(Ledger::parse_tiyn(""), std::invalid_argument);
}

}  // namespace
