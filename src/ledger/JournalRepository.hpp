/**
 * @file JournalRepository.hpp
 * @brief Read access to `journal_entries` / `journal_lines`
 *        (migrations/009_journal.sql — design spec §6.2).
 *
 * Org-scoped like Ledger::CounterpartyRepository, so this extends
 * Tenancy::OrgCrudBase for header reads — find_in_org/list_in_org/
 * count_in_org against `journal_entries`, mirroring
 * Tenancy::OrgMemberRepository's self-contained style (every method opens
 * its own Database::get().execute_read/execute_write; nothing here takes a
 * caller-supplied transaction).
 *
 * Deliberately NO insert/update methods: creating a draft entry, posting it,
 * and reversing a posted one each need MULTIPLE statements (header + N
 * lines, or header+lines+two status flips) to land in exactly ONE database
 * transaction — migrations/009_journal.sql's balance trigger is a DEFERRABLE
 * constraint checked once at COMMIT, and the lines-frozen trigger requires
 * the parent entry to already exist as 'draft' before its lines can be
 * inserted. Splitting those statements across several
 * Database::get().execute_write calls (as a repo method mirroring
 * OrgMemberRepository::add() one-statement-per-call would) would each commit
 * on its own, and the balance trigger would reject the first, still
 * incomplete, entry. There is no existing "transaction-composable
 * repository method" idiom in this codebase (every other repository and
 * src/database/Migrations.hpp's multi-statement migration-apply transaction
 * both just inline txn.exec_params calls in one execute_write lambda), so
 * the write side of the journal — including all of its raw SQL — lives in
 * Ledger::JournalService instead of here, each transition wrapped in its own
 * single Database::get().execute_write call. This repository only ever
 * reads.
 */

#pragma once

#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "ledger/JournalEntry.hpp"
#include "tenancy/OrgScoped.hpp"

namespace Ledger {

class JournalRepository : public Tenancy::OrgCrudBase<JournalRepository, JournalEntry, std::string> {
public:
    // OrgCrudBase contract — supplies find_in_org(id,org_id)/
    // list_in_org(org_id,limit,offset)/count_in_org(org_id) against
    // journal_entries headers. Lines are never selected here (see
    // load_lines) — a header row alone never tells you whether its lines
    // are loaded, so from_row leaves JournalEntry::lines empty by design.
    static constexpr const char* kTable = "journal_entries";
    static constexpr const char* kColumns =
        "id, org_id, entry_date, description, status, reverses_entry_id, created_by_user_id";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "entry_date DESC, created_at DESC";
    static constexpr const char* kOrgColumn = "org_id";

    /// Shared with JournalService's write-side RETURNING clauses so both
    /// files agree on the exact line shape read back from Postgres.
    static constexpr const char* kLineColumns = "id, entry_id, account_code, side, amount, counterparty_id, vat_amount";

    /**
     * @brief Lines of @p entry, in insertion order (oldest first). A
     *        standalone read — for the journal_lines rows written moments
     *        earlier inside a still-open write transaction, JournalService
     *        reads them off that same transaction directly instead (see
     *        that file for why).
     * @p from_primary mirrors OrgCrudBase::find_in_org's parameter of the
     *        same name: pass true right after a write (e.g. JournalService
     *        re-loading lines just posted/reversed) so a lagging replica
     *        can't return an empty/stale array.
     */
    std::vector<JournalLine> load_lines(const JournalEntry& entry, bool from_primary = false) {
        auto query = [&](auto& txn) {
            auto r = txn.exec_params("SELECT " + std::string(kLineColumns) +
                                         " FROM journal_lines WHERE entry_id = $1 ORDER BY created_at, id",
                                     entry.id);
            std::vector<JournalLine> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(JournalLine::from_row(row));
            return out;
        };
        return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
    }

    /// Entry headers for @p org_id (OrgCrudBase::list_in_org) with their
    /// lines attached — one extra query per entry (N+1); acceptable at P1
    /// volumes, revisit with a JOIN/array_agg if journal listing pages grow.
    std::vector<JournalEntry> list_in_org_with_lines(const std::string& org_id, int limit = 100, int offset = 0) {
        auto entries = list_in_org(org_id, limit, offset);
        for (auto& entry : entries)
            entry.lines = load_lines(entry);
        return entries;
    }
};

}  // namespace Ledger
