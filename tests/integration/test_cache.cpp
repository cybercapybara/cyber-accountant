#include <vector>

#include <gtest/gtest.h>

#include "cache/Cache.hpp"
#include "test_helpers.hpp"

static const std::string REDIS_URL = TestHelpers::redis_url();

// Lifecycle tests drive initialize/shutdown themselves — no auto-init.
class CacheLifecycleTest : public ::testing::Test {
protected:
    std::vector<std::string> created_keys;

    void SetUp() override {
        if (!TestHelpers::is_redis_available()) {
            GTEST_SKIP() << "Redis not available";
        }
    }

    void TearDown() override {
        // Clean up test keys
        if (Cache::is_initialized()) {
            try {
                for (const auto& key : created_keys) {
                    Cache::get().del(key);
                }
            } catch (...) {}
        }
        TestHelpers::reset_all_globals();
    }

    std::string tk(const std::string& name) {
        auto key = TestHelpers::test_key(name);
        created_keys.push_back(key);
        return key;
    }
};

// Operation tests get an initialized cache from the fixture — repeating
// Cache::initialize() as the first line of two dozen tests invited a
// forgotten-init test to silently run against a leaked global.
class CacheTest : public CacheLifecycleTest {
protected:
    void SetUp() override {
        CacheLifecycleTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Cache::initialize(REDIS_URL, 2);
    }
};

// --- Lifecycle ---

TEST_F(CacheLifecycleTest, InitializeAndShutdown) {
    Cache::initialize(REDIS_URL, 2);
    EXPECT_TRUE(Cache::is_initialized());

    Cache::shutdown();
    EXPECT_FALSE(Cache::is_initialized());
}

TEST_F(CacheLifecycleTest, DoubleInitThrows) {
    Cache::initialize(REDIS_URL, 2);
    EXPECT_THROW(Cache::initialize(REDIS_URL, 2), std::runtime_error);
}

TEST_F(CacheLifecycleTest, GetBeforeInitThrows) {
    EXPECT_THROW(Cache::get(), std::runtime_error);
}

TEST_F(CacheTest, HealthCheck) {
    EXPECT_TRUE(Cache::get().health_check());
}

TEST_F(CacheLifecycleTest, ShutdownIdempotent) {
    Cache::initialize(REDIS_URL, 2);
    EXPECT_NO_THROW(Cache::shutdown());
    EXPECT_NO_THROW(Cache::shutdown());
}

// --- Basic operations ---

TEST_F(CacheTest, SetAndGet) {
    auto key = tk("set_get");

    EXPECT_TRUE(Cache::get().set(key, "hello"));
    auto val = Cache::get().get(key);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "hello");
}

TEST_F(CacheTest, GetNonexistent) {
    auto val = Cache::get().get(tk("nonexistent_key_xyz"));
    EXPECT_FALSE(val.has_value());
}

TEST_F(CacheTest, SetWithTTL) {
    auto key = tk("with_ttl");

    EXPECT_TRUE(Cache::get().set(key, "expires", 60));
    auto ttl_val = Cache::get().ttl(key);
    EXPECT_GT(ttl_val, 0);
    EXPECT_LE(ttl_val, 60);
}

TEST_F(CacheTest, Delete) {
    auto key = tk("to_delete");

    Cache::get().set(key, "value");
    EXPECT_TRUE(Cache::get().exists(key));

    long deleted = Cache::get().del(key);
    EXPECT_EQ(deleted, 1);
    EXPECT_FALSE(Cache::get().exists(key));
}

TEST_F(CacheTest, DeleteMultiple) {
    auto k1 = tk("multi_del_1");
    auto k2 = tk("multi_del_2");

    Cache::get().set(k1, "v1");
    Cache::get().set(k2, "v2");

    long deleted = Cache::get().del(std::vector<std::string>{k1, k2});
    EXPECT_EQ(deleted, 2);
}

TEST_F(CacheTest, Exists) {
    auto key = tk("exists_test");

    EXPECT_FALSE(Cache::get().exists(key));
    Cache::get().set(key, "value");
    EXPECT_TRUE(Cache::get().exists(key));
}

TEST_F(CacheTest, Expire) {
    auto key = tk("expire_test");

    Cache::get().set(key, "value");
    EXPECT_TRUE(Cache::get().expire(key, 30));

    auto ttl_val = Cache::get().ttl(key);
    EXPECT_GT(ttl_val, 0);
    EXPECT_LE(ttl_val, 30);
}

TEST_F(CacheTest, TTLNoExpiry) {
    auto key = tk("no_expiry");

    Cache::get().set(key, "permanent");
    auto ttl_val = Cache::get().ttl(key);
    EXPECT_EQ(ttl_val, -1);  // -1 means no expiry
}

// --- SET NX (used as idempotency lock primitive) ---

TEST_F(CacheTest, SetNxAcquiresWhenAbsent) {
    auto key = tk("nx_absent");

    EXPECT_TRUE(Cache::get().set_nx(key, "owner-A", std::chrono::seconds(5)));
    auto v = Cache::get().get(key);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "owner-A");
}

TEST_F(CacheTest, SetNxRejectsWhenPresent) {
    auto key = tk("nx_present");

    EXPECT_TRUE(Cache::get().set_nx(key, "owner-A", std::chrono::seconds(5)));
    EXPECT_FALSE(Cache::get().set_nx(key, "owner-B", std::chrono::seconds(5)));
    auto v = Cache::get().get(key);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "owner-A");
}

