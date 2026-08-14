/**
 * @file Fno910.hpp
 * @brief XML generator for ФНО 910.00 (СНР на основе упрощённой декларации),
 *        consuming Tax::TaxService::calculate_snr's persisted Calculation
 *        (design spec P2 task-7-brief.md).
 *
 * @par Step 1 — format research (required by the brief; documented here in
 *      full since this is the artifact it governs)
 *
 * Searched kgd.gov.kz's "Программное обеспечение" / "ИС СОНО" sections and
 * the KGD "API-сервис по приёму форм налоговой отчётности" page for an
 * official XSD or wire-format description of ФНО 910.00. Findings:
 *
 * 1. **No content XSD exists.** The API-service page links ONLY to WSDL/XSD
 *    for the SOAP-ish SONO_FNO_SEND / SONO_FNO_GET_STATUS *transport*
 *    calls (an opaque `errorInfo`/`requestId` ack, and a
 *    notification/status-history payload) — verified by downloading and
 *    reading every `.xsd` in KGD's own
 *    "Документация.7z" (linked from that page): `responce.xsd`
 *    (`RecvFnoResponce`/`ErrorInfo`) and the `SONO_FNO_GET_STATUS`
 *    schemas (`notificationType`, `fnoHistoryElementType`, etc.). None of
 *    them describe a single ФНО FORM's field grid — the form body travels
 *    through these services as an opaque XML document, validated (if at
 *    all) only by the taxpayer's own СОНО client software, not by a
 *    published schema. This is a genuine, permanent gap in KGD's public
 *    documentation, not something this task failed to find.
 *
 * 2. **The real wire-format XML for 910.00 WAS found and is authoritative.**
 *    The same documentation archive ships
 *    `.../Описание форматов, примеры XML ФНО/910.00v27.xml` — a real,
 *    KGD-published EXAMPLE of the exact XML the СОНО client transmits for
 *    this form, at `version="27"` — which matches the `version="27"`
 *    revision of KGD's live 2026 form template
 *    (`kgd.gov.kz/.../forms/910.00/20251230/form_910_00_v27_r133.tar.bz2`,
 *    linked from the "ФНО на 2026 год" page). The shape, confirmed
 *    directly from that example:
 *
 *    ```xml
 *    <fno code="910.00" formatVersion="1" version="27">
 *      <form name="form_910_00">
 *        <sheetGroup>
 *          <sheet name="page_910_00_01">
 *            <field name="iin">123456789012</field>
 *            <field name="rnn">123456789012</field>
 *            <field name="payer_name1">Тест</field>
 *            <field name="period_year">2023</field>
 *            <field name="period_half_year">1</field>
 *            <field name="currency_code">KZT</field>
 *            <field name="dt_regular">true</field>
 *            <field name="field_910_00_001">10000</field>
 *            <!-- ... dozens more field_910_00_NNN data cells ... -->
 *          </sheet>
 *          <!-- more <sheet> pages, plus a 910.01 appendix sheetGroup -->
 *        </sheetGroup>
 *      </form>
 *    </fno>
 *    ```
 *
 *    `field_910_00_001` = "Доход за налоговый период" (the declaration's
 *    first data line) is independently and consistently documented across
 *    public accounting sources (uchet.kz, pro1c.kz) describing form
 *    910.00's structure, so it is used here with confidence. The example's
 *    remaining ~70 `field_910_00_NNN`/`field_910_00_NNN_X` cells (OPV/ВОСМС
 *    per-employee breakdown grids, the 910.01 appendix, etc.) have NO
 *    reliable public field-by-field legend — KGD's own field-description
 *    document for this form converts to unusable OCR-mangled text, and
 *    guessing a specific `field_910_00_NNN` code for "computed tax amount"
 *    from one example's arbitrary sample values would risk mislabeling a
 *    real government field code, which is worse than omitting it.
 *
 * @par Consequence for this generator
 *
 * `build_xml` reproduces the REAL envelope/header shape faithfully (root
 * `<fno>` attributes, `<form>/<sheetGroup>/<sheet>` nesting, and the
 * `iin`/`rnn`/`payer_name1`/`period_year`/`period_half_year`/
 * `currency_code`/`dt_regular`/`field_910_00_001` fields, all confirmed
 * above) but does NOT fabricate field codes for the tax-computation
 * figures it cannot confidently map. Those instead go in a clearly
 * separate, explicitly non-official `<calcSummary>` element (its own XML
 * namespace) alongside `schemaValidated="false"` on the root — this is the
 * brief's required "mark the artifact experimental" step: it is
 * experimental specifically because KGD publishes no content XSD to
 * validate ANY of this against (see finding 1), not because the header
 * shape itself is a guess (it is not — see finding 2).
 *
 * @par Library choice (Step 1 requirement)
 *
 * Hand-rolled string assembly, not pugixml. The structure is a flat,
 * shallow tree with no attribute namespaces this code needs to resolve and
 * no XSD to validate against (see above) — pugixml would add a real
 * dependency for zero benefit in production code. `pugixml` IS added (to
 * `tests/unit` only) to parse the generated string back and independently
 * verify well-formedness in tests/unit/test_fno910_xml.cpp — trusting a
 * hand-rolled escaper's own round-trip check would prove nothing.
 *
 * Pure, DB-free by construction (only <string>/<nlohmann/json.hpp> are
 * included) — safe to unit-test without Postgres, per the P2 task-7 brief.
 */

