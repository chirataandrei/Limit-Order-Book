// bench.cpp
// Compile: g++ -std=c++20 -O2 -march=native -Wno-unused-result -o bench bench.cpp

#include "Benchmark.hpp"
#include "OrderBook.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

using namespace lob;
using namespace bench;

static constexpr int      WARMUP  = 3;
static constexpr int      ITERS   = 10;

// ID ranges must not overlap across benchmarks (same static book is reused).
static constexpr uint64_t ADD_BASE      =         1;  // 1M slots
static constexpr uint64_t CANCEL_BASE   = 2'000'001;  // 500k slots
static constexpr uint64_t MATCH_ASK_BASE= 4'000'001;  // 500k slots
static constexpr uint64_t MATCH_BID_BASE= 6'000'001;  // 500k slots
static constexpr uint64_t SWEEP_ASK_BASE= 8'000'001;  // 1k slots
static constexpr uint64_t SWEEP_BID_BASE= 8'100'001;  // one per iteration

// ── 1. add_order – no matching ───────────────────────────────────────────────
// Each order goes to a unique price level (prices are id-derived), so this
// exercises std::map::try_emplace on every insert. Expect ~log(1M)=20
// comparisons per op, which is the main cost. Swap for tick-array to fix this.

static Stats bench_add() {
    static OrderBook book;
    static constexpr uint64_t N = 1'000'000;

    struct S { OrderBook* b; };
    return run_benchmark<S>(
        "add_order (1M resting, no match)", N, WARMUP, ITERS,
        []()    -> S  { return { &book }; },
        [](S& s)      {
            for (uint64_t i = 0; i < N; ++i) {
                auto t = s.b->add_order(ADD_BASE + i, 100,
                             1'000'001.0 + static_cast<double>(i), Side::Buy);
                do_not_optimize(t.size());
            }
        },
        [](S& s) {
            for (uint64_t i = 0; i < N; ++i)
                s.b->cancel_order(ADD_BASE + i);
        }
    );
}

// ── 2. cancel_order – random access pattern ──────────────────────────────────
// Orders are pre-placed, then cancelled in shuffled order. The shuffle
// deliberately thrashes unordered_map bucket chains and map node cache lines –
// worst case, and closer to real cancel-replace traffic than FIFO cancels.

