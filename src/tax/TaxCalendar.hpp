/**
 * @file TaxCalendar.hpp
 * @brief Turns the `tax_deadlines` rule table (migrations/015_tax_deadlines.sql
 *        — design spec §7.1, Task 6) into concrete upcoming due dates.
 *
 * `tax_deadlines` is a SYSTEM table — no `org_id` — the THIRD documented
 * exception to "every domain table is org-scoped" (first: `accounts`,
 * src/ledger/AccountRepository.hpp; second: `tax_rates`/`tax_constants`,
 * src/tax/TaxReferenceRepository.hpp). A due-date RULE (form/kind/period_kind/
 * due_day/due_month_offset) is a fact about Kazakhstani tax law, identical for
 * every tenant; see the migration's header comment for the full source list
 * (adilet.zan.kz, kgd.gov.kz) and how the Step-1 official-source reconciliation
 * resolved the periodicity contradiction for form 910.00 ("ежемесячно" vs
 * "полугодие" — resolved to полугодие per НК РК ст.722/ст.727) and pinned the
 * form 300.00 (НДС) report-vs-payment day split (15th vs 25th of the SAME
 * second month after the quarter, per ст.505/ст.506).
 *
 * KNOWN LIMITATION (documented per the P2 task brief, not a bug): the
 * weekend-shift rule below moves a due date landing on Saturday/Sunday to the
 * next business day, but Kazakhstani PUBLIC HOLIDAYS are out of scope for P2
 * — there is no holiday calendar here, so a due date that falls on a holiday
 * (but not a weekend) is reported un-shifted. A future phase should add a
 * `kz_public_holidays` reference table and consult it in shift_weekend_forward().
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"

namespace Tax {

/// One concrete, dated occurrence of a `tax_deadlines` rule — what
/// `TaxCalendar::upcoming()` returns. `form`/`kind` mirror the rule's own
/// CHECK-constrained columns (e.g. "910.00"/"report"); `period_label` is a
/// human-readable Russian label for the reporting period the deadline covers
/// (e.g. "I полугодие 2026", "II квартал 2026"); `due_date` is
/// weekend-shifted (see the file-level Doxygen for the holiday caveat);
/// `days_left` is `due_date - on_date` in whole days (negative would mean
/// "already overdue", but upcoming() never returns those — see below).
struct Deadline {
    std::string form;
    std::string kind;
    std::string period_label;
    std::string due_date;  // ISO "YYYY-MM-DD", already weekend-shifted
    int days_left = 0;
};

inline void to_json(nlohmann::json& j, const Deadline& d) {
    j = nlohmann::json{
        {"form", d.form},
        {"kind", d.kind},
        {"period_label", d.period_label},
        {"due_date", d.due_date},
        {"days_left", d.days_left},
    };
}

/**
 * @brief Reads `tax_deadlines` rules and expands them into concrete
 *        `Deadline`s falling in `[on_date, on_date + horizon_days]`.
 *
 * Does NOT extend Tenancy::OrgCrudBase — same reasoning as
 * TaxReferenceRepository: the table has no org_id to filter by, because tax
 * deadline RULES are facts about Kazakhstani law, not per-tenant data. Every
 * read here takes an effective date instead of an org_id, exactly like
 * TaxReferenceRepository::rate_on/constant_on.
 */
class TaxCalendar {
public:
    static constexpr const char* kColumns =
        "form, kind, period_kind, due_day, due_month_offset, effective_from, source_note";

    /**
     * @brief Every deadline whose (weekend-shifted) due date falls in
     *        `[on_date, on_date + horizon_days]`, sorted by due_date
     *        ascending (ties broken by form then kind, via a stable sort
     *        over the DB's own `ORDER BY form, kind` rule order).
     *
     * @param on_date ISO "YYYY-MM-DD". Deadlines strictly before this date
     *        are never returned, even if their period hasn't been queried
     *        before — this is an "upcoming", not a "history", view.
     * @param horizon_days How far forward to look. A period that hasn't
     *        ENDED yet by @p on_date can still surface here: its due date
     *        (period end + due_month_offset months) is computable in advance
     *        and may already fall inside the horizon (e.g. on 2026-01-01
     *        with a 400-day horizon, I полугодие 2026's 15/25 August due
     *        dates already show up, months before the half-year itself ends).
     */
    std::vector<Deadline> upcoming(const std::string& on_date, int horizon_days) {
        const auto rules = load_rules(on_date);
        const std::chrono::year_month_day on = parse_date(on_date);
        const std::chrono::year_month_day horizon_end{std::chrono::sys_days{on} + std::chrono::days{horizon_days}};

        std::vector<Deadline> out;
        for (const auto& r : rules)
            expand_rule(r, on, horizon_end, out);

        std::stable_sort(
            out.begin(), out.end(), [](const Deadline& a, const Deadline& b) { return a.due_date < b.due_date; });
        return out;
    }

private:
    /// One row of `tax_deadlines` — a RULE, not yet a dated Deadline.
    struct Rule {
        std::string form;
        std::string kind;
        std::string period_kind;
        int due_day = 0;
        int due_month_offset = 0;
        std::string effective_from;
        std::string source_note;
    };

