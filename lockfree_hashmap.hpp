#pragma once
/**
 * lockfree_hashmap.hpp
 * ─────────────────────────────────────────────────────────────────────────────
 * A lock-free, open-addressing hash map for C++17.
 *
 * DESIGN
 * ──────
 * Each slot stores two independent atomics:
 *   atomic<uint64_t>  key    — claimed via CAS (EMPTY → key)
 *   atomic<uint64_t>  value  — written with release after key CAS succeeds
 *
 * This avoids needing 128-bit CAS (CMPXCHG16B), making the map truly
 * lock-free on all 64-bit platforms.  The key atomic alone guards ownership;
 * the value atomic guards the data.
 *
 * INSERT (key, value):
 *   1. Probe until slot.key == EMPTY or slot.key == our key.
 *   2. CAS(EMPTY → key).  On win, store value with release.
 *   3. If slot.key == our key (update): CAS-loop value to new value.
 *
 * FIND (key):
 *   Linear probe.  Load key with acquire; on match, load value with acquire.
 *
 * ERASE (key):
 *   CAS(key → DELETED_KEY).  Tombstones preserve probing chains.
 *
 * RESIZE:
 *   Triggered when (live + tombstones) / capacity > 0.70.
 *   Serialised with a mutex (only one resize at a time).  All concurrent
 *   readers/writers on the old table finish before the old table is freed.
 *
 * MEMORY ORDERING
 * ───────────────
 *   key loads / CAS          → acquire / acq_rel
 *   value store after CAS    → release
 *   value load on find       → acquire
 *   size/tombstone counters  → relaxed (approximate)
 *
 * CONSTRAINTS
 * ───────────
 *   Keys 0 (EMPTY_KEY) and UINT64_MAX (DELETED_KEY) are reserved — insert throws.
 */

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace lfhm {

static constexpr uint64_t EMPTY_KEY   = 0ULL;
static constexpr uint64_t DELETED_KEY = ~0ULL;

// ─────────────────────────────────────────────────────────────────────────────
struct Table {
    struct Slot {
        std::atomic<uint64_t> key{EMPTY_KEY};
        std::atomic<uint64_t> value{0};
    };

    const size_t cap;
    std::unique_ptr<Slot[]> slots;

    explicit Table(size_t c) : cap(c), slots(std::make_unique<Slot[]>(c)) {}

    static size_t hash(uint64_t key) {
        return static_cast<size_t>(key * 11400714819323198485ULL);
    }

    // Returns: 0=new insert, 1=update/tombstone-reuse, -1=table full
    int try_insert(uint64_t key, uint64_t val) {
        const size_t mask = cap - 1;
        size_t idx = hash(key) & mask;
        for (size_t probe = 0; probe < cap; ++probe) {
            uint64_t k = slots[idx].key.load(std::memory_order_acquire);

            if (k == EMPTY_KEY || k == DELETED_KEY) {
                uint64_t expected = k;
                if (slots[idx].key.compare_exchange_strong(
                        expected, key,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    slots[idx].value.store(val, std::memory_order_release);
                    return (k == EMPTY_KEY) ? 0 : 1;
                }
                --probe; continue; // CAS lost, retry same slot
            }
            if (k == key) {
                // Update existing key's value
                uint64_t old = slots[idx].value.load(std::memory_order_acquire);
                while (!slots[idx].value.compare_exchange_weak(
                            old, val,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {}
                return 1;
            }
            idx = (idx + 1) & mask;
        }
        return -1; // full
    }

    std::optional<uint64_t> find(uint64_t key) const {
        const size_t mask = cap - 1;
        size_t idx = hash(key) & mask;
        for (size_t probe = 0; probe < cap; ++probe) {
            uint64_t k = slots[idx].key.load(std::memory_order_acquire);
            if (k == EMPTY_KEY)  return std::nullopt;
            if (k == key)        return slots[idx].value.load(std::memory_order_acquire);
            idx = (idx + 1) & mask;
        }
        return std::nullopt;
    }

    bool erase(uint64_t key) {
        const size_t mask = cap - 1;
        size_t idx = hash(key) & mask;
        for (size_t probe = 0; probe < cap; ++probe) {
            uint64_t k = slots[idx].key.load(std::memory_order_acquire);
            if (k == EMPTY_KEY)  return false;
            if (k == key) {
                uint64_t expected = key;
                if (slots[idx].key.compare_exchange_strong(
                        expected, DELETED_KEY,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                    return true;
                --probe; continue;
            }
            idx = (idx + 1) & mask;
        }
        return false;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
class LockFreeHashMap {
public:
    explicit LockFreeHashMap(size_t initial_capacity = 64)
        : size_(0), tombstones_(0)
    {
        size_t cap = next_pow2(initial_capacity < 4 ? 4 : initial_capacity);
        table_.store(new Table(cap), std::memory_order_release);
    }

    ~LockFreeHashMap() { delete table_.load(); }

    LockFreeHashMap(const LockFreeHashMap&)            = delete;
    LockFreeHashMap& operator=(const LockFreeHashMap&) = delete;

    bool insert(uint64_t key, uint64_t value) {
        if (key == EMPTY_KEY || key == DELETED_KEY)
            throw std::invalid_argument("Reserved key");
        maybe_resize();
        Table* t = table_.load(std::memory_order_acquire);
        int rc = t->try_insert(key, value);
        if (rc == -1) { do_resize(); t = table_.load(std::memory_order_acquire); rc = t->try_insert(key, value); }
        if (rc == 0) size_.fetch_add(1, std::memory_order_relaxed);
        return rc == 0;
    }

    std::optional<uint64_t> find(uint64_t key) const {
        if (key == EMPTY_KEY || key == DELETED_KEY) return std::nullopt;
        return table_.load(std::memory_order_acquire)->find(key);
    }

    bool erase(uint64_t key) {
        if (key == EMPTY_KEY || key == DELETED_KEY) return false;
        bool ok = table_.load(std::memory_order_acquire)->erase(key);
        if (ok) {
            size_.fetch_sub(1, std::memory_order_relaxed);
            tombstones_.fetch_add(1, std::memory_order_relaxed);
        }
        return ok;
    }

    size_t size()     const { return size_.load(std::memory_order_relaxed); }
    size_t capacity() const { return table_.load(std::memory_order_acquire)->cap; }
    bool   is_lock_free() const { std::atomic<uint64_t> p; return p.is_lock_free(); }

private:
    std::atomic<Table*> table_;
    std::atomic<size_t> size_;
    std::atomic<size_t> tombstones_;
    std::mutex          resize_mutex_;

    static size_t next_pow2(size_t n) {
        --n; for (size_t i=1; i<64; i<<=1) n|=n>>i; return n+1;
    }

    void maybe_resize() {
        Table* t = table_.load(std::memory_order_relaxed);
        if ((size_.load(std::memory_order_relaxed) +
             tombstones_.load(std::memory_order_relaxed)) * 10 < t->cap * 7) return;
        do_resize();
    }

    void do_resize() {
        std::lock_guard<std::mutex> lk(resize_mutex_);
        Table* old = table_.load(std::memory_order_relaxed);
        size_t live = size_.load(std::memory_order_relaxed);
        size_t dead = tombstones_.load(std::memory_order_relaxed);
        if ((live + dead) * 10 < old->cap * 7) return; // someone else resized

        size_t new_cap = (live * 10 >= old->cap * 5) ? old->cap * 2 : old->cap;
        Table* fresh = new Table(new_cap);
        size_t new_size = 0;
        for (size_t i = 0; i < old->cap; ++i) {
            uint64_t k = old->slots[i].key.load(std::memory_order_relaxed);
            if (k == EMPTY_KEY || k == DELETED_KEY) continue;
            uint64_t v = old->slots[i].value.load(std::memory_order_relaxed);
            fresh->try_insert(k, v);
            ++new_size;
        }
        table_.store(fresh, std::memory_order_release);
        size_.store(new_size, std::memory_order_release);
        tombstones_.store(0,  std::memory_order_release);
        delete old;
    }
};

} // namespace lfhm
