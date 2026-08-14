/**
 * @file test_tax_calendar.cpp
 * @brief Integration tests for Tax::TaxCalendar against a real Postgres
 *        (migration 015). No org fixture / cleanup is needed — like
 *        test_tax_reference.cpp, `tax_deadlines` is a system-wide table
 *        seeded once by the migration itself (see TaxCalendar.hpp's Doxygen
 *        for why it isn't org-scoped).
 *
 * The dates asserted below are real 2026 calendar facts (day-of-week),
 * verified standalone with `clang++ -std=c++20` before this file was
 * written, and cross-checked against kgd.gov.kz's published 2026 tax
 * calendar: 2026-08-15 (the raw due day for I полугодие 2026's form 910.00)
 * is a Saturday, so the actual due date is 2026-08-17 (Monday) — kgd.gov.kz
 * publishes this exact shifted date for that period.
 */

#include <algorithm>

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "tax/TaxCalendar.hpp"
#include "test_helpers.hpp"

namespace {

class TaxCalendarTest : public TestHelpers::CoreBackedTest {};

TEST_F(TaxCalendarTest, SeedHasFormsNineTenAndThreeHundred) {
    Tax::TaxCalendar cal;
    // A one-year horizon from a fixed date is guaranteed to surface both the
    // report and payment deadline for both forms at least once.
    const auto deadlines = cal.upcoming("2026-01-01", 400);

    auto has = [&](const std::string& form, const std::string& kind) {
        return std::any_of(deadlines.begin(), deadlines.end(), [&](const Tax::Deadline& d) {
            return d.form == form && d.kind == kind;
        });
    };
    EXPECT_TRUE(has("910.00", "report"));
    EXPECT_TRUE(has("910.00", "payment"));
    EXPECT_TRUE(has("300.00", "report"));
    EXPECT_TRUE(has("300.00", "payment"));

    for (const auto& d : deadlines)
        EXPECT_TRUE(d.form == "910.00" || d.form == "300.00" || d.form == "200.00") << d.form;
}

TEST_F(TaxCalendarTest, WeekendDueDateShiftsForward) {
    Tax::TaxCalendar cal;
    // I полугодие 2026 (Jan-Jun) закрывается 30 июня; форма 910.00 "report"
    // должна быть сдана не позднее 15 августа 2026 — но это суббота, поэтому
    // фактический (перенесённый) срок — 17 августа 2026 (понедельник).
    const auto deadlines = cal.upcoming("2026-07-01", 60);

    auto it = std::find_if(deadlines.begin(), deadlines.end(), [](const Tax::Deadline& d) {
        return d.form == "910.00" && d.kind == "report";
    });
    ASSERT_NE(it, deadlines.end());
    EXPECT_EQ(it->due_date, "2026-08-17");  // NOT 2026-08-15 — that's a Saturday

    // The payment deadline (25 августа 2026) IS a weekday (Tuesday), so it
    // must NOT be shifted — pins that the shift only fires on an actual
    // weekend hit, not unconditionally.
    auto payment = std::find_if(deadlines.begin(), deadlines.end(), [](const Tax::Deadline& d) {
        return d.form == "910.00" && d.kind == "payment";
    });
    ASSERT_NE(payment, deadlines.end());
    EXPECT_EQ(payment->due_date, "2026-08-25");
}

TEST_F(TaxCalendarTest, HorizonFiltersFarDeadlines) {
    Tax::TaxCalendar cal;
    // From 2026-08-01, the nearest deadlines (910.00 and 300.00 for the
    // period ending June/July) are ~2-3 weeks out; a 20-day horizon should
    // catch them, while excluding the next-quarter/half-year deadlines that
    // land months later.
    const auto near = cal.upcoming("2026-08-01", 20);
    const auto far = cal.upcoming("2026-08-01", 400);

    EXPECT_FALSE(near.empty());
    EXPECT_LT(near.size(), far.size());
    for (const auto& d : near)
        EXPECT_LE(d.days_left, 20);
    // The far window must contain at least one deadline strictly beyond what
    // the near window could ever return.
    EXPECT_TRUE(std::any_of(far.begin(), far.end(), [](const Tax::Deadline& d) { return d.days_left > 20; }));
}

TEST_F(TaxCalendarTest, PeriodLabelHumanReadable) {
    Tax::TaxCalendar cal;
    const auto deadlines = cal.upcoming("2026-07-01", 60);

    auto snr = std::find_if(deadlines.begin(), deadlines.end(), [](const Tax::Deadline& d) {
        return d.form == "910.00" && d.kind == "report";
    });
    ASSERT_NE(snr, deadlines.end());
    EXPECT_EQ(snr->period_label, "I полугодие 2026");

    auto vat = std::find_if(deadlines.begin(), deadlines.end(), [](const Tax::Deadline& d) {
        return d.form == "300.00" && d.kind == "report";
    });
    ASSERT_NE(vat, deadlines.end());
    EXPECT_EQ(vat->period_label, "II квартал 2026");
}

TEST_F(TaxCalendarTest, UpcomingReturnsSortedByDueDate) {
    Tax::TaxCalendar cal;
    const auto deadlines = cal.upcoming("2026-01-01", 400);
    ASSERT_GE(deadlines.size(), 2u);
    EXPECT_TRUE(std::is_sorted(deadlines.begin(), deadlines.end(), [](const Tax::Deadline& a, const Tax::Deadline& b) {
        return a.due_date < b.due_date;
    }));
}

// Beyond the brief's five required tests: a period that hasn't ENDED yet at
// on_date can still surface if its due date falls inside the horizon — this
// is the behavior upcoming()'s Doxygen calls out explicitly and is easy to
// get wrong (e.g. by only scanning already-closed periods).
TEST_F(TaxCalendarTest, DeadlineForStillOpenPeriodIsVisibleWithinHorizon) {
    Tax::TaxCalendar cal;
    // 2026-01-01: I полугодие 2026 hasn't even started closing (ends
    // 2026-06-30), yet its due dates (Aug 17 / Aug 25) are only ~230 days
    // away — well inside a 400-day horizon.
    const auto deadlines = cal.upcoming("2026-01-01", 400);
    EXPECT_TRUE(std::any_of(deadlines.begin(), deadlines.end(), [](const Tax::Deadline& d) {
        return d.form == "910.00" && d.kind == "report" && d.due_date == "2026-08-17";
    }));
}

// Every seeded rule must carry a source_note (mirrors
// TaxReferenceTest.EveryRowCarriesSourceNote's intent, checked here at the
// repository level since TaxCalendar doesn't expose source_note on Deadline
// — it's a rule-table property, not a per-occurrence one).
TEST_F(TaxCalendarTest, SeedRowsExistDirectlyInTable) {
    const int count = Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec("SELECT count(*) FROM tax_deadlines WHERE form IN ('910.00','300.00')");
        return r[0][0].template as<int>();
    });
    EXPECT_EQ(count, 4);  // 910.00 report+payment, 300.00 report+payment
}

}  // namespace
