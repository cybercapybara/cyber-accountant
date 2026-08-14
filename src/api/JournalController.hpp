/**
 * @file JournalController.hpp
 * @brief Double-entry journal API — draft/post/storno lifecycle over
 *        Ledger::JournalService (design spec §6.2 / Task 13).
 *
 * Routes (all under /api/v1, every handler starts with
 * API_REQUIRE_ORG(req, callback, ctx) — same guard order as
 * CounterpartiesController/LedgerDocumentsController):
 *   GET  /api/v1/journal-entries               list, paginated, optional
 *                                                ?from=&to= filters on
 *                                                entry_date (YYYY-MM-DD).
 *                                                Header-only — `lines` is
 *                                                always present in the JSON
 *                                                shape but empty here (see
 *                                                Ledger::JournalRepository's
 *                                                header for why a header row
 *                                                never carries lines).
 *   GET  /api/v1/journal-entries/{id}           fetch one, WITH lines
 *   POST /api/v1/journal-entries                create a draft entry
 *   POST /api/v1/journal-entries/{id}/post      draft -> posted
 *   POST /api/v1/journal-entries/{id}/reverse   posted -> reversed + a new,
 *                                                 already-posted storno entry
 *
 * RBAC: every mutating route (create/post/reverse) additionally rejects
 * `ctx.role == "viewer"` with 403 — the brief's rule for every ledger route.
 * `org_id`/`user_id` come EXCLUSIVELY from `ctx` (the access token's
 * membership-backed claims), never from the body or a query param.
 *
 * Error-code split (mirrors the 400-vs-422 line CounterpartiesController's
 * identifier check-digit already draws): a request-BODY SHAPE problem
 * (missing field, wrong JSON type) is a 400 via Validation::response_400();
 * a syntactically fine VALUE that fails a domain rule — a malformed
 * entry_date, or one of Ledger::JournalService's typed exceptions
 * (UnbalancedEntry/UnknownAccount/ForeignCounterparty), or the plain
 * std::invalid_argument JournalService::parse_tiyn()/the side check throw
 * for a malformed amount or a side other than "debit"/"credit" — is a 422
 * via Api::Validation::response_422(), with a field name and a stable code
 * a client can branch on without parsing the message string.
 * Ledger::InvalidEntryState (post/reverse called on an entry in the wrong
 * status) is the one exception mapped to 409, not 422 — it's a conflict
 * with the entry's current state, not a malformed request.
 *
 * A per-line counterparty_id that is present but not a well-formed UUID is
 * rejected here as a 400 (structural) BEFORE it ever reaches
 * JournalService — passing a non-uuid string straight through to
 * CounterpartyRepository::find_in_org would otherwise surface as a raw
 * pqxx "invalid input syntax for type uuid" error (a 500), not the 422 the
 * ForeignCounterparty path is meant to produce.
 */

#pragma once

