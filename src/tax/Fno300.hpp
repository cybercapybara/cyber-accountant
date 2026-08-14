/**
 * @file Fno300.hpp
 * @brief XML generator for ФНО 300.00 (Декларация по налогу на добавленную
 *        стоимость, quarterly), consuming Tax::TaxService::calculate_vat's
 *        persisted Calculation (design spec P2 task-8-brief.md).
 *
 * @par Step 1 — format research (required by the brief; documented here in
 *      full, same posture as Fno910.hpp's Step 1 for the shared foundation)
 *
 * Repeated the Task 7 search, this time for 300.00. Findings:
 *
 * 1. **No content XSD, same as 910.00.** kgd.gov.kz's "ФНО на 2026 год" page
 *    links to KGD's live 2026 SONO-client template for this form —
 *    `.../forms/300.00/20260104/form_300_00_v29_r170.tar.bz2` (version 29,
 *    revision 170, dated 2026-01-04) — which is a form *renderer* definition
 *    (field coordinates/widths/validators for the SONO desktop client), not
 *    a content-validation schema. As with 910.00, KGD's own "API-сервис по
 *    приёму форм налоговой отчётности" documentation only ships XSD for the
 *    SOAP-ish transport ack/status messages, never for a form's own field
 *    grid. Genuinely no content XSD exists to validate 300.00 against
 *    either, so this generator is `schemaValidated="false"` for the same
 *    permanent reason Fno910 is (see FnoXml.hpp's file header).
 *
 * 2. **Real field names WERE extracted directly from that live template**
 *    (`form_300_00_v29_r170.xml`, downloaded from the URL above) — not
 *    guessed. That file lists every `<numericField>`/`<textField>` the SONO
 *    client renders, each carrying a `fieldName` attribute plus a
 *    `labelCode` that resolves (via the same archive's
 *    `form_300_00_v29_r170_loc.xml` localization resource) to the field's
 *    real Russian label. Confirmed this way:
 *      - `iin`, `rnn`, `payer_name1`/`payer_name2`/`payer_name3` — identical
 *        identity fields to 910.00 (FnoXml::identity_fields is reused as-is).
 *      - `period_year`, **`period_quarter`** — the period fields, and
 *        genuinely `period_quarter` (not a half-year field): confirmed a
 *        second, independent way too — the template's own `<rules>` engine
 *        (`form_300_00_v29_r170_chr.xml`) contains a due-date formula
 *        literally referencing `@page.period_quarter*3+2` and
 *        `@page.period_year`, i.e. KGD's own internal tooling treats these
 *        two fields as the form's period, exactly as expected for a
 *        quarterly return.
 *      - `currency_code`, `dt_regular`/`dt_additional`/`dt_notice` — same
 *        shape as 910.00's declaration-type flags.
 *      - `field_300_00_012`, labelCode resolves to "Всего начислено НДС"
 *        (total VAT accrued) — matches calculate_vat's `accrued_tiyn`.
 *      - `field_300_00_023`, labelCode resolves to "Общая сумма НДС,
 *        относимого в зачет, за исключением строки 300.00.024" (total VAT
 *        credited/deductible, excluding the proportional-method line 024) —
 *        matches calculate_vat's `deductible_tiyn`.
 *      - `field_300_00_030_01`, labelCode resolves to "Исчисленная сумма
 *        НДС за налоговый период: сумма НДС, подлежащая уплате
 *        (300.00.012-300.00.025...)" (VAT amount payable) and
 *        `field_300_00_030_02` resolves to "превышение суммы НДС,
 *        относимого в зачет, над суммой начисленного налога" (excess of
 *        credited VAT over accrued tax) — together these are the real
 *        form's own to-pay/excess-credit pair, i.e. exactly the
 *        payable-vs-refund split the brief asks for. The real KGD formula
 *        for both additionally nets in appendix lines 300.00.025/300.00.029
 *        (opening/closing carried-forward excess credit from PRIOR
 *        periods), which calculate_vat does not track — see "Consequence"
 *        below for how that gap is handled honestly rather than papered
 *        over.
 *    Additionally located a genuine KGD-published wire-format EXAMPLE for
 *    this form (not merely the renderer template) —
 *    `Пример_300.00_v27_for_knp.xml`, from KGD's own historical
 *    "300.00.rar" XML-format documentation archive (linked from the same
 *    "Программное обеспечение" page as the 910.00 archive Task 7 used) —
 *    confirming the exact envelope shape:
 *      `<fno code="300.00" formatVersion="1" version="27"><form
 *      name="form_300_00"><sheetGroup><sheet
 *      name="page_300_00_01"><field name="...">...` — the same
 *      `<fno>/<form>/<sheetGroup>/<sheet>/<field>` nesting Fno910.hpp
 *      documented for 910.00, just with `page_300_00_01` as the (first)
 *      sheet name and `form_300_00` as the form name. `kFormVersion` below
 *      uses "29" rather than that example's "27" to track KGD's *current*
 *      (2026) live template revision, mirroring Fno910's own
 *      "match the live template, not an old archived example" choice.
 *
 * @par Consequence for this generator
 *
 * Faithfully reproduces the real envelope, identity, period, and
 * declaration-type fields, plus the three data fields it can confidently
 * map (`field_300_00_012`, `field_300_00_023`, `field_300_00_030_01`/`_02`)
 * — but calculate_vat's `balance_tiyn` (accrued − deductible for THIS
 * period only) is a simplification of the real multi-period, multi-appendix
 * legal formula those KGD lines nominally encode (see finding 2's note on
 * lines 025/029). Rather than either fabricating the missing
 * carried-forward inputs or silently mislabeling a partial figure as the
 * full legal one, this generator: (a) still populates the two real KGD
 * fields with the best available same-period figure, matching Fno910's own
 * precedent of using a real field for a value it can support with
 * confidence, and (b) mirrors the exact split into an explicit, clearly
 * non-official `<calcSummary>` element (own XML namespace, same pattern as
 * Fno910) with `toPayTenge`/`toRefundTenge` alongside the raw
 * accrued/deductible/balance figures, so a caller never has to reverse-
 * engineer the payable/refund split out of the KGD field pair alone.
 * `schemaValidated="false"` on the root, same reasoning as Fno910 (finding
 * 1) and FnoXml.hpp.
 *
 * @par Library choice
 *
 * Same as Fno910.hpp: hand-rolled string assembly in production code (no
 * XML-library dependency), `pugixml` used only in
 * tests/unit/test_fno300_xml.cpp to parse the generated string back and
 * independently verify well-formedness.
 *
 * Pure, DB-free by construction (only <string>/<nlohmann/json.hpp> are
 * included, via FnoXml.hpp/TaxCalculation.hpp) — safe to unit-test without
 * Postgres, per the P2 task-8 brief.
 */

