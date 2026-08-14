/**
 * @file TaxFilingRepository.hpp
 * @brief All SQL touching `tax_filings` (migrations/016_tax_filings.sql) lives
 *        here — design spec §7.2, Task 12.
 *
 * Org-scoped, so this extends Tenancy::OrgCrudBase for
 * find_in_org/list_in_org/count_in_org, exactly like
 * Tax::TaxCalculationRepository next door. Unlike that repository there is NO
 * upsert: a filing is an EVENT (this ФНО was formed at this instant), not an
 * idempotent recomputation of a period — see the migration's header for why
 * there is deliberately no UNIQUE(org_id, kind, period_from, period_to) to
 * upsert against.
 *
 * `create()` writes every column in one INSERT (including xml_s3_key /
 * document_id / status), rather than an insert-then-patch pair: the API
 * handler already knows whether the XML landed in storage and which document
 * carries the printable form BEFORE it writes the row (see
 * Api::TaxController::createFiling's ordering), so there is no
 * "known-only-later" column of the kind that forced
 * Ledger::DocumentRepository's set_pending_upload/set_file split.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "tax/TaxFiling.hpp"
#include "tenancy/OrgScoped.hpp"

namespace Tax {

class TaxFilingRepository : public Tenancy::OrgCrudBase<TaxFilingRepository, Filing, std::string> {
public:
    // OrgCrudBase contract — supplies find_in_org(id,org_id) /
    // list_in_org(org_id,limit,offset) / count_in_org(org_id) against
    // `tax_filings`.
    static constexpr const char* kTable = "tax_filings";
    static constexpr const char* kColumns =
        "id, org_id, kind, period_from, period_to, status, calculation_id, xml_s3_key, document_id, "
        "schema_validated, created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "created_at DESC";
    static constexpr const char* kOrgColumn = "org_id";

    /**
     * @brief Insert one filing. A `calculation_id` or `document_id` from
     *        another organization cannot be written at all — both composite
     *        FKs in migration 016 reject it as SQLSTATE 23503 — so this
     *        method does no EXISTS pre-check of its own (same posture as
     *        Ledger::DocumentRepository::link_entry). The API layer still
     *        pre-checks `calculation_id` to answer 422 instead of 500 for a
     *        caller-correctable mistake.
     */
    Filing create(const std::string& org_id,
                  const std::string& kind,
                  const std::string& period_from,
                  const std::string& period_to,
                  const std::string& status,
                  const std::string& calculation_id,
                  const std::optional<std::string>& xml_s3_key,
                  const std::optional<std::string>& document_id,
                  bool schema_validated) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "INSERT INTO tax_filings (org_id, kind, period_from, period_to, status, calculation_id, "
                "xml_s3_key, document_id, schema_validated) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9) RETURNING " +
                    std::string(kColumns),
                org_id,
                kind,
                period_from,
                period_to,
                status,
                calculation_id,
                xml_s3_key,
                document_id,
                schema_validated);
            return Filing::from_row(r[0]);
        });
    }

    /// Filings for @p org_id, newest first, optionally narrowed to one form
    /// `kind`. Paired with count_filtered() below so `GET /tax/filings`'
    /// pagination `total` always agrees with the page it labels — same
    /// rationale as Ledger::DocumentRepository::list_filtered.
    std::vector<Filing> list_filtered(const std::string& org_id,
                                      const std::optional<std::string>& kind,
                                      int limit,
                                      int offset) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT " + std::string(kColumns) +
                                         " FROM tax_filings WHERE org_id = $1 "
                                         "AND ($2::text IS NULL OR kind = $2) "
                                         "ORDER BY " +
                                         std::string(kOrderBy) + " LIMIT $3 OFFSET $4",
                                     org_id,
                                     kind,
                                     limit,
                                     offset);
            std::vector<Filing> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Filing::from_row(row));
            return out;
        });
    }

    /// Total row count for the same `kind` filter list_filtered() applies.
    long count_filtered(const std::string& org_id, const std::optional<std::string>& kind) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT COUNT(*) FROM tax_filings WHERE org_id = $1 AND ($2::text IS NULL OR kind = $2)", org_id, kind);
            return r.at(0).at(0).template as<long>();
        });
    }
};

}  // namespace Tax
