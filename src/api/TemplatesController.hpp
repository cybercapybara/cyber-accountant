/**
 * @file TemplatesController.hpp
 * @brief Шаблоны документов организации — конструктор (спека §10).
 *
 * ПУБЛИКАЦИЯ АСИНХРОННА, и это не оптимизация. Гейт обязан отрендерить шаблон
 * НАСТОЯЩИМ движком, а Typst лежит только в образе воркера — в образе API его
 * нет и быть не должно. Поэтому `publish` ставит задачу
 * `docgen.publish_template` и отвечает 202; вердикт гейта автор читает в
 * результате задачи. Шаблон остаётся черновиком, пока проверка не прошла.
 *
 * RBAC: ресурс зависит от ВИДА документа. Шаблоны первички — `templates`
 * (владелец и бухгалтер), кадровые — `hr_templates` (плюс кадровик).
 * Разделение то же, что у kDocuments / kHrDocs: кадровик правит приказы, но не
 * счета.
 *
 * Режим `source` (прямой текст Typst) через эти маршруты НЕДОСТУПЕН вовсе:
 * им владеет администратор площадки, и это отдельная проверка, а не ещё одна
 * колонка матрицы прав. Арендатор строит шаблон из блоков — не потому, что
 * Typst опасен (песочница настоящая: свой каталог на рендер, `--root` на него,
 * таймаут), а потому, что из сырого текста нельзя вывести список обязательных
 * подписей, и слой «потеря содержимого» тогда не работает.
 */

#pragma once

