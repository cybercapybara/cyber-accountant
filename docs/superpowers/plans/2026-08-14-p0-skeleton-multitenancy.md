# P0: Skeleton + Multitenancy — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Превратить пустой репозиторий `cyber-accountant` в работающий мультитенантный скелет на базе `cpp-rapid-rest-template`: зелёный CI в GitHub Actions, организации + membership + org-scoped доступ, деплой umbrella-чарта в Talos-кластер, зафиксированное решение по S3.

**Architecture:** Модульный монолит (api + worker) из шаблона. Новый модуль `src/tenancy/` (организации, участники, org-контекст в JWT, org-scoped база репозиториев). Demo-модуль posts/content удаляется. Релизные образы собираются только в GitHub Actions и публикуются в GHCR.

**Tech Stack:** Drogon C++20, PostgreSQL 15, Redis 7, vcpkg/CMake, GoogleTest, React 18 SPA, Helm, GitHub Actions, GHCR.

**Spec:** `docs/superpowers/specs/2026-08-14-cyber-accountant-design.md` (§4 архитектура, §5 мультитенантность, §16 сборка/деплой, §17 фаза P0)

## Global Constraints

- Релизные артефакты собираются **только в GitHub Actions**; локально — только dev-сборка и `make ci-local` для отладки (спека §16).
- Все бизнес-роуты под `/api/v1`; triple-sync: контроллер + `src/api/Endpoints.hpp` + `docs/openapi.yaml` (ADR 0006 шаблона).
- `src/` header-only: реализация в `.hpp`, новых `.cpp` кроме существующих entry points не добавлять (ADR 0003 шаблона).
- Единый формат ошибок `{error, status, message}` — только `ErrorResponse::*` / `Api::Validation::*`.
- Каждая доменная таблица несёт `org_id NOT NULL REFERENCES organizations(id)` (спека §5); в P0 это `org_members`, все последующие фазы наследуют правило.
- Миграции: `migrations/NNN_slug.sql`, последовательная нумерация, без `BEGIN`/`COMMIT`.
- Коммиты: conventional commits, **без AI-attribution трейлеров** (глобальная инструкция пользователя).
- Дефолт безопасности fail-closed: нет membership — нет доступа; нет org-контекста — 403.
- Перед каждым коммитом: `make fmt` и быстрые гейты `./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh && ./scripts/check-test-buckets.sh`.

