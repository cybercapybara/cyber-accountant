/**
 * @file PayrollCalculator.hpp
 * @brief Pure, allocation-free KZ payroll withholding calculator (design
 *        spec §7.2, phase P2 task-2-brief.md).
 * @details No database access and no dependency on `src/tax/` here — the
 *          caller (a service/controller layer) resolves `Tax::TaxRate` /
 *          `Tax::TaxConstant` rows via `Tax::TaxReferenceRepository` and
 *          maps them onto `Payroll::Rates` before calling `calculate()`.
 *          This keeps the calculator neutral to any dispute over where a
 *          given limit or rate comes from (see task-1's reconciliation in
 *          `migrations/011_tax_reference.sql`) — every threshold is a field
 *          on `Rates`, never a literal in this file.
 *
 *          Money is always integer TIYN (1 ₸ = 100 tiyn); rates are integer
 *          BASIS POINTS (1 bp = 0.01%, 16% = 1600). No floating point
 *          anywhere in the arithmetic — see `apply_bp` for the rounding rule.
 *
 *          Order of calculation (task-2-brief.md, "Порядок расчёта";
 *          cross-checked against Task 1 Step 1 / `migrations/011_tax_reference.sql`
 *          — Социальный кодекс РК №224-VII ст.244/245/249/251, Закон РК
 *          №405-V ст.27/28, НК РК №214-VIII соответствующие статьи по ИПН и
 *          соцналогу):
 *            1. `opv    = apply_bp(min(gross, mzp * opv_base_max_mzp), opv_bp)`
 *               — ОПВ (Соцкодекс ст.249): удерживается с работника, база
 *               ограничена сверху 50-кратным МЗП.
 *            2. `vosms  = apply_bp(min(gross, mzp * vosms_base_max_mzp), vosms_bp)`
 *               — ВОСМС (Закон №405-V ст.28): удерживается с работника.
 *            3. `ipn_base = max(0, gross - opv - vosms - (ipn_deduction_claimed ? mrp * ipn_deduction_mrp : 0))`,
 *               `ipn = apply_bp(ipn_base, ipn_bp)` — ИПН (НК РК ст.363/403):
 *               база после вычета ОПВ, ВОСМС и (если заявлен) стандартного
 *               вычета в МРП.
 *
 *               ВАЖНО (осознанное упрощение P2): НК РК ст.363 в действительности
 *               задаёт ПРОГРЕССИВНУЮ шкалу ИПН — 10% на доход в пределах
 *               8500 МРП нарастающим итогом за календарный год, 15% сверх
 *               этого порога (на 2026 год 8500 МРП ≈ 36 762 500 ₸/год,
 *               ~3,06 млн ₸/мес). Эта версия калькулятора — ОДНОСТАВОЧНАЯ:
 *               `ipn_bp` применяется целиком к `ipn_base` без учёта дохода с
 *               начала года. Прогрессия сознательно не реализована в P2 —
 *               `calculate()` чистая функция без состояния и не имеет
 *               накопительного контекста «доход сотрудника с начала года»,
 *               а профиль пользователей продукта (ТОО на упрощёнке) редко
 *               достигает порога. См. `HighSalaryStillUsesFlatRateInP2` в
 *               `tests/unit/test_payroll_calculator.cpp` и бэклог — при
 *               добавлении year-to-date контекста этот шаг и тест нужно
 *               переписать.
 *            4. `net = gross - opv - vosms - ipn` — сумма к выплате на руки.
 *            5. `so_base = clamp(gross - opv, mzp * so_base_min_mzp, mzp * so_base_max_mzp)`,
 *               `so = apply_bp(so_base, so_bp)` — СО (Соцкодекс ст.245):
 *               база — доход за вычетом ОПВ, зажатый в коридор
 *               [1 МЗП, 7 МЗП] (нижний предел применяется и при доходе ниже
 *               1 МЗП).
 *            6. `osms = apply_bp(min(gross, mzp * osms_base_max_mzp), osms_bp)`
 *               — ОСМС (Закон №405-V ст.27): за счёт работодателя, база
 *               ограничена сверху 40-кратным МЗП.
 *            7. `opvr = opvr_exempt ? 0 : apply_bp(min(gross, mzp * opv_base_max_mzp), opvr_bp)`
 *               — ОПВР (Соцкодекс ст.251): за счёт работодателя, та же
 *               потолочная база, что у ОПВ; не начисляется для пенсионеров/
 *               инвалидов (`opvr_exempt`).
 *            8. `social_tax = social_tax_applies ? apply_bp(gross - opv - vosms, social_tax_bp) : 0`
 *               — соцналог (НК РК, новая редакция): база — доход за вычетом
 *               ОПВ и ВОСМС, БЕЗ уменьшения на сумму СО (взаимозачёт отменён
 *               с 2026 года, см. `migrations/011_tax_reference.sql` п.3
 *               разрешённых расхождений); `social_tax_applies=false` на СНР
 *               на основе упрощённой декларации, где соцналог не платится.
 *            9. `employer_cost_total = gross + opvr + so + osms + social_tax`
 *               — полная стоимость сотрудника для работодателя.
 *
 *          If a Task-1 source dispute is ever resolved differently than the
 *          seed above, only the `Rates` values a caller passes in need to
 *          change — this file's formula is deliberately mechanical and
 *          should not need to move.
 */

