/**
 * @file BankAccountsController.hpp
 * @brief Расчётные счета организации — CRUD (спека конструктора шаблонов §7.2).
 *
 * ЗАЧЕМ ЭТО СУЩЕСТВУЕТ. Реквизиты сторон в первичке сегодня присылает клиент
 * целиком (`definitions.party` в схемах шаблонов), а в `organizations` счетов
 * нет вовсе. Значит, бухгалтер перенабирает собственные банковские реквизиты
 * в каждом счёте, и два документа одной организации могут разойтись. Эти
 * маршруты дают счетам единственное место хранения, из которого их
 * подставляет сервер.
 *
 * RBAC: каждый маршрут проходит через API_REQUIRE_ORG_PERM против матрицы
 * §5.3. Запись отдана ТОЛЬКО владельцу — подменённый ИИК уводит платежи
 * покупателей на чужой счёт; бухгалтер счета видит, но не меняет.
 */

#pragma once

#include <drogon/HttpController.h>
#include <drogon/drogon.h>

#include <nlohmann/json.hpp>

// Контроллеры НЕ подключают api/Api.hpp (он подключает контроллеры — вышел бы
// цикл). Только маленькие общие помощники.
#include "api/Guards.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "tenancy/BankAccountRepository.hpp"
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrgPermissions.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class BankAccountsController : public HttpController<BankAccountsController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(BankAccountsController::listBankAccounts, "/api/v1/bank-accounts", Get);
    ADD_METHOD_TO(BankAccountsController::createBankAccount, "/api/v1/bank-accounts", Post);
    ADD_METHOD_TO(BankAccountsController::patchBankAccount, "/api/v1/bank-accounts/{1}", Patch);
    ADD_METHOD_TO(BankAccountsController::deleteBankAccount, "/api/v1/bank-accounts/{1}", Delete);
    METHOD_LIST_END

    // -------------------------------------------------------------------
    // GET /api/v1/bank-accounts — org-scoped; основной счёт идёт первым.
    // -------------------------------------------------------------------
    void listBankAccounts(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kRequisites, Tenancy::OrgPerm::Action::kRead);
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);

        with_repo_errors(callback, "bank accounts list", [&] {
            Tenancy::BankAccountRepository repo;
            auto rows = repo.list_in_org(ctx.org_id, page.limit, page.offset);
            long total = repo.count_in_org(ctx.org_id);
            json data = json::array();
            for (const auto& a : rows)
                data.push_back(a);
            callback(Response::paginated(data, total, page.limit, page.offset));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/bank-accounts — только владелец.
    // -------------------------------------------------------------------
    void createBankAccount(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kRequisites, Tenancy::OrgPerm::Action::kWrite);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Tenancy::BankAccount draft;
        if (!validate_and_fill(body, draft, callback))
            return;

        with_repo_errors(callback, "bank accounts create", [&] {
            Tenancy::BankAccountRepository repo;
            auto created = repo.create(ctx.org_id, draft);
            callback(Response::created({{"data", json(created)}}));
        });
    }

    // -------------------------------------------------------------------
    // PATCH /api/v1/bank-accounts/{id} — только владелец.
    // -------------------------------------------------------------------
    void patchBankAccount(const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback,
                          const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kRequisites, Tenancy::OrgPerm::Action::kWrite);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed bank account id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        with_repo_errors(callback, "bank accounts patch", [&] {
            Tenancy::BankAccountRepository repo;
            // Патч поверх текущей строки, а не поверх пустой: PATCH обязан
            // менять только присланные поля, иначе отсутствующий в теле `kbe`
            // молча затёрся бы пустой строкой.
            auto current = repo.find_in_org(id, ctx.org_id);
            if (!current) {
                callback(ErrorResponse::not_found("bank_account"));
                return;
            }
            Tenancy::BankAccount patch = *current;
            if (!apply_patch(body, patch, callback))
                return;

            auto updated = repo.update(ctx.org_id, id, patch);
            if (!updated) {
                callback(ErrorResponse::not_found("bank_account"));
                return;
            }
            callback(Response::ok({{"data", json(*updated)}}));
        });
    }

    // -------------------------------------------------------------------
    // DELETE /api/v1/bank-accounts/{id} — только владелец.
    // -------------------------------------------------------------------
    void deleteBankAccount(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& callback,
                           const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        API_REQUIRE_ORG_PERM(callback, ctx, Tenancy::OrgPerm::Resource::kRequisites, Tenancy::OrgPerm::Action::kWrite);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed bank account id"));
            return;
        }

        with_repo_errors(callback, "bank accounts delete", [&] {
            Tenancy::BankAccountRepository repo;
            if (!repo.remove(ctx.org_id, id)) {
                callback(ErrorResponse::not_found("bank_account"));
                return;
            }
            callback(Response::ok({{"deleted", true}}));
        });
    }

private:
    /// Полная проверка тела для POST. `iik` и `bank_name` обязательны: счёт
    /// без номера или без банка нельзя напечатать в документе, а значит, и
    /// хранить его незачем. Ошибки КОПЯТСЯ и отдаются одним ответом
    /// (Validation::Errors), а не по первой — заполняющий форму должен
    /// увидеть все промахи сразу.
    static bool validate_and_fill(const json& body,
                                  Tenancy::BankAccount& out,
                                  const std::function<void(const HttpResponsePtr&)>& callback) {
        Validation::Errors errs;
        Validation::require(errs, body, "iik");
        Validation::require(errs, body, "bank_name");
        for (const char* f : {"iik", "bank_name", "bik", "kbe", "currency"}) {
            if (body.contains(f) && !body[f].is_string())
                errs.add(f, "not_string", "must be a string");
        }
        if (body.contains("is_primary") && !body["is_primary"].is_boolean())
            errs.add("is_primary", "not_boolean", "must be a boolean");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return false;
        }

        out.iik = body["iik"].get<std::string>();
        out.bank_name = body["bank_name"].get<std::string>();
        out.bik = body.value("bik", "");
        out.kbe = body.value("kbe", "");
        out.currency = body.value("currency", "KZT");
        out.is_primary = body.value("is_primary", false);
        return true;
    }

    /// Частичное обновление: трогаются только присутствующие в теле ключи.
    /// Именно поэтому патч кладётся поверх ТЕКУЩЕЙ строки, а не поверх
    /// пустой — иначе отсутствующий в теле `kbe` молча затёрся бы.
    static bool apply_patch(const json& body,
                            Tenancy::BankAccount& out,
                            const std::function<void(const HttpResponsePtr&)>& callback) {
        Validation::Errors errs;
        for (const char* f : {"iik", "bank_name", "bik", "kbe", "currency"}) {
            if (body.contains(f) && !body[f].is_string())
                errs.add(f, "not_string", "must be a string");
        }
        if (body.contains("is_primary") && !body["is_primary"].is_boolean())
            errs.add("is_primary", "not_boolean", "must be a boolean");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return false;
        }

        if (body.contains("iik"))
            out.iik = body["iik"].get<std::string>();
        if (body.contains("bank_name"))
            out.bank_name = body["bank_name"].get<std::string>();
        if (body.contains("bik"))
            out.bik = body["bik"].get<std::string>();
        if (body.contains("kbe"))
            out.kbe = body["kbe"].get<std::string>();
        if (body.contains("currency"))
            out.currency = body["currency"].get<std::string>();
        if (body.contains("is_primary"))
            out.is_primary = body["is_primary"].get<bool>();
        return true;
    }
};

}  // namespace Api
