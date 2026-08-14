/**
 * @file test_tenancy_domain.cpp
 * @brief Pure domain tests for the tenancy structs (no services). Mirrors
 *        tests/unit/test_domain_serialization.cpp: the to_json contract is
 *        exercised via the free-function ADL hook (`json j = value;`), the
 *        same idiom src/domain/User.hpp and src/domain/Role.hpp use.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "tenancy/OrgMember.hpp"
#include "tenancy/Organization.hpp"

using json = nlohmann::json;

TEST(TenancyDomain, OrganizationToJsonRoundTrip) {
    Tenancy::Organization o;
    o.id = "11111111-1111-1111-1111-111111111111";
    o.bin = "123456789012";
    o.name = "Test LLP";
    o.tax_regime = "snr_simplified";
    o.vat_payer = true;
    o.status = "active";
    json j = o;
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
