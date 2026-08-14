/**
 * @file Counterparty.hpp
 * @brief Counterparty row (contractor of an organization). Mirrors the
 *        `counterparties` table (migrations/007_counterparties.sql — design
 *        spec §6.3).
 *
 * Domain-only — no SQL here; persistence lives in
 * src/ledger/CounterpartyRepository.hpp. Follows the same from_row/to_json
 * idioms as src/tenancy/Organization.hpp: from_row is a templated static
 * factory (works with any pqxx row-like type), and to_json is a free
 * function found via ADL so `nlohmann::json j = counterparty;` "just works"
 * without a member method.
 *
 * `identifier` is the counterparty's BIN/IIN, stored as-is; validating its
 * check digit (Ledger::is_valid_bin_iin, src/ledger/KzIdentifiers.hpp) is
 * the API layer's job (Task 12), not this repository's.
 */

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace Ledger {

struct Counterparty {
    std::string id;
    std::string org_id;
    std::string identifier;  // БИН/ИИН, 12 digits, stored as-is
    std::string name;
    std::string address;
    std::string iik;  // IBAN KZ.. (format not validated in P1)
    std::string bik;
    std::string kbe;
    bool is_resident = true;
    bool vat_payer = false;
    std::string contact_email;
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Counterparty from_row(const Row& row) {
        Counterparty c;
        c.id = row["id"].template as<std::string>();
        c.org_id = row["org_id"].template as<std::string>();
        c.identifier = row["identifier"].template as<std::string>();
        c.name = row["name"].template as<std::string>();
        c.address = row["address"].template as<std::string>();
        c.iik = row["iik"].template as<std::string>();
        c.bik = row["bik"].template as<std::string>();
        c.kbe = row["kbe"].template as<std::string>();
        c.is_resident = row["is_resident"].template as<bool>();
        c.vat_payer = row["vat_payer"].template as<bool>();
        c.contact_email = row["contact_email"].template as<std::string>();
        c.created_at = row["created_at"].template as<std::string>();
        c.updated_at = row["updated_at"].template as<std::string>();
        return c;
    }
};

/// Public JSON shape — everything on the row is safe to expose (no secrets).
inline void to_json(nlohmann::json& j, const Counterparty& c) {
    j = nlohmann::json{
        {"id", c.id},
        {"org_id", c.org_id},
        {"identifier", c.identifier},
        {"name", c.name},
        {"address", c.address},
        {"iik", c.iik},
        {"bik", c.bik},
        {"kbe", c.kbe},
        {"is_resident", c.is_resident},
        {"vat_payer", c.vat_payer},
        {"contact_email", c.contact_email},
        {"created_at", c.created_at},
        {"updated_at", c.updated_at},
    };
}

}  // namespace Ledger