**Рабочая директория:** `/Users/moveeeax/Public/cybercapybara/cyber-accountant`. Эталон шаблона: `/Users/moveeeax/Public/cybercapybara/_reference/cpp-rapid-rest-template` (уже без `.git`-истории связи с fork'ом; копируем из него).

---

### Task 1: Импорт шаблона и инициализация проекта

**Files:**
- Create: всё дерево шаблона в корне репо (копия `_reference/cpp-rapid-rest-template`, без его `.git`)
- Modify: `project.env`, все файлы с токенами шаблона (через `init-project.sh`)

**Interfaces:**
- Produces: рабочее дерево проекта `cyber-accountant` с `PROJECT_NAME=cyber-accountant`, `REGISTRY=ghcr.io/cybercapybara`; все последующие задачи работают в нём.

- [ ] **Step 1: Скопировать шаблон в репо**

```bash
cd /Users/moveeeax/Public/cybercapybara/cyber-accountant
rsync -a --exclude '.git' /Users/moveeeax/Public/cybercapybara/_reference/cpp-rapid-rest-template/ .
git status --short | head   # docs/superpowers/ не должен быть перезаписан
```

Важно: существующие `docs/superpowers/{specs,plans}/` должны остаться нетронутыми (шаблон таких путей не содержит — конфликтов не будет).

- [ ] **Step 2: Инициализировать идентичность проекта**

```bash
./scripts/init-project.sh --no-demo cyber-accountant ghcr.io/cybercapybara
```

Домен намеренно не передаём — скрипт подставит placeholder `example.com`; реальный домен продукта заменим одним sed'ом, когда он будет выбран. `--no-demo` удаляет `_reference/flask-base/` и `docs/PATTERNS-FROM-FLASK-BASE.md`.

- [ ] **Step 3: Проверить, что скрипт ничего не пропустил**

```bash
grep -rn "cpp-rapid-rest-template\|moveeeax" --include="*.yml" --include="*.yaml" --include="*.env" --include="*.json" . | grep -v docs/superpowers || echo CLEAN
cat project.env   # ожидаем PROJECT_NAME=cyber-accountant, REGISTRY=ghcr.io/cybercapybara
```

Expected: `CLEAN` (или только упоминания в docs/superpowers — это история решений, их не трогаем).

- [ ] **Step 4: Локальная dev-проверка сборки и тестов шаблона**

```bash
make test-quick || make test
```

Expected: PASS (это тесты самого шаблона; они обязаны быть зелёными до наших изменений).

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "chore: import cpp-rapid-rest-template as project skeleton"
```

---

### Task 2: Удаление demo-модуля posts/content

**Files:**
- Delete: `src/api/PostsController.hpp`, `src/api/ContentPagesController.hpp`, `src/repositories/PostRepository.hpp`, `src/domain/Post.hpp`, `migrations/006_add_posts.sql`, `tests/integration/test_posts_api.cpp`, `tests/integration/test_post_repository.cpp`, `tests/integration/test_content_pages.cpp`
- Modify: `src/api/Api.hpp` (убрать `#include` обоих контроллеров), `src/api/Endpoints.hpp` (убрать строки роутов posts/public/posts/sitemap — в эталоне это строки 67–76), `docs/openapi.yaml` (убрать paths и schemas постов), `src/core/Core.hpp` (убрать `content_enabled()` и его конфиг-ключ), `config/config.json` (ключ content-модуля, если есть), frontend-страницы постов (найти: `grep -rl -i post frontend/src`)

**Interfaces:**
- Produces: миграция `006` свободна для организаций (Task 3); дерево без demo-кода, все гейты зелёные.

- [ ] **Step 1: Найти полный footprint модуля**

```bash
grep -rn -il "post" src frontend/src docs/openapi.yaml | sort
grep -rn "content_enabled\|sitemap" src config | sort
```

Удалять по этому списку, а не по памяти: тестовые упоминания в чужих файлах (например, `tests/api/test_api_endpoints.cpp` может перечислять роуты) правятся, не удаляются целиком.

- [ ] **Step 2: Удалить файлы модуля и вычистить wiring**

```bash
git rm src/api/PostsController.hpp src/api/ContentPagesController.hpp \
       src/repositories/PostRepository.hpp src/domain/Post.hpp \
       migrations/006_add_posts.sql \
       tests/integration/test_posts_api.cpp tests/integration/test_post_repository.cpp \
       tests/integration/test_content_pages.cpp
```

Затем вручную: убрать includes из `src/api/Api.hpp`, строки роутов из `Api::get_endpoints()` в `src/api/Endpoints.hpp`, paths/schemas из `docs/openapi.yaml`, `content_enabled()` из `src/core/Core.hpp` вместе с местами вызова, конфиг-ключ, frontend-страницы постов + их роуты в SPA-роутере.

- [ ] **Step 3: Быстрые гейты + полный тест**

```bash
make fmt
./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh && ./scripts/check-test-buckets.sh
make test
```

Expected: PASS; количество тестов уменьшилось ровно на posts/content-сьюты.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "chore: remove posts/content demo module"
```

---

### Task 3: Миграция 006 — organizations и org_members

**Files:**
- Create: `migrations/006_organizations.sql`
- Test: `tests/integration/test_tenancy_schema.cpp`

**Interfaces:**
- Produces: таблицы `organizations`, `org_members`; роли `'owner' | 'accountant' | 'viewer'` (CHECK-констрейнт) — их используют все последующие задачи.

- [ ] **Step 1: Написать падающий тест схемы**

`tests/integration/test_tenancy_schema.cpp` (fixture-паттерн — как в `tests/integration/test_migrations.cpp`):

```cpp
#include <gtest/gtest.h>
#include "test_helpers.hpp"

class TenancySchemaTest : public TestHelpers::CoreBackedTest {};

TEST_F(TenancySchemaTest, OrganizationsTableExists) {
    auto n = Database::get().execute_read([](auto& txn) {
        return txn.exec("SELECT COUNT(*) FROM information_schema.tables "
                        "WHERE table_name IN ('organizations','org_members')")
            .at(0).at(0).template as<int>();
    });
    EXPECT_EQ(n, 2);
}

TEST_F(TenancySchemaTest, MemberRoleIsConstrained) {
    EXPECT_THROW(Database::get().execute_write([](auto& txn) {
        txn.exec("INSERT INTO org_members (org_id, user_id, role) "
                 "VALUES (gen_random_uuid(), gen_random_uuid(), 'superuser')");
    }), std::exception);
}
```

Точные имена хелперов БД (`execute_read`/`execute_write`) сверить с `src/database/Database.hpp` и `tests/integration/test_database.cpp` — использовать те же идиомы, что и существующие тесты.

- [ ] **Step 2: Запустить тест — убедиться, что падает**

```bash
make test-quick FILTER=TenancySchemaTest
```

Expected: FAIL (таблиц нет). Формат FILTER сверить с Makefile; fallback: `make test`.

- [ ] **Step 3: Создать миграцию**

```bash
make new-migration SLUG=organizations
```

Содержимое `migrations/006_organizations.sql` (без BEGIN/COMMIT — раннер оборачивает сам):

```sql
-- organizations: тенанты системы (спека §5).
CREATE TABLE organizations (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    bin         CHAR(12) NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    tax_regime  TEXT NOT NULL DEFAULT 'snr_simplified'
                CHECK (tax_regime IN ('snr_simplified', 'standard')),
    vat_payer   BOOLEAN NOT NULL DEFAULT FALSE,
    status      TEXT NOT NULL DEFAULT 'active'
                CHECK (status IN ('active', 'suspended', 'archived')),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
SELECT install_updated_at_trigger('organizations');

-- org_members: membership пользователя в организации с ролью.
CREATE TABLE org_members (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id      UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    user_id     UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    role        TEXT NOT NULL CHECK (role IN ('owner', 'accountant', 'viewer')),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_id, user_id)
);
SELECT install_updated_at_trigger('org_members');
CREATE INDEX idx_org_members_user ON org_members (user_id);
```

Имя функции триггера `updated_at` сверить с `migrations/000_updated_at_trigger.sql` (использовать ровно тот механизм, что в шаблоне; если там триггер ставится иначе — повторить его паттерн). Тип PK `users.id` сверить с `migrations/001_users_and_roles.sql`; если он не UUID — привести `user_id` к фактическому типу.

- [ ] **Step 4: Применить и убедиться, что тест зелёный**

```bash
make migrate
make test-quick FILTER=TenancySchemaTest
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add migrations/006_organizations.sql tests/integration/test_tenancy_schema.cpp
git commit -m "feat: add organizations and org_members schema"
```

---

### Task 4: Доменные структуры и репозитории tenancy

**Files:**
- Create: `src/tenancy/Organization.hpp`, `src/tenancy/OrgMember.hpp`, `src/tenancy/OrganizationRepository.hpp`, `src/tenancy/OrgMemberRepository.hpp`
- Test: `tests/unit/test_tenancy_domain.cpp`, `tests/integration/test_tenancy_repositories.cpp`

**Interfaces:**
- Consumes: таблицы из Task 3; `Repositories::CrudBase` (`src/repositories/CrudBase.hpp`); идиомы `from_row`/`to_json` — образец `src/domain/User.hpp`.
- Produces:
  - `Tenancy::Organization { std::string id, bin, name, tax_regime, status; bool vat_payer; ... }` + `from_row` + `to_json`;
  - `Tenancy::OrgMember { std::string id, org_id, user_id, role; }`;
  - `Tenancy::OrganizationRepository : CrudBase<...>` c `create(bin, name, tax_regime, vat_payer)`, `update_status(id, status)`;
  - `Tenancy::OrgMemberRepository` c `std::optional<OrgMember> find_membership(const std::string& org_id, const std::string& user_id)`, `std::vector<OrgMember> list_for_user(user_id)`, `std::vector<OrgMember> list_members(org_id)`, `OrgMember add(org_id, user_id, role)`, `bool set_role(org_id, user_id, role)`, `bool remove(org_id, user_id)`.

- [ ] **Step 1: Unit-тест доменных структур (без инфраструктуры)**

`tests/unit/test_tenancy_domain.cpp`:

```cpp
#include <gtest/gtest.h>
#include "tenancy/Organization.hpp"
#include "tenancy/OrgMember.hpp"

TEST(TenancyDomain, OrganizationToJsonRoundTrip) {
    Tenancy::Organization o;
    o.id = "11111111-1111-1111-1111-111111111111";
    o.bin = "123456789012";
    o.name = "Test LLP";
    o.tax_regime = "snr_simplified";
    o.vat_payer = true;
    o.status = "active";
    auto j = o.to_json();
    EXPECT_EQ(j["bin"], "123456789012");
    EXPECT_EQ(j["vat_payer"], true);
    EXPECT_EQ(j["status"], "active");
}

TEST(TenancyDomain, MemberRoleValues) {
    EXPECT_TRUE(Tenancy::is_valid_role("owner"));
    EXPECT_TRUE(Tenancy::is_valid_role("accountant"));
    EXPECT_TRUE(Tenancy::is_valid_role("viewer"));
    EXPECT_FALSE(Tenancy::is_valid_role("admin"));
}
```

- [ ] **Step 2: Запустить — FAIL (заголовков нет)**

```bash
make test-quick FILTER=TenancyDomain
```

- [ ] **Step 3: Реализовать доменные структуры**

`src/tenancy/Organization.hpp` — поля/`from_row`/`to_json` строго по идиомам `src/domain/User.hpp` (открыть и повторить стиль доступа к pqxx-строке). Каркас:

```cpp
#pragma once
#include <string>
#include <nlohmann/json.hpp>

namespace Tenancy {

struct Organization {
    std::string id;
    std::string bin;
    std::string name;
    std::string tax_regime;   // 'snr_simplified' | 'standard'
    bool vat_payer = false;
    std::string status;       // 'active' | 'suspended' | 'archived'
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Organization from_row(const Row& r) {
        Organization o;
        o.id = r["id"].template as<std::string>();
        o.bin = r["bin"].template as<std::string>();
        o.name = r["name"].template as<std::string>();
        o.tax_regime = r["tax_regime"].template as<std::string>();
        o.vat_payer = r["vat_payer"].template as<bool>();
        o.status = r["status"].template as<std::string>();
        o.created_at = r["created_at"].template as<std::string>();
        o.updated_at = r["updated_at"].template as<std::string>();
        return o;
    }

    nlohmann::json to_json() const {
        return {{"id", id}, {"bin", bin}, {"name", name},
                {"tax_regime", tax_regime}, {"vat_payer", vat_payer},
                {"status", status}, {"created_at", created_at},
                {"updated_at", updated_at}};
    }
};

inline bool is_valid_role(const std::string& role) {
    return role == "owner" || role == "accountant" || role == "viewer";
}

}  // namespace Tenancy
```

`src/tenancy/OrgMember.hpp` — аналогично с полями `id, org_id, user_id, role, created_at, updated_at`. Если `from_row` в шаблоне принимает конкретный тип (не шаблонный) — повторить фактическую сигнатуру из `src/domain/User.hpp`.

- [ ] **Step 4: Unit-тест зелёный**

```bash
make test-quick FILTER=TenancyDomain
```

Expected: PASS.

- [ ] **Step 5: Интеграционный тест репозиториев (падающий)**

`tests/integration/test_tenancy_repositories.cpp`:

```cpp
#include <gtest/gtest.h>
#include "tenancy/OrganizationRepository.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "test_helpers.hpp"

class TenancyRepoTest : public TestHelpers::CoreBackedTest {};

TEST_F(TenancyRepoTest, CreateFindOrganization) {
    Tenancy::OrganizationRepository repo;
    auto org = repo.create("111240000001", "Cyber Capybara LLP", "snr_simplified", false);
    auto found = repo.find(org.id, /*from_primary=*/true);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->bin, "111240000001");
}

