/**
 * @file test_tax_service.cpp
 * @brief Integration tests for Tax::TaxService (migration 014) against a
 *        real Postgres — СНР на основе упрощённой декларации, НДС
 *        начислено/к зачёту, snapshot reproducibility, recalculate-overwrites,
 *        the near-threshold registration alert, and org isolation.
 *
 * Journal fixtures are built through Ledger::JournalService (not raw INSERTs)
 * so every posted entry actually satisfies the balance trigger and the
 * draft/posted lifecycle migrations/009_journal.sql enforces — the same
 * posture test_journal_service.cpp itself takes.
 */

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "ledger/AccountRepository.hpp"
#include "ledger/JournalEntry.hpp"
#include "ledger/JournalService.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "tax/TaxCalculationRepository.hpp"
#include "tax/TaxService.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

class TaxServiceTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // Same fixture shape as JournalServiceTest (test_journal_service.cpp):
        // wipe_org_data() TRUNCATEs journal_lines/journal_entries (bypassing
        // journal_entries_immutability(), since PostedIncome fixtures below
        // deliberately leave posted rows behind) and DELETEs organizations —
        // tax_calculations.org_id cascades off that DELETE with no blocking
        // trigger of its own, so no extra step is needed for this table.
        // `users` is truncated separately for the same reason
        // JournalServiceTest does it: created_by_user_id is a real FK.
        TestHelpers::wipe_org_data();
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            return 0;
        });
    }

    std::string make_org(const std::string& bin) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "Tax Service Test Org " + bin, "snr_simplified", false).id;
    }

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

    static Ledger::JournalLine line(const std::string& account_code,
                                    const std::string& side,
                                    const std::string& amount,
                                    std::optional<std::string> vat_amount = std::nullopt) {
        Ledger::JournalLine l;
        l.account_code = account_code;
        l.side = side;
        l.amount = amount;
        l.vat_amount = std::move(vat_amount);
        return l;
    }

    /// Posts a balanced two-line entry (cash in 1030 <-> income 6010) dated
    /// @p entry_date for @p amount ₸ and returns it. Used wherever a test
    /// only cares "this much posted income landed in this period".
    Ledger::JournalEntry post_income(Ledger::JournalService& svc,
                                     const std::string& org_id,
                                     const std::string& user_id,
                                     const std::string& entry_date,
                                     const std::string& amount) {
        auto entry = svc.create_draft(org_id,
                                      user_id,
                                      entry_date,
                                      "Income " + amount,
                                      {line("1030", "debit", amount), line("6010", "credit", amount)});
        auto posted = svc.post(org_id, entry.id);
        return *posted;
    }
};

TEST_F(TaxServiceTest, SnrTaxOnPostedIncome) {
    Ledger::JournalService journal_svc;
    Tax::TaxService tax_svc;
    auto org_id = make_org("111280000001");
    auto user_id = seed_user("snr1@example.com");

    // 500,000 ₸ + 300,000 ₸ = 800,000 ₸ posted income inside H1 2026.
    post_income(journal_svc, org_id, user_id, "2026-02-01", "500000.00");
    post_income(journal_svc, org_id, user_id, "2026-03-01", "300000.00");

    auto calc = tax_svc.calculate_snr(org_id, "2026-01-01", "2026-06-30");

    EXPECT_EQ(calc.kind, "snr_simplified");
    EXPECT_EQ(calc.org_id, org_id);
    EXPECT_EQ(calc.period_from, "2026-01-01");
    EXPECT_EQ(calc.period_to, "2026-06-30");
    ASSERT_EQ(calc.result_snapshot.at("income_tiyn").get<long long>(), 80000000LL);  // 800,000 ₸
    ASSERT_EQ(calc.result_snapshot.at("rate_bp").get<long long>(), 400);             // 4%
    ASSERT_EQ(calc.result_snapshot.at("tax_tiyn").get<long long>(), 3200000LL);      // exactly 4% of 800,000 ₸
    EXPECT_EQ(calc.total_tiyn, 3200000LL);
    // Deliberately no legal split to encode — see TaxService.hpp file header.
    EXPECT_FALSE(calc.result_snapshot.contains("ipn_part_tiyn"));
    EXPECT_FALSE(calc.result_snapshot.contains("social_tax_part_tiyn"));
}

TEST_F(TaxServiceTest, SnrIgnoresDraftEntries) {
    Ledger::JournalService journal_svc;
    Tax::TaxService tax_svc;
    auto org_id = make_org("111280000002");
    auto user_id = seed_user("snr2@example.com");

    // Posted: 200,000 ₸ — this is the ONLY income that should count.
    post_income(journal_svc, org_id, user_id, "2026-02-01", "200000.00");

    // A second entry with much larger income, left as a draft (never
    // posted) — must NOT contribute to the SNR base.
    journal_svc.create_draft(org_id,
                             user_id,
                             "2026-03-01",
                             "Still a draft",
                             {line("1030", "debit", "900000.00"), line("6010", "credit", "900000.00")});

    auto calc = tax_svc.calculate_snr(org_id, "2026-01-01", "2026-06-30");

    EXPECT_EQ(calc.result_snapshot.at("income_tiyn").get<long long>(), 20000000LL);  // 200,000 ₸ only
    EXPECT_EQ(calc.total_tiyn, 800000LL);                                            // 4% of 200,000 ₸
}