#pragma once

#include <stdexcept>
#include <string>

#include "tax/FnoXml.hpp"
#include "tax/TaxCalculation.hpp"

namespace Tax {

/**
 * @brief Builds the ФНО 300.00 XML for one НДС (quarterly) calculation.
 *
 * Consumes exactly what Tax::TaxService::calculate_vat produces (a
 * Calculation of kind `CalculationKind::kVat`) plus the OrgInfo identifying
 * the filer — no database access, no dependency on Tax::TaxService itself
 * (only its output type).
 */
class Fno300 {
public:
    /// KGD does not publish a content XSD for this form either (Step 1, file
    /// header) — see Fno910::kSchemaValidated for the identical reasoning.
    static constexpr bool kSchemaValidated = false;

    static constexpr const char* kFormCode = "300.00";
    /// Matches KGD's live 2026 template revision (form_300_00_v29_r170, see
    /// file header) — bump alongside that template if KGD ships a new major
    /// `version`.
    static constexpr const char* kFormVersion = "29";

    /**
     * @brief Renders @p calc (a `vat` Calculation) and @p org into a
     *        well-formed ФНО 300.00 XML document string.
     *
     * @throws std::invalid_argument if `calc.kind` is not
     *         `CalculationKind::kVat` — this generator is 300.00-specific
     *         and would otherwise silently mislabel a different tax's
     *         figures as a НДС declaration.
     */
    static std::string build_xml(const Calculation& calc, const OrgInfo& org) {
        if (calc.kind != CalculationKind::kVat) {
            throw std::invalid_argument("Fno300::build_xml: expected Calculation.kind='" +
                                        std::string(CalculationKind::kVat) + "', got '" + calc.kind + "'");
        }

        // accrued_tiyn/deductible_tiyn are read defensively (default 0) —
        // they are calculate_vat's own result_snapshot annotations, not part
        // of Calculation's guaranteed contract, unlike total_tiyn below
        // (every Calculation row has it, per its NOT NULL column, and it is
        // the balance calculate_vat itself computed as accrued −
        // deductible) — total_tiyn is therefore the sole source of truth for
        // the payable/refund split, with accrued/deductible used only to
        // populate the two informational KGD fields above it.
        const long long accrued_tiyn = calc.result_snapshot.value("accrued_tiyn", 0LL);
        const long long deductible_tiyn = calc.result_snapshot.value("deductible_tiyn", 0LL);
        const long long balance_tiyn = calc.total_tiyn;
        // balance > 0 -> payable; balance < 0 -> excess credit (a refund
        // position, per calculate_vat's own doc comment); balance == 0 ->
        // both zero, a routine "nothing due, nothing to reclaim" filing.
        const long long to_pay_tiyn = balance_tiyn > 0 ? balance_tiyn : 0;
        const long long to_refund_tiyn = balance_tiyn < 0 ? -balance_tiyn : 0;

        std::string sheet;
        sheet += FnoXml::identity_fields(org);
        sheet += FnoXml::field("period_year", org.tax_period_year);
        // OrgInfo has no quarter-number field — its `tax_period_half` is
        // documented as half-year-specific ("1" or "2"), which cannot stand
        // in for a 1-4 quarter number without contradicting that contract
        // (and 300.00 IS quarterly per finding 2's `period_quarter*3+2`
        // formula, not `period_half_year` — the two forms genuinely use
        // different period fields, this is not an oversight to paper over).
        // Rather than widen the shared OrgInfo struct (which would also
        // touch Fno910.hpp's contract for no benefit to that form), the
        // quarter number is derived from `calc.period_from`'s month — the
        // same closed date range calculate_vat itself used to run the
        // aggregate query, so it is guaranteed consistent with the figures
        // in this very document.
        sheet += FnoXml::field("period_quarter", std::to_string(quarter_from_period_from(calc.period_from)));
        sheet += FnoXml::field("currency_code", std::string("KZT"));
        // Always a regular ("очередная") declaration — Calculation carries
        // no signal distinguishing a corrective (dt_additional) or
        // notice-triggered (dt_notice) filing, same scope limitation Fno910
        // documents for its own dt_regular field.
        sheet += FnoXml::field("dt_regular", std::string("true"));
        // Each amount is rounded to whole tenge independently from its raw
        // tiyn value, so `012 - 023` may differ from `030_01` by up to one
        // tenge. That is deliberate and matches how tax forms are filled:
        // every reported line is the rounding of its own exact amount, not a
        // recomputation from already-rounded neighbours.
        sheet += FnoXml::field("field_300_00_012", FnoXml::round_half_up_to_tenge(accrued_tiyn));
        sheet += FnoXml::field("field_300_00_023", FnoXml::round_half_up_to_tenge(deductible_tiyn));
        sheet += FnoXml::field("field_300_00_030_01", FnoXml::round_half_up_to_tenge(to_pay_tiyn));
        sheet += FnoXml::field("field_300_00_030_02", FnoXml::round_half_up_to_tenge(to_refund_tiyn));

        // Non-official summary — see file header's "Consequence" section
        // for why the KGD fields above cannot, by themselves, be trusted as
        // the full legal payable/refund computation.
        std::string summary = "<calcSummary xmlns=\"urn:cyber-accountant:fno-summary\">";
        summary += "<accruedTenge>" + FnoXml::tenge_amount(accrued_tiyn) + "</accruedTenge>";
        summary += "<deductibleTenge>" + FnoXml::tenge_amount(deductible_tiyn) + "</deductibleTenge>";
        summary += "<balanceTenge>" + FnoXml::tenge_amount(balance_tiyn) + "</balanceTenge>";
        summary += "<toPayTenge>" + FnoXml::tenge_amount(to_pay_tiyn) + "</toPayTenge>";
        summary += "<toRefundTenge>" + FnoXml::tenge_amount(to_refund_tiyn) + "</toRefundTenge>";
        summary += "<periodFrom>" + FnoXml::escape(calc.period_from) + "</periodFrom>";
        summary += "<periodTo>" + FnoXml::escape(calc.period_to) + "</periodTo>";
        summary += "</calcSummary>";

        std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
        xml += "<fno code=\"" + std::string(kFormCode) + "\" version=\"" + std::string(kFormVersion) +
               "\" formatVersion=\"1\" schemaValidated=\"false\" bin=\"" + FnoXml::escape(org.bin) + "\">";
        xml += "<form name=\"form_300_00\"><sheetGroup><sheet name=\"page_300_00_01\">" + sheet +
               "</sheet></sheetGroup></form>";
        xml += summary;
        xml += "</fno>";
        return xml;
    }

private:
    /// Extracts the 1-4 quarter number from a "YYYY-MM-DD" date's month
    /// component. `period_from` is calculate_vat's own `quarter_start`
    /// argument (see TaxCalculation.hpp's Calculation::period_from), always
    /// well-formed by construction (TaxService never persists a Calculation
    /// without it) — no validation beyond what substr's bounds already
    /// enforce is needed here.
    static int quarter_from_period_from(const std::string& period_from) {
        if (period_from.size() < 7)
            throw std::invalid_argument("Fno300::build_xml: malformed period_from '" + period_from + "'");
        const int month = std::stoi(period_from.substr(5, 2));
        return (month - 1) / 3 + 1;
    }
};

}  // namespace Tax
