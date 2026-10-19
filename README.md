# Limit Order Book (C++20)

[![CI](https://github.com/chirataandrei/Limit-Order-Book/actions/workflows/ci.yml/badge.svg)](https://github.com/chirataandrei/Limit-Order-Book/actions/workflows/ci.yml)

A from-scratch central limit order book aimed at low latency. No external dependencies, no `boost::intrusive`, no `std::list`. Built as a learning project / reference implementation for HFT internals.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/lob_sim     # multi-agent market simulation (also self-checking)
./build/lob_tests   # correctness tests
./build/lob_bench   # micro-benchmarks
```

Requires GCC 11+ or Clang 14+ for C++20 concepts and designated initializers, and CMake 3.16+. Tests rely on `assert`, so don't build them with `-DNDEBUG` (the default Release flags here are plain `-O2`).

CI (GitHub Actions) builds and runs the tests with g++ and clang++, runs them again under ASan + UBSan, and compile-checks the benchmarks. Benchmarks are not run in CI: shared runners are too noisy to measure latency.

## Structure

```
include/lob/
  OrderPool.hpp    – fixed-size pool, O(1) alloc/free, no heap after init
  Order.hpp        – 64-byte cache-line-aligned node, intrusive list pointers + back-pointer
  PriceLevel.hpp   – intrusive doubly-linked FIFO of orders at one price
  TickBitmap.hpp   – hierarchical occupancy bitmap (O(log64 n) lowest/highest set tick)
  FlatIdMap.hpp    – open-addressing order-id -> Order* table (linear probing, backward-shift delete)
  OrderBook.hpp    – tick-indexed bid/ask level arrays, order-id lookup, add/cancel/match
  Benchmark.hpp    – minimal timing harness (setup/bench/teardown separation, warmup)
src/main.cpp       – event-driven market simulation (lob_sim)
tests/tests.cpp    – correctness tests (lob_tests)
benchmarks/bench.cpp – micro-benchmarks (lob_bench)
CMakeLists.txt     – builds lob_sim, lob_tests and lob_bench
```

## How it works

**Memory** — `OrderPool(1'000'000)` grabs 64 MB once at startup. Every `add_order` pops from a free list; every `cancel_order` or fill pushes back. The pool, the id map and the level arrays are all allocated up front, so resting, cancelling and matching never touch the heap, as long as the caller's trade buffer has reached its working capacity (see Trade output).

**Price levels** — each side is a dense `PriceLevel[]` indexed by tick: level `i` sits at `min_price + i * tick_size`, set through `BookConfig{min_price, tick_size, num_ticks}` (default grid is 0.00–999.99 in 0.01 steps). `add_order` converts the price to a tick once and then indexes straight into the array — O(1), no tree walk, no node allocation. Prices off the grid or outside it throw `std::invalid_argument`.

**Best bid/ask** — each side caches its best tick. A `TickBitmap` (one bit per tick, plus one summary bit per 64-bit word, recursively) records which levels are non-empty, so when the best level empties the next one is found with one `ctz`/`clz` per bitmap level (4 steps for 1M ticks) instead of a scan.

**Order ids** — `FlatIdMap` is a flat 16-byte-per-slot open-addressing table (2M slots = 32 MB, load ≤ 0.5 because the pool caps live orders), Fibonacci hash, linear probing, backward-shift deletion so there are no tombstones. It replaced `std::unordered_map`, which costs a heap node and a pointer chase per entry.

**Intrusive list** — `Order` carries its own `prev`/`next` pointers, so `PriceLevel::remove()` is O(1) with no search. The `level` back-pointer lets `cancel_order()` go straight from order ID → `Order*` → `PriceLevel*` → intrusive remove, with no price lookup at all.

**Trade output** — `add_order(id, qty, price, side, out)` appends the generated `Trade`s to a caller-owned `std::vector<Trade>` and returns nothing. `clear()` the buffer between calls and its capacity is reused, so matching does no heap allocation once the buffer has grown to the working size. There is deliberately no overload that returns a vector: it would allocate on every trade and make the no-heap guarantee easy to lose by accident. Tests wrap the call in a small helper that builds a fresh vector. Returning the trades by value instead cost about 25 ns on `add+match` on the M4 (~101 ns vs ~75–83 ns) and ~1 ns/level on the sweep.

**Matching** — `match_()` is called inside `add_order()`. It loops while `best_bid >= best_ask`, pops the front of each queue (FIFO priority), fills them, and cleans up any exhausted levels. Execution price is the resting (maker) order's price: `add_order()` matches after every insert, so the book is never crossed on entry and the aggressor is always the order just added. Each `Trade` records its `aggressor` side; a buy that lifts an ask trades at the ask, a sell that hits a bid trades at the bid.

## Market simulation

`src/main.cpp` runs 30 simulated minutes of trading through the real `OrderBook` (about 0.15 s of wall time). A hidden fundamental value follows a diffusion with occasional news jumps, and six agents trade against each other:

- **3 market makers** (fast / mid / slow) quote a 3-level ladder around a lagged estimate of the fundamental, skewed by inventory.
- **Noise trader** — passive limits near the mid, random marketable (IOC) orders and random cancels.
- **Informed trader** — sees the true fundamental and sweeps anything priced away from it.
- **Momentum trader** — chases the mid-price move of the last 5 seconds.

Time is simulated and passed to `add_order` as nanosecond timestamps. The run prints a progress table, the final book, a fundamental-vs-mid price path, market statistics (spread, depth, volatility, VWAP, order-to-trade ratio) and per-agent inventory and PnL. It also checks itself: a shadow model rebuilt from the trade stream is compared with the book throughout, the book must never be crossed, draining it must leave it empty, and inventory and cash must net to zero across agents. Like the tests, it relies on `assert`.

## Benchmark results

Apple M4, Apple clang 16, `-O2 -march=native`, `steady_clock`. Warmup: 3 iterations, samples: 10 iterations. Mean ns/op, run-to-run variation up to ~10%. Each column changes one thing, same machine, run back to back.

| Scenario | `std::map` levels + `unordered_map` ids | tick array + `unordered_map` | tick array + `FlatIdMap` | + caller-owned trade buffer |
|---|---|---|---|---|
| `add_order` — 1M resting orders, each opening a new level | 65 ns | 30 ns | 39–40 ns | 38–42 ns |
| `cancel_order` — 500k random cancels | 540–590 ns | 225–250 ns | 55–56 ns | 58–62 ns |
| `add+match` — 500k aggressive orders, 1:1 fill | 133 ns | 105–111 ns | 99–105 ns | 69–86 ns |
| sweep — 1 order clearing 1 000 price levels | 56 ns/level | 31–33 ns/level | 17 ns/level | 15–17 ns/level |

The first two columns are the original measurements from earlier versions of the code; the last two were re-measured together on the current tree (5–6 back-to-back runs each), so the `FlatIdMap` column differs slightly from older versions of this README (43 / 58 / 111–116 / 17–19) because the machine and tree moved on. The last column is the only change between the two: `add_order` appends to a reused `std::vector<Trade>` instead of returning a fresh one. Matching benchmarks gain the most because every trade used to allocate; the cases with no trades (`add_order`, `cancel_order`) do not change.

Notes:

- **Cancel is 4× faster with `FlatIdMap`** (240 → 58 ns): cancel is dominated by the id lookup, and the shuffled ids made every `unordered_map` lookup a bucket miss plus a node miss. Sweep also gets ~1.8× faster because each fill erases two ids; the caller-owned trade buffer then takes `add+match` from ~100 to ~75 ns.
- **`add_order` got slower (30 → ~40 ns)**, and that is real. The benchmark inserts sequential ids; `unordered_map` hashes `uint64` as the identity, so consecutive ids hit consecutive buckets and consecutive nodes — cache-friendly by accident. A multiplicative hash scatters them over 32 MB, so every insert is a cache miss. I tried an identity hash in `FlatIdMap` to get that locality back; it clustered catastrophically as soon as two id ranges overlapped modulo the table size (the 1:1 match benchmark went from ~100 ns to minutes), so it is not used. `add+match` is flat for the same reason: it is dominated by other work.
- The 30 ns `add_order` figure was the best case: every insert opens a brand-new level and ids are perfectly sequential, so nothing misses cache. Real flow mostly adds to existing levels and has non-sequential ids, so treat ~40 ns as the more honest number for that benchmark and `add+match` (~75 ns with the caller-owned buffer, ~100 ns when returning a vector) as the realistic hot path.
- The tick array trades memory for speed: 40 B per tick per side, allocated up front (a 1M-tick grid is 40 MB per side); `FlatIdMap` is another 32 MB.
- Absolute numbers do not transfer between machines (an earlier version of this README quoted 268 ns for `add_order` from a Linux VM; the same code did 65 ns here). Compare columns, not rows against old figures.

### Linux x86

Measured by the manual `Benchmark (Linux x86)` workflow (`.github/workflows/bench.yml`) on a GitHub-hosted runner: AMD EPYC 9V45 (4 vCPU VM, 32 MB L3), Ubuntu 24.04, g++ 13.3, `-O2 -march=native`, pinned with `taskset -c 1`. Same code as the `FlatIdMap` column above (before the caller-owned trade buffer, so the M4 column here is the older 43 / 58 / 111–116 / 17–19 ns run, not the table above), three back-to-back runs:

| Scenario | M4 (macOS) | Linux ARM64 VM on the same Mac | EPYC VM (Linux x86) |
|---|---|---|---|
| `add_order` — 1M resting | 43 ns | 100–128 ns | 148–168 ns |
| `cancel_order` — 500k random | 58 ns | 63–75 ns | 112–121 ns |
| `add+match` — 1:1 fill | 111–116 ns | 150–191 ns | 208–228 ns |
| sweep (per level) | 17–19 ns | 29–49 ns | 28–30 ns |

The middle column is a Linux (aarch64) guest under Apple Virtualization on the same M4 Mac, three runs, pinned with `taskset -c 2`. It is **not x86**: it isolates the cost of "Linux in a VM" from the cost of the CPU architecture. The x86 column is the GitHub runner only.

Caveats, because this is not a tuned trading box: it is a shared cloud VM (noisy neighbours, no isolated cores, no huge pages, virtualised memory), and neither VM exposes usable hardware counters (`perf stat` reports `<not supported>` on the runner, and `perf_event_paranoid=4` blocks it in the Apple VM) and I could not confirm *why* `add_order` is ~3.5× slower than on the M4 (the M4 has far lower memory latency and a much larger cache for this 64 MB + 32 MB working set; first-touch page faults in a VM are the other suspect). Treat these as an upper bound. For `perf record`/cache-miss numbers, run on bare metal:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
perf stat -e cycles,instructions,cache-misses,branch-misses taskset -c 2 ./build/lob_bench
perf record -g taskset -c 2 ./build/lob_bench && perf report
```

## Known limitations / next steps

- **Fixed price grid** — the price range is fixed at construction. A book that needs to follow a drifting market has to re-centre or size the grid generously.
- **`FlatIdMap` is fixed-capacity and hash-scattered** — it is sized once from the pool capacity and never grows, and its hash trades `add_order` locality for robustness against any id pattern (see benchmark notes). Direct-indexed ids (when the exchange guarantees a dense range) would beat it.
- **`double` prices at the API** — prices are snapped to integer ticks internally, but callers still pass `double`. An integer-tick API would remove the conversion.
- **No thread safety** — single-threaded by design. Adding a lock-free cancel path via `std::atomic<FreeNode*>` CAS is the next concurrency step.
- **No market orders** — straightforward to add: submit with price = `±infinity` and let `match_()` do the rest.

## Author

Andrei Chirata ([@chirataandrei](https://github.com/chirataandrei))
