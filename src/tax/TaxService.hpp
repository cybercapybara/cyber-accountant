/**
 * @file TaxService.hpp
 * @brief Business logic for the two P2 tax calculations — СНР на основе
 *        упрощённой декларации and НДС — plus registration-threshold
 *        monitoring (design spec §7.2, phase P2 task-5-brief.md).
 *
 * Every money figure this file touches is an integer TIYN and every rate is
 * integer BASIS POINTS (see src/tax/TaxRate.hpp) — no floating point
 * anywhere in the arithmetic. The one place floating point could sneak in is
 * reading `journal_lines.amount` / `journal_lines.vat_amount`
 * (NUMERIC(18,2), stored as decimal text — see Ledger::JournalLine's doc
 * comment): this file never fetches those columns as text and parses them
 * in C++ the way Ledger::parse_tiyn does. Instead every aggregate query
 * below does `SUM(ROUND(<column> * 100))::bigint` INSIDE Postgres — Postgres
 * NUMERIC arithmetic is exact decimal, not IEEE float, so the multiply-by-100
 * and the SUM both stay exact, and only the final, already-integral result
 * crosses into C++ as a `long long`.
 *
 * Consumes Tax::TaxReferenceRepository (rates/constants) directly. It does
 * NOT go through Ledger::JournalRepository for the account-turnover
 * aggregates calculate_snr/calculate_vat/threshold_alerts need — that
 * repository is deliberately read-only over whole entries/lines (see its
 * file header) and offers no "sum of credit lines for these account codes
 * over this date range" query, and adding one there would be scope creep
 * onto a file this task does not own. Every aggregate here is its own
 * `Database::get().execute_read` call directly against `journal_lines` /
 * `journal_entries`, filtered to `status = 'posted'` — the same posture
 * Ledger::JournalService itself takes for bespoke SQL that doesn't fit an
 * existing repository method.
 *
 * Account sets (§7.2, cross-checked against migrations/008_accounts.sql's
 * system chart):
 *   - "income" (both СНР's revenue base and НДС's accrual base) is the
 *     6000-group revenue accounts {6010, 6110, 6250, 6290} — sales revenue,
 *     interest income, FX-gain income, other income. This is the SAME set
 *     for both calculations because the brief defines СНР's revenue base as
 *     "credit turnover of 6000-group accounts" and НДS's accrual base as
 *     "vat_amount on credit lines of 6000-group accounts" — one account set,
 *     two different columns summed off it (amount for СНР, vat_amount for
 *     НДС).
 *   - "НДС к зачёту" (input VAT credit) is debit lines on {1330, 7010, 7210}
 *     — inventory (Товары), cost of sales, and administrative expense —
 *     the three system accounts the brief names explicitly.
 *
 * Reproducibility (§7.2): both calculate_* methods persist an
 * `input_snapshot` (every rate/constant/account-set/period-bound actually
 * used) and a `result_snapshot` (every intermediate and final figure
 * produced) via Tax::TaxCalculationRepository::upsert — re-running the same
 * period overwrites the one row for that (org, kind, period) rather than
 * accumulating a duplicate, so "recalculate after a late journal
 * correction" is idempotent per period by construction.
 *
 * СНР ipn_part_tiyn / social_tax_part_tiyn (deliberate omission): the brief
 * asks for a legal split of the 4% СНР tax into an ИПН part and a social-tax
 * part in `result_snapshot`, contingent on Task 1 Step 1 confirming the
 * proportion. It did not: migrations/011_tax_reference.sql's `social_tax`
 * seed row states plainly that "при СНР на основе упрощённой декларации
 * социальный налог с 2026 года не уплачивается вовсе" (social tax is not
 * levied AT ALL under СНР на основе упрощённой декларации) — there is no
 * current legal split to encode, and inventing a 50/50 (or any other) ratio
 * from a pre-2026 practice that Task 1 specifically found superseded would
 * misrepresent the law rather than reproduce it. Both fields are therefore
 * OMITTED from result_snapshot rather than populated with a fabricated
 * split or a pair of zeros that would misleadingly imply a real allocation
 * happened.
 */

#pragma once

#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "tax/TaxCalculation.hpp"
#include "tax/TaxCalculationRepository.hpp"
#include "tax/TaxRate.hpp"
#include "tax/TaxReferenceRepository.hpp"