TEST_F(TenancyRepoTest, DuplicateBinRejected) {
    Tenancy::OrganizationRepository repo;
    repo.create("111240000002", "First", "snr_simplified", false);
    EXPECT_THROW(repo.create("111240000002", "Second", "snr_simplified", false),
                 Repositories::ConflictError);  // тип сверить с RepoErrors.hpp
}

TEST_F(TenancyRepoTest, MembershipLifecycle) {
    Tenancy::OrganizationRepository orgs;
    Tenancy::OrgMemberRepository members;
    auto org = orgs.create("111240000003", "M LLP", "snr_simplified", false);
    auto user_id = seed_user_id();  // хелпер: создать пользователя как в test_admin_flow.cpp

    auto m = members.add(org.id, user_id, "accountant");
    auto found = members.find_membership(org.id, user_id);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->role, "accountant");

    EXPECT_TRUE(members.set_role(org.id, user_id, "owner"));
    EXPECT_EQ(members.find_membership(org.id, user_id)->role, "owner");

    EXPECT_TRUE(members.remove(org.id, user_id));
    EXPECT_FALSE(members.find_membership(org.id, user_id));
}
```

`seed_user_id()` реализовать локальным хелпером в тест-файле по образцу `seed_user()` из `tests/integration/test_admin_flow.cpp`.

- [ ] **Step 6: Реализовать репозитории**

`src/tenancy/OrganizationRepository.hpp` по образцу простого CrudBase-репо (см. шапку `src/repositories/CrudBase.hpp`); тип конфликта — из `src/repositories/RepoErrors.hpp` (использовать фактическое имя типа при уникальном конфликте, как это делает `UserRepository` для дубля email):

```cpp
#pragma once
#include "repositories/CrudBase.hpp"
#include "repositories/RepoErrors.hpp"
#include "tenancy/Organization.hpp"

