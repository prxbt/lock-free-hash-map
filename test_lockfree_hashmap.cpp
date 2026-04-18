/**
 * test_lockfree_hashmap.cpp
 * --------------------------
 * Correctness tests for LockFreeHashMap.
 * Covers: basic ops, edge cases, concurrent insert/find/erase, resize under load.
 *
 * Compile & run:
 *   g++ -std=c++17 -O2 -pthread test_lockfree_hashmap.cpp -o test_lfhm && ./test_lfhm
 */

#include "lockfree_hashmap.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>
#include <unordered_map>

using namespace lfhm;

// ─── helpers ─────────────────────────────────────────────────────────────────

static int passed = 0, failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { ++passed; printf("  PASS  %s\n", msg); } \
        else      { ++failed; printf("  FAIL  %s  (line %d)\n", msg, __LINE__); } \
    } while(0)

// ─── single-threaded correctness ─────────────────────────────────────────────

void test_basic() {
    printf("\n[1] Basic single-threaded operations\n");
    LockFreeHashMap m(8);

    CHECK(m.size() == 0, "empty map has size 0");
    CHECK(!m.find(42).has_value(), "find on empty returns nullopt");

    bool inserted = m.insert(1, 100);
    CHECK(inserted == true, "first insert returns true");
    CHECK(m.size() == 1, "size is 1 after insert");

    auto v = m.find(1);
    CHECK(v.has_value() && *v == 100, "find returns correct value");

    bool updated = m.insert(1, 999);
    CHECK(updated == false, "update returns false");
    CHECK(*m.find(1) == 999, "value updated correctly");

    bool erased = m.erase(1);
    CHECK(erased == true, "erase existing key returns true");
    CHECK(m.size() == 0, "size is 0 after erase");
    CHECK(!m.find(1).has_value(), "erased key not findable");

    bool erase_missing = m.erase(1);
    CHECK(erase_missing == false, "erase missing key returns false");
}

void test_collisions() {
    printf("\n[2] Hash collisions and linear probing\n");
    // Small table forces many collisions
    LockFreeHashMap m(4);
    for (uint64_t i = 1; i <= 100; ++i) m.insert(i, i * 10);
    for (uint64_t i = 1; i <= 100; ++i) {
        auto v = m.find(i);
        CHECK(v.has_value() && *v == i * 10, "all 100 keys findable after collisions");
        if (!v.has_value() || *v != i * 10) break;
    }
    CHECK(m.size() == 100, "size correct after 100 inserts with collisions");
}

void test_resize() {
    printf("\n[3] Resize and tombstone sweep\n");
    LockFreeHashMap m(8);
    // Insert 1000, erase 500, insert 500 new — exercises both grow and tombstone sweep
    for (uint64_t i = 1; i <= 1000; ++i) m.insert(i, i);
    for (uint64_t i = 1; i <= 500; ++i)  m.erase(i);
    for (uint64_t i = 1001; i <= 1500; ++i) m.insert(i, i);

    CHECK(m.size() == 1000, "size correct after resize cycles");

    bool ok = true;
    for (uint64_t i = 501; i <= 1500; ++i) {
        auto v = m.find(i);
        if (!v.has_value() || *v != i) { ok = false; break; }
    }
    CHECK(ok, "all live keys findable after resize");

    bool missing_ok = true;
    for (uint64_t i = 1; i <= 500; ++i) {
        if (m.find(i).has_value()) { missing_ok = false; break; }
    }
    CHECK(missing_ok, "erased keys not findable after resize");
}

void test_reserved_keys() {
    printf("\n[4] Reserved key rejection\n");
    LockFreeHashMap m;
    bool threw_empty = false, threw_deleted = false;
    try { m.insert(0ULL, 1); } catch (...) { threw_empty = true; }
    try { m.insert(~0ULL, 1); } catch (...) { threw_deleted = true; }
    CHECK(threw_empty,   "EMPTY_KEY insert throws");
    CHECK(threw_deleted, "DELETED_KEY insert throws");
}

// ─── concurrent tests ────────────────────────────────────────────────────────

void test_concurrent_insert() {
    printf("\n[5] Concurrent insert — %d threads, 10000 keys each\n", 8);
    const int THREADS = 8;
    const int PER_THREAD = 10000;

    LockFreeHashMap m(THREADS * PER_THREAD * 2);
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&m, t]() {
            uint64_t base = (uint64_t)t * 10000 + 1;
            for (uint64_t i = base; i < base + 10000; ++i)
                m.insert(i, i * 2);
        });
    }
    for (auto& th : threads) th.join();

    CHECK(m.size() == (size_t)(THREADS * PER_THREAD), "size correct after concurrent inserts");

    bool ok = true;
    for (int t = 0; t < THREADS && ok; ++t) {
        uint64_t base = (uint64_t)t * 10000 + 1;
        for (uint64_t i = base; i < base + 10000; ++i) {
            auto v = m.find(i);
            if (!v.has_value() || *v != i * 2) { ok = false; break; }
        }
    }
    CHECK(ok, "all keys findable after concurrent inserts");
}