namespace Tax {

/// Thrown by calculate_snr/calculate_vat/threshold_alerts when a rate or
/// constant the calculation needs is not in force on the queried date (e.g.
/// asking for a period before migrations/011_tax_reference.sql's
/// 2026-01-01 seed). A calculation cannot proceed without its inputs, so
/// this is a hard failure rather than a silently-zeroed figure.
struct MissingTaxReference : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// One registration-threshold warning. `kind` is one of "vat_registration"
/// (12-month income approaching/over `vat_threshold_tenge`) or "snr_limit"
/// (12-month income approaching/over `snr_income_limit_mrp * mrp`).
struct Alert {
    std::string kind;
    std::string message;
    long long current_tiyn = 0;
    long long threshold_tiyn = 0;
};

inline void to_json(nlohmann::json& j, const Alert& a) {
    j = nlohmann::json{
        {"kind", a.kind},
        {"message", a.message},
        {"current_tiyn", a.current_tiyn},
        {"threshold_tiyn", a.threshold_tiyn},
    };
}

class TaxService {
public:
    /**
     * @brief СНР на основе упрощённой декларации for one half-year.
     *
     * income = credit turnover of the 6000-group revenue accounts (see file
     * header) over posted entries in [half_year_start, half_year_end];
     * tax = apply_bp(income, snr_simplified rate in force on
     * half_year_end — a return is filed after the period closes, so the
     * rate governing the whole period is the one in force at its end).
     *
     * @throws MissingTaxReference if no `snr_simplified` rate is in force on
     *         @p half_year_end.
     */
    Calculation calculate_snr(const std::string& org_id,
                              const std::string& half_year_start,
                              const std::string& half_year_end) {
        const long long income_tiyn = sum_line_amount_tiyn(org_id,
                                                           kIncomeAccounts,
                                                           "credit",
                                                           half_year_start,
                                                           half_year_end,
                                                           /*vat_only=*/false);

        auto rate = tax_ref_.rate_on(RateKind::kSnrSimplified, half_year_end, "");
        if (!rate)
            throw MissingTaxReference("no snr_simplified rate in force on " + half_year_end);

        const long long tax_tiyn = apply_bp(income_tiyn, rate->rate_bp);

        nlohmann::json input_snapshot = {
            {"period_from", half_year_start},
            {"period_to", half_year_end},
            {"rate_bp", rate->rate_bp},
            {"rate_effective_from", rate->effective_from},
            {"income_accounts", kIncomeAccounts},
        };
        // ipn_part_tiyn / social_tax_part_tiyn intentionally absent — see
        // file header.
        nlohmann::json result_snapshot = {
            {"income_tiyn", income_tiyn},
            {"rate_bp", rate->rate_bp},
            {"tax_tiyn", tax_tiyn},
        };

        return calc_repo_.upsert(org_id,
                                 CalculationKind::kSnrSimplified,
                                 half_year_start,
                                 half_year_end,
                                 input_snapshot,
                                 result_snapshot,
                                 tax_tiyn);
    }

    /**
     * @brief НДС for one quarter: accrued (начислено) minus deductible
     *        (к зачёту).
     *
     * accrued   = Σ vat_amount over posted CREDIT lines on the 6000-group
     *             revenue accounts (see file header) in
     *             [quarter_start, quarter_end];
     * deductible = Σ vat_amount over posted DEBIT lines on {1330, 7010, 7210}
     *             in the same period;
     * balance   = accrued − deductible (negative ⇒ к возврату, a refund
     *             position — `total_tiyn` and the column it is stored in are
     *             both signed, this is a normal, expected outcome, not an
     *             error).
     *
     * The `vat` rate itself is NOT an input to this arithmetic — every
     * journal line already carries its own realized vat_amount (set when the
     * underlying invoice/entry was recorded), so recomputing НДС from
     * `rate × base` here would double-apply it. The rate in force on
     * @p quarter_end is still looked up and recorded in `input_snapshot`
     * purely as audit context (§7.2 reproducibility: "what rate was
     * nominally in force, even though this calculation didn't need to apply
     * it itself") — its absence from the reference table is NOT fatal here,
     * unlike calculate_snr, since it plays no role in the actual sum.
     */
    Calculation calculate_vat(const std::string& org_id,
                              const std::string& quarter_start,
                              const std::string& quarter_end) {
        const long long accrued_tiyn =
            sum_line_amount_tiyn(org_id, kIncomeAccounts, "credit", quarter_start, quarter_end, /*vat_only=*/true);
        const long long deductible_tiyn = sum_line_amount_tiyn(
            org_id, kVatDeductibleAccounts, "debit", quarter_start, quarter_end, /*vat_only=*/true);
        const long long balance_tiyn = accrued_tiyn - deductible_tiyn;

        nlohmann::json input_snapshot = {
            {"period_from", quarter_start},
            {"period_to", quarter_end},
            {"income_accounts", kIncomeAccounts},
            {"deductible_accounts", kVatDeductibleAccounts},
        };
        auto vat_rate = tax_ref_.rate_on(RateKind::kVat, quarter_end, "");
        if (vat_rate)
            input_snapshot["vat_rate_bp"] = vat_rate->rate_bp;

        nlohmann::json result_snapshot = {
            {"accrued_tiyn", accrued_tiyn},
            {"deductible_tiyn", deductible_tiyn},
            {"balance_tiyn", balance_tiyn},
        };

        return calc_repo_.upsert(
            org_id, CalculationKind::kVat, quarter_start, quarter_end, input_snapshot, result_snapshot, balance_tiyn);
    }

