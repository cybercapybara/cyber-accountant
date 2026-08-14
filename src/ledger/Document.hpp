/**
 * @file Document.hpp
 * @brief Document row — единый реестр первички любого происхождения. Mirrors
 *        the `documents` table (migrations/010_documents.sql — design spec
 *        §6.4).
 *
 * Domain-only — no SQL here; persistence lives in
 * src/ledger/DocumentRepository.hpp. Follows the same from_row/to_json
 * idioms as src/ledger/JournalEntry.hpp: from_row is a templated static
 * factory (works with any pqxx row-like type), to_json is a free function
 * found via ADL, and every nullable DB column becomes a
 * std::optional<...> here — populated only when the column isn't NULL, but
 * always rendered as an explicit JSON `null` (never an omitted key) so every
 * response has a stable shape.
 *
 * `input_snapshot` is the one JSONB column. It is read back with
 * nlohmann::json::parse — same idiom as Domain::AuditEntry::details
 * (src/domain/AuditEntry.hpp) — but stays std::nullopt (not
 * nlohmann::json::object()) when the column is actually NULL: a document
 * that never went through docgen never had an input snapshot to begin with,
 * and callers (Task 12/13) need to tell "no snapshot" apart from "an empty
 * one" — a malformed/corrupt stored value, on the other hand, is not
 * expected to occur (the column is only ever written by
 * DocumentRepository::create() via `nlohmann::json::dump()`), so a parse
 * failure there falls back to an empty object exactly like AuditEntry does,
 * rather than being treated as "absent".
 */

#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace Ledger {

struct Document {
    std::string id;
    std::string org_id;
    std::string doc_type;  // CHECK list — migrations/010_documents.sql
    std::string source;    // 'generated' | 'uploaded' | 'email'
    std::string status;    // CHECK list spanning both lifecycles — see that migration's header
    std::optional<std::string> counterparty_id;
    std::optional<std::string> s3_key;
    std::optional<std::string> checksum_sha256;
    std::optional<std::string> mime;
    std::optional<long long> size_bytes;
    std::optional<std::string> template_slug;
    std::optional<std::string> template_version;
    std::optional<nlohmann::json> input_snapshot;
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Document from_row(const Row& row) {
        Document d;
        d.id = row["id"].template as<std::string>();
        d.org_id = row["org_id"].template as<std::string>();
        d.doc_type = row["doc_type"].template as<std::string>();
        d.source = row["source"].template as<std::string>();
        d.status = row["status"].template as<std::string>();
        if (!row["counterparty_id"].is_null())
            d.counterparty_id = row["counterparty_id"].template as<std::string>();
        if (!row["s3_key"].is_null())
            d.s3_key = row["s3_key"].template as<std::string>();
        if (!row["checksum_sha256"].is_null())
            d.checksum_sha256 = row["checksum_sha256"].template as<std::string>();
        if (!row["mime"].is_null())
            d.mime = row["mime"].template as<std::string>();
        if (!row["size_bytes"].is_null())
            d.size_bytes = row["size_bytes"].template as<long long>();
        if (!row["template_slug"].is_null())
            d.template_slug = row["template_slug"].template as<std::string>();
        if (!row["template_version"].is_null())
            d.template_version = row["template_version"].template as<std::string>();
        if (!row["input_snapshot"].is_null()) {
            try {
                d.input_snapshot = nlohmann::json::parse(row["input_snapshot"].template as<std::string>());
            } catch (...) {
                d.input_snapshot = nlohmann::json::object();
            }
        }
        d.created_at = row["created_at"].template as<std::string>();
        d.updated_at = row["updated_at"].template as<std::string>();
        return d;
    }
};

inline void to_json(nlohmann::json& j, const Document& d) {
    j = nlohmann::json{
        {"id", d.id},
        {"org_id", d.org_id},
        {"doc_type", d.doc_type},
        {"source", d.source},
        {"status", d.status},
        {"counterparty_id", d.counterparty_id ? nlohmann::json(*d.counterparty_id) : nlohmann::json(nullptr)},
        {"s3_key", d.s3_key ? nlohmann::json(*d.s3_key) : nlohmann::json(nullptr)},
        {"checksum_sha256", d.checksum_sha256 ? nlohmann::json(*d.checksum_sha256) : nlohmann::json(nullptr)},
        {"mime", d.mime ? nlohmann::json(*d.mime) : nlohmann::json(nullptr)},
        {"size_bytes", d.size_bytes ? nlohmann::json(*d.size_bytes) : nlohmann::json(nullptr)},
        {"template_slug", d.template_slug ? nlohmann::json(*d.template_slug) : nlohmann::json(nullptr)},
        {"template_version", d.template_version ? nlohmann::json(*d.template_version) : nlohmann::json(nullptr)},
        {"input_snapshot", d.input_snapshot ? *d.input_snapshot : nlohmann::json(nullptr)},
        {"created_at", d.created_at},
        {"updated_at", d.updated_at},
    };
}

}  // namespace Ledger