#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "ledger/JournalEntry.hpp"
#include "ledger/JournalRepository.hpp"
#include "ledger/JournalService.hpp"
#include "tenancy/OrgContext.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class JournalController : public HttpController<JournalController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(JournalController::list, "/api/v1/journal-entries", Get);
    ADD_METHOD_TO(JournalController::create, "/api/v1/journal-entries", Post);
    ADD_METHOD_TO(JournalController::get, "/api/v1/journal-entries/{1}", Get);
    ADD_METHOD_TO(JournalController::post, "/api/v1/journal-entries/{1}/post", Post);
    ADD_METHOD_TO(JournalController::reverse, "/api/v1/journal-entries/{1}/reverse", Post);
    METHOD_LIST_END

    // -------------------------------------------------------------------
    // GET /api/v1/journal-entries — paginated, optional ?from=&to= filters
    // on entry_date (inclusive, YYYY-MM-DD).
    // -------------------------------------------------------------------
    void list(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);

        Validation::Errors errs;
        std::optional<std::string> from_filter;
        std::optional<std::string> to_filter;
        const std::string from_param = req->getParameter("from");
        const std::string to_param = req->getParameter("to");
        if (!from_param.empty()) {
            if (!is_valid_date(from_param))
                errs.add("from", "invalid_date", "must be in YYYY-MM-DD format");
            else
                from_filter = from_param;
        }
        if (!to_param.empty()) {
            if (!is_valid_date(to_param))
                errs.add("to", "invalid_date", "must be in YYYY-MM-DD format");
            else
                to_filter = to_param;
        }
        if (errs.any()) {
            // Query-param filters, not a JSON body — same 422
            // Api::Validation shape as LedgerDocumentsController::list's
            // type/status filter checks, built directly since more than one
            // field can fail at once (response_422 is single-field).
            callback(ErrorResponse::make(
                {drogon::k422UnprocessableEntity, "validation_failed", "", json{{"errors", errs.errors_json()}}}));
            return;
        }

        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);
        with_repo_errors(callback, "journal entries list", [&] {
            Ledger::JournalRepository repo;
            auto rows = repo.list_filtered(ctx.org_id, from_filter, to_filter, page.limit, page.offset);
            long total = repo.count_filtered(ctx.org_id, from_filter, to_filter);
            json data = json::array();
            for (const auto& e : rows)
                data.push_back(e);
            callback(Response::paginated(data, total, page.limit, page.offset));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/journal-entries — create a draft entry. Accountant/owner
    // only. Body: {entry_date, description, lines: [{account_code, side,
    // amount, counterparty_id?, vat_amount?}, ...]}.
    // -------------------------------------------------------------------
    void create(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (ctx.role == "viewer") {
            callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot create journal entries"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "entry_date");
        Validation::require(errs, body, "description");
        Validation::require(errs, body, "lines");
        if (body.contains("entry_date") && !body["entry_date"].is_string())
            errs.add("entry_date", "not_string", "must be a string");
        if (body.contains("description") && !body["description"].is_string())
            errs.add("description", "not_string", "must be a string");
        if (body.contains("lines") && !body["lines"].is_array())
            errs.add("lines", "not_array", "must be an array");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        const std::string entry_date = body["entry_date"].get<std::string>();
        if (!is_valid_date(entry_date)) {
            callback(Validation::response_422("entry_date", "invalid_date", "must be in YYYY-MM-DD format"));
            return;
        }

        std::vector<Ledger::JournalLine> lines;
        Validation::Errors line_errs;
        if (!parse_lines(body["lines"], lines, line_errs)) {
            callback(Validation::response_400(line_errs));
            return;
        }

        const std::string description = body["description"].get<std::string>();

        try {
            Ledger::JournalService svc;
            auto entry = svc.create_draft(ctx.org_id, ctx.user_id, entry_date, description, lines);
            callback(Response::created({{"data", json(entry)}}));
        } catch (const Ledger::UnbalancedEntry& e) {
            callback(Validation::response_422("lines", "unbalanced_entry", e.what()));
        } catch (const Ledger::UnknownAccount& e) {
            callback(Validation::response_422("lines", "unknown_account", e.what()));
        } catch (const Ledger::ForeignCounterparty& e) {
            callback(Validation::response_422("lines", "foreign_counterparty", e.what()));
        } catch (const std::invalid_argument& e) {
            // parse_tiyn() (malformed amount) and the side-must-be-debit-or-
            // credit check both throw this — neither is one of
            // JournalService's named typed errors, so it isn't caught above,
            // but it is exactly as much a 422-shaped bad VALUE as they are.
            callback(Validation::response_422("lines", "invalid_line", e.what()));
        } catch (const std::exception& e) {
            spdlog::error("journal entries create failed: {}", e.what());
            callback(ErrorResponse::internal_error());
        }
    }

    // -------------------------------------------------------------------
    // GET /api/v1/journal-entries/{id} — with lines.
    // -------------------------------------------------------------------
    void get(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed journal entry id"));
            return;
        }

        with_repo_errors(callback, "journal entries get", [&] {
            Ledger::JournalRepository repo;
            auto found = repo.find_in_org(id, ctx.org_id);
            if (!found) {
                callback(ErrorResponse::not_found("journal entry"));
                return;
            }
            found->lines = repo.load_lines(*found);
            callback(Response::ok({{"data", json(*found)}}));
        });
    }

    // -------------------------------------------------------------------
    // POST /api/v1/journal-entries/{id}/post — draft -> posted. Accountant/
    // owner only.
    // -------------------------------------------------------------------
    void post(const HttpRequestPtr& req,
              std::function<void(const HttpResponsePtr&)>&& callback,
              const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (ctx.role == "viewer") {
            callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot post journal entries"));
            return;
        }
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed journal entry id"));
            return;
        }

        try {
            Ledger::JournalService svc;
            auto updated = svc.post(ctx.org_id, id);
            if (!updated) {
                callback(ErrorResponse::not_found("journal entry"));
                return;
            }
            callback(Response::ok({{"data", json(*updated)}}));
        } catch (const Ledger::InvalidEntryState& e) {
            callback(ErrorResponse::conflict("invalid_entry_state", e.what()));
        } catch (const std::exception& e) {
            spdlog::error("journal entries post failed: {}", e.what());
            callback(ErrorResponse::internal_error());
        }
    }

    // -------------------------------------------------------------------
    // POST /api/v1/journal-entries/{id}/reverse — posted -> reversed, plus a
    // NEW, already-posted storno entry (returned as `data`). Accountant/
    // owner only.
    // -------------------------------------------------------------------
    void reverse(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 const std::string& id) {
        API_REQUIRE_ORG(req, callback, ctx);
        if (ctx.role == "viewer") {
            callback(ErrorResponse::forbidden("viewer_read_only", "Viewers cannot reverse journal entries"));
            return;
        }
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed journal entry id"));
            return;
        }

        try {
            Ledger::JournalService svc;
            auto storno = svc.reverse(ctx.org_id, id, ctx.user_id);
            if (!storno) {
                callback(ErrorResponse::not_found("journal entry"));
                return;
            }
            callback(Response::ok({{"data", json(*storno)}}));
        } catch (const Ledger::InvalidEntryState& e) {
            callback(ErrorResponse::conflict("invalid_entry_state", e.what()));
        } catch (const std::exception& e) {
            spdlog::error("journal entries reverse failed: {}", e.what());
            callback(ErrorResponse::internal_error());
        }
    }

