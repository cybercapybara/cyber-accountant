/**
 * @file DocumentVersion.hpp
 * @brief Одна версия документа: файл в S3 + снапшот входа, из которого он
 *        отрендерен (спека P3 §4.1). Mirrors the `document_versions` table
 *        (migrations/018_document_versions.sql).
 * @details Версии не удаляются по отдельности НИКОГДА. Удаление и
 *          аннулирование — операции над документом целиком; строка версии
 *          исчезает только вместе с ним (FK ON DELETE CASCADE).
 *
 * Тот же from_row/to_json-идиом, что и у Ledger::Document: from_row —
 * ШАБЛОН. Первая редакция сделала его нешаблонным ради избавления от
 * `.template as<T>()`, и это уронило сборку: libpqxx отдаёт `row_ref` при
 * индексации и обходе результата, а `row_ref` не приводится к
 * `const pqxx::row&`. Конкретный тип здесь не компилируется нигде, где
 * фабрику реально вызывают.
 *
 * `input_snapshot` наружу через to_json НЕ отдаётся: это полный вход
 * рендера, включая суммы и подписантов, и его место — в детальном ответе
 * по одной версии, а не в перечне (задача 9 решает, отдавать ли его
 * вообще).
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>

#include <nlohmann/json.hpp>

namespace Ledger {

struct DocumentVersion {
    std::string id;
    std::string org_id;
    std::string document_id;
    int version_no = 0;
    std::optional<std::string> s3_key;
    std::optional<std::string> checksum_sha256;
    std::optional<std::string> mime;
    std::optional<long long> size_bytes;
    std::optional<std::string> template_version;
    /// Движок, собравший этот PDF: `xelatex` или `typst <версия>`
    /// (migrations/024_document_versions_render_engine.sql). NULL у версий
    /// без рендера — загруженный файл и ещё не отрендеренная версия.
    std::optional<std::string> render_engine;
    std::optional<nlohmann::json> input_snapshot;
    std::optional<std::string> created_by_user_id;
    std::string created_at;
    std::string updated_at;

    /// Templated like every other model's factory (Ledger::Document,
    /// Ledger::Counterparty, Hr::Employee): libpqxx hands out `row_ref` when a
    /// result is indexed or iterated, which does not convert to `const row&`.
    /// A non-templated overload compiles nowhere it is actually called.
    template <typename Row>
    static DocumentVersion from_row(const Row& r) {
        DocumentVersion v;
        v.id = r["id"].template as<std::string>();
        v.org_id = r["org_id"].template as<std::string>();
        v.document_id = r["document_id"].template as<std::string>();
        v.version_no = r["version_no"].template as<int>();
        if (!r["s3_key"].is_null())
            v.s3_key = r["s3_key"].template as<std::string>();
        if (!r["checksum_sha256"].is_null())
            v.checksum_sha256 = r["checksum_sha256"].template as<std::string>();
        if (!r["mime"].is_null())
            v.mime = r["mime"].template as<std::string>();
        if (!r["size_bytes"].is_null())
            v.size_bytes = r["size_bytes"].template as<long long>();
        if (!r["template_version"].is_null())
            v.template_version = r["template_version"].template as<std::string>();
        if (!r["render_engine"].is_null())
            v.render_engine = r["render_engine"].template as<std::string>();
        if (!r["input_snapshot"].is_null()) {
            // Same fallback as Ledger::Document::from_row / AuditEntry: the
            // column is only ever written from nlohmann::json::dump(), so a
            // parse failure is corruption, not "absent".
            try {
                v.input_snapshot = nlohmann::json::parse(r["input_snapshot"].template as<std::string>());
            } catch (...) {
                v.input_snapshot = nlohmann::json::object();
            }
        }
        if (!r["created_by_user_id"].is_null())
            v.created_by_user_id = r["created_by_user_id"].template as<std::string>();
        v.created_at = r["created_at"].template as<std::string>();
        v.updated_at = r["updated_at"].template as<std::string>();
        return v;
    }
};

inline void to_json(nlohmann::json& j, const DocumentVersion& v) {
    j = nlohmann::json{
        {"id", v.id},
        {"document_id", v.document_id},
        {"version_no", v.version_no},
        {"s3_key", v.s3_key ? nlohmann::json(*v.s3_key) : nlohmann::json(nullptr)},
        {"checksum_sha256", v.checksum_sha256 ? nlohmann::json(*v.checksum_sha256) : nlohmann::json(nullptr)},
        {"mime", v.mime ? nlohmann::json(*v.mime) : nlohmann::json(nullptr)},
        {"size_bytes", v.size_bytes ? nlohmann::json(*v.size_bytes) : nlohmann::json(nullptr)},
        {"template_version", v.template_version ? nlohmann::json(*v.template_version) : nlohmann::json(nullptr)},
        // Отдаётся наружу, в отличие от input_snapshot: это ровно тот факт,
        // которого не хватает, когда человек спрашивает «почему этот PDF не
        // такой, как тот, что я печатал в марте». template_version без
        // движка на такой вопрос не отвечает — Typst pre-1.0 меняет вёрстку
        // одного и того же шаблона от релиза к релизу.
        {"render_engine", v.render_engine ? nlohmann::json(*v.render_engine) : nlohmann::json(nullptr)},
        {"created_by_user_id", v.created_by_user_id ? nlohmann::json(*v.created_by_user_id) : nlohmann::json(nullptr)},
        {"created_at", v.created_at},
        {"updated_at", v.updated_at},
    };
    // input_snapshot наружу НЕ отдаётся списком версий — см. заголовок файла.
}

}  // namespace Ledger