namespace Tenancy {

class OrganizationRepository
    : public Repositories::CrudBase<OrganizationRepository, Organization, std::string> {
public:
    static constexpr const char* kTable = "organizations";
    static constexpr const char* kColumns =
        "id, bin, name, tax_regime, vat_payer, status, created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "created_at DESC";

    Organization create(const std::string& bin, const std::string& name,
                        const std::string& tax_regime, bool vat_payer) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "INSERT INTO organizations (bin, name, tax_regime, vat_payer) "
                "VALUES ($1, $2, $3, $4) RETURNING " + std::string(kColumns),
                bin, name, tax_regime, vat_payer);
            return Organization::from_row(r[0]);
        });  // unique_violation → ConflictError: обернуть как в UserRepository
    }

    bool update_status(const std::string& id, const std::string& status) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE organizations SET status = $2 WHERE id = $1", id, status);
            return r.affected_rows() == 1;
        });
    }
};

}  // namespace Tenancy
```

`src/tenancy/OrgMemberRepository.hpp` — методы из блока Interfaces, каждый одним параметризованным запросом (`WHERE org_id = $1 AND user_id = $2`); маппинг SQL-ошибок — через тот же механизм, что в существующих репозиториях (`SqlErrors.hpp`/`RepoErrors.hpp`).

- [ ] **Step 7: Интеграционные тесты зелёные**

```bash
make test
```

Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add src/tenancy tests/unit/test_tenancy_domain.cpp tests/integration/test_tenancy_repositories.cpp
git commit -m "feat: tenancy domain structs and repositories"
```

---

### Task 5: OrgCrudBase — org-scoped база для всех будущих репозиториев

**Files:**
- Create: `src/tenancy/OrgScoped.hpp`
- Test: `tests/integration/test_org_scoped_crud.cpp`

**Interfaces:**
- Consumes: `Repositories::CrudBase` как образец; `Database::get()`.
- Produces: `Tenancy::OrgCrudBase<Derived, Entity, KeyT>` — CRTP-база, дающая ТОЛЬКО org-scoped чтения: `find_in_org(id, org_id)`, `list_in_org(org_id, limit, offset)`, `count_in_org(org_id)`. Derived обязан объявить `kTable/kColumns/kIdColumn/kOrderBy` и `static constexpr const char* kOrgColumn = "org_id"`. Это правило спеки §5: «методов "выбрать без org" не существует» — все доменные репозитории P1+ наследуют её, а не CrudBase.

- [ ] **Step 1: Падающий тест изоляции**

`tests/integration/test_org_scoped_crud.cpp` — тестовая таблица создаётся прямо в тесте (DDL в fixture SetUp, DROP в TearDown), чтобы не плодить миграцию ради теста:

```cpp
#include <gtest/gtest.h>
#include "tenancy/OrgScoped.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

struct Widget {
    std::string id, org_id, name;
    template <typename Row>
    static Widget from_row(const Row& r) {
        return {r["id"].template as<std::string>(),
                r["org_id"].template as<std::string>(),
                r["name"].template as<std::string>()};
    }
};

class WidgetRepository : public Tenancy::OrgCrudBase<WidgetRepository, Widget, std::string> {
public:
    static constexpr const char* kTable = "test_widgets";
    static constexpr const char* kColumns = "id, org_id, name";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "id";
    static constexpr const char* kOrgColumn = "org_id";
};

class OrgScopedTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        Database::get().execute_write([](auto& txn) {
            txn.exec("CREATE TABLE IF NOT EXISTS test_widgets ("
                     "id UUID PRIMARY KEY DEFAULT gen_random_uuid(), "
                     "org_id UUID NOT NULL, name TEXT NOT NULL)");
        });
    }
    void TearDown() override {
        Database::get().execute_write([](auto& txn) {
            txn.exec("DROP TABLE IF EXISTS test_widgets");
        });
        TestHelpers::CoreBackedTest::TearDown();
    }
};

TEST_F(OrgScopedTest, RowsAreIsolatedByOrg) {
    Tenancy::OrganizationRepository orgs;
    auto a = orgs.create("111240000010", "Org A", "snr_simplified", false);
    auto b = orgs.create("111240000011", "Org B", "snr_simplified", false);
    Database::get().execute_write([&](auto& txn) {
        txn.exec_params("INSERT INTO test_widgets (org_id, name) VALUES ($1,'wa'),($2,'wb')",
                        a.id, b.id);
    });
    WidgetRepository repo;
    auto in_a = repo.list_in_org(a.id);
    ASSERT_EQ(in_a.size(), 1u);
    EXPECT_EQ(in_a[0].name, "wa");
    EXPECT_EQ(repo.count_in_org(b.id), 1);
    auto wb_id = repo.list_in_org(b.id)[0].id;
    EXPECT_FALSE(repo.find_in_org(wb_id, a.id));  // чужой org не видит запись
}

}  // namespace
```

- [ ] **Step 2: Запустить — FAIL (OrgScoped.hpp нет)**

