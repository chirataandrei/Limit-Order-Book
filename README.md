# Limit Order Book (C++20)

[![CI](https://github.com/chirataandrei/Limit-Order-Book/actions/workflows/ci.yml/badge.svg)](https://github.com/chirataandrei/Limit-Order-Book/actions/workflows/ci.yml)

A from-scratch central limit order book aimed at low latency. No external dependencies, no `boost::intrusive`, no `std::list`. Built as a learning project / reference implementation for HFT internals.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/lob_tests   # correctness tests
./build/lob_bench   # micro-benchmarks
```

Requires GCC 11+ or Clang 14+ for C++20 concepts and designated initializers, and CMake 3.16+. Tests rely on `assert`, so don't build them with `-DNDEBUG` (the default Release flags here are plain `-O2`).

CI (GitHub Actions) builds and runs the tests with g++ and clang++, runs them again under ASan + UBSan, and compile-checks the benchmarks. Benchmarks are not run in CI: shared runners are too noisy to measure latency.

## Structure

```
include/lob/
  ObjectPool.hpp   – fixed-size pool, O(1) alloc/free, no heap after init
  Order.hpp        – 64-byte cache-line-aligned node, intrusive list pointers + back-pointer
  PriceLevel.hpp   – intrusive doubly-linked FIFO of orders at one price
  TickBitmap.hpp   – hierarchical occupancy bitmap (O(log64 n) lowest/highest set tick)
  OrderBook.hpp    – tick-indexed bid/ask level arrays, order-id lookup, add/cancel/match
  Benchmark.hpp    – minimal timing harness (setup/bench/teardown separation, warmup)
src/main.cpp       – correctness tests
benchmarks/bench.cpp – micro-benchmarks
CMakeLists.txt     – builds lob_tests and lob_bench
```

## How it works

**Memory** — `ObjectPool<Order, 1'000'000>` grabs 64 MB once at startup. Every `add_order` pops from a free list; every `cancel_order` or fill pushes back. No heap traffic on the hot path.

**Price levels** — each side is a dense `PriceLevel[]` indexed by tick: level `i` sits at `min_price + i * tick_size`, set through `BookConfig{min_price, tick_size, num_ticks}` (default grid is 0.00–999.99 in 0.01 steps). `add_order` converts the price to a tick once and then indexes straight into the array — O(1), no tree walk, no node allocation. Prices off the grid or outside it throw `std::invalid_argument`.

**Best bid/ask** — each side caches its best tick. A `TickBitmap` (one bit per tick, plus one summary bit per 64-bit word, recursively) records which levels are non-empty, so when the best level empties the next one is found with one `ctz`/`clz` per bitmap level (4 steps for 1M ticks) instead of a scan.

**Intrusive list** — `Order` carries its own `prev`/`next` pointers, so `PriceLevel::remove()` is O(1) with no search. The `level` back-pointer lets `cancel_order()` go straight from order ID → `Order*` → `PriceLevel*` → intrusive remove, with no price lookup at all.

**Matching** — `match_()` is called inside `add_order()`. It loops while `best_bid >= best_ask`, pops the front of each queue (FIFO priority), fills them, and cleans up any exhausted levels. Execution price is the resting side's price.

## Benchmark results

`std::map<double, PriceLevel>` → tick-indexed array, same benchmarks, same machine, run back to back.  
Apple M4, Apple clang 16, `-O2 -mcpu=native`, `steady_clock`. Warmup: 3 iterations, samples: 10 iterations. Mean ns/op, two runs each (run-to-run variation up to ~10%).

| Scenario | `std::map` | tick array | |
|---|---|---|---|
| `add_order` — 1M resting orders, each opening a new level | 65 ns | 30 ns | 2.2× |
| `cancel_order` — 500k random cancels | 540–590 ns | 225 ns | 2.4× |
| `add+match` — 500k aggressive orders, 1:1 fill | 133 ns | 105–111 ns | 1.2× |
| sweep — 1 order clearing 1 000 price levels | 56 ns/level | 33 ns/level | 1.7× |

Notes:

- The `add_order` benchmark opens a brand-new level on every insert, which is the worst case for a tree (1M nodes, 1M allocations) and the best case for an array. `add+match` only ever touches one price, so the old tree was tiny and cache-resident; that is why it moves least.
- `cancel_order` also gains because cancelling the last order at a level used to be a tree erase; now it clears a bit.
- The tick array trades memory for speed: 40 B per tick per side, allocated up front (a 1M-tick grid is 40 MB per side).
- The first version of this README quoted 268 ns for `add_order` from a Linux VM. On this machine the same code measured 65 ns, so absolute numbers do not transfer between machines; compare the columns, not the rows against old figures.

## Known limitations / next steps

- **Fixed price grid** — the price range is fixed at construction. A book that needs to follow a drifting market has to re-centre or size the grid generously.
- **`order_map_` is still `std::unordered_map`** — it is probably the largest remaining cost in `add_order` and `cancel_order` (not profiled yet). A flat open-addressing table (or direct-indexed ids) is the next step.
- **`double` prices at the API** — prices are snapped to integer ticks internally, but callers still pass `double`. An integer-tick API would remove the conversion.
- **No thread safety** — single-threaded by design. Adding a lock-free cancel path via `std::atomic<FreeNode*>` CAS is the next concurrency step.
- **Trade price convention** — execution always at ask price. A proper exchange needs to track which order was the aggressor.
- **No market orders** — straightforward to add: submit with price = `±infinity` and let `match_()` do the rest.

## Author

Andrei Chirata ([@chirataandrei](https://github.com/chirataandrei))
