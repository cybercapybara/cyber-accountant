#include <gtest/gtest.h>

#include "ledger/KzIdentifiers.hpp"

TEST(KzIdentifiers, AcceptsValidChecksums) {
    for (const auto* id :
         {"104332181962", "001338908381", "637940265425", "351161559402", "781618495938", "103413164751"}) {
        EXPECT_TRUE(Ledger::is_valid_bin_iin(id)) << id;
    }
}

TEST(KzIdentifiers, RejectsMutatedCheckDigit) {
    for (const auto* id :
         {"104332181963", "001338908382", "637940265426", "351161559403", "781618495939", "103413164752"}) {
        EXPECT_FALSE(Ledger::is_valid_bin_iin(id)) << id;
    }
}

TEST(KzIdentifiers, RejectsMalformed) {
    for (const auto* id : {"", "123", "12345678901", "1234567890123", "12345678901a", "12345678901 "}) {
        EXPECT_FALSE(Ledger::is_valid_bin_iin(id)) << id;
    }
}