```bash
make test-quick FILTER=OrgScopedTest
```

- [ ] **Step 3: Реализовать OrgCrudBase**

`src/tenancy/OrgScoped.hpp` — те же приёмы, что org-owner методы CrudBase (`find_owned`/`list_owned`/`count_owned` в `src/repositories/CrudBase.hpp:92-127`), но без унаследованных глобальных методов:

```cpp
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "database/Database.hpp"

namespace Tenancy {

// Org-scoped CRTP-база (спека §5): каждый запрос несёт WHERE kOrgColumn = $org.
// Глобальных find/list/count здесь НЕТ намеренно — забыть org-фильтр невозможно.
template <typename Derived, typename Entity, typename KeyT = std::string>
class OrgCrudBase {
public:
    std::optional<Entity> find_in_org(const KeyT& id, const std::string& org_id,
                                      bool from_primary = false) {
        auto query = [&](auto& txn) -> std::optional<Entity> {
            auto r = txn.exec_params(select_prefix() + " WHERE " + Derived::kIdColumn +
                                         " = $1 AND " + Derived::kOrgColumn + " = $2",
                                     id, org_id);
            if (r.empty()) return std::nullopt;
            return Entity::from_row(r[0]);
        };
        return from_primary ? Database::get().execute_read_primary(query)
                            : Database::get().execute_read(query);
    }

    std::vector<Entity> list_in_org(const std::string& org_id, int limit = 100, int offset = 0) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(select_prefix() + " WHERE " + Derived::kOrgColumn +
                                         " = $1 ORDER BY " + Derived::kOrderBy +
                                         " LIMIT $2 OFFSET $3",
                                     org_id, limit, offset);
            std::vector<Entity> out;
            out.reserve(r.size());
            for (const auto& row : r) out.push_back(Entity::from_row(row));
            return out;
        });
    }

    long count_in_org(const std::string& org_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT COUNT(*) FROM " + std::string(Derived::kTable) +
                                         " WHERE " + Derived::kOrgColumn + " = $1",
                                     org_id);
            return r.at(0).at(0).template as<long>();
        });
    }

private:
    static std::string select_prefix() {
        return "SELECT " + std::string(Derived::kColumns) + " FROM " +
               std::string(Derived::kTable);
    }
};

}  // namespace Tenancy
```

- [ ] **Step 4: Тест зелёный**

```bash
make test-quick FILTER=OrgScopedTest
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/tenancy/OrgScoped.hpp tests/integration/test_org_scoped_crud.cpp
git commit -m "feat: org-scoped CRUD base for tenant-isolated repositories"
```

---

### Task 6: Org-контекст — JWT-claim, переключение организации, guard

**Files:**
- Create: `src/tenancy/OrgContext.hpp`
- Modify: `src/security/Auth.hpp` (парсинг claim `org` в principal — рядом с `p.subject = claims.value("sub", "")`, строка ~177), `src/api/AuthController.hpp` (claim `org` при логине, строки ~312–335), `src/api/Guards.hpp` (макрос `API_REQUIRE_ORG`)
- Test: `tests/integration/test_org_context.cpp`

**Interfaces:**
- Consumes: `Tenancy::OrgMemberRepository::find_membership` (Task 4); `Security::Auth::principal_of(req)`; `issue_hs256_jwt` (`src/security/Jwt.hpp`).
- Produces:
  - поле `std::string org` в `AuthPrincipal` (пустая строка = claim не задан);
  - `Tenancy::OrgContext { std::string org_id; std::string role; std::string user_id; }`;
  - `std::optional<Tenancy::OrgContext> Tenancy::org_context_of(const drogon::HttpRequestPtr& req)` — читает claim `org` из principal, проверяет membership в БД, отдаёт контекст; нет claim'а или membership → `nullopt` (fail-closed);
  - макрос `API_REQUIRE_ORG(req, callback, ctx)` — биндит `Tenancy::OrgContext ctx` или отвечает 403 `ErrorResponse::forbidden()`. Все org-scoped контроллеры P0+ начинаются с него.

- [ ] **Step 1: Падающий тест**

`tests/integration/test_org_context.cpp` (минт токенов и прямой вызов — по паттерну `test_admin_flow.cpp` / `test_posts_api.cpp`):

```cpp
#include <gtest/gtest.h>
#include "tenancy/OrgContext.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "tenancy/OrgMemberRepository.hpp"
#include "test_helpers.hpp"

class OrgContextTest : public TestHelpers::CoreBackedTest {
    // auth.mode=jwt; хелперы seed_user()/make_authed_request() —
    // скопировать локально по образцу test_admin_flow.cpp.
};

TEST_F(OrgContextTest, MemberGetsContextWithRole) {
    // seed: user + org + membership(accountant); JWT c claims {"sub": user_id, "org": org_id}
    // expect: org_context_of(req) → ctx, ctx->role == "accountant"
}

TEST_F(OrgContextTest, NonMemberGetsNoContext) {
    // JWT c claim org = чужая организация (membership нет)
    // expect: org_context_of(req) == nullopt
}

TEST_F(OrgContextTest, MissingOrgClaimGetsNoContext) {
    // JWT только с sub
    // expect: nullopt
}
```

Тела тестов дописать по фактическим хелперам fixture (создание запроса с Bearer-токеном — так же, как это делает `TestHelpers::authed`/`authed_json`, см. `tests/test_helpers.hpp`). Тесты обязаны покрыть все три случая из блока Produces.

- [ ] **Step 2: Запустить — FAIL**

```bash
make test-quick FILTER=OrgContextTest
```

- [ ] **Step 3: Реализовать**

1. `src/security/Auth.hpp`: в месте разбора клеймов (`p.subject = claims.value("sub", "")`) добавить `p.org = claims.value("org", "");` и поле `std::string org;` в структуру principal.
2. `src/tenancy/OrgContext.hpp`:

```cpp
#pragma once
#include <optional>
#include <string>

#include <drogon/HttpRequest.h>

#include "security/Auth.hpp"
#include "tenancy/OrgMemberRepository.hpp"

namespace Tenancy {

struct OrgContext {
    std::string org_id;
    std::string role;     // 'owner' | 'accountant' | 'viewer'
    std::string user_id;
};

// Fail-closed: контекст существует только при валидном claim + живом membership.
inline std::optional<OrgContext> org_context_of(const drogon::HttpRequestPtr& req) {
    auto p = Security::Auth::principal_of(req);
    if (!p || p->subject.empty() || p->org.empty())
        return std::nullopt;
    OrgMemberRepository members;
    auto m = members.find_membership(p->org, p->subject);
    if (!m)
        return std::nullopt;
    return OrgContext{p->org, m->role, p->subject};
}

}  // namespace Tenancy
```

3. `src/api/Guards.hpp` — рядом с `API_REQUIRE_OWNER`:

```cpp
/// Bind the caller's organization context (org id + role) into `ctx`, or
/// reject with 403. Fail-closed: no org claim / no membership → forbidden.
/// NOT a no-op when auth is disabled — tenant data requires an identity.
#define API_REQUIRE_ORG(req, callback, ctx)              \
    Tenancy::OrgContext ctx;                             \
    do {                                                 \
        auto _org_ctx = Tenancy::org_context_of(req);    \
        if (!_org_ctx) {                                 \
            callback(ErrorResponse::forbidden());        \
            return;                                      \
        }                                                \
        (ctx) = *_org_ctx;                               \
    } while (0)
```

Имя фабрики 403-ответа сверить с `src/utils/ErrorResponse.hpp` (использовать фактическое, как в `API_REQUIRE_ADMIN`).
4. `src/api/AuthController.hpp`: при сборке access-клеймов (строка ~312) — если у пользователя ровно одно membership, добавить `{"org", <его org_id>}`; иначе claim не ставить (клиент выберет организацию через switch, Task 7).

- [ ] **Step 4: Тесты зелёные**

```bash
make test
```

Expected: PASS, включая нетронутые auth-сьюты шаблона (refresh-flow не должен сломаться от нового клейма).

- [ ] **Step 5: Commit**

```bash
git add src/tenancy/OrgContext.hpp src/security/Auth.hpp src/api/Guards.hpp src/api/AuthController.hpp tests/integration/test_org_context.cpp
git commit -m "feat: org claim in JWT and org-context guard"
```

---

### Task 7: Organizations API — управление тенантами и membership

**Files:**
- Create: `src/api/OrganizationsController.hpp`
- Modify: `src/api/Api.hpp` (include), `src/api/Endpoints.hpp` (роуты), `docs/openapi.yaml` (paths + schemas)
- Test: `tests/integration/test_organizations_api.cpp`

**Interfaces:**
- Consumes: репозитории Task 4, guard Task 6, `API_REQUIRE_ADMIN`, `ErrorResponse::*`, `Api::Validation::*`.
- Produces (роуты — добавить ровно эти в `Endpoints.hpp` и `openapi.yaml`):
  - `POST /api/v1/orgs` — создать организацию (admin-gated; спека §5: тенантов создаёт системный админ); тело `{bin, name, tax_regime?, vat_payer?}`; 201 + JSON организации; 409 при дубле БИН; 422 при невалидном БИН (12 цифр).
  - `GET /api/v1/orgs` — список организаций (admin-gated, пагинация limit/offset как в AdminController).
  - `GET /api/v1/orgs/mine` — организации текущего пользователя с его ролью (любой аутентифицированный).
  - `POST /api/v1/orgs/{id}/switch` — выдать новый access-токен с claim `org={id}` после проверки membership; 200 `{access}`; 403 если не участник.
  - `POST /api/v1/orgs/{id}/members` — добавить участника `{user_id, role}` (admin ИЛИ owner этой организации); 409 при дубле.
  - `PATCH /api/v1/orgs/{id}/members/{user_id}` — сменить роль `{role}` (admin или owner).
  - `DELETE /api/v1/orgs/{id}/members/{user_id}` — убрать участника (admin или owner); запрет на удаление последнего owner — 409.
- Валидация БИН в v1 — формат: ровно 12 цифр (полная проверка контрольного разряда — P1, ledger/counterparties).

- [ ] **Step 1: Падающие API-тесты**

`tests/integration/test_organizations_api.cpp` — прямой вызов контроллера по паттерну `test_admin_flow.cpp`. Обязательные случаи:

```cpp
// AdminCreatesOrganization:      admin POST /orgs → 201, org в БД
// NonAdminCannotCreate:          обычный user POST /orgs → 403
// InvalidBinRejected:            bin "12345" → 422
// DuplicateBinConflict:          второй POST с тем же bin → 409
// MineListsMembershipsWithRole:  user с membership видит org + свою роль
// SwitchIssuesOrgToken:          member POST /orgs/{id}/switch → 200, verify_hs256_jwt
//                                декодирует claim org == id
// SwitchForbiddenForNonMember:   не-member → 403
// OwnerManagesMembers:           owner добавляет/меняет роль/удаляет → 2xx
// ViewerCannotManageMembers:     viewer POST members → 403
// LastOwnerNotRemovable:         удаление единственного owner → 409
```

