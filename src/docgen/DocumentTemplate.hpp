/**
 * @file DocumentTemplate.hpp
 * @brief Пользовательский шаблон документа. Отражает таблицу
 *        `document_templates` (migrations/027_document_templates.sql — спека
 *        конструктора §5).
 *
 * Только домен, без SQL; персистентность — в
 * src/docgen/DocumentTemplateRepository.hpp.
 *
 * `org_id` пуст у шаблона ПЛОЩАДКИ: он виден всем арендаторам, а менять его
 * вправе только администратор инсталляции. Пустой org_id — это «общий», а не
 * «ничей», и код обязан различать эти случаи: шаблон организации недостижим из
 * другой организации, шаблон площадки — читаем всеми.
 */

#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace Docgen {

/// Способ авторства (§4 спеки). `kBlocks` — конструктор для арендаторов,
/// `kSource` — прямой текст Typst, доступный только администратору площадки.
namespace TemplateMode {
inline constexpr const char* kBlocks = "blocks";
inline constexpr const char* kSource = "source";
}  // namespace TemplateMode

namespace TemplateStatus {
inline constexpr const char* kDraft = "draft";
inline constexpr const char* kPublished = "published";
inline constexpr const char* kArchived = "archived";
}  // namespace TemplateStatus

struct DocumentTemplate {
    std::string id;
    /// Пусто — шаблон площадки (org_id IS NULL в БД).
    std::optional<std::string> org_id;
    std::string slug;
    int version = 1;
    std::string mode;
    /// Источник правды для режима блоков; пусто для режима исходника.
    std::optional<nlohmann::json> blocks;
    /// Текст Typst: порождённый (блоки) либо авторский (исходник). Хранится
    /// всегда — рендер обязан быть воспроизводим без повторной сборки.
    std::string source;
    nlohmann::json schema;
    nlohmann::json form;
    /// Статические подписи, которые шаблон обязан напечатать. Пусто для
    /// режима исходника: вывести их оттуда неоткуда (§12 спеки).
    std::optional<std::string> expected;
    std::string status;
    std::optional<std::string> created_by;
    std::string created_at;
    std::string updated_at;

    bool is_platform_template() const { return !org_id.has_value(); }

    template <typename Row>
    static DocumentTemplate from_row(const Row& row) {
        DocumentTemplate t;
        t.id = row["id"].template as<std::string>();
        if (!row["org_id"].is_null())
            t.org_id = row["org_id"].template as<std::string>();
        t.slug = row["slug"].template as<std::string>();
        t.version = row["version"].template as<int>();
        t.mode = row["mode"].template as<std::string>();
        if (!row["blocks"].is_null())
            t.blocks = nlohmann::json::parse(row["blocks"].template as<std::string>());
        t.source = row["source"].template as<std::string>();
        t.schema = nlohmann::json::parse(row["schema"].template as<std::string>());
        t.form = nlohmann::json::parse(row["form"].template as<std::string>());
        if (!row["expected"].is_null())
            t.expected = row["expected"].template as<std::string>();
        t.status = row["status"].template as<std::string>();
        if (!row["created_by"].is_null())
            t.created_by = row["created_by"].template as<std::string>();
        t.created_at = row["created_at"].template as<std::string>();
        t.updated_at = row["updated_at"].template as<std::string>();
        return t;
    }
};

/// Публичная форма JSON. `source` отдаётся: для шаблона в режиме блоков это
/// порождённый текст, и увидеть, что именно будет собрано, — законное право
/// автора шаблона. Секретов на строке нет.
inline void to_json(nlohmann::json& j, const DocumentTemplate& t) {
    j = nlohmann::json{
        {"id", t.id},
        {"org_id", t.org_id.has_value() ? nlohmann::json(*t.org_id) : nlohmann::json(nullptr)},
        {"slug", t.slug},
        {"version", t.version},
        {"mode", t.mode},
        {"blocks", t.blocks.has_value() ? *t.blocks : nlohmann::json(nullptr)},
        {"source", t.source},
        {"schema", t.schema},
        {"form", t.form},
        {"expected", t.expected.has_value() ? nlohmann::json(*t.expected) : nlohmann::json(nullptr)},
        {"status", t.status},
        {"created_at", t.created_at},
        {"updated_at", t.updated_at},
    };
}

}  // namespace Docgen
