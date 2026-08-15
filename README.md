# Limit Order Book (C++20)

A from-scratch central limit order book aimed at low latency. No external dependencies, no `boost::intrusive`, no `std::list`. Built as a learning project / reference implementation for HFT internals.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/lob_tests   # correctness tests
./build/lob_bench   # micro-benchmarks
```

Requires GCC 11+ or Clang 14+ for C++20 concepts and `std::chrono::steady_clock`, and CMake 3.16+.

## Structure

```
include/lob/
  ObjectPool.hpp   – fixed-size pool, O(1) alloc/free, no heap after init
  Order.hpp        – 64-byte cache-line-aligned node, intrusive list pointers + back-pointer
  PriceLevel.hpp   – intrusive doubly-linked FIFO of orders at one price
  OrderBook.hpp    – bid/ask maps, order-id lookup, add/cancel/match
  Benchmark.hpp    – minimal timing harness (setup/bench/teardown separation, warmup)
src/main.cpp       – correctness tests
benchmarks/bench.cpp – micro-benchmarks
CMakeLists.txt     – builds lob_tests and lob_bench
```

## How it works

**Memory** — `ObjectPool<Order, 1'000'000>` grabs 64 MB once at startup. Every `add_order` pops from a free list; every `cancel_order` or fill pushes back. No heap traffic on the hot path.

**Price levels** — `std::map<double, PriceLevel>` (bids descending, asks ascending). `begin()` is always the best price. The plan is to replace this with a flat tick array once the price range is known.

**Intrusive list** — `Order` carries its own `prev`/`next` pointers, so `PriceLevel::remove()` is O(1) with no search. The `level` back-pointer lets `cancel_order()` skip the map lookup entirely — you go straight from order ID → `Order*` → `PriceLevel*` → intrusive remove.

**Matching** — `match_()` is called inside `add_order()`. It loops while `best_bid >= best_ask`, pops the front of each queue (FIFO priority), fills them, and cleans up any exhausted levels. Execution price is the resting side's price.

## Benchmark results

Measured on a sandbox VM (x86-64 Linux, `-O2 -march=native`, `steady_clock`).  
Warmup: 3 iterations. Samples: 10 iterations.

| Scenario | mean | p99 |
|---|---|---|
| `add_order` — 1M resting orders, no match | 268 ns | 277 ns |
| `cancel_order` — 500k random cancels | 834 ns | 936 ns |
| `add+match` — 500k aggressive orders, 1:1 fill | 83 ns | 84 ns |
| sweep — 1 order clearing 1 000 price levels | 45 ns/level | 57 ns |

The `add_order` number (268 ns) is almost entirely `std::map::try_emplace` at O(log P) where P = 1M distinct prices. In practice most instruments have far fewer active levels, and this drops significantly.

The `cancel_order` number (834 ns) looks high because the benchmark cancels in shuffled order, maximising hash-table cache misses. Sequential cancels are much faster.

The `add+match` number (83 ns) is the most relevant for HFT: full round-trip of an aggressive order from submission to pool deallocation.

## Known limitations / next steps

- **`std::map` for price levels** — O(log P) insert/lookup. For tick-quantised prices, a flat array indexed by tick gives O(1). This is the biggest latency improvement left on the table.
- **`double` prices** — floating-point comparison is fine here but production systems use integer ticks to avoid FP issues.
- **No thread safety** — single-threaded by design. Adding a lock-free cancel path via `std::atomic<FreeNode*>` CAS is the next concurrency step.
- **Trade price convention** — execution always at ask price. A proper exchange needs to track which order was the aggressor.
- **No market orders** — straightforward to add: submit with price = `±infinity` and let `match_()` do the rest.
