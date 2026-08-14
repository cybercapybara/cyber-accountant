/**
 * @file JournalEntry.hpp
 * @brief Journal entry (header) and journal line rows. Mirror the
 *        `journal_entries` / `journal_lines` tables (migrations/009_journal.sql
 *        — design spec §6.2, double-entry ledger).
 *
 * Domain-only — no SQL here; persistence lives in
 * src/ledger/JournalRepository.hpp (reads) and src/ledger/JournalService.hpp
 * (the writes, which need several statements per DB transaction — see that
 * file's header for why). Follows the same from_row/to_json idioms as
 * src/ledger/Account.hpp and src/ledger/Counterparty.hpp: from_row is a
 * templated static factory (works with any pqxx row-like type), and to_json
 * is a free function found via ADL.
 *
 * `amount` and `vat_amount` are kept as DECIMAL-shaped strings ("1234.56"),
 * not a numeric type — they round-trip the `NUMERIC(18,2)` columns exactly as
 * Postgres formats them, with no float rounding in between. Ledger::JournalService
 * is the only place that ever needs the value as an integer (tiyn) for the
 * balance check, via its `parse_tiyn()` helper; the domain type itself stays
 * string-based end to end, same rationale as `entry_date` staying a string
 * instead of a date type.
 *
 * `JournalEntry::from_row` fills only the header columns — it does NOT
 * populate `lines` (there is no line data in a header-only SELECT). Callers
 * that need the lines call JournalRepository::load_lines /
 * list_in_org_with_lines, or (inside JournalService) load them within the
 * same transaction.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Ledger {

struct JournalLine {
    std::string id;
    std::string entry_id;
    std::string account_code;
    std::string side;    // 'debit' | 'credit'
    std::string amount;  // NUMERIC(18,2) as text, e.g. "1234.56" — always > 0
    std::optional<std::string> counterparty_id;
    std::optional<std::string> vat_amount;  // NUMERIC(18,2) as text, >= 0 when present

    template <typename Row>
    static JournalLine from_row(const Row& row) {
        JournalLine l;
        l.id = row["id"].template as<std::string>();
        l.entry_id = row["entry_id"].template as<std::string>();
        l.account_code = row["account_code"].template as<std::string>();
        l.side = row["side"].template as<std::string>();
        l.amount = row["amount"].template as<std::string>();
        if (!row["counterparty_id"].is_null())
            l.counterparty_id = row["counterparty_id"].template as<std::string>();
        if (!row["vat_amount"].is_null())
            l.vat_amount = row["vat_amount"].template as<std::string>();
        return l;
    }
};

inline void to_json(nlohmann::json& j, const JournalLine& l) {
    j = nlohmann::json{
        {"id", l.id},
        {"entry_id", l.entry_id},
        {"account_code", l.account_code},
        {"side", l.side},
        {"amount", l.amount},
        {"counterparty_id", l.counterparty_id ? nlohmann::json(*l.counterparty_id) : nlohmann::json(nullptr)},
        {"vat_amount", l.vat_amount ? nlohmann::json(*l.vat_amount) : nlohmann::json(nullptr)},
    };
}

struct JournalEntry {
    std::string id;
    std::string org_id;
    std::string entry_date;  // DATE as text, "YYYY-MM-DD"
    std::string description;
    std::string status;  // 'draft' | 'posted' | 'reversed'
    std::optional<std::string> reverses_entry_id;
    std::optional<std::string> created_by_user_id;
    std::vector<JournalLine> lines;  // NOT populated by from_row — see file header

    template <typename Row>
    static JournalEntry from_row(const Row& row) {
        JournalEntry e;
        e.id = row["id"].template as<std::string>();
        e.org_id = row["org_id"].template as<std::string>();
        e.entry_date = row["entry_date"].template as<std::string>();
        e.description = row["description"].template as<std::string>();
        e.status = row["status"].template as<std::string>();
        if (!row["reverses_entry_id"].is_null())
            e.reverses_entry_id = row["reverses_entry_id"].template as<std::string>();
        if (!row["created_by_user_id"].is_null())
            e.created_by_user_id = row["created_by_user_id"].template as<std::string>();
        return e;
    }
};

inline void to_json(nlohmann::json& j, const JournalEntry& e) {
    j = nlohmann::json{
        {"id", e.id},
        {"org_id", e.org_id},
        {"entry_date", e.entry_date},
        {"description", e.description},
        {"status", e.status},
        {"reverses_entry_id", e.reverses_entry_id ? nlohmann::json(*e.reverses_entry_id) : nlohmann::json(nullptr)},
        {"created_by_user_id", e.created_by_user_id ? nlohmann::json(*e.created_by_user_id) : nlohmann::json(nullptr)},
        {"lines", e.lines},
    };
}

}  // namespace Ledger
