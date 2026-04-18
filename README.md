# Lock-Free Hash Map — C++17

A header-only, lock-free open-addressing hash map built with `std::atomic<uint64_t>`.  
All read/write/delete operations are safe to call concurrently from any number of threads with **no external locking**.

## Benchmark Results

Measured on a single machine: 1M insert + 1M find operations, varying thread count.

| Threads | LockFreeHashMap | `mutex` + `unordered_map` | Speedup |
|---------|----------------|--------------------------|---------|
| 1       | 63 ms          | 726 ms                   | **11.5×** |
| 2       | 105 ms         | 882 ms                   | **8.4×**  |
| 4       | 113 ms         | 904 ms                   | **8.0×**  |
| 8       | 98 ms          | 1076 ms                  | **11.0×** |

> The mutex baseline serialises all operations — throughput plateaus and then degrades with threads due to contention. The lock-free map scales because concurrent readers never block each other.

## Design

### Data layout

Each slot holds two independent 64-bit atomics:

```
Slot {
    atomic<uint64_t>  key    // ownership: EMPTY → claimed via CAS
    atomic<uint64_t>  value  // data:      written with release after CAS
}
```

This avoids needing 128-bit CMPXCHG16B, making the map truly lock-free on all 64-bit platforms.

### Insert

1. Probe (linear) until `slot.key == EMPTY` or `slot.key == our key`.
2. `CAS(EMPTY → key)` with `acq_rel`. On win: `store(value, release)`.
3. If `slot.key == our key` (update): CAS-loop value to new value.

### Find

Linear probe with `acquire` loads. Stop at `EMPTY_KEY` (definite miss) or matching key.  
The load of `value` after a key match uses `acquire` — happens-after the `release` store in insert.

### Erase

`CAS(key → DELETED_KEY)`. Tombstones preserve probing chains so subsequent finds remain correct.

### Resize

Triggered when `(live + tombstones) / capacity > 0.70`.  
Resize is the **only locked path** — a `std::mutex` ensures a single thread rebuilds the table while others continue reading/writing the old one. After the new table is published via `store(release)`, the old table is freed.

### Memory ordering rationale

| Operation           | Ordering     | Why |
|---------------------|-------------|-----|
| `key.load`          | `acquire`    | See prior `release` stores from other threads |
| `key CAS (success)` | `acq_rel`    | Reads old state + publishes new key atomically |
| `value.store`       | `release`    | Pairs with `acquire` load in find |
| `value.load`        | `acquire`    | See the `release` store from insert |
| `size` / `tombstone`| `relaxed`    | Approximate counters; exact value not critical for correctness |

### Hash function

Fibonacci hashing: `key × 11400714819323198485` (≈ 2⁶⁴/φ).  
Excellent avalanche property, zero divisions, no modulo bias with power-of-two tables.

## API

```cpp
#include "lockfree_hashmap.hpp"
using namespace lfhm;

LockFreeHashMap m(/*initial_capacity=*/ 1024);

m.insert(key, value);            // true = new key, false = updated
auto v = m.find(key);            // std::optional<uint64_t>
m.erase(key);                    // true if key was present
m.size();                        // approximate live count
m.is_lock_free();                // true on all 64-bit platforms
```

**Reserved keys:** `0` (EMPTY) and `UINT64_MAX` (DELETED) — `insert` throws `std::invalid_argument`.

## Build & Test

```bash
g++ -std=c++17 -O2 -pthread test_lockfree_hashmap.cpp -o test_lfhm -latomic
./test_lfhm
```

Expected output: `122 passed, 0 failed`

## Test coverage

| Category | What is tested |
|----------|---------------|
| Basic ops | insert, find, update, erase, size |
| Collisions | 100 inserts into a 4-slot table (forced probing) |
| Resize | 1000 inserts → 500 erases → 500 new inserts; tombstone sweep |
| Reserved keys | throws on EMPTY_KEY and DELETED_KEY |
| Concurrent insert | 8 threads × 10k disjoint keys |
| Concurrent mixed | 4 threads erasing/inserting + 4 threads reading simultaneously |
| Concurrent resize | 8 threads flooding a capacity-4 table, triggering repeated resize |

## Limitations & future work

- Keys and values are `uint64_t`. Extend with templates for arbitrary types + custom hash.
- Resize is a stop-the-world rebuild behind a mutex. A fully lock-free resize (via a migration protocol or epoch-based reclamation) is possible but significantly more complex.
- No iterator support — add a snapshot-based `for_each` that scans the slot array.
