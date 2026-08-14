/**
 * @file AccountRepository.hpp
 * @brief All SQL touching `accounts` lives here.
 *
 * `accounts.org_id` is the ONE documented exception to "org_id NOT NULL"
 * (design spec §6.1 / Global Constraints of the P1 plan): NULL rows are the
 * Kazakhstan standard chart of accounts (приказ МФ РК №185), seeded once in
 * migrations/008_accounts.sql and shared read-only across every tenant;
 * non-NULL rows are subaccounts a tenant created under a system (or their
 * own) parent. Because "visible" means "system OR mine" rather than "mine",
 * this repository does NOT extend Tenancy::OrgCrudBase — that base only
 * knows how to scope a query to exactly one org (`WHERE org_id = $1`), which
 * would hide every system row from every tenant. Instead list_visible/
 * find_visible/create_subaccount run their own `WHERE org_id IS NULL OR
 * org_id = $1` queries. Do not "fix" this into OrgCrudBase without revisiting
 * that base's contract — the two-way OR is the point, not an oversight.
 *
 * Mirrors Ledger::CounterpartyRepository: constraint violations surface as a
 * typed exception (DuplicateAccount) via Repositories::detail::translate_sql,
 * so the HTTP layer maps them to 409 via Api::with_repo_errors() without
 * sniffing SQLSTATEs itself. Subaccount validation (parent missing/invisible,
 * code not extending the parent's) is NOT a constraint violation — it can
 * never reach Postgres, so it is surfaced as InvalidSubaccount instead (see
 * that type for why it isn't a Repositories::RepoError).
 */

#pragma once

#include <optional>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "ledger/Account.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"

namespace Ledger {

// Stable 409 code carried on the exception, so with_repo_errors() maps it
// without including this header.
struct DuplicateAccount : Repositories::ConflictError {
    DuplicateAccount()
        : Repositories::ConflictError("account_code_taken",
                                      "An account with that code already exists for this organization (or is a "
                                      "reserved system code)") {}
};

/**
 * @brief Thrown by create_subaccount() when the requested subaccount is
 *        malformed at the domain level (parent not found/not visible to the
 *        caller's org, or the new code doesn't extend the parent's). This is
 *        a 4xx-shaped input error, but RepoErrors.hpp only offers
 *        NotFoundError (404) and ConflictError (409) — neither fits a plain
 *        validation failure — so, following the sibling precedent of
 *        Jobs::PermanentJobError (src/jobs/Job.hpp), it derives directly
 *        from std::runtime_error rather than growing a new Repositories base
 *        for a single caller. Callers that expose this over HTTP map it
 *        explicitly (it is not one of the types Api::with_repo_errors()
 *        already understands).
 */
struct InvalidSubaccount : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class AccountRepository {
public:
    static constexpr const char* kTable = "accounts";
    static constexpr const char* kColumns =
        "id, org_id, code, name_ru, name_kk, type, parent_code, currency_tracked, created_at, updated_at";

    /// System accounts (org_id IS NULL) plus @p org_id's own subaccounts,
    /// ordered by code. There is deliberately no unscoped list() — every
    /// caller has an org context (or is explicitly asking for the system
    /// chart via a NULL-safe caller of its own).
    std::vector<Account> list_visible(const std::string& org_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT " + std::string(kColumns) +
                                         " FROM accounts WHERE org_id IS NULL OR org_id = $1 ORDER BY code",
                                     org_id);
            std::vector<Account> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Account::from_row(row));
            return out;
        });
    }

    /// Single account by code, visible to @p org_id (system or its own).
    std::optional<Account> find_visible(const std::string& org_id, const std::string& code) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Account> {
            auto r = txn.exec_params("SELECT " + std::string(kColumns) +
                                         " FROM accounts WHERE (org_id IS NULL OR org_id = $1) AND code = $2",
                                     org_id,
                                     code);
            if (r.empty())
                return std::nullopt;
            return Account::from_row(r[0]);
        });
    }

    /**
     * @brief Create a subaccount for @p org_id under @p parent_code.
     *
     * Validates (throwing InvalidSubaccount, not a SQL error, since neither
     * check corresponds to a table constraint):
     *  - the parent must exist and be visible to @p org_id (system or the
     *    org's own account) — looked up via find_visible, so a parent that
     *    belongs to a DIFFERENT org is indistinguishable from a missing one;
     *  - @p code must extend the parent's code (start with it) — enforces
     *    the subaccount-numbering convention (e.g. "1030.1" under "1030").
     *
     * Then checks find_visible(org_id, code) for an existing row under that
     * exact code — this is what makes DuplicateAccount fire for a code that
     * collides with a RESERVED SYSTEM code, not just the org's own rows: the
     * UNIQUE index on (org_id, code) only guards a tenant against duplicating
     * ITS OWN codes (system rows have org_id IS NULL, a different partial
     * index), so without this pre-check a tenant could shadow "1030" under
     * their own org_id. translate_sql() on the INSERT is defense-in-depth
     * against the same-org race the unique index still catches.
     *
     * @p type and @p currency_tracked are NOT parameters — the subaccount
     * always inherits both from the parent (design spec §6.1: a subaccount
     * is the same kind of account as its parent, just more granular).
     */
    Account create_subaccount(const std::string& org_id,
                              const std::string& code,
                              const std::string& name_ru,
                              const std::string& name_kk,
                              const std::string& parent_code) {
        auto parent = find_visible(org_id, parent_code);
        if (!parent)
            throw InvalidSubaccount("parent account '" + parent_code +
                                    "' not found or not visible to this organization");
        if (!code.starts_with(parent->code))
            throw InvalidSubaccount("subaccount code '" + code + "' must start with parent code '" + parent->code +
                                    "'");
        if (find_visible(org_id, code))
            throw DuplicateAccount{};

        return Repositories::detail::translate_sql(
            [&]() -> Account {
                return Database::get().execute_write([&](auto& txn) -> Account {
                    auto r = txn.exec_params(
                        "INSERT INTO accounts (org_id, code, name_ru, name_kk, type, parent_code, "
                        "currency_tracked) VALUES ($1, $2, $3, $4, $5, $6, $7) RETURNING " +
                            std::string(kColumns),
                        org_id,
                        code,
                        name_ru,
                        name_kk,
                        parent->type,
                        parent->code,
                        parent->currency_tracked);
                    return Account::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicateAccount{};
            });
    }
};

}  // namespace Ledger
