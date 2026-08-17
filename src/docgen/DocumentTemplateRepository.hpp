/**
 * @file DocumentTemplateRepository.hpp
 * @brief Весь SQL, трогающий `document_templates`, живёт здесь.
 *
 * НЕ наследует Tenancy::OrgCrudBase, в отличие от остальных org-scoped
 * репозиториев, и это осознанно: у этой таблицы `org_id` НУЛЛАБЕЛЕН — NULL
 * означает шаблон площадки, видимый всем арендаторам. Базовый класс
 * подставляет `org_id = $1` во всякий запрос, что отрезало бы общие шаблоны
 * целиком. Видимость здесь — «свои ИЛИ общие», и выражать её надо явно, а не
 * прятать в базовом классе, где следующий читатель её не найдёт.
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "docgen/DocumentTemplate.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"

namespace Docgen {

/// Стабильный код 409 на исключении, чтобы with_repo_errors() отобразил его,
/// не подключая этот заголовок.
struct DuplicateTemplateVersion : Repositories::ConflictError {
    DuplicateTemplateVersion()
        : Repositories::ConflictError("template_version_taken",
                                      "That template already has a version with this number") {}
};

/// Попытка изменить или удалить опубликованную версию. Триггер
/// document_templates_immutability() поднимает restrict_violation (23001).
struct PublishedTemplateIsImmutable : Repositories::ConflictError {
    PublishedTemplateIsImmutable()
        : Repositories::ConflictError("template_published_immutable",
                                      "A published template version cannot be changed — publish a new version, or "
                                      "archive this one") {}
};

class DocumentTemplateRepository {
public:
    static constexpr const char* kColumns =
        "id, org_id, slug, version, mode, blocks::text AS blocks, source, schema::text AS schema, "
        "form::text AS form, expected, status, created_by, created_at, updated_at";

    /**
     * @brief Шаблон, которым нужно рендерить документ организации @p org_id.
     * @details Порядок разрешения из §5.1 спеки: шаблон ОРГАНИЗАЦИИ, затем
     *          шаблон ПЛОЩАДКИ. Диска здесь нет — если не нашлось ни того ни
     *          другого, вызывающий откатывается на встроенный шаблон
     *          (TemplateRegistry), и это его решение, а не наше.
     *
     *          Сортировка `org_id NULLS LAST` и есть приоритет: своя версия
     *          побеждает общую. Внутри одного владельца берётся СТАРШАЯ
     *          опубликованная версия.
     */
    std::optional<DocumentTemplate> resolve(const std::string& org_id, const std::string& slug) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<DocumentTemplate> {
            auto r = txn.exec_params("SELECT " + std::string(kColumns) +
                                         " FROM document_templates "
                                         " WHERE slug = $2 AND status = 'published' "
                                         "   AND (org_id = $1 OR org_id IS NULL) "
                                         " ORDER BY org_id NULLS LAST, version DESC "
                                         " LIMIT 1",
                                     org_id,
                                     slug);
            if (r.empty())
                return std::nullopt;
            return DocumentTemplate::from_row(r[0]);
        });
    }

    /// Все шаблоны, ВИДИМЫЕ организации: её собственные плюс общие. Черновики
    /// площадки сюда не попадают — чужая незаконченная работа арендатора не
    /// касается; свои черновики видны, их же и правят.
    std::vector<DocumentTemplate> list_visible(const std::string& org_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT " + std::string(kColumns) +
                                         " FROM document_templates "
                                         " WHERE (org_id = $1) OR (org_id IS NULL AND status = 'published') "
                                         " ORDER BY slug, org_id NULLS LAST, version DESC",
                                     org_id);
            std::vector<DocumentTemplate> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(DocumentTemplate::from_row(row));
            return out;
        });
    }

    /**
     * @brief Найти шаблон организации по id.
     * @details Пара (@p id, @p org_id) в WHERE — это и есть изоляция
     *          арендаторов: чужой id не совпадёт с org_id и вернёт пусто, а не
     *          чужую строку. Шаблон ПЛОЩАДКИ этим методом не находится
     *          намеренно: его правит другой маршрут с другой проверкой прав.
     */
    std::optional<DocumentTemplate> find_in_org(const std::string& org_id, const std::string& id) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<DocumentTemplate> {
            auto r = txn.exec_params(
                "SELECT " + std::string(kColumns) + " FROM document_templates WHERE id = $1 AND org_id = $2", id, org_id);
            if (r.empty())
                return std::nullopt;
            return DocumentTemplate::from_row(r[0]);
        });
    }

    /// Следующий свободный номер версии для (владелец, слаг). Считается в БД,
    /// а не в C++: два одновременных черновика иначе получили бы один номер и
    /// один из них упал бы на UNIQUE уже после того, как автор нажал «создать».
    int next_version(const std::optional<std::string>& org_id, const std::string& slug) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT COALESCE(MAX(version), 0) + 1 AS next FROM document_templates "
                                     " WHERE slug = $2 AND org_id IS NOT DISTINCT FROM $1",
                                     org_id,
                                     slug);
            return r[0]["next"].template as<int>();
        });
    }

    DocumentTemplate create(const DocumentTemplate& draft, const std::optional<std::string>& created_by) {
        return Repositories::detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    auto r = txn.exec_params(
                        "INSERT INTO document_templates "
                        "  (org_id, slug, version, mode, blocks, source, schema, form, expected, status, created_by) "
                        "VALUES ($1, $2, $3, $4, $5::jsonb, $6, $7::jsonb, $8::jsonb, $9, $10, $11) RETURNING " +
                            std::string(kColumns),
                        draft.org_id,
                        draft.slug,
                        draft.version,
                        draft.mode,
                        draft.blocks.has_value() ? std::optional<std::string>(draft.blocks->dump()) : std::nullopt,
                        draft.source,
                        draft.schema.dump(),
                        draft.form.dump(),
                        draft.expected,
                        draft.status.empty() ? std::string(TemplateStatus::kDraft) : draft.status,
                        created_by);
                    return DocumentTemplate::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateTemplateVersion{};
            });
    }

    /**
     * @brief Обновить ЧЕРНОВИК шаблона организации.
     * @details Опубликованную версию не пропустит триггер — и это правильное
     *          место для такой проверки: гарантия, живущая только в C++,
     *          обходится любым другим путём записи. Здесь она лишь переводится
     *          в понятный 409.
     * @return std::nullopt, если строки нет — ожидаемая ветка для 404.
     */
    std::optional<DocumentTemplate> update_draft(const std::string& org_id,
                                                 const std::string& id,
                                                 const DocumentTemplate& patch) {
        return Repositories::detail::translate_sql(
            [&]() -> std::optional<DocumentTemplate> {
                return Database::get().execute_write([&](auto& txn) -> std::optional<DocumentTemplate> {
                    auto r = txn.exec_params(
                        "UPDATE document_templates SET blocks = $3::jsonb, source = $4, schema = $5::jsonb, "
                        "  form = $6::jsonb, expected = $7 "
                        " WHERE id = $1 AND org_id = $2 RETURNING " +
                            std::string(kColumns),
                        id,
                        org_id,
                        patch.blocks.has_value() ? std::optional<std::string>(patch.blocks->dump()) : std::nullopt,
                        patch.source,
                        patch.schema.dump(),
                        patch.form.dump(),
                        patch.expected);
                    if (r.empty())
                        return std::nullopt;
                    return DocumentTemplate::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23001")
                    throw PublishedTemplateIsImmutable{};
            });
    }

    /// Перевести статус. Единственный переход, разрешённый триггером для
    /// опубликованной версии, — в `archived`; всё остальное он отвергает.
    std::optional<DocumentTemplate> set_status(const std::string& org_id,
                                               const std::string& id,
                                               const std::string& status) {
        return Repositories::detail::translate_sql(
            [&]() -> std::optional<DocumentTemplate> {
                return Database::get().execute_write([&](auto& txn) -> std::optional<DocumentTemplate> {
                    auto r = txn.exec_params("UPDATE document_templates SET status = $3 "
                                             " WHERE id = $1 AND org_id = $2 RETURNING " +
                                                 std::string(kColumns),
                                             id,
                                             org_id,
                                             status);
                    if (r.empty())
                        return std::nullopt;
                    return DocumentTemplate::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23001")
                    throw PublishedTemplateIsImmutable{};
            });
    }
};

}  // namespace Docgen
