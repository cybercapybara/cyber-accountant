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
 * P3 task 11: удаление и аннулирование — remove() и void_document(). Что
 * из двух допустимо, решает НЕ статус, а has_posted_entry_link(): документ,
 * висящий на проведённой (или сторнированной) проводке, физически удалить
 * нельзя никогда — журнал append-only и правится только сторно. Аннулирование
 * живёт в колонках voided_at/voided_by_user_id/void_reason
 * (migrations/019_document_voiding.sql), а не в значении `status`, чтобы не
 * стирать, был документ 'final' или 'sent'. Объекты в S3 ни один из двух путей
 * не удаляет — политика хранения записана в заголовке той миграции.
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

/// Можно ли класть результат рендера в эту версию — ответ
/// DocumentRepository::version_render_state() ниже.
enum class VersionRenderState {
    kMissing,     ///< версии нет в этой организации (или документ удалён)
    kRenderable,  ///< самая новая версия живого документа без файла — результат принимается
    kAlreadyRendered,  ///< в версии УЖЕ лежит файл — повтор джобы не перезаписывает его
    kSuperseded,  ///< поверх неё уже создана более новая версия — джоба no-op
    kVoided,      ///< документ аннулирован — джоба no-op
};

/// Исход DocumentRepository::remove(). Не bool: «нет такого документа» и
/// «удалять нельзя» — разные ответы HTTP (404 против 409), и разные причины
/// «нельзя» тоже различимы, потому что подсказка пользователю у них разная.
enum class DeleteOutcome {
    kDeleted,   ///< строка физически удалена вместе с версиями и связями
    kNotFound,  ///< документа нет в этой организации
    kHasPostedEntries,  ///< есть связь с проведённой/сторнированной проводкой — только аннулирование
    kReferenced,  ///< на документ ссылается приказ или налоговая форма — только аннулирование
};

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
        "d.voided_at, d.voided_by_user_id, d.void_reason, "
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
    /// @p from_primary forces the primary, the same read-after-write escape
    /// hatch find_in_org() has: every caller that looks the version up in
    /// order to NAME it in a docgen.render payload has just created the
    /// document, and a lagging replica would answer "no versions" for a row
    /// it simply has not seen yet.
    std::optional<DocumentVersion> latest_version(const std::string& org_id,
                                                  const std::string& document_id,
                                                  bool from_primary = false) {
        auto query = [&](auto& txn) -> std::optional<DocumentVersion> {
            auto r = txn.exec_params(std::string("SELECT ") + kVersionColumns +
                                         " FROM document_versions WHERE org_id = $1 AND document_id = $2 "
                                         "ORDER BY version_no DESC LIMIT 1",
                                     org_id,
                                     document_id);
            if (r.empty())
                return std::nullopt;
            return DocumentVersion::from_row(r[0]);
        };
        return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
    }

    /**
     * @brief Решение «принимать ли результат рендера для этой версии».
     * @details Один запрос вместо трёх чтений: гонка «версия вытеснена
     *          между проверкой и записью» здесь всё равно возможна, и
     *          защищает от неё not-superseded-условие в самом UPDATE
     *          set_current_version(), а эта функция даёт джобе внятную
     *          причину не делать ничего и не шуметь ошибкой.
     *
     *          Читает с ПЕРВИЧНОЙ базы (execute_read_primary), а не с реплики:
     *          это решение «писать или не писать», и версия здесь моложе
     *          типичного лага репликации — джоба ставится в очередь сразу
     *          после INSERT'а версии. С реплики та же версия выглядела бы как
     *          kMissing, и рендер молча пропал бы вместо того, чтобы
     *          выполниться.
     */
    VersionRenderState version_render_state(const std::string& org_id,
                                            const std::string& document_id,
                                            const std::string& version_id) {
        return Database::get().execute_read_primary([&](auto& txn) -> VersionRenderState {
            auto r = txn.exec_params(
                // Задача 11 заменила заглушку `FALSE AS voided` настоящим
                // условием: колонка documents.voided_at существует
                // (migrations/019_document_voiding.sql), и рендер НЕ имеет
                // права воскрешать аннулированный документ — иначе джоба,
                // поставленная в очередь до аннулирования, дописала бы файл и
                // перевела статус обратно в 'final' уже после того, как
                // человек объявил документ недействительным.
                "SELECT (d.voided_at IS NOT NULL) AS voided, "
                "       (v.s3_key IS NOT NULL) AS rendered, "
                "       (v.version_no = (SELECT MAX(vv.version_no) FROM document_versions vv "
                "                         WHERE vv.document_id = d.id)) AS newest "
                "  FROM document_versions v JOIN documents d ON d.id = v.document_id "
                " WHERE v.id = $1 AND v.document_id = $2 AND v.org_id = $3",
                version_id,
                document_id,
                org_id);
            if (r.empty())
                return VersionRenderState::kMissing;
            if (r[0]["voided"].template as<bool>())
                return VersionRenderState::kVoided;
            if (!r[0]["newest"].template as<bool>())
                return VersionRenderState::kSuperseded;
            // Файл уже лежит в версии — повтор джобы (ручной re-enqueue,
            // ретрай после таймаута, дубль в очереди) НЕ перерендеривает её.
            // Спека §4.1: предыдущий PDF остаётся; версия — свидетельство, и
            // подменять байты под уже выданной ссылкой нельзя. Проверка
            // именно состояния, а не сравнение контрольных сумм: XeLaTeX в
            // этом дереве собирает PDF без SOURCE_DATE_EPOCH (его нет нигде
            // в репозитории), поэтому два рендера одного входа дают РАЗНЫЕ
            // байты, и «перерендерил ли кто-то» по checksum не определяется.
            if (r[0]["rendered"].template as<bool>())
                return VersionRenderState::kAlreadyRendered;
            return VersionRenderState::kRenderable;
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
     *
     * P3 task 10 — the EXISTS additionally requires @p version_id to still be
     * the NEWEST version of the document. That is the anti-race half of
     * version_render_state(): a render can take a minute, an edit takes
     * milliseconds, and a job that checked "renderable" and only then finished
     * would otherwise publish a version an edit has already superseded —
     * i.e. the document would report an OLD file under a NEW version number.
     * Losing that race is not an error for the job (its own version's file is
     * safely stored under its own key either way); the newer version's render
     * publishes next. Every existing caller publishes the newest version, so
     * this narrows nothing they rely on.
     */
    bool set_current_version(const std::string& org_id, const std::string& document_id, const std::string& version_id) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE documents SET current_version_id = $3 "
                " WHERE id = $1 AND org_id = $2 "
                "   AND EXISTS (SELECT 1 FROM document_versions v "
                "                WHERE v.id = $3 AND v.org_id = $2 AND v.document_id = $1 "
                "                  AND v.version_no = (SELECT MAX(vv.version_no) FROM document_versions vv "
                "                                       WHERE vv.document_id = $1)) "
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
     * @brief Перевести статус, только если он всё ещё @p from. Возвращает
     *        false и когда документа нет, и когда он уже в другом состоянии —
     *        джобе достаточно знать, что ничего не записано.
     * @details Нужен рендер-джобе: безусловный set_status(..., "final") на
     *          документе, который успели отправить ('sent') или аннулировать,
     *          откатывал бы его назад по жизненному циклу — джоба не имеет
     *          права двигать статус, который сменил кто-то другой, пока она
     *          рендерила. Именно эта форма (одно условие в WHERE), а не
     *          «прочитать статус и потом записать»: между чтением и записью
     *          состояние может смениться, а здесь проверка и запись — один
     *          оператор.
     */
    bool set_status_if(const std::string& org_id,
                       const std::string& id,
                       const std::string& from,
                       const std::string& to) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE documents SET status = $4 WHERE id = $1 AND org_id = $2 AND status = $3 RETURNING id",
                id,
                org_id,
                from,
                to);
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

    /// Есть ли у документа связь с ПРОВЕДЁННОЙ (или сторнированной)
    /// проводкой. Это, а не status, отделяет удаляемое от аннулируемого:
    /// 'draft' бывает только у source='generated', и по статусу
    /// ошибочно загруженный скан не удалился бы никогда.
    ///
    /// Читает с ПЕРВИЧНОЙ базы по той же причине, что и
    /// version_render_state(): ответ — основание решения «удалять или нет», а
    /// отставшая реплика ответила бы «связи нет» про связь, которую она просто
    /// ещё не видела, и документ под проведённой проводкой был бы уничтожен.
    bool has_posted_entry_link(const std::string& org_id, const std::string& document_id) {
        return Database::get().execute_read_primary(
            [&](auto& txn) { return posted_link_exists(txn, org_id, document_id); });
    }

    /**
     * @brief Физически удалить документ вместе с его версиями и связями.
     * @details Связь с ЧЕРНОВОЙ проводкой удалению не мешает:
     *          document_entries.document_id — ON DELETE CASCADE, черновик
     *          останется без основания, и это принято (факт удаления
     *          пишется в аудит вызывающим). Связь с ПРОВЕДЁННОЙ проводкой
     *          проверяется в той же транзакции и даёт kHasPostedEntries.
     *
     *          hr_orders.document_id и tax_filings.document_id — NO ACTION
     *          (migrations/012_hr.sql, migrations/016_tax_filings.sql), и
     *          последний ещё и DEFERRABLE, то есть срабатывает на COMMIT.
     *          Поэтому SQLSTATE 23503 ловится здесь и превращается в
     *          kReferenced -> 409, а не всплывает 500-й: подписанный
     *          трудовой договор, на который ссылается приказ, физически
     *          уничтожить нельзя — только аннулировать. try/catch обнимает
     *          ВЕСЬ execute_write, а не одну инструкцию внутри него, именно
     *          из-за отложенного FK: он падает на COMMIT, то есть уже за
     *          пределами лямбды.
     *
     *          Объекты в S3 этот метод не трогает — принятая политика
     *          хранения, см. заголовок migrations/019_document_voiding.sql.
     */
    DeleteOutcome remove(const std::string& org_id, const std::string& document_id) {
        try {
            return Database::get().execute_write([&](auto& txn) -> DeleteOutcome {
                // Проверка живёт В ТОЙ ЖЕ транзакции, что и DELETE, а не в
                // отдельном чтении перед ней. Иначе между «связи нет» и
                // удалением успевает вклиниться проведение проводки, а
                // document_entries.document_id — ON DELETE CASCADE, то есть
                // связь исчезла бы МОЛЧА и проведённая проводка осталась бы
                // без основания. Публичный has_posted_entry_link() выше
                // отвечает на тот же вопрос вызывающим, которым не нужно
                // удаление.
                if (posted_link_exists(txn, org_id, document_id))
                    return DeleteOutcome::kHasPostedEntries;
                auto r = txn.exec_params(
                    "DELETE FROM documents WHERE id = $1 AND org_id = $2 RETURNING id", document_id, org_id);
                return r.empty() ? DeleteOutcome::kNotFound : DeleteOutcome::kDeleted;
            });
        } catch (const pqxx::sql_error& e) {
            if (std::string_view(e.sqlstate()) == "23503")
                return DeleteOutcome::kReferenced;
            throw;
        }
    }

    /// Пометить документ аннулированным. Повторное аннулирование — no-op
    /// (`voided_at IS NULL` в WHERE): первое решение и его автор важнее
    /// последнего. Строка остаётся на месте вместе с файлом и историей —
    /// аннулирование не прячет документ, оно его помечает.
    /// @return false, если документа нет в этой организации ИЛИ он уже
    ///         аннулирован — вызывающий различает их отдельным find_in_org.
    bool void_document(const std::string& org_id,
                       const std::string& document_id,
                       const std::string& user_id,
                       const std::string& reason) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE documents SET voided_at = now(), voided_by_user_id = $3, void_reason = $4 "
                " WHERE id = $1 AND org_id = $2 AND voided_at IS NULL RETURNING id",
                document_id,
                org_id,
                user_id,
                reason);
            return !r.empty();
        });
    }

private:
    /// Единственная формулировка предиката «документ висит на проведённой
    /// (или сторнированной) проводке» — вызывается и из чтения, и изнутри
    /// транзакции удаления, чтобы две копии одного условия не разъехались.
    /// Шаблон по типу транзакции: libpqxx отдаёт разные типы для work и
    /// read_transaction, и оба приходят сюда завёрнутыми в TracingTxn.
    template <typename Txn>
    static bool posted_link_exists(Txn& txn, const std::string& org_id, const std::string& document_id) {
        auto r = txn.exec_params(
            "SELECT EXISTS (SELECT 1 FROM document_entries de "
            "                 JOIN journal_entries je ON je.id = de.entry_id "
            "                WHERE de.document_id = $1 AND de.org_id = $2 "
            "                  AND je.status IN ('posted','reversed'))",
            document_id,
            org_id);
        return r.at(0).at(0).template as<bool>();
    }
};

}  // namespace Ledger
