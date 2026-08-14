/**
 * @file Payslip.hpp
 * @brief Payroll run (header) and payslip rows. Mirror the `payroll_runs` /
 *        `payslips` tables (migrations/013_payroll.sql — design spec §7.2).
 *
 * Domain-only — no SQL here; persistence lives in
 * src/payroll/PayrollRepository.hpp (reads) and src/payroll/PayrollService.hpp
 * (the writes, which need several statements per DB transaction — same
 * rationale as src/ledger/JournalService.hpp: a recalculation replaces every
 * payslip of a run in one transaction, so a half-written prior attempt is
 * never visible). Follows the same from_row/to_json idioms as
 * src/ledger/JournalEntry.hpp: from_row is a templated static factory (works
 * with any pqxx row-like type), to_json is a free function found via ADL, and
 * `PayrollRun::from_row` fills only the header columns — it does NOT
 * populate `payslips` (there is no payslip data in a header-only SELECT),
 * exactly mirroring JournalEntry::lines. Callers that need the payslips call
 * PayrollRepository::list_payslips, or (inside PayrollService) load/build
 * them within the same transaction.
 *
 * Every amount is integer TIYN — no floating point (same convention as
 * Payroll::Result, whose fields these columns mirror one-to-one).
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Payroll {

struct Payslip {
    std::string id;
    std::string org_id;
    std::string run_id;
    std::string employee_id;
    long long gross_tiyn = 0;
    long long opv = 0;
    long long vosms = 0;
    long long ipn = 0;
    long long net = 0;
    long long opvr = 0;
    long long so = 0;
    long long osms = 0;
    long long social_tax = 0;
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Payslip from_row(const Row& row) {
        Payslip p;
        p.id = row["id"].template as<std::string>();
        p.org_id = row["org_id"].template as<std::string>();
        p.run_id = row["run_id"].template as<std::string>();
        p.employee_id = row["employee_id"].template as<std::string>();
        p.gross_tiyn = row["gross_tiyn"].template as<long long>();
        p.opv = row["opv"].template as<long long>();
        p.vosms = row["vosms"].template as<long long>();
        p.ipn = row["ipn"].template as<long long>();
        p.net = row["net"].template as<long long>();
        p.opvr = row["opvr"].template as<long long>();
        p.so = row["so"].template as<long long>();
        p.osms = row["osms"].template as<long long>();
        p.social_tax = row["social_tax"].template as<long long>();
        p.created_at = row["created_at"].template as<std::string>();
        p.updated_at = row["updated_at"].template as<std::string>();
        return p;
    }
};

inline void to_json(nlohmann::json& j, const Payslip& p) {
    j = nlohmann::json{
        {"id", p.id},
        {"org_id", p.org_id},
        {"run_id", p.run_id},
        {"employee_id", p.employee_id},
        {"gross_tiyn", p.gross_tiyn},
        {"opv", p.opv},
        {"vosms", p.vosms},
        {"ipn", p.ipn},
        {"net", p.net},
        {"opvr", p.opvr},
        {"so", p.so},
        {"osms", p.osms},
        {"social_tax", p.social_tax},
        {"created_at", p.created_at},
        {"updated_at", p.updated_at},
    };
}

/// One row of `payroll_runs`: one org/period's payroll calculation. `status`
/// is a simple two-position lifecycle ('draft' -> 'approved', see
/// PayrollService::approve) — no state machine beyond the DB CHECK, same
/// posture as Hr::Employee::status. `rates_snapshot` is the exact
/// Tax::TaxReferenceRepository rates/constants PayrollService resolved for
/// this period (see PayrollService::calculate_run), so a later change to
/// migrations/011_tax_reference.sql's seed can never silently change what an
/// already-calculated run reports.
struct PayrollRun {
    std::string id;
    std::string org_id;
    int period_year = 0;
    int period_month = 0;
    std::string status;  // 'draft' | 'approved' — CHECK in migrations/013_payroll.sql
    std::string calculated_at;
    nlohmann::json rates_snapshot = nlohmann::json::object();
    // NULL until PayrollService::post_to_journal succeeds; set together in
    // the same statement — journal_entry_id having a value IS "this run has
    // been posted", the guard PayrollService::post_to_journal's
    // compare-and-swap reads (see that method's Doxygen, Fix round 1).
    std::optional<std::string> journal_entry_id;
    std::optional<std::string> posted_at;
    std::string created_at;
    std::string updated_at;
    std::vector<Payslip> payslips;  // NOT populated by from_row — see file header

    template <typename Row>
    static PayrollRun from_row(const Row& row) {
        PayrollRun r;
        r.id = row["id"].template as<std::string>();
        r.org_id = row["org_id"].template as<std::string>();
        r.period_year = row["period_year"].template as<int>();
        r.period_month = row["period_month"].template as<int>();
        r.status = row["status"].template as<std::string>();
        r.calculated_at = row["calculated_at"].template as<std::string>();
        try {
            r.rates_snapshot = nlohmann::json::parse(row["rates_snapshot"].template as<std::string>());
        } catch (...) {
            r.rates_snapshot = nlohmann::json::object();
        }
        if (!row["journal_entry_id"].is_null())
            r.journal_entry_id = row["journal_entry_id"].template as<std::string>();
        if (!row["posted_at"].is_null())
            r.posted_at = row["posted_at"].template as<std::string>();
        r.created_at = row["created_at"].template as<std::string>();
        r.updated_at = row["updated_at"].template as<std::string>();
        return r;
    }
};

inline void to_json(nlohmann::json& j, const PayrollRun& r) {
    j = nlohmann::json{
        {"id", r.id},
        {"org_id", r.org_id},
        {"period_year", r.period_year},
        {"period_month", r.period_month},
        {"status", r.status},
        {"calculated_at", r.calculated_at},
        {"rates_snapshot", r.rates_snapshot},
        {"journal_entry_id", r.journal_entry_id ? nlohmann::json(*r.journal_entry_id) : nlohmann::json(nullptr)},
        {"posted_at", r.posted_at ? nlohmann::json(*r.posted_at) : nlohmann::json(nullptr)},
        {"created_at", r.created_at},
        {"updated_at", r.updated_at},
        {"payslips", r.payslips},
    };
}

}  // namespace Payroll
