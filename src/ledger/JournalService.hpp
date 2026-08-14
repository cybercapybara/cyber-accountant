/**
 * @file JournalService.hpp
 * @brief Business logic for the double-entry journal: draft/post/storno
 *        lifecycle (design spec §6.2, migrations/009_journal.sql).
 *
 * Ledger::JournalRepository is read-only (see that file's header for why);
 * every write lives here, one Database::get().execute_write per state
 * transition:
 *   - create_draft(): one transaction inserts the header AND every line —
 *     migrations/009_journal.sql's balance trigger is a DEFERRABLE
 *     constraint checked once at COMMIT, so all lines must land in the same
 *     transaction as the header or an intermediate, still-unbalanced state
 *     would be rejected.
 *   - post(): a single atomic `UPDATE ... WHERE status = 'draft'` — one
 *     statement is already one transaction, no extra wrapping needed.
 *   - reverse(): ONE transaction does all four steps the brief's ruling
 *     requires, in this exact order (trg_journal_lines_frozen — migration
 *     009 — rejects inserting lines into a non-draft entry, which is why the
 *     storno header must be created as 'draft' FIRST):
 *       1. INSERT the storno header, status='draft', reverses_entry_id =
 *          the original;
 *       2. INSERT its lines, mirrored from the original with sides flipped,
 *          same accounts/amounts/counterparties/VAT;
 *       3. UPDATE the storno header to 'posted' (storno is posted
 *          immediately, in the same transaction as its own creation);
 *       4. UPDATE the original to 'reversed' — legal under
 *          journal_entries_immutability() only because this UPDATE touches
 *          ONLY the status column, so every other column stays identical to
 *          OLD (the trigger's sole legal posted->reversed transition).
 *
 * Validation for create_draft (line count, account visibility, counterparty
 * ownership, Σdebit=Σcredit) all happens BEFORE any of this touches the
 * database — the brief's explicit reason is so the HTTP layer can map a bad
 * request to 422 without ever seeing a wrapped 500 from a failed INSERT or a
 * deferred-constraint violation at COMMIT. The DB triggers are strictly a
 * second line of defense (they can't be bypassed even if this service has a
 * bug), not the primary validation path.
 *
 * Typed errors (UnbalancedEntry, UnknownAccount, ForeignCounterparty,
 * InvalidEntryState) derive directly from std::runtime_error rather than
 * Repositories::RepoError — same rationale as Ledger::AccountRepository's
 * InvalidSubaccount: RepoErrors.hpp only offers NotFoundError (404) and
 * ConflictError (409), neither of which fits a plain validation failure,
 * and these are 422-shaped input errors a caller maps explicitly, not
 * SQLSTATE-driven repository conflicts. UnknownAccount (a line references an
 * account code not visible to the org) and ForeignCounterparty (a line's
 * counterparty_id belongs to a DIFFERENT org) are kept as two DISTINCT types
 * rather than reusing one type for both: they name unrelated invariants
 * (accounts.org_id visibility vs. counterparties.org_id ownership) checked
 * against two different repositories, and a caller mapping errors to
 * response bodies benefits from telling "bad account" and "bad counterparty"
 * apart without parsing the message string.
 */

#pragma once

#include <cctype>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "ledger/AccountRepository.hpp"
#include "ledger/CounterpartyRepository.hpp"
#include "ledger/JournalEntry.hpp"
#include "ledger/JournalRepository.hpp"

