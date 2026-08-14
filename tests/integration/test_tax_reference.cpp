/**
 * @file test_tax_reference.cpp
 * @brief Integration tests for Tax::TaxReferenceRepository against a real
 *        Postgres (migration 011). No org fixture / cleanup is needed —
 *        unlike every other integration suite, this reference table is
 *        system-wide and seeded once by the migration itself (see
 *        TaxReferenceRepository.hpp's Doxygen for why it isn't org-scoped).
 */

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "tax/TaxReferenceRepository.hpp"
#include "test_helpers.hpp"

namespace {

class TaxReferenceTest : public TestHelpers::CoreBackedTest {};

TEST_F(TaxReferenceTest, SeedHasVatAndSnrForTwentyTwentySix) {
    Tax::TaxReferenceRepository repo;
    auto vat = repo.rate_on("vat", "2026-06-01", "");
    ASSERT_TRUE(vat);
    EXPECT_EQ(vat->rate_bp, 1600);
    auto snr = repo.rate_on("snr_simplified", "2026-06-01", "");
    ASSERT_TRUE(snr);
    EXPECT_EQ(snr->rate_bp, 400);
}

TEST_F(TaxReferenceTest, ConstantsResolveByDate) {
    Tax::TaxReferenceRepository repo;
    auto mrp = repo.constant_on("mrp", "2026-06-01");
    ASSERT_TRUE(mrp);
    EXPECT_EQ(mrp->value_tiyn, 432500);  // 4325 ₸ в тиынах
    EXPECT_FALSE(repo.constant_on("mrp", "2019-01-01").has_value());
}

TEST_F(TaxReferenceTest, PayrollRatesPresent) {
    Tax::TaxReferenceRepository repo;
    for (const auto* kind : {"ipn", "opv", "opvr", "so", "osms", "vosms", "social_tax"})
        EXPECT_TRUE(repo.rate_on(kind, "2026-06-01", "").has_value()) << kind;
}

TEST_F(TaxReferenceTest, EveryRowCarriesSourceNote) {
    Tax::TaxReferenceRepository repo;
    for (const auto& r : repo.list_rates_on("2026-06-01"))
        EXPECT_FALSE(r.source_note.empty());
    for (const auto& c : repo.list_constants_on("2026-06-01"))
        EXPECT_FALSE(c.source_note.empty());
}

// Beyond the brief's four required tests: the two Step-1 contradictions that
// most directly threatened downstream payroll math (ОПВ base limit, and the
// НДС threshold being МРП-denominated rather than a flat 40М ₸) are pinned
// here so a future re-seed can't silently regress them.
TEST_F(TaxReferenceTest, OpvBaseMaxIsFiftyMzpNotOneToSevenCorridor) {
    Tax::TaxReferenceRepository repo;
    auto opv_max = repo.constant_on("opv_base_max_mzp", "2026-06-01");
    ASSERT_TRUE(opv_max);
    ASSERT_TRUE(opv_max->value_units);
    EXPECT_EQ(*opv_max->value_units, 50);

    auto so_min = repo.constant_on("so_base_min_mzp", "2026-06-01");
    auto so_max = repo.constant_on("so_base_max_mzp", "2026-06-01");
    ASSERT_TRUE(so_min);
    ASSERT_TRUE(so_max);
    ASSERT_TRUE(so_min->value_units);
    ASSERT_TRUE(so_max->value_units);
    EXPECT_EQ(*so_min->value_units, 1);
    EXPECT_EQ(*so_max->value_units, 7);
}

TEST_F(TaxReferenceTest, VatThresholdIsMrpDenominatedFortyThreeMillionTwoFifty) {
    Tax::TaxReferenceRepository repo;
    auto threshold = repo.constant_on("vat_threshold_tenge", "2026-06-01");
    ASSERT_TRUE(threshold);
    ASSERT_TRUE(threshold->value_units);
    EXPECT_EQ(*threshold->value_units, 10000);       // 10 000 МРП, НК РК ст.99 п.4 пп.2
    EXPECT_EQ(threshold->value_tiyn, 4325000000LL);  // 43 250 000 ₸ на 2026 год, не 40 000 000 ₸
}

}  // namespace
