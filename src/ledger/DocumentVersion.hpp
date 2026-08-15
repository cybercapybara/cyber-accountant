/**
 * @file DocumentVersion.hpp
 * @brief Одна версия документа: файл в S3 + снапшот входа, из которого он
 *        отрендерен (спека P3 §4.1). Mirrors the `document_versions` table
 *        (migrations/018_document_versions.sql).
 * @details Версии не удаляются по отдельности НИКОГДА. Удаление и
 *          аннулирование — операции над документом целиком; строка версии
 *          исчезает только вместе с ним (FK ON DELETE CASCADE).
 *
 * Тот же from_row/to_json-идиом, что и у Ledger::Document, с одним
 * отличием: from_row здесь НЕ шаблон — версии читаются только этим
 * репозиторием и только из pqxx::row, а конкретный тип избавляет от
 * `.template as<T>()` внутри тела.
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
    std::optional<nlohmann::json> input_snapshot;
    std::optional<std::string> created_by_user_id;
    std::string created_at;
    std::string updated_at;

    static DocumentVersion from_row(const pqxx::row& r) {
        DocumentVersion v;
        v.id = r["id"].as<std::string>();
        v.org_id = r["org_id"].as<std::string>();
        v.document_id = r["document_id"].as<std::string>();
        v.version_no = r["version_no"].as<int>();
        if (!r["s3_key"].is_null())
            v.s3_key = r["s3_key"].as<std::string>();
        if (!r["checksum_sha256"].is_null())
            v.checksum_sha256 = r["checksum_sha256"].as<std::string>();
        if (!r["mime"].is_null())
            v.mime = r["mime"].as<std::string>();
        if (!r["size_bytes"].is_null())
            v.size_bytes = r["size_bytes"].as<long long>();
        if (!r["template_version"].is_null())
            v.template_version = r["template_version"].as<std::string>();
        if (!r["input_snapshot"].is_null()) {
            // Same fallback as Ledger::Document::from_row / AuditEntry: the
            // column is only ever written from nlohmann::json::dump(), so a
            // parse failure is corruption, not "absent".
            try {
                v.input_snapshot = nlohmann::json::parse(r["input_snapshot"].as<std::string>());
            } catch (...) {
                v.input_snapshot = nlohmann::json::object();
            }
        }
        if (!r["created_by_user_id"].is_null())
            v.created_by_user_id = r["created_by_user_id"].as<std::string>();
        v.created_at = r["created_at"].as<std::string>();
        v.updated_at = r["updated_at"].as<std::string>();
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
        {"created_by_user_id", v.created_by_user_id ? nlohmann::json(*v.created_by_user_id) : nlohmann::json(nullptr)},
        {"created_at", v.created_at},
        {"updated_at", v.updated_at},
    };
    // input_snapshot наружу НЕ отдаётся списком версий — см. заголовок файла.
}

}  // namespace Ledger