namespace Ledger {

/// → 422. Thrown when an entry has fewer than 2 lines, or Σdebit != Σcredit
/// (compared in tiyn, i.e. exactly — no floating-point tolerance).
struct UnbalancedEntry : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// → 422. A line's account_code is not visible to the entry's org (neither a
/// system account nor one of the org's own subaccounts — see
/// AccountRepository::find_visible).
struct UnknownAccount : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// → 422. A line's counterparty_id does not belong to the entry's org (see
/// file header for why this is a distinct type from UnknownAccount).
struct ForeignCounterparty : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// → 422/409-shaped. Thrown when post()/reverse() is asked to act on an
/// entry that exists (in the caller's org) but is in the wrong status for
/// that operation (e.g. posting an already-posted entry, or reversing a
/// draft). Contrast with returning std::nullopt, which means "no such entry
/// visible to this org" — the same not-found-vs-wrong-state distinction
/// OrgCrudBase::find_in_org's own doc comment draws.
struct InvalidEntryState : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/**
 * @brief Parse a decimal amount string ("1234.56") into tiyn (1/100 of a
 *        tenge) as an exact integer — the only money representation
 *        JournalService compares or sums, so two entries can never disagree
 *        by a floating-point rounding error.
 *
 * Accepts: an optional integer part, an optional '.' followed by AT MOST 2
 * digits (a bare "123" means "123.00"). Rejects: empty input, a sign of
 * either kind (amounts are always positive — side, debit vs credit, carries
 * the direction), more than one decimal point, more than 2 fractional
 * digits, any non-digit character, an integer part longer than the
 * journal_lines.amount column can hold, and a zero result (that column has
 * a DB-level `CHECK (amount > 0)` — this mirrors both at the service layer
 * so the 422 fires before the DB is touched).
 *
 * The integer-part length guard (`> 16` digits) is NOT the NUMERIC(18,2)
 * column's own limit re-derived carelessly — it is specifically sized so
 * `whole * 100 + frac` below cannot overflow `long long`. NUMERIC(18,2)
 * allows at most 16 integer digits (18 total precision - 2 scale), and the
 * worst case at that length, 9999999999999999 * 100 + 99 = 999999999999999999,
 * is still well under LLONG_MAX (~9.22e18). One more digit
 * (17, e.g. 99999999999999999) would make `whole * 100` alone
 * (9999999999999999900) exceed LLONG_MAX — undefined behaviour on a plain
 * signed multiply, not an exception — so the guard runs BEFORE std::stoll,
 * not as a catch on its own std::out_of_range (std::stoll would only throw
 * for int_part itself overflowing long long, i.e. >19 digits; the
 * downstream `* 100 + frac` overflow at 17-19 digits would otherwise slip
 * through as UB, reachable straight from caller-supplied input before any
 * DB constraint ever sees it).
 *
 * @throws std::invalid_argument on any of the above.
 */
inline long long parse_tiyn(const std::string& amount) {
    if (amount.empty())
        throw std::invalid_argument("parse_tiyn: empty amount");

    const auto dot = amount.find('.');
    const std::string int_part = (dot == std::string::npos) ? amount : amount.substr(0, dot);
    std::string frac_part;
    if (dot != std::string::npos) {
        frac_part = amount.substr(dot + 1);
        if (frac_part.find('.') != std::string::npos)
            throw std::invalid_argument("parse_tiyn: more than one decimal point in '" + amount + "'");
        if (frac_part.size() > 2)
            throw std::invalid_argument("parse_tiyn: more than 2 digits after the decimal point in '" + amount + "'");
    }
    if (int_part.empty())
        throw std::invalid_argument("parse_tiyn: missing integer part in '" + amount + "'");
    for (char c : int_part)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            throw std::invalid_argument("parse_tiyn: non-digit character (no sign allowed) in '" + amount + "'");
    for (char c : frac_part)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            throw std::invalid_argument("parse_tiyn: non-digit character in fractional part of '" + amount + "'");

    // Guard BEFORE std::stoll: at 17+ digits, `whole * 100` alone overflows
    // long long (UB), which is not something a try/catch around stoll can
    // ever observe — see the function doc comment for the exact bound.
    if (int_part.size() > 16)
        throw std::invalid_argument("parse_tiyn: integer part too long (max 16 digits) in '" + amount + "'");

    while (frac_part.size() < 2)
        frac_part.push_back('0');

    long long whole = 0;
    long long frac = 0;
    try {
        whole = std::stoll(int_part);
        frac = frac_part.empty() ? 0 : std::stoll(frac_part);
    } catch (const std::exception&) {
        throw std::invalid_argument("parse_tiyn: integer overflow in '" + amount + "'");
    }

    const long long tiyn = whole * 100 + frac;
    if (tiyn <= 0)
        throw std::invalid_argument("parse_tiyn: amount must be strictly positive, got '" + amount + "'");
    return tiyn;
}

/**
 * @brief The inverse of parse_tiyn(): render @p tiyn (1/100 of a tenge) as
 *        the decimal string ("1234.56") every journal line amount and
 *        vat_amount is stored as. Callers that only ever have an integer sum
 *        in hand (e.g. Payroll::PayrollService::post_to_journal summing
 *        several payslips) use this instead of hand-rolling `std::to_string
 *        (whole) + "." + ...` and its zero-padding edge cases.
 *
 * @p tiyn is expected non-negative — every caller in this codebase sums
 * amounts that are themselves non-negative by construction (payroll
 * withholding, tax calculations), so there is no sign to round-trip. Unlike
 * parse_tiyn(), zero is a valid input here: 0 -> "0.00" (a caller building a
 * complete rates_snapshot or displaying a per-category total may legitimately
 * have a zero bucket; only journal_lines.amount itself carries a DB-level
 * `CHECK (amount > 0)`, which JournalService::create_draft's own validation
 * mirrors before this formatter is ever involved).
 *
 * @throws std::invalid_argument if @p tiyn is negative — the doc comment
 * above documented this as an assumption but nothing enforced it (Fix round
 * 1, code review), so a caller bug upstream (a subtraction gone the wrong
 * way) would have silently produced a mangled string like "-1.-50" instead
 * of failing loudly at the one place that knows the invariant was broken.
 */
inline std::string format_tiyn(long long tiyn) {
    if (tiyn < 0)
        throw std::invalid_argument("format_tiyn: amount must be non-negative, got " + std::to_string(tiyn));
    const long long whole = tiyn / 100;
    const long long frac = tiyn % 100;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld.%02lld", whole, frac);
    return std::string(buf);
}

class JournalService {
public:
    /**
     * @brief Validate and insert a new draft entry with its lines, all in
     *        one transaction. See file header for the exact validation
     *        order and why it all happens before any SQL runs.
     */
    JournalEntry create_draft(const std::string& org_id,
                              const std::string& user_id,
                              const std::string& entry_date,
                              const std::string& description,
                              std::vector<JournalLine> lines) {
        if (lines.size() < 2)
            throw UnbalancedEntry("a journal entry needs at least 2 lines to balance, got " +
                                  std::to_string(lines.size()));

        long long debit_total = 0;
        long long credit_total = 0;
        for (const auto& line : lines) {
            const long long tiyn = parse_tiyn(line.amount);
            if (line.side == "debit") {
                debit_total += tiyn;
            } else if (line.side == "credit") {
                credit_total += tiyn;
            } else {
                throw std::invalid_argument("journal line side must be 'debit' or 'credit', got '" + line.side + "'");
            }

            if (!accounts_.find_visible(org_id, line.account_code))
                throw UnknownAccount("account '" + line.account_code + "' is not visible to this organization");

            if (line.counterparty_id && !counterparties_.find_in_org(*line.counterparty_id, org_id))
                throw ForeignCounterparty("counterparty '" + *line.counterparty_id +
                                          "' does not belong to this organization");
        }
        if (debit_total != credit_total)
            throw UnbalancedEntry("entry is unbalanced: debit=" + std::to_string(debit_total) +
                                  " tiyn, credit=" + std::to_string(credit_total) + " tiyn");

        return Database::get().execute_write([&](auto& txn) -> JournalEntry {
            auto er = txn.exec_params(
                "INSERT INTO journal_entries (org_id, entry_date, description, created_by_user_id) "
                "VALUES ($1, $2, $3, $4) RETURNING " +
                    std::string(JournalRepository::kColumns),
                org_id,
                entry_date,
                description,
                user_id);
            auto entry = JournalEntry::from_row(er[0]);

            entry.lines.reserve(lines.size());
            for (const auto& line : lines) {
                auto lr = txn.exec_params(
                    "INSERT INTO journal_lines (org_id, entry_id, account_code, side, amount, counterparty_id, "
                    "vat_amount) VALUES ($1, $2, $3, $4, $5, $6, $7) RETURNING " +
                        std::string(JournalRepository::kLineColumns),
                    org_id,
                    entry.id,
                    line.account_code,
                    line.side,
                    line.amount,
                    line.counterparty_id,
                    line.vat_amount);
                entry.lines.push_back(JournalLine::from_row(lr[0]));
            }
            return entry;
        });
    }