Каждый случай — отдельный TEST_F с полным телом (минт токена нужной роли, вызов handler'а, ASSERT статуса и тела — идиомы из test_admin_flow.cpp).

- [ ] **Step 2: Запустить — FAIL**

```bash
make test-quick FILTER=OrganizationsApi
```

- [ ] **Step 3: Реализовать контроллер**

`src/api/OrganizationsController.hpp` — drogon `HttpController` по образцу `AdminController.hpp` (тот же стиль `ADD_METHOD_TO`, `with_repo_errors`, JSON-парсинг через существующие хелперы `RequestUtils.hpp`/`Validation.hpp`). Выпуск токена в `switch` — тот же код-путь, что refresh в `AuthController.hpp` (скопировать сборку claims и `issue_hs256_jwt`, добавив `{"org", org_id}`; TTL и остальные клеймы — идентичны access-токену логина).

- [ ] **Step 4: Triple-sync**

Добавить все 7 роутов в `Api::get_endpoints()` (`src/api/Endpoints.hpp`) и в `docs/openapi.yaml` (paths + компоненты схем `Organization`, `OrgMember`, request-тела). Прогнать:

```bash
./scripts/check-openapi-drift.sh && ./scripts/check-routes-registered.sh
make lint-openapi
```

Expected: PASS.

- [ ] **Step 5: Все тесты зелёные**

```bash
make test
```

- [ ] **Step 6: Commit**

```bash
git add src/api/OrganizationsController.hpp src/api/Api.hpp src/api/Endpoints.hpp docs/openapi.yaml tests/integration/test_organizations_api.cpp
git commit -m "feat: organizations and membership API with org switch"
```

---

### Task 8: Frontend — регенерация клиента и страница организаций

**Files:**
- Modify: `frontend/src/api/` (сгенерированные типы из openapi), SPA-роутер
- Create: страница `Organizations` (админ: список/создание; пользователь: выбор активной организации → switch → замена токена)

**Interfaces:**
- Consumes: роуты Task 7 через сгенерированный openapi-typescript клиент.
- Produces: SPA собирается (`npm run build`), пользователь может выбрать организацию, admin — создать её.

- [ ] **Step 1: Регенерировать API-клиент**

```bash
cd frontend && npm ci && npm run generate:api 2>/dev/null || npx openapi-typescript ../docs/openapi.yaml -o src/api/schema.d.ts
```

Точное имя скрипта генерации взять из `frontend/package.json`.

- [ ] **Step 2: Скаффолд страницы**

```bash
cd .. && ./scripts/new-react-page.sh Organizations
```

Реализовать: таблица организаций (`GET /orgs` для админа, `GET /orgs/mine` для пользователя), форма создания (admin), кнопка «Работать в этой организации» → `POST /orgs/{id}/switch` → сохранить новый access-токен тем же механизмом, каким SPA хранит токен после логина (посмотреть auth-хук/стор шаблона и переиспользовать его setter). Стиль — shadcn/ui, как соседние страницы.

- [ ] **Step 3: Сборка и линт фронтенда**

```bash
cd frontend && npm run build && npm run lint
```

Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add frontend
git commit -m "feat(frontend): organizations page and org switch"
```

---

### Task 9: GitHub — репозиторий, CI зелёный, релиз v0.1.0

**Files:**
- Modify: `.github/workflows/release.yml` (`IMAGE_NAME: ghcr.io/cybercapybara/cyber-accountant` — проверить, что init-project уже заменил; если нет — заменить руками), `README.md` (badges/URLs)

**Interfaces:**
- Consumes: всё дерево из Task 1–8.
- Produces: зелёный `ci.yml` на `main`; образы `ghcr.io/cybercapybara/cyber-accountant{,-worker,-frontend}:v0.1.0` — их потребляет Task 11.

- [ ] **Step 1: Проверить workflows на остатки старой идентичности**

```bash
grep -rn "moveeeax\|cpp-rapid-rest-template" .github/workflows README.md || echo CLEAN
```

Всё найденное заменить на `cybercapybara` / `cyber-accountant`.

- [ ] **Step 2: Push в GitHub**

```bash
git push origin main
```

- [ ] **Step 3: Прогреть кэш билдера и дождаться CI**

```bash
gh workflow run builder-cache.yml && gh run watch
gh run list --workflow=ci.yml --limit 1
```

`builder-cache.yml` публикует vcpkg-слой в `ghcr.io/cybercapybara/cyber-accountant/builder:cache` — без него первый CI-прогон уйдёт в ~30-минутную холодную сборку. Затем убедиться, что `ci.yml` на последнем коммите зелёный: `gh run watch <id>`. Если падают гейты — чинить до зелёного (это definition of done задачи, а не «прогнали и посмотрели»).

- [ ] **Step 4: Убедиться, что пакеты GHCR доступны кластеру**

В настройках GitHub-организации: пакеты `cyber-accountant*` — private; создать PAT `read:packages` (или использовать существующий деплой-токен) для imagePullSecret — понадобится в Task 11.

- [ ] **Step 5: Релиз**

```bash
git tag v0.1.0 && git push origin v0.1.0
gh run watch   # release.yml: build+push трёх образов + Trivy
```

Expected: три образа в GHCR с тегом v0.1.0.

- [ ] **Step 6: Commit (если правились workflows/README)**

```bash
git add .github README.md && git commit -m "ci: point workflows at cybercapybara registry" && git push
```

---

### Task 10: ADR — выбор S3-бэкенда

**Files:**
- Create: `docs/adr/0007-s3-backend.md` (номер — следующий свободный в `docs/adr/`; проверить `ls docs/adr`)

**Interfaces:**
- Produces: зафиксированное решение + бакет и креды, которые P1 (модуль `files`) превратит в S3-реализацию `StorageBackend` (`src/storage/Storage.hpp` — интерфейс уже готов под подмену).

- [ ] **Step 1: Проверить фактические ресурсы кластера**

```bash
KUBECONFIG=../cluster/kubeconfig kubectl top nodes
KUBECONFIG=../cluster/kubeconfig kubectl get pv,pvc -A
```

- [ ] **Step 2: Написать ADR**

Решение (из спеки §19, подтверждаем данными шага 1): **Hetzner Object Storage**, а не MinIO в кластере. Обоснование в ADR: кластер маленький (Hetzner Cloud), MinIO съедает RAM/диск и требует собственного бэкапа; Object Storage — управляемый, S3-совместимый, в том же ДЦ; код не зависит от выбора (единый S3-клиент за `StorageBackend`). Зафиксировать fallback: если появится требование data-locality в кластере — MinIO ставится subchart'ом без изменения кода. Если `kubectl top` покажет обилие свободных ресурсов и владелец захочет MinIO — решение пересматривается в ADR, но дефолт плана — Object Storage.

- [ ] **Step 3: Провизионировать бакет и секрет**

В консоли Hetzner Cloud (проект кластера): создать Object Storage bucket `cyber-accountant-prod` (регион тот же, что кластер — см. `../cluster/main.tf`), сгенерировать access/secret key. Затем:

```bash
KUBECONFIG=../cluster/kubeconfig kubectl create namespace cyber-accountant --dry-run=client -o yaml | kubectl --kubeconfig ../cluster/kubeconfig apply -f -
KUBECONFIG=../cluster/kubeconfig kubectl -n cyber-accountant create secret generic s3-credentials \
  --from-literal=S3_ENDPOINT=<endpoint-url> \
  --from-literal=S3_BUCKET=cyber-accountant-prod \
  --from-literal=S3_ACCESS_KEY=<key> \
  --from-literal=S3_SECRET_KEY=<secret>
```

Ключи в git не попадают (секрет живёт только в кластере); в ADR — только имя секрета.

- [ ] **Step 4: Commit**

```bash
git add docs/adr/0007-s3-backend.md
git commit -m "docs: ADR for S3 backend (Hetzner Object Storage)" && git push
```

---

### Task 11: Деплой umbrella-чарта в Talos-кластер

**Files:**
- Create: `helm/<umbrella>/values-cybercapybara.yaml` (имя каталога umbrella после init-project — проверить `ls helm/`; в эталоне был `cpp-env`)

**Interfaces:**
- Consumes: образы v0.1.0 из Task 9, namespace и секрет из Task 10.
- Produces: работающий stack (api + worker + frontend + Postgres + Redis) в namespace `cyber-accountant`; `/healthz` и `/ready` отвечают 200 — definition of done фазы P0.

- [ ] **Step 1: Изучить вводные кластера**

```bash
ls ../cluster/bootstrap/                                   # что уже стоит: ingress, cert-manager, CNPG?
KUBECONFIG=../cluster/kubeconfig kubectl get pods -A | head -30
KUBECONFIG=../cluster/kubeconfig kubectl get ingressclass,storageclass
```

Umbrella-чарт ждёт CloudNativePG для Postgres (см. Chart.yaml umbrella) — если CNPG-оператора в кластере нет, поставить его первым:

```bash
KUBECONFIG=../cluster/kubeconfig kubectl apply --server-side -f \
  https://raw.githubusercontent.com/cloudnative-pg/cloudnative-pg/release-1.24/releases/cnpg-1.24.1.yaml
```

- [ ] **Step 2: Собрать values-файл**

`values-cybercapybara.yaml` — за основу взять `values-stage.yaml` umbrella-чарта, переопределить: registry `ghcr.io/cybercapybara`, тег `v0.1.0`, домен/ingressClass из фактического состояния кластера (шаг 1), imagePullSecret с PAT из Task 9 Step 4, ресурсные лимиты по размеру нод. Kafka/Mailpit/Jaeger — off (минимальный профиль, они опциональны в чарте).

- [ ] **Step 3: Рендер-проверка без кластера**

```bash
make helm-lint
helm template test helm/<umbrella> -f helm/<umbrella>/values-cybercapybara.yaml >/dev/null && echo RENDER-OK
```

- [ ] **Step 4: Установка**

```bash
KUBECONFIG=../cluster/kubeconfig helm upgrade --install cyber-accountant helm/<umbrella> \
  -n cyber-accountant -f helm/<umbrella>/values-cybercapybara.yaml
KUBECONFIG=../cluster/kubeconfig kubectl -n cyber-accountant get pods -w
```

Expected: все поды Running/Ready.

- [ ] **Step 5: Смоук**

```bash
KUBECONFIG=../cluster/kubeconfig kubectl -n cyber-accountant port-forward svc/<api-svc> 8080:8080 &
curl -fsS localhost:8080/healthz && curl -fsS localhost:8080/ready
curl -fsS localhost:8080/api/v1/orgs -H "Authorization: Bearer $(./scripts/make-jwt.sh admin)" | head
```

Имя api-сервиса — из `kubectl get svc -n cyber-accountant`; `make-jwt.sh` — хелпер шаблона (посмотреть его usage). Expected: 200 на пробах; `/orgs` отвечает JSON (пустой список), а не 5xx.

- [ ] **Step 6: Commit**

```bash
git add helm/
git commit -m "deploy: cluster values for cybercapybara environment" && git push
```

---

## Definition of Done (фаза P0)

1. `ci.yml` зелёный на `main`; релиз v0.1.0 опубликовал три образа в GHCR (сборка только в GitHub Actions).
2. `make test` локально зелёный; тесты покрывают: схему tenancy, репозитории, org-изоляцию OrgCrudBase, org-claim/guard (member/non-member/no-claim), Organizations API (включая last-owner и RBAC-отказы).
3. В кластере отвечают `/healthz`, `/ready`; admin может создать организацию, пользователь — переключиться в неё и получить org-токен.
4. ADR по S3 зафиксирован, бакет и секрет существуют.
5. Никаких методов чтения доменных данных мимо org-scope не появилось (OrgCrudBase — единственная база для будущих доменных репозиториев; правило записано в её doc-комментарии).