#pragma once

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Payroll {

/// The largest gross a single payslip may carry: 10^14 tiyn = 1 000 000 000
/// 000 ₸ (one trillion tenge) of salary for ONE employee for ONE month.
///
/// This is an overflow bound, not a business rule. `apply_bp` computes
/// `base * bp` in `long long`, and the ИПН and социальный налог bases are
/// UNCAPPED (unlike ОПВ/ОПВР/СО/ОСМС, whose bases are clamped to a small
/// multiple of МЗП) — they track `gross` all the way up. With `bp` never
/// exceeding 10 000 (100%, and the seeded rates are two orders of magnitude
/// below that), a gross of at most 10^14 keeps every product at or under
/// 10^18, comfortably inside `long long`'s ~9.22·10^18 range; the remaining
/// `+ 5000` and the `employer_cost_total` sum of five such terms cannot close
/// that gap either.
///
/// A bound checked once, at the entry of `calculate()`, was chosen over
/// overflow-detecting arithmetic inside `apply_bp` because it is far easier
/// to reason about: there is exactly one place where an input can be
/// rejected, every intermediate below it is then provably in range, and the
/// arithmetic stays the plain integer expression the formula documentation
/// describes. Overflow-checked multiplication would have to answer "and then
/// what?" at nine separate call sites, each mid-formula.
///
/// The bound is deliberately absurd as a salary — the largest real-world
/// payslip is many orders of magnitude below it — so no legitimate payroll is
/// ever refused by it. It exists because `employees.salary_tiyn` is a plain
/// BIGINT (migrations/012_hr.sql) and the API layer's `Ledger::parse_tiyn`
/// accepts up to 16 digits of tenge, so a typo or a hostile request CAN put a
/// value in the table that would overflow the arithmetic — silently, as
/// signed overflow is undefined behaviour, producing a wrong number on a
/// payslip rather than an error.
inline constexpr long long kMaxGrossTiyn = 100'000'000'000'000LL;

/// Resolved rates/constants for one calculation. All money fields are TIYN,
/// all `*_bp` fields are basis points, all `*_mzp`/`*_mrp` fields are plain
/// multipliers (the caller already multiplied out `Tax::Constant::value_units`
/// against the current `mzp`/`mrp` — this struct does the final `mzp * N`
/// multiplication itself so every step reads the same way as the brief).
struct Rates {
    /// ИПН rate in basis points. Flat/single-rate only — the real НК РК
    /// ст.363 progression (10% up to 8500 МРП/year cumulative income, 15%
    /// above) is NOT implemented in P2: it needs year-to-date income
    /// context this pure function doesn't carry. See the file header and
    /// `HighSalaryStillUsesFlatRateInP2` for the deliberate scope cut.
    long long ipn_bp = 0;
    long long opv_bp = 0;
    long long opvr_bp = 0;
    long long so_bp = 0;
    long long osms_bp = 0;
    long long vosms_bp = 0;
    long long social_tax_bp = 0;

    long long mrp_tiyn = 0;
    long long mzp_tiyn = 0;

    long long ipn_deduction_mrp = 0;
    long long opv_base_max_mzp = 0;
    long long so_base_min_mzp = 0;
    long long so_base_max_mzp = 0;
    long long vosms_base_max_mzp = 0;
    long long osms_base_max_mzp = 0;

    /// false on СНР на основе упрощённой декларации, where the social tax
    /// is not levied at all (see Step 8 above).
    bool social_tax_applies = true;
};

/// One employee's calculation input for a single payroll period.
struct Input {
    long long gross_tiyn = 0;

    /// Employee has filed a statement (заявление) claiming the standard
    /// ИПН deduction (НК РК ст.403, `ipn_deduction_mrp` МРП/month).
    bool ipn_deduction_claimed = false;

    /// Employee is a pensioner/person with disability, exempt from ОПВР
    /// (Соцкодекс ст.251).
    bool opvr_exempt = false;
};

/// Every amount computed for the period, in TIYN.
struct Result {
    long long opv = 0;
    long long vosms = 0;
    long long ipn = 0;
    long long net = 0;  ///< к выплате (gross - opv - vosms - ipn)
    long long opvr = 0;
    long long so = 0;
    long long osms = 0;
    long long social_tax = 0;
    long long employer_cost_total = 0;
};

/// Multiplies `base_tiyn` by `rate_bp` basis points, rounding half-up to the
/// nearest whole tiyn: `(base*bp + 5000) / 10000`. Integer-only; both
/// arguments are expected non-negative (every call site in `calculate()`
/// guarantees this) and `base_tiyn * rate_bp` is expected to fit in
/// `long long` — `calculate()` guarantees THAT by rejecting a gross above
/// kMaxGrossTiyn up front, which is what keeps this expression a plain
/// multiplication instead of a checked one (see kMaxGrossTiyn).
inline long long apply_bp(long long base_tiyn, long long rate_bp) {
    return (base_tiyn * rate_bp + 5000) / 10000;
}

/// Runs the 9-step KZ withholding order (see file header) over one
/// employee/period. Pure function: no I/O, no shared state, safe to call
/// concurrently and to unit-test with plain structs.
///
/// @throws std::out_of_range if `in.gross_tiyn` is negative or exceeds
///         kMaxGrossTiyn — see that constant for why the bound exists and why
///         it is checked here rather than inside apply_bp. Callers that can
///         report a better-shaped error to a user (Payroll::PayrollService)
///         screen the value themselves before getting here; this check is the
///         last line of defense for every other caller, so that the
///         alternative is never undefined behaviour.
inline Result calculate(const Input& in, const Rates& r) {
    Result out;

    if (in.gross_tiyn < 0 || in.gross_tiyn > kMaxGrossTiyn)
        throw std::out_of_range("payroll: gross of " + std::to_string(in.gross_tiyn) +
                                " tiyn is outside the supported range [0, " + std::to_string(kMaxGrossTiyn) + "]");

    const long long gross = in.gross_tiyn;

    // 1. ОПВ — удержание с работника, база до 50 МЗП.
    const long long opv_base = std::min(gross, r.mzp_tiyn * r.opv_base_max_mzp);
    out.opv = apply_bp(opv_base, r.opv_bp);

    // 2. ВОСМС — удержание с работника, база до 20 МЗП.
    const long long vosms_base = std::min(gross, r.mzp_tiyn * r.vosms_base_max_mzp);
    out.vosms = apply_bp(vosms_base, r.vosms_bp);

    // 3. ИПН — база после ОПВ, ВОСМС и (если заявлен) вычета в 30 МРП.
    //    Одноставочно: прогрессия 10%/15% при 8500 МРП/год НЕ реализована в
    //    P2 (нет year-to-date контекста) — см. Rates::ipn_bp и файл-хедер.
    const long long ipn_deduction = in.ipn_deduction_claimed ? r.mrp_tiyn * r.ipn_deduction_mrp : 0;
    const long long ipn_base = std::max<long long>(0, gross - out.opv - out.vosms - ipn_deduction);
    out.ipn = apply_bp(ipn_base, r.ipn_bp);

    // 4. К выплате на руки.
    out.net = gross - out.opv - out.vosms - out.ipn;

    // 5. СО — база (gross - ОПВ), зажатая в коридор [1 МЗП, 7 МЗП].
    const long long so_floor = r.mzp_tiyn * r.so_base_min_mzp;
    const long long so_ceiling = r.mzp_tiyn * r.so_base_max_mzp;
    const long long so_base = std::clamp(gross - out.opv, so_floor, so_ceiling);
    out.so = apply_bp(so_base, r.so_bp);

    // 6. ОСМС — за счёт работодателя, база до 40 МЗП.
    const long long osms_base = std::min(gross, r.mzp_tiyn * r.osms_base_max_mzp);
    out.osms = apply_bp(osms_base, r.osms_bp);

    // 7. ОПВР — за счёт работодателя, база до 50 МЗП; 0 для льготников.
    out.opvr = in.opvr_exempt ? 0 : apply_bp(std::min(gross, r.mzp_tiyn * r.opv_base_max_mzp), r.opvr_bp);

    // 8. Соцналог — база (gross - ОПВ - ВОСМС), БЕЗ зачёта СО (отменён с
    //    2026 года); 0 на СНР на основе упрощённой декларации.
    out.social_tax = r.social_tax_applies ? apply_bp(gross - out.opv - out.vosms, r.social_tax_bp) : 0;

    // 9. Полная стоимость сотрудника для работодателя.
    out.employer_cost_total = gross + out.opvr + out.so + out.osms + out.social_tax;

    return out;
}

}  // namespace Payroll