    /**
     * @brief Registration-threshold warnings as of @p on_date, based on
     *        income over the trailing 12 months (on_date - 1 year, on_date].
     *
     * Fires at 90% of either threshold, not only once it is actually
     * exceeded — the brief's own test name (`ThresholdAlertFiresNearVatLimit`)
     * calls for an early warning, not a post-mortem: by the time 12-month
     * income literally exceeds `vat_threshold_tenge`, НК РК ст.101 п.3 has
     * already started an unforgiving 5-business-day registration clock, so
     * a useful alert has to fire while there is still runway to act. 90% is
     * an arbitrary-but-documented early-warning margin (kNearThresholdNum /
     * kNearThresholdDen below), not a legal figure — the two `threshold_*`
     * constants used for the COMPARISON base (`vat_threshold_tenge`,
     * `snr_income_limit_mrp × mrp`) are the only legally-sourced numbers
     * here, and `threshold_tiyn` on the returned Alert is always the real
     * (100%) legal threshold, not the 90% trigger point — the near-margin
     * only decides WHETHER an alert fires, never what number it reports.
     *
     * @throws MissingTaxReference if `vat_threshold_tenge`, `mrp`, or
     *         `snr_income_limit_mrp` is not in force on @p on_date.
     */
    std::vector<Alert> threshold_alerts(const std::string& org_id, const std::string& on_date) {
        const std::string period_from = subtract_one_year(on_date);
        const long long income_tiyn =
            sum_line_amount_tiyn(org_id, kIncomeAccounts, "credit", period_from, on_date, /*vat_only=*/false);

        std::vector<Alert> alerts;

        auto vat_threshold = tax_ref_.constant_on("vat_threshold_tenge", on_date);
        if (!vat_threshold)
            throw MissingTaxReference("no vat_threshold_tenge constant in force on " + on_date);
        if (is_near_or_over(income_tiyn, vat_threshold->value_tiyn)) {
            Alert a;
            a.kind = "vat_registration";
            a.current_tiyn = income_tiyn;
            a.threshold_tiyn = vat_threshold->value_tiyn;
            a.message = "12-month income (" + std::to_string(income_tiyn) +
                        " tiyn) is approaching or over the VAT registration threshold (" +
                        std::to_string(vat_threshold->value_tiyn) + " tiyn) — НК РК ст.99 п.4 пп.2 / ст.101 п.3";
            alerts.push_back(std::move(a));
        }

        auto mrp = tax_ref_.constant_on("mrp", on_date);
        auto snr_limit_mrp = tax_ref_.constant_on("snr_income_limit_mrp", on_date);
        if (!mrp || !snr_limit_mrp || !snr_limit_mrp->value_units)
            throw MissingTaxReference("no mrp / snr_income_limit_mrp constant in force on " + on_date);
        const long long snr_threshold_tiyn = mrp->value_tiyn * (*snr_limit_mrp->value_units);
        if (is_near_or_over(income_tiyn, snr_threshold_tiyn)) {
            Alert a;
            a.kind = "snr_limit";
            a.current_tiyn = income_tiyn;
            a.threshold_tiyn = snr_threshold_tiyn;
            a.message = "12-month income (" + std::to_string(income_tiyn) +
                        " tiyn) is approaching or over the SNR simplified-declaration income limit (" +
                        std::to_string(snr_threshold_tiyn) + " tiyn) — НК РК ст.722";
            alerts.push_back(std::move(a));
        }

        return alerts;
    }

private:
    TaxReferenceRepository tax_ref_;
    TaxCalculationRepository calc_repo_;

    /// The 6000-group revenue accounts — both СНР's income base and НДС's
    /// accrual base (see file header for why the two share one set).
    static inline const std::vector<std::string> kIncomeAccounts = {"6010", "6110", "6250", "6290"};
    /// НДС к зачёту source accounts.
    static inline const std::vector<std::string> kVatDeductibleAccounts = {"1330", "7010", "7210"};