    /**
     * @return std::nullopt if no such entry is visible to @p org_id.
     * @throws InvalidEntryState if the entry exists but is not 'draft'
     *         (covers posting an already-posted or a reversed entry).
     */
    std::optional<JournalEntry> post(const std::string& org_id, const std::string& entry_id) {
        auto existing = journal_.find_in_org(entry_id, org_id, /*from_primary=*/true);
        if (!existing)
            return std::nullopt;
        if (existing->status != "draft")
            throw InvalidEntryState("cannot post an entry in status '" + existing->status + "'");

        auto updated = Database::get().execute_write([&](auto& txn) -> std::optional<JournalEntry> {
            auto r = txn.exec_params(
                "UPDATE journal_entries SET status = 'posted' WHERE id = $1 AND org_id = $2 AND status = 'draft' "
                "RETURNING " +
                    std::string(JournalRepository::kColumns),
                entry_id,
                org_id);
            if (r.empty())
                return std::nullopt;
            return JournalEntry::from_row(r[0]);
        });
        if (!updated)
            throw InvalidEntryState("entry status changed concurrently — no longer 'draft'");

        updated->lines = journal_.load_lines(*updated, /*from_primary=*/true);
        return updated;
    }

    /**
     * @brief Storno: create a new, immediately-posted entry that mirrors
     *        @p entry_id's lines with sides flipped, and mark the original
     *        'reversed'. See file header for the exact four-step order.
     * @return the NEW (storno) entry, posted, with its mirrored lines
     *         attached. std::nullopt if no such entry is visible to
     *         @p org_id.
     * @throws InvalidEntryState if the entry exists but is not 'posted'
     *         (covers reversing a draft or an already-reversed entry).
     */
    std::optional<JournalEntry> reverse(const std::string& org_id,
                                        const std::string& entry_id,
                                        const std::string& user_id) {
        auto original = journal_.find_in_org(entry_id, org_id, /*from_primary=*/true);
        if (!original)
            return std::nullopt;
        if (original->status != "posted")
            throw InvalidEntryState("cannot reverse an entry in status '" + original->status + "'");

        // Safe to load outside the write transaction: a 'posted' entry's
        // lines are frozen by trg_journal_lines_frozen (migration 009) —
        // they cannot change between this read and the transaction below.
        // from_primary=true for the same read-after-write reason as the
        // header lookup above (a caller reversing an entry it just posted
        // moments ago must not see a stale/empty replica read).
        const auto original_lines = journal_.load_lines(*original, /*from_primary=*/true);

        return Database::get().execute_write([&](auto& txn) -> JournalEntry {
            // (1) storno header, status='draft' — trg_journal_lines_frozen
            // only allows inserting lines into a 'draft' entry, so this
            // MUST happen before step (2).
            auto er = txn.exec_params(
                "INSERT INTO journal_entries (org_id, entry_date, description, reverses_entry_id, "
                "created_by_user_id) VALUES ($1, $2, $3, $4, $5) RETURNING " +
                    std::string(JournalRepository::kColumns),
                org_id,
                original->entry_date,
                "Сторно: " + original->description,
                original->id,
                user_id);
            auto storno = JournalEntry::from_row(er[0]);

            // (2) mirrored lines: same account/amount/counterparty/VAT,
            // side flipped.
            storno.lines.reserve(original_lines.size());
            for (const auto& line : original_lines) {
                const std::string mirrored_side = (line.side == "debit") ? "credit" : "debit";
                auto lr = txn.exec_params(
                    "INSERT INTO journal_lines (org_id, entry_id, account_code, side, amount, counterparty_id, "
                    "vat_amount) VALUES ($1, $2, $3, $4, $5, $6, $7) RETURNING " +
                        std::string(JournalRepository::kLineColumns),
                    org_id,
                    storno.id,
                    line.account_code,
                    mirrored_side,
                    line.amount,
                    line.counterparty_id,
                    line.vat_amount);
                storno.lines.push_back(JournalLine::from_row(lr[0]));
            }

            // (3) post the storno entry itself, in the same transaction.
            auto pr = txn.exec_params(
                "UPDATE journal_entries SET status = 'posted' WHERE id = $1 AND org_id = $2 AND status = 'draft' "
                "RETURNING status",
                storno.id,
                org_id);
            if (pr.empty())
                throw InvalidEntryState("failed to post the storno entry");
            storno.status = pr[0]["status"].template as<std::string>();

            // (4) mark the original 'reversed'. Only the status column is
            // written, so journal_entries_immutability()'s sole legal
            // posted->reversed transition (every OTHER column identical to
            // OLD) is satisfied by construction.
            auto orr = txn.exec_params(
                "UPDATE journal_entries SET status = 'reversed' WHERE id = $1 AND org_id = $2 AND status = 'posted' "
                "RETURNING status",
                original->id,
                org_id);
            if (orr.empty())
                throw InvalidEntryState("failed to mark the original entry reversed");

            return storno;
        });
    }

private:
    JournalRepository journal_;
    AccountRepository accounts_;
    CounterpartyRepository counterparties_;
};

}  // namespace Ledger