private:
    /// `YYYY-MM-DD`, month 01-12, day 01-31 — a shape+range check, not a
    /// full calendar validator (it does not know April has 30 days or
    /// reject a Feb 30). Good enough to keep an obviously-garbage string
    /// (wrong separator, letters, wrong field count) from ever reaching
    /// Postgres's own `::date` cast; a rare calendar-invalid-but-shape-valid
    /// value (e.g. "2026-02-30") still surfaces as a raw SQL error mapped to
    /// 500 by with_repo_errors(), the same residual gap
    /// DocumentRepository's CHECK-trusting posture leaves elsewhere.
    static bool is_valid_date(const std::string& s) {
        static const std::regex re(R"(^(\d{4})-(\d{2})-(\d{2})$)");
        std::smatch m;
        if (!std::regex_match(s, m, re))
            return false;
        const int month = std::stoi(m[2].str());
        const int day = std::stoi(m[3].str());
        return month >= 1 && month <= 12 && day >= 1 && day <= 31;
    }

    /**
     * @brief Parse the `lines` JSON array into `Ledger::JournalLine`s,
     *        checking only SHAPE (presence + JSON type of each field) —
     *        VALUE-level checks (side is actually "debit"/"credit", amount
     *        parses as a positive decimal, account_code/counterparty_id
     *        actually resolve for this org) are JournalService's job, not
     *        this controller's; see the file header for the resulting
     *        400-vs-422 split. The one exception is `counterparty_id`'s
     *        UUID *format* — checked here (see file header) because a
     *        malformed uuid string would otherwise reach a raw SQL cast
     *        before JournalService gets a chance to translate it into
     *        ForeignCounterparty.
     * @return false (with @p errs filled) on any shape violation; true with
     *         @p out filled otherwise.
     */
    static bool parse_lines(const json& arr, std::vector<Ledger::JournalLine>& out, Validation::Errors& errs) {
        out.reserve(arr.size());
        for (const auto& item : arr) {
            if (!item.is_object()) {
                errs.add("lines", "not_object", "each line must be an object");
                continue;
            }
            Ledger::JournalLine line;

            if (!item.contains("account_code") || !item["account_code"].is_string() ||
                item["account_code"].get<std::string>().empty()) {
                errs.add("lines", "missing_account_code", "each line needs a non-empty account_code");
            } else {
                line.account_code = item["account_code"].get<std::string>();
            }

            if (!item.contains("side") || !item["side"].is_string()) {
                errs.add("lines", "missing_side", "each line needs a side ('debit' or 'credit')");
            } else {
                line.side = item["side"].get<std::string>();
            }

            if (!item.contains("amount") || !item["amount"].is_string()) {
                errs.add("lines", "missing_amount", "each line needs an amount as a decimal string");
            } else {
                line.amount = item["amount"].get<std::string>();
            }

            if (item.contains("counterparty_id") && !item["counterparty_id"].is_null()) {
                if (!item["counterparty_id"].is_string() || !is_valid_uuid(item["counterparty_id"].get<std::string>()))
                    errs.add("lines", "invalid_counterparty_id", "counterparty_id must be a uuid");
                else
                    line.counterparty_id = item["counterparty_id"].get<std::string>();
            }

            if (item.contains("vat_amount") && !item["vat_amount"].is_null()) {
                if (!item["vat_amount"].is_string())
                    errs.add("lines", "invalid_vat_amount", "vat_amount must be a decimal string");
                else
                    line.vat_amount = item["vat_amount"].get<std::string>();
            }

            out.push_back(std::move(line));
        }
        return !errs.any();
    }
};

}  // namespace Api