TEST_F(CacheTest, SetNxAppliesTTL) {
    auto key = tk("nx_ttl");

    EXPECT_TRUE(Cache::get().set_nx(key, "v", std::chrono::seconds(30)));
    auto ttl = Cache::get().ttl(key);
    EXPECT_GT(ttl, 0);
    EXPECT_LE(ttl, 30);
}

// --- Atomic operations ---

TEST_F(CacheTest, Increment) {
    auto key = tk("incr_test");

    auto val = Cache::get().incr(key);
    EXPECT_EQ(val, 1);
    val = Cache::get().incr(key);
    EXPECT_EQ(val, 2);
}

TEST_F(CacheTest, IncrementByN) {
    auto key = tk("incr_by_test");

    auto val = Cache::get().incr(key, 5);
    EXPECT_EQ(val, 5);
    val = Cache::get().incr(key, 3);
    EXPECT_EQ(val, 8);
}

TEST_F(CacheTest, Decrement) {
    auto key = tk("decr_test");

    Cache::get().incr(key, 10);
    auto val = Cache::get().decr(key);
    EXPECT_EQ(val, 9);
}

TEST_F(CacheTest, DecrementByN) {
    auto key = tk("decr_by_test");

    Cache::get().incr(key, 10);
    auto val = Cache::get().decr(key, 3);
    EXPECT_EQ(val, 7);
}

// --- Set operations ---

TEST_F(CacheTest, SaddAndSmembers) {
    auto key = tk("set_ops");

    Cache::get().sadd(key, "member1");
    Cache::get().sadd(key, "member2");

    auto members = Cache::get().smembers(key);
    EXPECT_EQ(members.size(), 2u);

    // Check both members are present (order is undefined)
    std::sort(members.begin(), members.end());
    EXPECT_EQ(members[0], "member1");
    EXPECT_EQ(members[1], "member2");
}

TEST_F(CacheTest, SaddDuplicate) {
    auto key = tk("set_dup");

    long added1 = Cache::get().sadd(key, "member");
    EXPECT_EQ(added1, 1);

    long added2 = Cache::get().sadd(key, "member");
    EXPECT_EQ(added2, 0);  // Already exists
}

// --- Sorted set operations ---

TEST_F(CacheTest, Zadd) {
    auto key = tk("zset_test");

    long added = Cache::get().zadd(key, "item1", 1.0);
    EXPECT_EQ(added, 1);

    added = Cache::get().zadd(key, "item2", 2.0);
    EXPECT_EQ(added, 1);
}

// --- Pub/Sub ---

TEST_F(CacheTest, Publish) {
    // publish returns number of subscribers (likely 0 in test)
    auto count = Cache::get().publish(tk("channel"), "hello");
    EXPECT_GE(count, 0);
}

// --- Error handling ---

TEST_F(CacheLifecycleTest, OperationBeforeInitThrows) {
    // Create a standalone CacheManager (not the global one)
    Cache::CacheManager mgr;
    EXPECT_THROW(mgr.set("key", "val"), std::runtime_error);
    EXPECT_THROW(mgr.get("key"), std::runtime_error);
}

// --- Logical-DB isolation (T13b) ---
//
// Motivation: this app shares a single Redis instance with another app, and
// their queue/rate-limit/session key names are identical (unprefixed). Without
// a distinct logical DB, the other app's worker was confirmed live to dequeue
// and drop this app's jobs. ConnectionOptions.db (REDIS_DB / cache.db) fixes
// this by giving each app its own `SELECT`ed keyspace on the same instance.
//
// CacheManager is a plain class (not a hidden singleton — see
// OperationBeforeInitThrows above), so two independent instances can each
// hold an open connection on a different db for the duration of one test.

TEST_F(CacheLifecycleTest, DbIsolationBetweenLogicalDatabases) {
    auto key = tk("db_isolation");

    Cache::CacheManager db0;
    Cache::CacheManager db1;
    db0.initialize(REDIS_URL, 2, "", std::chrono::milliseconds(500), std::chrono::milliseconds(500), /*db=*/0);
    db1.initialize(REDIS_URL, 2, "", std::chrono::milliseconds(500), std::chrono::milliseconds(500), /*db=*/1);

    // Write through the Cache facade on db1 only.
    ASSERT_TRUE(db1.set(key, "db1-only"));

    // Same key, same Redis instance, different logical DB: db0 must not see it.
    EXPECT_FALSE(db0.get(key).has_value());

    // db1 sees its own write — SELECT-semantics work through our facade.
    auto v = db1.get(key);
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "db1-only");

    // Direct redis-plus-plus verification, bypassing CacheManager entirely —
    // confirms the isolation is a real Redis SELECT, not a CacheManager
    // bookkeeping artifact. (sw::redis++ types come in transitively via
    // cache/Cache.hpp, same as TestHelpers::is_redis_available.)
    sw::redis::ConnectionOptions raw_opts;
    raw_opts.host = TestHelpers::redis_host();
    raw_opts.port = TestHelpers::redis_port();
    raw_opts.db = 0;
    raw_opts.socket_timeout = std::chrono::milliseconds(500);
    sw::redis::Redis raw_db0(raw_opts);
    auto raw_val = raw_db0.get(key);
    EXPECT_FALSE(static_cast<bool>(raw_val)) << "key written on db1 leaked into db0";

    raw_opts.db = 1;
    sw::redis::Redis raw_db1(raw_opts);
    auto raw_val_db1 = raw_db1.get(key);
    ASSERT_TRUE(static_cast<bool>(raw_val_db1));
    EXPECT_EQ(*raw_val_db1, "db1-only");

    db1.del(key);
    db0.shutdown();
    db1.shutdown();
}