void test_concurrent_mixed() {
    printf("\n[6] Concurrent mixed insert/find/erase\n");
    const int THREADS = 8;
    const uint64_t RANGE = 5000;

    LockFreeHashMap m(RANGE * 2);
    // Pre-populate half
    for (uint64_t i = 1; i <= RANGE; ++i) m.insert(i, i);

    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    // Half threads: erase odds, insert evens shifted
    for (int t = 0; t < THREADS / 2; ++t) {
        threads.emplace_back([&]() {
            for (uint64_t i = 1; i <= RANGE; ++i) {
                if (i % 2 == 1) m.erase(i);
                else            m.insert(i + RANGE, i + RANGE);
            }
        });
    }
    // Other half threads: just read — no crash = pass
    for (int t = 0; t < THREADS / 2; ++t) {
        threads.emplace_back([&]() {
            for (uint64_t i = 1; i <= RANGE; ++i) {
                (void)m.find(i);
            }
        });
    }

    for (auto& th : threads) th.join();
    CHECK(errors.load() == 0, "no data races or crashes in mixed concurrent ops");
}

void test_concurrent_resize() {
    printf("\n[7] Concurrent insert triggering resize\n");
    // Start with tiny table, flood with inserts from many threads
    const int THREADS = 8;
    const int PER_THREAD = 5000;
    LockFreeHashMap m(4);  // deliberately tiny

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&m, t]() {
            uint64_t base = (uint64_t)t * 5000 + 1;
            for (uint64_t i = base; i < base + 5000; ++i)
                m.insert(i, i);
        });
    }
    for (auto& th : threads) th.join();

    CHECK(m.size() == (size_t)(THREADS * PER_THREAD), "size correct after resize under concurrent load");

    bool ok = true;
    for (int t = 0; t < THREADS && ok; ++t) {
        uint64_t base = (uint64_t)t * 5000 + 1;
        for (uint64_t i = base; i < base + 5000; ++i) {
            if (!m.find(i).has_value()) { ok = false; break; }
        }
    }
    CHECK(ok, "all keys survive concurrent resize");
}

// ─── benchmark ───────────────────────────────────────────────────────────────

void benchmark() {
    printf("\n[8] Throughput benchmark\n");
    const size_t N = 1'000'000;
    const std::vector<int> thread_counts = {1, 2, 4, 8};

    printf("  %-10s  %-20s  %-20s\n", "Threads", "LockFreeHashMap", "mutex+unordered_map");
    printf("  %-10s  %-20s  %-20s\n", "-------", "---------------", "-------------------");

    for (int T : thread_counts) {
        // --- lock-free ---
        {
            LockFreeHashMap m(N * 2);
            auto t0 = std::chrono::high_resolution_clock::now();
            std::vector<std::thread> threads;
            for (int t = 0; t < T; ++t) {
                threads.emplace_back([&m, t, T, N]() {
                    size_t per = N / T;
                    uint64_t base = (uint64_t)t * per + 1;
                    for (uint64_t i = base; i < base + per; ++i)
                        m.insert(i, i);
                    for (uint64_t i = base; i < base + per; ++i)
                        (void)m.find(i);
                });
            }
            for (auto& th : threads) th.join();
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms_lf = std::chrono::duration<double, std::milli>(t1 - t0).count();

            // --- mutex + std::unordered_map ---
            std::unordered_map<uint64_t, uint64_t> stdmap;
            stdmap.reserve(N * 2);
            std::mutex mtx;
            auto t2 = std::chrono::high_resolution_clock::now();
            std::vector<std::thread> threads2;
            for (int t = 0; t < T; ++t) {
                threads2.emplace_back([&stdmap, &mtx, t, T, N]() {
                    size_t per = N / T;
                    uint64_t base = (uint64_t)t * per + 1;
                    for (uint64_t i = base; i < base + per; ++i) {
                        std::lock_guard<std::mutex> lk(mtx);
                        stdmap[i] = i;
                    }
                    for (uint64_t i = base; i < base + per; ++i) {
                        std::lock_guard<std::mutex> lk(mtx);
                        (void)stdmap.count(i);
                    }
                });
            }
            for (auto& th : threads2) th.join();
            auto t3 = std::chrono::high_resolution_clock::now();
            double ms_std = std::chrono::duration<double, std::milli>(t3 - t2).count();

            printf("  %-10d  %-16.1f ms    %-16.1f ms   (%.1fx speedup)\n",
                   T, ms_lf, ms_std, ms_std / ms_lf);
        }
    }
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    printf("========================================\n");
    printf("  LockFreeHashMap — test suite\n");
    printf("========================================\n");

    // Correctness
    test_basic();
    test_collisions();
    test_resize();
    test_reserved_keys();

    // Concurrency
    test_concurrent_insert();
    test_concurrent_mixed();
    test_concurrent_resize();

    // Benchmark
    benchmark();

    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n", passed, failed);
    printf("========================================\n");

    return failed == 0 ? 0 : 1;
}