    /// The rule in force on @p on_date for each (form, kind) — mirrors
    /// TaxReferenceRepository::list_rates_on's DISTINCT ON idiom, so a future
    /// re-seed (a new law changing a due day) can add a new effective_from
    /// row without breaking queries for dates still governed by the old one.
    std::vector<Rule> load_rules(const std::string& on_date) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT DISTINCT ON (form, kind) " + std::string(kColumns) +
                                         " FROM tax_deadlines"
                                         " WHERE effective_from <= $1::date"
                                         " ORDER BY form, kind, effective_from DESC",
                                     on_date);
            std::vector<Rule> out;
            out.reserve(r.size());
            for (const auto& row : r) {
                Rule rule;
                rule.form = row["form"].template as<std::string>();
                rule.kind = row["kind"].template as<std::string>();
                rule.period_kind = row["period_kind"].template as<std::string>();
                rule.due_day = row["due_day"].template as<int>();
                rule.due_month_offset = row["due_month_offset"].template as<int>();
                rule.effective_from = row["effective_from"].template as<std::string>();
                rule.source_note = row["source_note"].template as<std::string>();
                out.push_back(std::move(rule));
            }
            return out;
        });
    }

    // ---- Pure date arithmetic below — no DB, no locale, all std::chrono ----
    // (Verified standalone with `clang++ -std=c++20` against real 2026
    // calendar facts before being wired up here — see the task report: e.g.
    // 2026-08-15 IS a Saturday, so the I полугодие 2026 due dates shift to
    // 2026-08-17, matching kgd.gov.kz's published 2026 tax calendar.)

    static std::chrono::year_month_day parse_date(const std::string& s) {
        using namespace std::chrono;
        const int y = std::stoi(s.substr(0, 4));
        const unsigned m = static_cast<unsigned>(std::stoi(s.substr(5, 2)));
        const unsigned d = static_cast<unsigned>(std::stoi(s.substr(8, 2)));
        return year_month_day{year{y}, month{m}, day{d}};
    }

    static std::string format_date(const std::chrono::year_month_day& ymd) {
        char buf[11];
        std::snprintf(buf,
                      sizeof(buf),
                      "%04d-%02u-%02u",
                      static_cast<int>(ymd.year()),
                      static_cast<unsigned>(ymd.month()),
                      static_cast<unsigned>(ymd.day()));
        return buf;
    }

    static std::chrono::year_month_day last_day_of(std::chrono::year_month ym) {
        using namespace std::chrono;
        return year_month_day{year_month_day_last{ym.year(), month_day_last{ym.month()}}};
    }

    static long long days_between(const std::chrono::year_month_day& a, const std::chrono::year_month_day& b) {
        return (std::chrono::sys_days{b} - std::chrono::sys_days{a}).count();
    }

    /// How many calendar months a `period_kind` spans — the step used to walk
    /// from one period to the next/previous one.
    static int period_length_months(const std::string& period_kind) {
        if (period_kind == "month")
            return 1;
        if (period_kind == "quarter")
            return 3;
        if (period_kind == "half_year")
            return 6;
        return 12;  // "year"
    }

    /// The (year, month) of the END of the period containing @p on_date, for
    /// the given `period_kind` (e.g. on_date=2026-08-14, period_kind=quarter
    /// -> 2026-09, because August falls in the Jul-Sep quarter).
    static std::chrono::year_month current_period_end_month(const std::chrono::year_month_day& on_date,
                                                            const std::string& period_kind) {
        using namespace std::chrono;
        const unsigned m = static_cast<unsigned>(on_date.month());
        if (period_kind == "month")
            return year_month{on_date.year(), on_date.month()};
        if (period_kind == "quarter") {
            const unsigned end_m = ((m - 1) / 3 + 1) * 3;
            return year_month{on_date.year(), month{end_m}};
        }
        if (period_kind == "half_year")
            return year_month{on_date.year(), month{m <= 6 ? 6u : 12u}};
        return year_month{on_date.year(), month{12}};  // "year"
    }

    /// Saturday/Sunday -> next Monday (see the file-level Doxygen: КZ public
    /// holidays are explicitly out of scope for P2 and are NOT consulted
    /// here).
    static std::chrono::year_month_day shift_weekend_forward(std::chrono::year_month_day d) {
        using namespace std::chrono;
        sys_days sd{d};
        weekday wd{sd};
        while (wd == Saturday || wd == Sunday) {
            sd += days{1};
            wd = weekday{sd};
        }
        return year_month_day{sd};
    }

    /// "I полугодие 2026" / "II квартал 2026" / "2026 год" / "Август 2026" —
    /// human-readable Russian label for the period ENDING at @p end_ym.
    static std::string period_label(const std::chrono::year_month& end_ym, const std::string& period_kind) {
        const int y = static_cast<int>(end_ym.year());
        const unsigned m = static_cast<unsigned>(end_ym.month());
        if (period_kind == "half_year")
            return (m == 6 ? std::string("I полугодие ") : std::string("II полугодие ")) + std::to_string(y);
        if (period_kind == "quarter") {
            static const char* kRoman[] = {"I", "II", "III", "IV"};
            const unsigned q = m / 3;  // 3,6,9,12 -> 1,2,3,4
            return std::string(kRoman[q - 1]) + " квартал " + std::to_string(y);
        }
        if (period_kind == "year")
            return std::to_string(y) + " год";
        static const char* kMonthsRu[] = {"",
                                          "Январь",
                                          "Февраль",
                                          "Март",
                                          "Апрель",
                                          "Май",
                                          "Июнь",
                                          "Июль",
                                          "Август",
                                          "Сентябрь",
                                          "Октябрь",
                                          "Ноябрь",
                                          "Декабрь"};
        return std::string(kMonthsRu[m]) + " " + std::to_string(y);
    }

    /// Expands one rule into every Deadline whose due date lands in
    /// `[on_date, horizon_end]`, walking BOTH directions from the period
    /// containing `on_date`: forward (later periods — needed because a due
    /// date is knowable before its period even ends, see upcoming()'s
    /// Doxygen) until the due date exceeds `horizon_end`, and backward
    /// (earlier, already-ended periods whose due date may still be ahead of
    /// `on_date`) until the due date drops below `on_date` or the rule's
    /// `effective_from` floor is crossed. Due dates are monotonic in the
    /// walk direction, so both loops can stop as soon as they leave the
    /// window — no unbounded scanning.
    static void expand_rule(const Rule& r,
                            const std::chrono::year_month_day& on_date,
                            const std::chrono::year_month_day& horizon_end,
                            std::vector<Deadline>& out) {
        using namespace std::chrono;
        const year_month_day effective_from = parse_date(r.effective_from);
        const int len = period_length_months(r.period_kind);
        const year_month current_end_ym = current_period_end_month(on_date, r.period_kind);

        auto due_for = [&](year_month end_ym) {
            const year_month due_ym = end_ym + months{r.due_month_offset};
            const year_month_day raw{due_ym.year(), due_ym.month(), day{static_cast<unsigned>(r.due_day)}};
            return shift_weekend_forward(raw);
        };
        auto make_deadline = [&](year_month end_ym, year_month_day due) {
            Deadline d;
            d.form = r.form;
            d.kind = r.kind;
            d.period_label = period_label(end_ym, r.period_kind);
            d.due_date = format_date(due);
            d.days_left = static_cast<int>(days_between(on_date, due));
            return d;
        };

        // Current period and every later one, while the due date still fits.
        // The horizon check comes BEFORE the effective_from check so the loop
        // always terminates in bounded steps regardless of how far in the
        // future effective_from might be (load_rules() already guarantees
        // effective_from <= on_date <= this period's end for every rule
        // reaching this function, so the effective_from branch below is
        // unreachable today — kept as a defensive guard against that
        // invariant changing later, not because it currently fires).
        for (int i = 0;; ++i) {
            const year_month end_ym = current_end_ym + months{len * i};
            const year_month_day due = due_for(end_ym);
            if (due > horizon_end)
                break;
            if (last_day_of(end_ym) < effective_from)
                continue;
            if (due >= on_date)
                out.push_back(make_deadline(end_ym, due));
        }
        // Earlier periods whose due date might still be >= on_date.
        for (int i = -1;; --i) {
            const year_month end_ym = current_end_ym + months{len * i};
            if (last_day_of(end_ym) < effective_from)
                break;  // walking further back only predates the rule further
            const year_month_day due = due_for(end_ym);
            if (due < on_date)
                break;
            if (due <= horizon_end)
                out.push_back(make_deadline(end_ym, due));
        }
    }
};

}  // namespace Tax
