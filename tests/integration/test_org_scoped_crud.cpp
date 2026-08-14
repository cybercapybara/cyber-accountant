/**
 * @file test_org_scoped_crud.cpp
 * @brief Integration test for Tenancy::OrgCrudBase — proves that
 *        find_in_org/list_in_org/count_in_org isolate rows by organization.
 *        The test table is created/dropped in the fixture itself (no
 *        migration exists for it, and none should — it's test-only scaffolding).
 */

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
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec(
                "CREATE TABLE IF NOT EXISTS test_widgets ("
                "id UUID PRIMARY KEY DEFAULT gen_random_uuid(), "
                "org_id UUID NOT NULL, name TEXT NOT NULL)");
            return 0;
        });
    }

    void TearDown() override {
        Database::get().execute_write([](auto& txn) {
            txn.exec("DROP TABLE IF EXISTS test_widgets");
            return 0;
        });
        TestHelpers::CoreBackedTest::TearDown();
    }
};

TEST_F(OrgScopedTest, RowsAreIsolatedByOrg) {
    Tenancy::OrganizationRepository orgs;
    auto a = orgs.create("111240000010", "Org A", "snr_simplified", false);
    auto b = orgs.create("111240000011", "Org B", "snr_simplified", false);
    Database::get().execute_write([&](auto& txn) {
        txn.exec_params("INSERT INTO test_widgets (org_id, name) VALUES ($1,'wa'),($2,'wb')", a.id, b.id);
        return 0;
    });

    WidgetRepository repo;
    auto in_a = repo.list_in_org(a.id);
    ASSERT_EQ(in_a.size(), 1u);
    EXPECT_EQ(in_a[0].name, "wa");

    EXPECT_EQ(repo.count_in_org(b.id), 1);

    auto wb_id = repo.list_in_org(b.id)[0].id;
    EXPECT_FALSE(repo.find_in_org(wb_id, a.id));  // a's org must not see b's row
}

}  // namespace