static Stats bench_cancel() {
    static OrderBook book;
    static constexpr uint64_t N = 500'000;

    // Fixed shuffle so results are reproducible across runs.
    static std::vector<uint64_t> shuffled = []() {
        std::vector<uint64_t> v(N);
        std::iota(v.begin(), v.end(), CANCEL_BASE);
        std::shuffle(v.begin(), v.end(), std::mt19937_64{42});
        return v;
    }();

    struct S { OrderBook* b; std::vector<uint64_t>& ids; };
    return run_benchmark<S>(
        "cancel_order (500k random)", N, WARMUP, ITERS,
        []() -> S {
            for (uint64_t i = 0; i < N; ++i)
                book.add_order(CANCEL_BASE + i, 50,
                               500'000.0 + static_cast<double>(i), Side::Buy);
            return { &book, shuffled };
        },
        [](S& s) {
            for (uint64_t id : s.ids) {
                bool ok = s.b->cancel_order(id);
                do_not_optimize(ok);
            }
        },
        [](S&) { /* book is empty after bench */ }
    );
}

// ── 3a. add+match – 1:1 fill ─────────────────────────────────────────────────
// N asks sit at 100.0. N aggressive buys arrive one by one, each immediately
// matching the front of the ask queue (FIFO). This is the tightest hot path:
// pool allocate → map insert at existing level → match() pops one ask → pool free.
// Peak pool usage = N asks + 1 buy-in-flight = N+1. Must stay under POOL_CAPACITY.

static Stats bench_match_1for1() {
    static OrderBook book;
    static constexpr uint64_t N = 500'000;  // each side N, peak usage N+1

    struct S { OrderBook* b; };
    return run_benchmark<S>(
        "add+match (500k pairs, 1:1 fill)", N, WARMUP, ITERS,
        []() -> S {
            for (uint64_t i = 0; i < N; ++i)
                book.add_order(MATCH_ASK_BASE + i, 1, 100.0, Side::Sell);
            return { &book };
        },
        [](S& s) {
            for (uint64_t i = 0; i < N; ++i) {
                auto t = s.b->add_order(MATCH_BID_BASE + i, 1, 100.0, Side::Buy);
                do_not_optimize(t.size());
            }
        },
        [](S& s) {
            // defensive cleanup in case not everything was matched
            for (uint64_t i = 0; i < N; ++i) {
                s.b->cancel_order(MATCH_ASK_BASE + i);
                s.b->cancel_order(MATCH_BID_BASE + i);
            }
        }
    );
}

// ── 3b. sweep match – 1 order clears N levels ────────────────────────────────
// A single large buy sweeps through 1000 ask price levels consecutively.
// Measures how fast match() can iterate and erase map entries when a
// market order (or a very aggressive limit) walks the book.

static Stats bench_match_sweep() {
    static OrderBook book;
    static constexpr uint64_t LEVELS = 1'000;
    static uint64_t sweep_bid_id = SWEEP_BID_BASE;

    struct S { OrderBook* b; };
    return run_benchmark<S>(
        "sweep match (1 order clears 1k levels)", LEVELS, WARMUP, ITERS,
        []() -> S {
            for (uint64_t i = 0; i < LEVELS; ++i)
                book.add_order(SWEEP_ASK_BASE + i, 1,
                               100.0 + static_cast<double>(i), Side::Sell);
            return { &book };
        },
        [](S& s) {
            auto t = s.b->add_order(sweep_bid_id++, LEVELS,
                         100.0 + static_cast<double>(LEVELS - 1), Side::Buy);
            do_not_optimize(t.size());
        },
        [](S& s) {
            for (uint64_t i = 0; i < LEVELS; ++i)
                s.b->cancel_order(SWEEP_ASK_BASE + i);
            // cancel any un-filled buy remainder
            for (uint64_t id = SWEEP_BID_BASE; id < sweep_bid_id; ++id)
                s.b->cancel_order(id);
        }
    );
}

// ── summary ───────────────────────────────────────────────────────────────────
static void print_summary(const Stats& a, const Stats& c,
                          const Stats& m, const Stats& s)
{
    const std::string bar(56, '-');
    std::cout << "\n+" << bar << "+\n";
    std::cout << "| " << std::left << std::setw(54) << "SUMMARY  (mean / p99  ns per op)" << " |\n";
    std::cout << "+" << bar << "+\n";

    auto row = [&](const char* label, double mean, double p99) {
        std::cout << "| " << std::left << std::setw(34) << label
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(8) << mean << " ns   p99 "
                  << std::setw(7) << p99  << " ns |\n";
    };
    row("add_order (1M, no match)",      a.mean_ns, a.p99_ns);
    row("cancel_order (500k random)",    c.mean_ns, c.p99_ns);
    row("add+match (500k, 1:1 fill)",    m.mean_ns, m.p99_ns);
    row("sweep (1 order x 1k levels)",   s.mean_ns, s.p99_ns);
    std::cout << "+" << bar << "+\n\n";
}

int main() {
    std::cout << std::string(58, '=') << "\n"
              << "  LOB Micro-Benchmark  |  steady_clock  |  -O2 -march=native\n"
              << "  warmup=" << WARMUP << " iters   measure=" << ITERS << " iters\n"
              << std::string(58, '=') << "\n";

    auto ra = bench_add();
    auto rc = bench_cancel();
    auto rm = bench_match_1for1();
    auto rs = bench_match_sweep();

    print_summary(ra, rc, rm, rs);
}
