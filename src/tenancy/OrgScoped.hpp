/**
 * @file OrgScoped.hpp
 * @brief Header-only CRTP base for tenant-isolated reads — the org-scoped
 *        counterpart to Repositories::CrudBase's find_owned/list_owned/
 *        count_owned (src/repositories/CrudBase.hpp:91-126), but with no
 *        unscoped find/list/count at all.
 *
 * Design spec §5: "методов 'выбрать без org' не существует" — every domain
 * repository from P1 onward that stores per-tenant rows inherits THIS base,
 * not Repositories::CrudBase, so a forgotten org filter is a compile error
 * (the global methods simply don't exist) rather than a runtime IDOR.
 *
 * A derived repo provides the same four constants CrudBase expects, plus
 * kOrgColumn:
 *   class FooRepository : public Tenancy::OrgCrudBase<FooRepository, Domain::Foo, std::string> {
 *     public:
 *       static constexpr const char* kTable      = "foos";
 *       static constexpr const char* kColumns    = "id, org_id, name, created_at";
 *       static constexpr const char* kIdColumn   = "id";
 *       static constexpr const char* kOrderBy    = "created_at DESC";
 *       static constexpr const char* kOrgColumn  = "org_id";
 *       // ... bespoke create/update/remove ...
 *   };
 *
 * KeyT is the id type (std::string for uuid PKs, int for serial PKs).
 * Deliberately no global find/list/count is offered — do not add them here
 * (YAGNI + spec §5); a resource that genuinely needs a global read is not a
 * fit for this base.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "database/Database.hpp"

namespace Tenancy {

template <typename Derived, typename Entity, typename KeyT = std::string>
class OrgCrudBase {
public:
    /// Fetch by primary key, scoped to one organization. A row belonging to
    /// any other org is indistinguishable from a missing row (std::nullopt),
    /// which is the point — no separate 403 branch, no leak of existence.
    /// @p from_primary forces the primary (read-after-write).
    std::optional<Entity> find_in_org(const KeyT& id, const std::string& org_id, bool from_primary = false) {
        auto query = [&](auto& txn) -> std::optional<Entity> {
            auto r = txn.exec_params(select_prefix() + " WHERE " + Derived::kIdColumn + " = $1 AND " +
                                         Derived::kOrgColumn + " = $2",
                                     id,
                                     org_id);
            if (r.empty())
                return std::nullopt;
            return Entity::from_row(r[0]);
        };
        return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
    }

    std::vector<Entity> list_in_org(const std::string& org_id, int limit = 100, int offset = 0) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(select_prefix() + " WHERE " + Derived::kOrgColumn + " = $1 ORDER BY " +
                                         Derived::kOrderBy + " LIMIT $2 OFFSET $3",
                                     org_id,
                                     limit,
                                     offset);
            std::vector<Entity> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Entity::from_row(row));
            return out;
        });
    }

    long count_in_org(const std::string& org_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT COUNT(*) FROM " + std::string(Derived::kTable) + " WHERE " + Derived::kOrgColumn + " = $1",
                org_id);
            return r.at(0).at(0).template as<long>();
        });
    }

private:
    static std::string select_prefix() {
        return "SELECT " + std::string(Derived::kColumns) + " FROM " + std::string(Derived::kTable);
    }
};

}  // namespace Tenancy