// Fix round 1: pins the existing `status = 'posted'` filter's behaviour
// against a storno, not just an untouched draft (SnrIgnoresDraftEntries
// above). After Ledger::JournalService::reverse(): the original entry
// flips to 'reversed' (excluded by the filter directly) and its mirror
// entry is posted but with sides FLIPPED (a DEBIT line on 6010, not
// credit) — so it doesn't smuggle the income back in from the other side
// either. Both the SNR income base and the VAT accrued/deductible bases
// must see exactly zero once the whole pair has landed.
TEST_F(TaxServiceTest, ReversedEntriesExcludedFromBase) {
    Ledger::JournalService journal_svc;
    Tax::TaxService tax_svc;
    auto org_id = make_org("111280000009");
    auto user_id = seed_user("reversed1@example.com");

    auto entry =
        journal_svc.create_draft(org_id,
                                 user_id,
                                 "2026-02-01",
                                 "Income later reversed",
                                 {line("1030", "debit", "1120.00"), line("6010", "credit", "1120.00", "120.00")});
    ASSERT_TRUE(journal_svc.post(org_id, entry.id));
    ASSERT_TRUE(journal_svc.reverse(org_id, entry.id, user_id));

    auto snr = tax_svc.calculate_snr(org_id, "2026-01-01", "2026-06-30");
    EXPECT_EQ(snr.result_snapshot.at("income_tiyn").get<long long>(), 0LL);
    EXPECT_EQ(snr.total_tiyn, 0LL);

    auto vat = tax_svc.calculate_vat(org_id, "2026-01-01", "2026-03-31");
    EXPECT_EQ(vat.result_snapshot.at("accrued_tiyn").get<long long>(), 0LL);
    EXPECT_EQ(vat.result_snapshot.at("deductible_tiyn").get<long long>(), 0LL);
    EXPECT_EQ(vat.total_tiyn, 0LL);
}

TEST_F(TaxServiceTest, VatBalanceFromLineVatAmounts) {
    Ledger::JournalService journal_svc;
    Tax::TaxService tax_svc;
    auto org_id = make_org("111280000003");
    auto user_id = seed_user("vat1@example.com");

    // Q1 2026: НДС начислено (120.00 ₸, credit 6010) vs НДС к зачёту
    // (60.00 ₸, debit 7210) — net positive balance, к уплате.
    auto sale =
        journal_svc.create_draft(org_id,
                                 user_id,
                                 "2026-02-10",
                                 "Sale with VAT",
                                 {line("1030", "debit", "1120.00"), line("6010", "credit", "1120.00", "120.00")});
    journal_svc.post(org_id, sale.id);

    auto expense =
        journal_svc.create_draft(org_id,
                                 user_id,
                                 "2026-02-15",
                                 "Admin expense with input VAT",
                                 {line("7210", "debit", "560.00", "60.00"), line("1030", "credit", "560.00")});
    journal_svc.post(org_id, expense.id);

    auto q1 = tax_svc.calculate_vat(org_id, "2026-01-01", "2026-03-31");
    EXPECT_EQ(q1.result_snapshot.at("accrued_tiyn").get<long long>(), 12000LL);    // 120.00 ₸
    EXPECT_EQ(q1.result_snapshot.at("deductible_tiyn").get<long long>(), 6000LL);  // 60.00 ₸
    EXPECT_EQ(q1.result_snapshot.at("balance_tiyn").get<long long>(), 6000LL);
    EXPECT_EQ(q1.total_tiyn, 6000LL);

    // Q2 2026: only a purchase with input VAT, no sales at all — accrued=0,
    // deductible>0, so the balance must come out NEGATIVE (к возврату) and
    // be stored as such (BIGINT total_tiyn is signed, no clamping anywhere
    // in TaxService/TaxCalculationRepository).
    auto purchase =
        journal_svc.create_draft(org_id,
                                 user_id,
                                 "2026-04-05",
                                 "Inventory purchase with input VAT",
                                 {line("1330", "debit", "224.00", "24.00"), line("1030", "credit", "224.00")});
    journal_svc.post(org_id, purchase.id);

    auto q2 = tax_svc.calculate_vat(org_id, "2026-04-01", "2026-06-30");
    EXPECT_EQ(q2.result_snapshot.at("accrued_tiyn").get<long long>(), 0LL);
    EXPECT_EQ(q2.result_snapshot.at("deductible_tiyn").get<long long>(), 2400LL);  // 24.00 ₸
    EXPECT_EQ(q2.result_snapshot.at("balance_tiyn").get<long long>(), -2400LL);
    EXPECT_EQ(q2.total_tiyn, -2400LL);
}