#pragma once

#include <stdexcept>
#include <string>

#include "tax/FnoXml.hpp"
#include "tax/TaxCalculation.hpp"

namespace Tax {

/**
 * @brief Builds the ФНО 910.00 XML for one СНР calculation.
 *
 * Consumes exactly what Tax::TaxService::calculate_snr produces (a
 * Calculation of kind `CalculationKind::kSnrSimplified`) plus the OrgInfo
 * identifying the filer and period — no database access, no dependency on
 * Tax::TaxService itself (only its output type).
 */
class Fno910 {
public:
    /// KGD does not publish a content XSD for this form (Step 1, file
    /// header) — every document this generator produces is therefore
    /// permanently unvalidated against any official schema, regardless of
    /// how faithfully the header/field shape matches the real format.
    /// Exposed as a named constant (rather than only the XML attribute of
    /// the same name) so a caller — e.g. a future API response wrapping
    /// this XML — can surface it without re-parsing the document.
    static constexpr bool kSchemaValidated = false;

    static constexpr const char* kFormCode = "910.00";
    /// Matches KGD's live 2026 template revision (form_910_00_v27_r133,
    /// see file header) — bump alongside that template if KGD ships a new
    /// major `version`.
    static constexpr const char* kFormVersion = "27";

    /**
     * @brief Renders @p calc (a `snr_simplified` Calculation) and @p org
     *        into a well-formed ФНО 910.00 XML document string.
     *
     * @throws std::invalid_argument if `calc.kind` is not
     *         `CalculationKind::kSnrSimplified` — this generator is
     *         910.00-specific and would otherwise silently mislabel a
     *         different tax's figures as a СНР declaration.
     * @throws std::invalid_argument if `calc.org_id` and `org.org_id`
     *         disagree. Multi-tenancy is enforced by construction everywhere
     *         else in this codebase (every query is org-scoped, org_id comes
     *         only from the JWT claim), and it is enforced here too rather
     *         than trusting the caller to pair the two correctly: a mismatch
     *         means one organization's figures would be filed under another
     *         organization's BIN and name, which must fail loudly, not
     *         produce a document.
     */
    static std::string build_xml(const Calculation& calc, const OrgInfo& org) {
        if (calc.kind != CalculationKind::kSnrSimplified) {
            throw std::invalid_argument("Fno910::build_xml: expected Calculation.kind='" +
                                        std::string(CalculationKind::kSnrSimplified) + "', got '" + calc.kind + "'");
        }
        if (calc.org_id != org.org_id) {
            throw std::invalid_argument("Fno910::build_xml: calculation belongs to organization '" + calc.org_id +
                                        "' but the supplied OrgInfo identifies '" + org.org_id +
                                        "' — refusing to emit a filing that mixes two organizations' data");
        }

        // income_tiyn/rate_bp are read defensively (default 0) — they are
        // auxiliary annotations from calculate_snr's own result_snapshot
        // shape, not part of Calculation's guaranteed contract, unlike
        // total_tiyn below (which every Calculation row has, per its
        // NOT NULL column) and which is therefore the sole source of truth
        // for the actual tax amount.
        const long long income_tiyn = calc.result_snapshot.value("income_tiyn", 0LL);
        const long long rate_bp = calc.result_snapshot.value("rate_bp", 0LL);

        std::string sheet;
        sheet += FnoXml::identity_fields(org);
        sheet += FnoXml::field("period_year", org.tax_period_year);
        sheet += FnoXml::field("period_half_year", org.tax_period_half);
        sheet += FnoXml::field("currency_code", std::string("KZT"));
        // This generator always emits an "очередная" (regular) declaration
        // — Calculation carries no signal distinguishing a corrective
        // (dt_additional) or notice-triggered (dt_notice) filing, so those
        // variants are out of scope rather than guessed.
        sheet += FnoXml::field("dt_regular", std::string("true"));
        sheet += FnoXml::field("field_910_00_001", FnoXml::round_half_up_to_tenge(income_tiyn));

        // Deliberately NOT further field_910_00_NNN codes — see file
        // header's Step 1 finding 2. The actual tax figures live here
        // instead, in an explicitly non-official element.
        std::string summary = "<calcSummary xmlns=\"urn:cyber-accountant:fno-summary\">";
        summary += "<incomeTenge>" + FnoXml::tenge_amount(income_tiyn) + "</incomeTenge>";
        summary += "<rateBp>" + std::to_string(rate_bp) + "</rateBp>";
        summary += "<taxTenge>" + FnoXml::tenge_amount(calc.total_tiyn) + "</taxTenge>";
        summary += "<periodFrom>" + FnoXml::escape(calc.period_from) + "</periodFrom>";
        summary += "<periodTo>" + FnoXml::escape(calc.period_to) + "</periodTo>";
        summary += "</calcSummary>";

        std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
        xml += "<fno code=\"" + std::string(kFormCode) + "\" version=\"" + std::string(kFormVersion) +
               "\" formatVersion=\"1\" schemaValidated=\"false\" bin=\"" + FnoXml::escape(org.bin) + "\">";
        xml += "<form name=\"form_910_00\"><sheetGroup><sheet name=\"page_910_00_01\">" + sheet +
               "</sheet></sheetGroup></form>";
        xml += summary;
        xml += "</fno>";
        return xml;
    }
};

}  // namespace Tax