    static constexpr long long kNearThresholdNum = 9;
    static constexpr long long kNearThresholdDen = 10;

    /// current_tiyn * 10 >= threshold_tiyn * 9  ⇔  current >= 90% of
    /// threshold, computed with plain integer multiplication instead of
    /// division so no fractional/rounding tiyn ever enters the comparison.
    static bool is_near_or_over(long long current_tiyn, long long threshold_tiyn) {
        return current_tiyn * kNearThresholdDen >= threshold_tiyn * kNearThresholdNum;
    }

    /// Multiplies `base_tiyn` by `rate_bp` basis points, rounding half-up to
    /// the nearest whole tiyn. Deliberately re-implemented here (rather than
    /// including src/payroll/PayrollCalculator.hpp for its Payroll::apply_bp)
    /// so Tax:: has no compile-time dependency on Payroll:: — the two
    /// modules compute unrelated things and happen to share one rounding
    /// rule; the formula itself is copied verbatim from
    /// Payroll::apply_bp's doc comment so both stay in lockstep by
    /// inspection if either is ever revisited.
    static long long apply_bp(long long base_tiyn, long long rate_bp) { return (base_tiyn * rate_bp + 5000) / 10000; }

    /// "YYYY-MM-DD" minus one calendar year, same month/day. Used only for
    /// threshold_alerts' trailing-12-month window. Known limitation: a
    /// source date of exactly Feb 29 (leap day) produces "Feb 29" of a
    /// non-leap target year, which Postgres's `::date` cast will reject —
    /// acceptable for P2 (the brief's on_date is an arbitrary reporting
    /// date, not required to be Feb 29) but worth fixing if this is ever
    /// reused for arbitrary caller-supplied dates.
    static std::string subtract_one_year(const std::string& date) {
        if (date.size() < 4)
            throw std::invalid_argument("subtract_one_year: malformed date '" + date + "'");
        const int year = std::stoi(date.substr(0, 4)) - 1;
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(4) << year << date.substr(4);
        return oss.str();
    }

    /// Σ(amount) or Σ(vat_amount), in tiyn, over posted lines on @p accounts
    /// with the given @p side, for entries dated in
    /// [period_from, period_to] inclusive. Both sums happen INSIDE Postgres
    /// (`SUM(ROUND(<col> * 100))::bigint`) — see file header for why this
    /// never parses a NUMERIC column as text/float in C++. @p accounts is
    /// always one of this class's own hardcoded constants (never
    /// caller-supplied), so building its `IN (...)` list as plain text
    /// (rather than an array-typed bind parameter, which libpqxx as used
    /// elsewhere in this codebase has no established idiom for) carries no
    /// injection risk.
    long long sum_line_amount_tiyn(const std::string& org_id,
                                   const std::vector<std::string>& accounts,
                                   const std::string& side,
                                   const std::string& period_from,
                                   const std::string& period_to,
                                   bool vat_only) {
        return Database::get().execute_read([&](auto& txn) {
            const std::string column = vat_only ? "jl.vat_amount" : "jl.amount";
            const std::string vat_guard = vat_only ? " AND jl.vat_amount IS NOT NULL" : "";
            auto r = txn.exec_params("SELECT COALESCE(SUM(ROUND(" + column +
                                         " * 100)), 0)::bigint "
                                         "FROM journal_lines jl "
                                         "JOIN journal_entries je ON je.id = jl.entry_id AND je.org_id = jl.org_id "
                                         "WHERE jl.org_id = $1 AND je.status = 'posted' AND jl.side = $2 "
                                         "AND je.entry_date >= $3::date AND je.entry_date <= $4::date "
                                         "AND jl.account_code IN " +
                                         account_list_sql(accounts) + vat_guard,
                                     org_id,
                                     side,
                                     period_from,
                                     period_to);
            return r.at(0).at(0).template as<long long>();
        });
    }

    /// Renders @p accounts as a literal SQL `('a','b',...)` list. See
    /// sum_line_amount_tiyn's doc comment for why this is safe to build as
    /// plain text here.
    static std::string account_list_sql(const std::vector<std::string>& accounts) {
        std::string out = "(";
        for (std::size_t i = 0; i < accounts.size(); ++i) {
            if (i > 0)
                out += ",";
            out += "'" + accounts[i] + "'";
        }
        out += ")";
        return out;
    }
};

}  // namespace Tax
