/**
 * @file DocumentRepository.hpp
 * @brief All SQL touching `documents` / `document_entries` lives here.
 *
 * Org-scoped (design spec §5: "методов 'выбрать без org' не существует"), so
 * this extends Tenancy::OrgCrudBase rather than Repositories::CrudBase —
 * find_in_org/list_in_org/count_in_org come from the base (reads against
 * `documents` only), and create/set_file/set_status/link_entry/
 * list_for_entry are the bespoke queries this table needs. Mirrors
 * Ledger::CounterpartyRepository's overall shape, but unlike that repository
 * none of these writes throw a domain 409 — see link_entry()'s doc comment
 * for the one place a constraint violation is handled, and why it returns
 * `false` instead.
 *
 * `status` values are NOT validated here — migrations/010_documents.sql's
 * CHECK constraint is the source of truth for the allowed set (spanning both
 * the generated and the inbound lifecycle), and it is the API layer's job
 * (Task 12/13) to only ever request a transition sensible for a given
 * document. A caller passing a value outside the CHECK list gets a raw
 * pqxx::sql_error (SQLSTATE 23514), not a typed exception — this repository
 * trusts its caller on that axis, same posture the brief calls out.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "database/Database.hpp"
#include "ledger/Document.hpp"
#include "tenancy/OrgScoped.hpp"

namespace Ledger {

class DocumentRepository : public Tenancy::OrgCrudBase<DocumentRepository, Document, std::string> {
public:
    // OrgCrudBase contract — supplies find_in_org(id,org_id) /
    // list_in_org(org_id,limit,offset) / count_in_org(org_id) against
    // `documents`. document_entries link rows are never selected through
    // this base (list_for_entry below is the bespoke join for that).
    static constexpr const char* kTable = "documents";
    static constexpr const char* kColumns =
        "id, org_id, doc_type, source, status, counterparty_id, s3_key, checksum_sha256, mime, size_bytes, "
        "template_slug, template_version, input_snapshot, created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "created_at DESC";
    static constexpr const char* kOrgColumn = "org_id";

    /// Same column list as kColumns, qualified with the `d` alias used by
    /// list_for_entry()'s join — Document::from_row reads columns by their
    /// (unqualified) result name regardless, but the query text itself needs
    /// the alias to disambiguate `documents.id`/`document_entries.id` etc.
    static constexpr const char* kColumnsAliased =
        "d.id, d.org_id, d.doc_type, d.source, d.status, d.counterparty_id, d.s3_key, d.checksum_sha256, d.mime, "
        "d.size_bytes, d.template_slug, d.template_version, d.input_snapshot, d.created_at, d.updated_at";

    /**
     * @brief Insert a new document for @p org_id. No UNIQUE constraint to
     *        translate — the brief is explicit that a document number is not
     *        globally unique in P1 — so this never throws a domain
     *        exception; a genuinely malformed insert (bad doc_type/source/
     *        status, or a counterparty_id from another org tripping the
     *        FK — the latter is the same "let SQLSTATE surface raw" posture
     *        AccountRepository::create_subaccount rejects BEFORE it reaches
     *        SQL) bubbles up as a raw pqxx::sql_error.
     */
    Document create(const std::string& org_id,
                    const std::string& doc_type,
                    const std::string& source,
                    const std::string& status,
                    std::optional<std::string> counterparty_id = std::nullopt,
                    std::optional<std::string> template_slug = std::nullopt,
                    std::optional<std::string> template_version = std::nullopt,
                    std::optional<nlohmann::json> input_snapshot = std::nullopt) {
        std::optional<std::string> snapshot_text;
        if (input_snapshot)
            snapshot_text = input_snapshot->dump();

        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "INSERT INTO documents (org_id, doc_type, source, status, counterparty_id, template_slug, "
                "template_version, input_snapshot) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, $8::jsonb) "
                "RETURNING " +
                    std::string(kColumns),
                org_id,
                doc_type,
                source,
                status,
                counterparty_id,
                template_slug,
                template_version,
                snapshot_text);
            return Document::from_row(r[0]);
        });
    }

    /**
     * @brief Task 12 addition: persist the s3_key (+ mime) a presigned PUT
     *        was minted for, BEFORE the client's upload completes.
     *
     * Why this exists: `POST /documents/uploads` (LedgerDocumentsController)
     * mints one presigned PUT URL and must hand back a document whose
     * `s3_key` already matches that URL — a client that reloads the page, or
     * retries `confirm-upload`, needs to read the SAME key back off the row,
     * not regenerate it. But set_file() (below) requires checksum_sha256 AND
     * size_bytes together with s3_key/mime, and neither is known until the
     * client's PUT has actually finished — calling set_file() at upload-start
     * time would force fabricating a checksum/size for bytes that don't
     * exist yet. Splitting the write in two is the minimal fix: this method
     * fills s3_key + mime only (checksum_sha256/size_bytes stay NULL), and
     * confirm-upload's later set_file() call fills in the rest once the
     * object is verified to exist (Storage::exists()).
     *
     * @return false if no row matches (id, org_id) both — same
     *         "wrong org is indistinguishable from missing" contract as
     *         set_file()/set_status() below.
     */
    bool set_pending_upload(const std::string& org_id,
                            const std::string& id,
                            const std::string& s3_key,
                            const std::string& mime) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE documents SET s3_key = $3, mime = $4 WHERE id = $1 AND org_id = $2 RETURNING id",
                id,
                org_id,
                s3_key,
                mime);
            return !r.empty();
        });
    }

    /**
     * @brief Task 12 addition: `list_in_org`/`count_in_org` (OrgCrudBase)
     *        take no filters, but `GET /documents` needs allowlisted
     *        `?type=&status=` filters WITH accurate pagination totals — an
     *        in-memory filter-after-fetch would desync `total` from the
     *        filtered page. @p doc_type / @p status are nullopt for "no
     *        filter on this column"; the controller is responsible for
     *        allowlisting them against migrations/010_documents.sql's CHECK
     *        lists before calling this (see that file's header: this
     *        repository trusts its caller on CHECK-shaped values).
     */
    std::vector<Document> list_filtered(const std::string& org_id,
                                        const std::optional<std::string>& doc_type,
                                        const std::optional<std::string>& status,
                                        int limit,
                                        int offset) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT " + std::string(kColumns) +
                                         " FROM documents WHERE org_id = $1 "
                                         "AND ($2::text IS NULL OR doc_type = $2) "
                                         "AND ($3::text IS NULL OR status = $3) "
                                         "ORDER BY " +
                                         std::string(kOrderBy) + " LIMIT $4 OFFSET $5",
                                     org_id,
                                     doc_type,
                                     status,
                                     limit,
                                     offset);
            std::vector<Document> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Document::from_row(row));
            return out;
        });
    }

    /// Total row count for the same (@p doc_type, @p status) filter
    /// `list_filtered` uses — kept as a matching pair so `GET /documents`'s
    /// pagination `total` always agrees with the filtered page it labels.
    long count_filtered(const std::string& org_id,
                        const std::optional<std::string>& doc_type,
                        const std::optional<std::string>& status) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT COUNT(*) FROM documents WHERE org_id = $1 "
                "AND ($2::text IS NULL OR doc_type = $2) "
                "AND ($3::text IS NULL OR status = $3)",
                org_id,
                doc_type,
                status);
            return r.at(0).at(0).template as<long>();
        });
    }

    /**
     * @brief Attach S3 file metadata to a document already scoped to
     *        @p org_id.
     * @return false if no row matches (id, org_id) both — the standard
     *         OrgCrudBase-style "wrong org is indistinguishable from
     *         missing" contract, not a separate 403 branch.
     */
    bool set_file(const std::string& org_id,
                  const std::string& id,
                  const std::string& s3_key,
                  const std::string& checksum_sha256,
                  const std::string& mime,
                  long long size_bytes) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE documents SET s3_key = $3, checksum_sha256 = $4, mime = $5, size_bytes = $6 "
                "WHERE id = $1 AND org_id = $2 "
                "RETURNING id",
                id,
                org_id,
                s3_key,
                checksum_sha256,
                mime,
                size_bytes);
            return !r.empty();
        });
    }

    /**
     * @brief Set a document's status. The new value is trusted as-is (see
     *        file header) — this repository does not encode a state
     *        machine, only migrations/010_documents.sql's CHECK does.
     * @return false if no row matches (id, org_id) both.
     */
    bool set_status(const std::string& org_id, const std::string& id, const std::string& status) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE documents SET status = $3 WHERE id = $1 AND org_id = $2 RETURNING id", id, org_id, status);
            return !r.empty();
        });
    }

    /**
     * @brief Link a document to a journal entry (design spec §6.4's
     *        many-to-many). Both objects must belong to @p org_id — enforced
     *        entirely at the SQL level by document_entries' two composite
     *        FKs (migrations/010_documents.sql), so there is deliberately NO
     *        application-side EXISTS pre-check here: a document from one org
     *        paired with an entry from another cannot satisfy both
     *        `(document_id, org_id) -> documents(id, org_id)` and
     *        `(entry_id, org_id) -> journal_entries(id, org_id)`
     *        simultaneously, so the INSERT itself fails with SQLSTATE 23503
     *        (foreign_key_violation).
     *
     *        Deliberately NOT using Repositories::detail::translate_sql: that
     *        helper's contract is "translate a SQLSTATE into a thrown domain
     *        exception, else rethrow the original" — it has no path that
     *        swallows the error into a returned value. A cross-org link
     *        attempt is an expected, caller-correctable outcome (not a 409
     *        conflict to surface as an exception up through
     *        Api::with_repo_errors()), so this method catches
     *        pqxx::sql_error directly and returns false for both
     *        foreign_key_violation (23503, the cross-org case above) and
     *        unique_violation (23505, re-linking an already-linked pair —
     *        UNIQUE(document_id, entry_id)) — anything else (a genuinely
     *        unexpected SQL error) is rethrown unchanged.
     * @return true if the link was created, false if either FK rejected it
     *         or the pair was already linked.
     */
    bool link_entry(const std::string& org_id, const std::string& document_id, const std::string& entry_id) {
        try {
            return Database::get().execute_write([&](auto& txn) {
                txn.exec_params("INSERT INTO document_entries (org_id, document_id, entry_id) VALUES ($1, $2, $3)",
                                org_id,
                                document_id,
                                entry_id);
                return true;
            });
        } catch (const pqxx::sql_error& e) {
            const std::string_view sqlstate(e.sqlstate());
            if (sqlstate == "23503" || sqlstate == "23505")
                return false;
            throw;
        }
    }

    /// Documents linked to @p entry_id within @p org_id, newest first. The
    /// join is scoped by document_entries.org_id (not just document_id/
    /// entry_id) so a link row can never surface a document from another
    /// tenant even in principle — belt-and-braces alongside the FK-level
    /// isolation link_entry() relies on.
    std::vector<Document> list_for_entry(const std::string& org_id, const std::string& entry_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT " + std::string(kColumnsAliased) +
                                         " FROM documents d "
                                         "JOIN document_entries de ON de.document_id = d.id "
                                         "WHERE de.org_id = $1 AND de.entry_id = $2 "
                                         "ORDER BY d.created_at DESC",
                                     org_id,
                                     entry_id);
            std::vector<Document> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Document::from_row(row));
            return out;
        });
    }
};

}  // namespace Ledger
