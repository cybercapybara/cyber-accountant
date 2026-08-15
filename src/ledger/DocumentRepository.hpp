/**
 * @file DocumentRepository.hpp
 * @brief All SQL touching `documents` / `document_entries` lives here.
 *
 * P3 (migrations/018_document_versions.sql): the file metadata and the
 * docgen input snapshot moved OUT of `documents` into `document_versions`.
 * `documents.current_version_id` points at the published version, so every
 * read here goes through a LEFT JOIN to it (kTable below) and
 * Ledger::Document's file fields are populated from that version, not from
 * the document row. Editing a document produces a NEW version — the
 * previous PDF stays in S3 — so there is no method that overwrites a
 * version's file in place under a document id: writes address a
 * `version_id` (set_version_file), and publishing is a separate, explicit
 * step (set_current_version).
 *
 * Org-scoped (design spec §5: "методов 'выбрать без org' не существует"), so
 * this extends Tenancy::OrgCrudBase rather than Repositories::CrudBase —
 * find_in_org/list_in_org/count_in_org come from the base (reads against
 * that join), and create/add_version/set_status/link_entry/list_for_entry
 * are the bespoke queries these tables need. Mirrors
 * Ledger::CounterpartyRepository's overall shape, but unlike that repository
 * none of these writes throw a domain 409 — see link_entry()'s doc comment
 * for the one place a constraint violation is handled, and why it returns
 * `false` instead.
 *
 * P3 task 9: two cross-org outcomes that used to reach the caller as raw
 * SQLSTATEs (i.e. 500s) are now ordinary domain answers, because HTTP routes
 * sit in front of both — add_version() throws
 * Repositories::NotFoundError("document") instead of letting the composite FK
 * raise 23503, and set_current_version() returns `false` instead of letting
 * the DEFERRABLE documents_current_version_fk fail at COMMIT. See each
 * method's comment for the exact mechanism.
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
#include "ledger/DocumentVersion.hpp"
#include "repositories/RepoErrors.hpp"
#include "tenancy/OrgScoped.hpp"

namespace Ledger {

class DocumentRepository : public Tenancy::OrgCrudBase<DocumentRepository, Document, std::string> {
public:
    // OrgCrudBase contract — supplies find_in_org(id,org_id) /
    // list_in_org(org_id,limit,offset) / count_in_org(org_id). document_entries
    // link rows are never selected through this base (list_for_entry below is
    // the bespoke join for that).
    //
    // OrgCrudBase substitutes kTable into FROM verbatim, so the join to the
    // current version lives here and kIdColumn/kOrgColumn/kOrderBy are
    // qualified with the `d` alias. The file metadata comes from the CURRENT
    // version (v), not from the document row — `documents` has no such
    // columns of its own any more (migrations/018_document_versions.sql).
    // LEFT, not INNER: a document whose first render has not finished has no
    // current version yet and must still be listable.
    static constexpr const char* kTable = "documents d LEFT JOIN document_versions v ON v.id = d.current_version_id";
    static constexpr const char* kColumns =
        "d.id, d.org_id, d.doc_type, d.source, d.status, d.counterparty_id, "
        "v.s3_key, v.checksum_sha256, v.mime, v.size_bytes, "
        "d.template_slug, v.template_version, v.input_snapshot, "
        "d.current_version_id, "
        "COALESCE((SELECT MAX(vv.version_no) FROM document_versions vv WHERE vv.document_id = d.id), 0) "
        "AS latest_version_no, "
        "d.created_at, d.updated_at";
    static constexpr const char* kIdColumn = "d.id";
    static constexpr const char* kOrderBy = "d.created_at DESC";
    static constexpr const char* kOrgColumn = "d.org_id";

    /// Column list of `document_versions`, unqualified — every query using it
    /// selects from that table alone.
    static constexpr const char* kVersionColumns =
        "id, org_id, document_id, version_no, s3_key, checksum_sha256, mime, size_bytes, template_version, "
        "input_snapshot, created_by_user_id, created_at, updated_at";

    /**
     * @brief Insert a new document for @p org_id. No UNIQUE constraint to
     *        translate — the brief is explicit that a document number is not
     *        globally unique in P1 — so this never throws a domain
     *        exception; a genuinely malformed insert (bad doc_type/source/
     *        status, or a counterparty_id from another org tripping the
     *        FK — the latter is the same "let SQLSTATE surface raw" posture
     *        AccountRepository::create_subaccount rejects BEFORE it reaches
     *        SQL) bubbles up as a raw pqxx::sql_error.
     *
     *        A document is never versionless: this inserts the row AND its
     *        version 1 in ONE transaction, so `template_version`/
     *        `input_snapshot` land on that version. `current_version_id` is
     *        deliberately NOT set — the pointer moves only once a file
     *        actually exists (the render job, or set_pending_upload for the
     *        upload path, which has no async job to move it later). Until
     *        then download-url honestly answers 409 no_file.
     */
    Document create(const std::string& org_id,
                    const std::string& doc_type,
                    const std::string& source,
                    const std::string& status,
                    std::optional<std::string> counterparty_id = std::nullopt,
                    std::optional<std::string> template_slug = std::nullopt,
                    std::optional<std::string> template_version = std::nullopt,
                    std::optional<nlohmann::json> input_snapshot = std::nullopt,
                    std::optional<std::string> created_by_user_id = std::nullopt) {
        std::optional<std::string> snapshot_text;
        if (input_snapshot)
            snapshot_text = input_snapshot->dump();

        return Database::get().execute_write([&](auto& txn) {
            auto d = txn.exec_params(
                "INSERT INTO documents (org_id, doc_type, source, status, counterparty_id, template_slug) "
                "VALUES ($1, $2, $3, $4, $5, $6) RETURNING id",
                org_id,
                doc_type,
                source,
                status,
                counterparty_id,
                template_slug);
            const std::string document_id = d[0][0].template as<std::string>();
            txn.exec_params(
                "INSERT INTO document_versions (org_id, document_id, version_no, template_version, input_snapshot, "
                "created_by_user_id) VALUES ($1, $2, 1, $3, $4::jsonb, $5)",
                org_id,
                document_id,
                template_version,
                snapshot_text,
                created_by_user_id);
            auto r = txn.exec_params(
                std::string("SELECT ") + kColumns + " FROM " + kTable + " WHERE d.id = $1 AND d.org_id = $2",
                document_id,
                org_id);
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
     * not regenerate it. But set_version_file() (below) requires
     * checksum_sha256 AND size_bytes together with s3_key/mime, and neither
     * is known until the client's PUT has actually finished — calling it at
     * upload-start time would force fabricating a checksum/size for bytes
     * that don't exist yet. Splitting the write in two is the minimal fix:
     * this method fills s3_key + mime only (checksum_sha256/size_bytes stay
     * NULL), and confirm-upload's later set_version_file() call fills in the
     * rest once the object is verified to exist (Storage::exists()).
     *
     * P3: the write lands on the document's LATEST version (version 1 for
     * every document this endpoint creates) and, unlike the render path,
     * publishes it in the SAME transaction — see the comment on the second
     * statement below.
     *
     * Invariant (fix round 1): a pending-upload key may only be written
     * while the document is still 'draft'. Without this guard, calling
     * set_pending_upload() on an already-'final' (or any non-draft)
     * document would silently overwrite its real, confirmed s3_key/mime
     * with a fresh presigned-but-unconfirmed pair — today's only caller
     * (LedgerDocumentsController::startUpload, right after repo.create()
     * with status='draft') never does this, but nothing enforced it, which
     * is exactly the kind of implicit invariant a future caller (e.g. a
     * re-upload/replace-file feature) could violate by accident. The
     * `AND status = 'draft'` below makes that violation a no-op (returns
     * false) instead of a silent data-loss bug.
     *
     * @return false if no row matches (id, org_id, status='draft') all
     *         three — a wrong org, a missing id, and a non-draft document
     *         are all indistinguishable from each other here, same
     *         "can't tell why, only that nothing was written" contract as
     *         set_version_file()/set_status() below.
     */
    bool set_pending_upload(const std::string& org_id,
                            const std::string& id,
                            const std::string& s3_key,
                            const std::string& mime) {
        return Database::get().execute_write([&](auto& txn) {
            auto v = txn.exec_params(
                "UPDATE document_versions v SET s3_key = $3, mime = $4 "
                "  FROM documents d "
                " WHERE v.document_id = d.id AND v.org_id = d.org_id "
                "   AND d.id = $1 AND d.org_id = $2 AND d.status = 'draft' "
                "   AND v.version_no = (SELECT MAX(vv.version_no) FROM document_versions vv "
                "                        WHERE vv.document_id = d.id) "
                "RETURNING v.id",
                id,
                org_id,
                s3_key,
                mime);
            if (v.empty())
                return false;
            // The pointer moves RIGHT NOW — unlike the render path, where it
            // waits for the job to land the PDF. An uploaded document has no
            // async job at all, and confirm-upload reads the key through
            // `documents`, i.e. through the CURRENT version: without this
            // statement every upload would answer 409 no_pending_upload.
            // Same transaction as the version write, so the two can never
            // disagree; documents_current_version_fk is DEFERRABLE, which is
            // what makes writing the pointer alongside its target legal.
            txn.exec_params("UPDATE documents SET current_version_id = $3 WHERE id = $1 AND org_id = $2",
                            id,
                            org_id,
                            v[0][0].template as<std::string>());
            return true;
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
            auto r = txn.exec_params("SELECT " + std::string(kColumns) + " FROM " + std::string(kTable) +
                                         " WHERE d.org_id = $1 "
                                         "AND ($2::text IS NULL OR d.doc_type = $2) "
                                         "AND ($3::text IS NULL OR d.status = $3) "
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
    /// Counts documents, so it deliberately does NOT join document_versions:
    /// every column it touches still lives on `documents`, and the join
    /// (one row per document either way, LEFT on a unique id) could only
    /// cost time, never change the number.
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
     * @brief Append a new version to @p document_id and return it.
     *
     * The number is computed inside the INSERT (`MAX(version_no) + 1` over
     * this document's rows) rather than by a read followed by a write, so
     * two concurrent edits cannot both decide they are version N —
     * UNIQUE(document_id, version_no) turns the loser into a 23505 instead
     * of a silent overwrite. The scalar subquery over an empty set yields
     * NULL, so COALESCE makes the first version 1.
     *
     * P3 task 9 — cross-org call is a DOMAIN error, not a raw SQLSTATE.
     * The source row of the INSERT is now the `documents` row itself,
     * selected `WHERE d.id = $2 AND d.org_id = $1`: a document belonging to
     * another tenant (or none at all) selects nothing, so the INSERT writes
     * nothing and RETURNING comes back empty, which this method turns into
     * Repositories::NotFoundError("document") — the 404 that
     * Api::with_repo_errors() already maps, matching what find_in_org()
     * would have answered for the same id. Before this, the composite FK
     * `(document_id, org_id) -> documents(id, org_id)` was the only wall
     * and a cross-org id surfaced as a raw pqxx::sql_error (SQLSTATE 23503)
     * — i.e. a 500 — which is not an acceptable answer once an HTTP route
     * (POST /documents/{id}/versions) sits in front of it. `org_id` and
     * `document_id` are taken from that same authoritative row rather than
     * from the parameters, so the pair written can never disagree with the
     * document it belongs to.
     *
     * @throws Repositories::NotFoundError if @p document_id does not exist
     *         in @p org_id.
     */
    DocumentVersion add_version(const std::string& org_id,
                                const std::string& document_id,
                                std::optional<nlohmann::json> input_snapshot,
                                std::optional<std::string> template_version,
                                std::optional<std::string> created_by_user_id) {
        std::optional<std::string> snapshot_text;
        if (input_snapshot)
            snapshot_text = input_snapshot->dump();
        auto inserted = Database::get().execute_write([&](auto& txn) -> std::optional<DocumentVersion> {
            auto r = txn.exec_params(
                "INSERT INTO document_versions (org_id, document_id, version_no, template_version, input_snapshot, "
                "created_by_user_id) "
                "SELECT d.org_id, d.id, "
                "       COALESCE((SELECT MAX(v.version_no) FROM document_versions v "
                "                  WHERE v.document_id = d.id), 0) + 1, "
                "       $3::text, $4::jsonb, $5::uuid "
                "  FROM documents d WHERE d.id = $2::uuid AND d.org_id = $1::uuid "
                "RETURNING " +
                    std::string(kVersionColumns),
                org_id,
                document_id,
                template_version,
                snapshot_text,
                created_by_user_id);
            if (r.empty())
                return std::nullopt;
            return DocumentVersion::from_row(r[0]);
        });
        if (!inserted)
            throw Repositories::NotFoundError("document");
        return *inserted;
    }

    /// Every version of @p document_id, oldest first. Org-scoped on the
    /// version row itself (whose org_id the composite FK pins to its
    /// document's), so a document id from another tenant yields an empty
    /// list rather than someone else's history.
    std::vector<DocumentVersion> list_versions(const std::string& org_id, const std::string& document_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(std::string("SELECT ") + kVersionColumns +
                                         " FROM document_versions WHERE org_id = $1 AND document_id = $2 "
                                         "ORDER BY version_no",
                                     org_id,
                                     document_id);
            std::vector<DocumentVersion> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(DocumentVersion::from_row(row));
            return out;
        });
    }

    /// One version by its number within @p document_id, or nullopt.
    std::optional<DocumentVersion> find_version(const std::string& org_id,
                                                const std::string& document_id,
                                                int version_no) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<DocumentVersion> {
            auto r = txn.exec_params(std::string("SELECT ") + kVersionColumns +
                                         " FROM document_versions "
                                         "WHERE org_id = $1 AND document_id = $2 AND version_no = $3",
                                     org_id,
                                     document_id,
                                     version_no);
            if (r.empty())
                return std::nullopt;
            return DocumentVersion::from_row(r[0]);
        });
    }

    /// The highest-numbered version of @p document_id — the one an edit or a
    /// render is currently working on, which is NOT necessarily the current
    /// (published) one.
    std::optional<DocumentVersion> latest_version(const std::string& org_id, const std::string& document_id) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<DocumentVersion> {
            auto r = txn.exec_params(std::string("SELECT ") + kVersionColumns +
                                         " FROM document_versions WHERE org_id = $1 AND document_id = $2 "
                                         "ORDER BY version_no DESC LIMIT 1",
                                     org_id,
                                     document_id);
            if (r.empty())
                return std::nullopt;
            return DocumentVersion::from_row(r[0]);
        });
    }

    /**
     * @brief Attach S3 file metadata to ONE version, addressed by
     *        @p version_id (not by document id — the whole point of
     *        versioning is that "the document's file" is never overwritten).
     * @return false if no row matches (id, org_id) both — the standard
     *         OrgCrudBase-style "wrong org is indistinguishable from
     *         missing" contract, not a separate 403 branch.
     */
    bool set_version_file(const std::string& org_id,
                          const std::string& version_id,
                          const std::string& s3_key,
                          const std::string& checksum_sha256,
                          const std::string& mime,
                          long long size_bytes) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE document_versions SET s3_key = $3, checksum_sha256 = $4, mime = $5, size_bytes = $6 "
                "WHERE id = $1 AND org_id = $2 "
                "RETURNING id",
                version_id,
                org_id,
                s3_key,
                checksum_sha256,
                mime,
                size_bytes);
            return !r.empty();
        });
    }

    /**
     * @brief Publish @p version_id: make it the version every read of the
     *        document sees. Separate from set_version_file() on purpose —
     *        a version becomes visible only once its bytes are durably in
     *        object storage.
     * @return false if no document matches (id, org_id) both, OR if
     *         @p version_id is not a version OF THAT document in that org.
     *
     * P3 task 9 — the EXISTS predicate is the fix, not decoration.
     * documents_current_version_fk pins (current_version_id, org_id), so a
     * foreign version could never actually be published; but the constraint
     * is DEFERRABLE, so the UPDATE used to SUCCEED at statement time and
     * only die at COMMIT with a raw 23503 — i.e. the caller got a 500 for
     * what is an ordinary "no such version here" outcome, and got it from a
     * throw the calling handler could not attribute to this statement.
     * Checking the version's own (id, org_id, document_id) up front turns
     * that into this method's normal `false`, the same contract
     * set_version_file()/set_status() already have.
     */
    bool set_current_version(const std::string& org_id, const std::string& document_id, const std::string& version_id) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE documents SET current_version_id = $3 "
                " WHERE id = $1 AND org_id = $2 "
                "   AND EXISTS (SELECT 1 FROM document_versions v "
                "                WHERE v.id = $3 AND v.org_id = $2 AND v.document_id = $1) "
                "RETURNING id",
                document_id,
                org_id,
                version_id);
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
            auto r = txn.exec_params("SELECT " + std::string(kColumns) +
                                         " FROM documents d "
                                         "LEFT JOIN document_versions v ON v.id = d.current_version_id "
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
