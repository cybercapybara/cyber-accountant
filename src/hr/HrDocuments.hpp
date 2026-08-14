/**
 * @file HrDocuments.hpp
 * @brief LaborContract / HrOrder / Vacation rows — the three кадровый
 *        document types that hang off an Hr::Employee. Mirror the
 *        `labor_contracts` / `hr_orders` / `vacations` tables
 *        (migrations/012_hr.sql — design spec §7.2).
 *
 * Domain-only — no SQL here; persistence lives in src/hr/HrRepository.hpp.
 * Same from_row/to_json idioms as src/hr/Employee.hpp and
 * src/ledger/Document.hpp: from_row is a templated static factory, to_json
 * is a free function found via ADL, nullable columns become
 * std::optional<...> and are still rendered as an explicit JSON `null`.
 *
 * `terms_json` (LaborContract) and `payload` (HrOrder) are the JSONB
 * columns — read back with nlohmann::json::parse exactly like
 * Ledger::Document::input_snapshot: stays std::nullopt when the column is
 * actually NULL (no terms/payload were ever recorded), falls back to an
 * empty object on the (unexpected) case of a stored value that fails to
 * parse, since both columns are only ever written by this repository's own
 * create_contract()/create_order() via nlohmann::json::dump().
 */

#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace Hr {

struct LaborContract {
    std::string id;
    std::string org_id;
    std::string employee_id;
    std::string number;
    std::string signed_on;
    std::string starts_on;
    std::optional<std::string> ends_on;
    std::optional<nlohmann::json> terms_json;
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static LaborContract from_row(const Row& row) {
        LaborContract c;
        c.id = row["id"].template as<std::string>();
        c.org_id = row["org_id"].template as<std::string>();
        c.employee_id = row["employee_id"].template as<std::string>();
        c.number = row["number"].template as<std::string>();
        c.signed_on = row["signed_on"].template as<std::string>();
        c.starts_on = row["starts_on"].template as<std::string>();
        if (!row["ends_on"].is_null())
            c.ends_on = row["ends_on"].template as<std::string>();
        if (!row["terms_json"].is_null()) {
            try {
                c.terms_json = nlohmann::json::parse(row["terms_json"].template as<std::string>());
            } catch (...) {
                c.terms_json = nlohmann::json::object();
            }
        }
        c.created_at = row["created_at"].template as<std::string>();
        c.updated_at = row["updated_at"].template as<std::string>();
        return c;
    }
};

inline void to_json(nlohmann::json& j, const LaborContract& c) {
    j = nlohmann::json{
        {"id", c.id},
        {"org_id", c.org_id},
        {"employee_id", c.employee_id},
        {"number", c.number},
        {"signed_on", c.signed_on},
        {"starts_on", c.starts_on},
        {"ends_on", c.ends_on ? nlohmann::json(*c.ends_on) : nlohmann::json(nullptr)},
        {"terms_json", c.terms_json ? *c.terms_json : nlohmann::json(nullptr)},
        {"created_at", c.created_at},
        {"updated_at", c.updated_at},
    };
}

struct HrOrder {
    std::string id;
    std::string org_id;
    std::string employee_id;
    std::string kind;  // CHECK list — migrations/012_hr.sql
    std::string number;
    std::string issued_on;
    std::string effective_from;
    std::optional<std::string> effective_to;
    std::optional<nlohmann::json> payload;
    std::optional<std::string> document_id;
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static HrOrder from_row(const Row& row) {
        HrOrder o;
        o.id = row["id"].template as<std::string>();
        o.org_id = row["org_id"].template as<std::string>();
        o.employee_id = row["employee_id"].template as<std::string>();
        o.kind = row["kind"].template as<std::string>();
        o.number = row["number"].template as<std::string>();
        o.issued_on = row["issued_on"].template as<std::string>();
        o.effective_from = row["effective_from"].template as<std::string>();
        if (!row["effective_to"].is_null())
            o.effective_to = row["effective_to"].template as<std::string>();
        if (!row["payload"].is_null()) {
            try {
                o.payload = nlohmann::json::parse(row["payload"].template as<std::string>());
            } catch (...) {
                o.payload = nlohmann::json::object();
            }
        }
        if (!row["document_id"].is_null())
            o.document_id = row["document_id"].template as<std::string>();
        o.created_at = row["created_at"].template as<std::string>();
        o.updated_at = row["updated_at"].template as<std::string>();
        return o;
    }
};

inline void to_json(nlohmann::json& j, const HrOrder& o) {
    j = nlohmann::json{
        {"id", o.id},
        {"org_id", o.org_id},
        {"employee_id", o.employee_id},
        {"kind", o.kind},
        {"number", o.number},
        {"issued_on", o.issued_on},
        {"effective_from", o.effective_from},
        {"effective_to", o.effective_to ? nlohmann::json(*o.effective_to) : nlohmann::json(nullptr)},
        {"payload", o.payload ? *o.payload : nlohmann::json(nullptr)},
        {"document_id", o.document_id ? nlohmann::json(*o.document_id) : nlohmann::json(nullptr)},
        {"created_at", o.created_at},
        {"updated_at", o.updated_at},
    };
}

struct Vacation {
    std::string id;
    std::string org_id;
    std::string employee_id;
    std::string starts_on;
    std::string ends_on;
    int days = 0;
    std::string kind;  // 'annual' | 'unpaid' | 'sick' — CHECK in migrations/012_hr.sql

    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Vacation from_row(const Row& row) {
        Vacation v;
        v.id = row["id"].template as<std::string>();
        v.org_id = row["org_id"].template as<std::string>();
        v.employee_id = row["employee_id"].template as<std::string>();
        v.starts_on = row["starts_on"].template as<std::string>();
        v.ends_on = row["ends_on"].template as<std::string>();
        v.days = row["days"].template as<int>();
        v.kind = row["kind"].template as<std::string>();
        v.created_at = row["created_at"].template as<std::string>();
        v.updated_at = row["updated_at"].template as<std::string>();
        return v;
    }
};

inline void to_json(nlohmann::json& j, const Vacation& v) {
    j = nlohmann::json{
        {"id", v.id},
        {"org_id", v.org_id},
        {"employee_id", v.employee_id},
        {"starts_on", v.starts_on},
        {"ends_on", v.ends_on},
        {"days", v.days},
        {"kind", v.kind},
        {"created_at", v.created_at},
        {"updated_at", v.updated_at},
    };
}

}  // namespace Hr
