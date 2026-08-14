/**
 * @file TaxFiling.hpp
 * @brief Domain row for `tax_filings` (migrations/016_tax_filings.sql —
 *        design spec §7.2, Task 12): one generated ФНО — the XML artifact in
 *        object storage plus the printable document rendered from it.
 *
 * Domain-only — no SQL here; persistence lives in
 * src/tax/TaxFilingRepository.hpp. Same from_row/to_json idioms as
 * src/tax/TaxCalculation.hpp: from_row is a templated static factory, to_json
 * is a free function found via ADL, and every nullable column becomes a
 * std::optional<...> that still renders as an explicit JSON `null` so the
 * response shape is stable.
 *
 * `kind` is the FORM CODE ("910.00"/"300.00"), deliberately a different
 * vocabulary from Tax::CalculationKind ('snr_simplified'/'vat') — see the
 * migration's header for why the two are not merged.
 */

#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace Tax {

/// The closed set of `tax_filings.kind` values enforced by the migration's
/// CHECK constraint — one per ФНО generator (Tax::Fno910 / Tax::Fno300).
namespace FilingKind {
inline constexpr const char* kFno910 = "910.00";
inline constexpr const char* kFno300 = "300.00";
}  // namespace FilingKind

/// The closed set of `tax_filings.status` values — see the migration header
/// for what each one means and why 'submitted_manually' has no endpoint yet.
namespace FilingStatus {
inline constexpr const char* kDraft = "draft";
inline constexpr const char* kGenerated = "generated";
inline constexpr const char* kSubmittedManually = "submitted_manually";
}  // namespace FilingStatus

struct Filing {
    std::string id;
    std::string org_id;
    std::string kind;  // '910.00' | '300.00'
    std::string period_from;
    std::string period_to;
    std::string status;  // 'draft' | 'generated' | 'submitted_manually'
    std::string calculation_id;
    std::optional<std::string> xml_s3_key;
    std::optional<std::string> document_id;
    bool schema_validated = false;
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Filing from_row(const Row& row) {
        Filing f;
        f.id = row["id"].template as<std::string>();
        f.org_id = row["org_id"].template as<std::string>();
        f.kind = row["kind"].template as<std::string>();
        f.period_from = row["period_from"].template as<std::string>();
        f.period_to = row["period_to"].template as<std::string>();
        f.status = row["status"].template as<std::string>();
        f.calculation_id = row["calculation_id"].template as<std::string>();
        if (!row["xml_s3_key"].is_null())
            f.xml_s3_key = row["xml_s3_key"].template as<std::string>();
        if (!row["document_id"].is_null())
            f.document_id = row["document_id"].template as<std::string>();
        f.schema_validated = row["schema_validated"].template as<bool>();
        f.created_at = row["created_at"].template as<std::string>();
        f.updated_at = row["updated_at"].template as<std::string>();
        return f;
    }
};

inline void to_json(nlohmann::json& j, const Filing& f) {
    j = nlohmann::json{
        {"id", f.id},
        {"org_id", f.org_id},
        {"kind", f.kind},
        {"period_from", f.period_from},
        {"period_to", f.period_to},
        {"status", f.status},
        {"calculation_id", f.calculation_id},
        {"xml_s3_key", f.xml_s3_key ? nlohmann::json(*f.xml_s3_key) : nlohmann::json(nullptr)},
        {"document_id", f.document_id ? nlohmann::json(*f.document_id) : nlohmann::json(nullptr)},
        {"schema_validated", f.schema_validated},
        {"created_at", f.created_at},
        {"updated_at", f.updated_at},
    };
}

}  // namespace Tax