TEST_F(TaxServiceTest, SnapshotsStored) {
    Ledger::JournalService journal_svc;
    Tax::TaxService tax_svc;
    auto org_id = make_org("111280000004");
    auto user_id = seed_user("snap1@example.com");

    post_income(journal_svc, org_id, user_id, "2026-02-01", "100000.00");

    auto calc = tax_svc.calculate_snr(org_id, "2026-01-01", "2026-06-30");

    ASSERT_TRUE(calc.input_snapshot.contains("rate_bp"));
    EXPECT_EQ(calc.input_snapshot.at("rate_bp").get<long long>(), 400);
    ASSERT_TRUE(calc.input_snapshot.contains("period_from"));
    ASSERT_TRUE(calc.input_snapshot.contains("period_to"));
    EXPECT_EQ(calc.input_snapshot.at("period_from").get<std::string>(), "2026-01-01");
    EXPECT_EQ(calc.input_snapshot.at("period_to").get<std::string>(), "2026-06-30");

    // Persisted, not just returned in-memory — a fresh repository read sees
    // the same snapshot shape.
    Tax::TaxCalculationRepository calc_repo;
    auto reloaded = calc_repo.find_in_org(calc.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(reloaded->input_snapshot.at("rate_bp").get<long long>(), 400);
}

TEST_F(TaxServiceTest, RecalculateOverwrites) {
    Ledger::JournalService journal_svc;
    Tax::TaxService tax_svc;
    Tax::TaxCalculationRepository calc_repo;
    auto org_id = make_org("111280000005");
    auto user_id = seed_user("recalc1@example.com");

    post_income(journal_svc, org_id, user_id, "2026-02-01", "100000.00");
    auto first = tax_svc.calculate_snr(org_id, "2026-01-01", "2026-06-30");
    EXPECT_EQ(first.total_tiyn, 400000LL);  // 4% of 100,000 ₸
    EXPECT_EQ(calc_repo.count_in_org(org_id), 1);

    // A late correction adds more income to the SAME period — recalculating
    // must UPDATE the existing row (same id, new numbers), not insert a
    // second one.
    post_income(journal_svc, org_id, user_id, "2026-03-01", "50000.00");
    auto second = tax_svc.calculate_snr(org_id, "2026-01-01", "2026-06-30");

    EXPECT_EQ(second.id, first.id);
    EXPECT_EQ(second.total_tiyn, 600000LL);  // 4% of 150,000 ₸
    EXPECT_EQ(calc_repo.count_in_org(org_id), 1);

    auto reloaded = calc_repo.find_in_org(first.id, org_id, /*from_primary=*/true);
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(reloaded->total_tiyn, 600000LL);
}

TEST_F(TaxServiceTest, ThresholdAlertFiresNearVatLimit) {
    Ledger::JournalService journal_svc;
    Tax::TaxService tax_svc;
    auto org_id = make_org("111280000006");
    auto user_id = seed_user("alert1@example.com");

    // 40,000,000 ₸ trailing-12-month income: above the 90% early-warning
    // margin (38,925,000 ₸) but still below the full 43,250,000 ₸ threshold
    // — exercises the "near", not-yet-over case the test name calls out.
    // Nowhere close to the СНР income limit (≈2.595 billion ₸), so only the
    // vat_registration alert should fire.
    post_income(journal_svc, org_id, user_id, "2026-03-01", "40000000.00");

    auto alerts = tax_svc.threshold_alerts(org_id, "2026-06-30");

    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_EQ(alerts[0].kind, "vat_registration");
    EXPECT_EQ(alerts[0].current_tiyn, 4000000000LL);
    EXPECT_EQ(alerts[0].threshold_tiyn, 4325000000LL);
    EXPECT_FALSE(alerts[0].message.empty());
}

TEST_F(TaxServiceTest, CrossOrgIsolated) {
    Ledger::JournalService journal_svc;
    Tax::TaxService tax_svc;
    Tax::TaxCalculationRepository calc_repo;
    auto org_a = make_org("111280000007");
    auto org_b = make_org("111280000008");
    auto user_a = seed_user("iso-a@example.com");
    auto user_b = seed_user("iso-b@example.com");

    post_income(journal_svc, org_a, user_a, "2026-02-01", "100000.00");
    post_income(journal_svc, org_b, user_b, "2026-02-01", "500000.00");

    auto calc_a = tax_svc.calculate_snr(org_a, "2026-01-01", "2026-06-30");
    auto calc_b = tax_svc.calculate_snr(org_b, "2026-01-01", "2026-06-30");

    EXPECT_EQ(calc_a.total_tiyn, 400000LL);   // 4% of 100,000 ₸
    EXPECT_EQ(calc_b.total_tiyn, 2000000LL);  // 4% of 500,000 ₸

    // Org B never sees org A's calculation, and vice versa.
    EXPECT_FALSE(calc_repo.find_in_org(calc_a.id, org_b, /*from_primary=*/true));
    EXPECT_FALSE(calc_repo.find_in_org(calc_b.id, org_a, /*from_primary=*/true));
    EXPECT_EQ(calc_repo.count_in_org(org_a), 1);
    EXPECT_EQ(calc_repo.count_in_org(org_b), 1);

    auto listed_a = calc_repo.list_in_org(org_a);
    ASSERT_EQ(listed_a.size(), 1u);
    EXPECT_EQ(listed_a[0].id, calc_a.id);
}

}  // namespace