#include <optional>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "docgen/BlockCompiler.hpp"
#include "docgen/DocumentTemplateRepository.hpp"
#include "docgen/PublishJob.hpp"
#include "docgen/TemplateRegistry.hpp"
#include "docgen/Variables.hpp"
#include "jobs/Jobs.hpp"
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrgPermissions.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class TemplatesController : public HttpController<TemplatesController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(TemplatesController::listTemplates, "/api/v1/org-templates", Get);
    ADD_METHOD_TO(TemplatesController::createTemplate, "/api/v1/org-templates", Post);
    ADD_METHOD_TO(TemplatesController::patchTemplate, "/api/v1/org-templates/{1}", Patch);
    ADD_METHOD_TO(TemplatesController::publishTemplate, "/api/v1/org-templates/{1}/publish", Post);
    ADD_METHOD_TO(TemplatesController::archiveTemplate, "/api/v1/org-templates/{1}", Delete);
    ADD_METHOD_TO(TemplatesController::listVariables, "/api/v1/template-variables", Get);
    METHOD_LIST_END

    // -------------------------------------------------------------------
    // GET /api/v1/org-templates — свои шаблоны плюс общие.
    // -------------------------------------------------------------------
    void listTemplates(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kTemplates, Tenancy::OrgPerm::Action::kRead);

        with_repo_errors(callback, "templates list", [&] {
            Docgen::DocumentTemplateRepository repo;
            json data = json::array();
            for (const auto& t : repo.list_visible(ctx.org_id))
                data.push_back(t);
            callback(Response::list(data));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/org-templates — создать ЧЕРНОВИК из блоков.
    // -------------------------------------------------------------------
    void createTemplate(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "slug");
        Validation::require(errs, body, "blocks");
        if (body.contains("slug") && !body["slug"].is_string())
            errs.add("slug", "not_string", "must be a string");
        if (body.contains("blocks") && !body["blocks"].is_array())
            errs.add("blocks", "not_array", "must be an array of blocks");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        const std::string slug = body["slug"].get<std::string>();
        API_REQUIRE_ORG_PERM(callback, ctx, resource_for(slug), Tenancy::OrgPerm::Action::kWrite);
        if (!Docgen::TemplateRegistry::is_valid_slug(slug)) {
            callback(Validation::response_422("slug",
                                              "invalid_slug",
                                              "a slug is lowercase letters, digits and underscore — it becomes a "
                                              "directory name and a document type"));
            return;
        }

        // Собираем СРАЗУ: черновик, который не собирается, бесполезен, а
        // ошибка, названная при создании, дешевле ошибки, найденной при
        // публикации.
        Docgen::Blocks::Compiled compiled;
        if (auto err = Docgen::Blocks::compile(body["blocks"], compiled)) {
            callback(Validation::response_422(
                "blocks", err->code, "block " + std::to_string(err->block_index) + ": " + err->message));
            return;
        }

        with_repo_errors(callback, "templates create", [&] {
            Docgen::DocumentTemplateRepository repo;
            Docgen::DocumentTemplate draft;
            draft.org_id = ctx.org_id;
            draft.slug = slug;
            draft.version = repo.next_version(ctx.org_id, slug);
            draft.mode = Docgen::TemplateMode::kBlocks;
            draft.blocks = body["blocks"];
            draft.source = compiled.source;
            draft.schema = compiled.schema;
            draft.form = compiled.form;
            draft.expected = compiled.expected;
            draft.status = Docgen::TemplateStatus::kDraft;
            // Пустой user_id в колонку UUID не пишем: анонимного автора
            // честнее оставить NULL, чем подсунуть пустую строку.
            auto created =
                repo.create(draft, ctx.user_id.empty() ? std::nullopt : std::optional<std::string>(ctx.user_id));
            callback(Response::created({{"data", json(created)}}));
        });
    }

    // -------------------------------------------------------------------
    // PATCH /api/v1/org-templates/{id} — править черновик.
    // -------------------------------------------------------------------
    void patchTemplate(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed template id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        if (!body.contains("blocks") || !body["blocks"].is_array()) {
            Validation::Errors errs;
            errs.add("blocks", "not_array", "must be an array of blocks");
            callback(Validation::response_400(errs));
            return;
        }

        with_repo_errors(callback, "templates patch", [&] {
            Docgen::DocumentTemplateRepository repo;
            auto current = repo.find_in_org(ctx.org_id, id);
            if (!current) {
                callback(ErrorResponse::not_found("template"));
                return;
            }
            if (!Tenancy::OrgPerm::allows(ctx.role, resource_for(current->slug), Tenancy::OrgPerm::Action::kWrite)) {
                callback(ErrorResponse::forbidden("org_role_denied",
                                                  "your organization role may not edit this kind of "
                                                  "template"));
                return;
            }

            Docgen::Blocks::Compiled compiled;
            if (auto err = Docgen::Blocks::compile(body["blocks"], compiled)) {
                callback(Validation::response_422(
                    "blocks", err->code, "block " + std::to_string(err->block_index) + ": " + err->message));
                return;
            }

            Docgen::DocumentTemplate patch = *current;
            patch.blocks = body["blocks"];
            patch.source = compiled.source;
            patch.schema = compiled.schema;
            patch.form = compiled.form;
            patch.expected = compiled.expected;

            auto updated = repo.update_draft(ctx.org_id, id, patch);
            if (!updated) {
                callback(ErrorResponse::not_found("template"));
                return;
            }
            callback(Response::ok({{"data", json(*updated)}}));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/org-templates/{id}/publish — поставить задачу гейту.
    // -------------------------------------------------------------------
    void publishTemplate(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback,
                         const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed template id"));
            return;
        }

        with_repo_errors(callback, "templates publish", [&] {
            Docgen::DocumentTemplateRepository repo;
            auto current = repo.find_in_org(ctx.org_id, id);
            if (!current) {
                callback(ErrorResponse::not_found("template"));
                return;
            }
            if (!Tenancy::OrgPerm::allows(ctx.role, resource_for(current->slug), Tenancy::OrgPerm::Action::kWrite)) {
                callback(ErrorResponse::forbidden("org_role_denied",
                                                  "your organization role may not publish this kind "
                                                  "of template"));
                return;
            }
            if (current->status != Docgen::TemplateStatus::kDraft) {
                callback(Validation::response_422("status",
                                                  "not_a_draft",
                                                  "only a draft can be published — a published version is immutable, "
                                                  "so an edit creates a new version"));
                return;
            }

            const json payload = {{"org_id", ctx.org_id}, {"template_id", id}};
            std::string job_id;
            try {
                job_id = Jobs::get().submit(Docgen::kPublishJobType, payload);
            } catch (const std::exception& e) {
                // Тот же best-effort идиом, что у постановки рендера: перебой
                // Redis не должен превращаться в 500 без всякой информации.
                spdlog::error("templates publish: enqueue failed for template {}: {}", id, e.what());
                callback(ErrorResponse::service_unavailable("publish_not_queued",
                                                            "the publish check could not be queued — try again"));
                return;
            }
            callback(Response::accepted({{"job_id", job_id}, {"template_id", id}}));
        });
    }

    // -------------------------------------------------------------------
    // DELETE /api/v1/org-templates/{id} — АРХИВИРОВАТЬ, не удалять.
    // -------------------------------------------------------------------
    void archiveTemplate(const HttpRequestPtr& req,
                         std::function<void(const HttpResponsePtr&)>&& callback,
                         const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed template id"));
            return;
        }

        with_repo_errors(callback, "templates archive", [&] {
            Docgen::DocumentTemplateRepository repo;
            auto current = repo.find_in_org(ctx.org_id, id);
            if (!current) {
                callback(ErrorResponse::not_found("template"));
                return;
            }
            if (!Tenancy::OrgPerm::allows(ctx.role, resource_for(current->slug), Tenancy::OrgPerm::Action::kWrite)) {
                callback(ErrorResponse::forbidden("org_role_denied",
                                                  "your organization role may not archive this kind "
                                                  "of template"));
                return;
            }
            // Архивация, а не удаление: на версию шаблона ссылаются уже
            // выпущенные документы, и стереть её значило бы сделать их
            // невоспроизводимыми.
            auto archived = repo.set_status(ctx.org_id, id, Docgen::TemplateStatus::kArchived);
            if (!archived) {
                callback(ErrorResponse::not_found("template"));
                return;
            }
            callback(Response::ok({{"data", json(*archived)}}));
        });
    }

    // -------------------------------------------------------------------
    // GET /api/v1/template-variables — каталог готовых переменных.
    // -------------------------------------------------------------------
    void listVariables(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kTemplates, Tenancy::OrgPerm::Action::kRead);
        callback(Response::list(Docgen::Variables::to_json()));
    }

private:
    /// Ресурс прав по слагу шаблона. Кадровые документы — свой ресурс,
    /// симметрично делению kDocuments / kHrDocs.
    static const char* resource_for(const std::string& slug) {
        const bool hr = slug.rfind("hr_", 0) == 0 || slug == "labor_contract" || slug == "payslip";
        return hr ? Tenancy::OrgPerm::Resource::kHrTemplates : Tenancy::OrgPerm::Resource::kTemplates;
    }
};

}  // namespace Api
