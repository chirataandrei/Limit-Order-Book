#pragma once

// Minimal benchmark harness.
//
// Usage pattern:
//   run_benchmark<State>(name, ops_per_iter, warmup, iters,
//       setup_fn,    // () -> State,  called BEFORE the clock
//       bench_fn,    // (State&) -> void, the only timed region
//       teardown_fn  // (State&) -> void, called AFTER the clock
//   );
//
// A few things this doesn't do that a real harness (e.g. Google Benchmark) would:
//   - CPU frequency pinning / turbo-boost detection
//   - RDTSC instead of steady_clock (lower overhead, ~3 ns vs ~15 ns per call)
//   - Outlier rejection / geometric mean
// Good enough for sanity-checking at ~10-ns granularity.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace bench {

using Clock     = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;

inline double elapsed_ns(TimePoint t0, TimePoint t1) noexcept {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

// Prevent the compiler from optimizing away benchmark work.
template <typename T>
[[gnu::always_inline]] inline void do_not_optimize(T const& v) noexcept {
    asm volatile("" : : "r,m"(v) : "memory");
}

[[gnu::always_inline]] inline void compiler_fence() noexcept {
    asm volatile("" : : : "memory");
}

struct Stats {
    double mean_ns   = 0;
    double median_ns = 0;
    double p95_ns    = 0;
    double p99_ns    = 0;
    double min_ns    = 0;
    double max_ns    = 0;
    double stddev_ns = 0;
    uint64_t ops     = 0;
};

inline Stats compute_stats(std::vector<double> s, uint64_t ops) {
    assert(!s.empty());
    std::sort(s.begin(), s.end());
    const std::size_t n = s.size();

    Stats r;
    r.ops       = ops;
    r.min_ns    = s.front();
    r.max_ns    = s.back();
    r.median_ns = s[n / 2];
    r.p95_ns    = s[std::min(static_cast<std::size_t>(std::ceil(0.95 * n)) - 1, n - 1)];
    r.p99_ns    = s[std::min(static_cast<std::size_t>(std::ceil(0.99 * n)) - 1, n - 1)];

    double sum = 0; for (double v : s) sum += v;
    r.mean_ns = sum / n;

    double sq = 0; for (double v : s) { double d = v - r.mean_ns; sq += d * d; }
    r.stddev_ns = std::sqrt(sq / n);
    return r;
}

inline std::string commafy(uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, ",");
    return s;
}

inline void print_report(const char* name, int warmup, int iters, const Stats& s) {
    const std::string bar(56, '-');
    auto row = [](const char* label, double v) {
        std::cout << "  " << std::left << std::setw(10) << label
                  << std::right << std::setw(9) << std::fixed
                  << std::setprecision(2) << v << " ns/op\n";
    };

    std::cout << "\n+" << bar << "+\n";
    std::cout << "| " << std::left << std::setw(54) << name << " |\n";
    std::cout << "| ops: " << std::left << std::setw(12) << commafy(s.ops)
              << " warmup: " << warmup << "  iters: " << iters
              << std::setw(14) << "" << " |\n";
    std::cout << "+" << bar << "+\n";
    row("mean",   s.mean_ns);
    row("median", s.median_ns);
    row("p95",    s.p95_ns);
    row("p99",    s.p99_ns);
    row("min",    s.min_ns);
    row("max",    s.max_ns);
    row("stddev", s.stddev_ns);
    std::cout << "+" << bar << "+\n";
}

template <typename State, typename SetupFn, typename BenchFn, typename TeardownFn>
Stats run_benchmark(const char* name, uint64_t ops_per_iter,
                    int warmup_iters, int measure_iters,
                    SetupFn&& setup, BenchFn&& bench, TeardownFn&& teardown)
{
    std::cout << "[bench] " << name << "\n";
    std::cout << "        warming up..."; std::cout.flush();

    for (int i = 0; i < warmup_iters; ++i) {
        State st = setup();
        bench(st);
        teardown(st);
    }

    std::cout << " done\n        measuring..."; std::cout.flush();

    std::vector<double> samples;
    samples.reserve(measure_iters);
    const double opsd = static_cast<double>(ops_per_iter);

    for (int i = 0; i < measure_iters; ++i) {
        State st = setup();
        compiler_fence();
        auto t0 = Clock::now();
        bench(st);
        auto t1 = Clock::now();
        compiler_fence();
        teardown(st);
        samples.push_back(elapsed_ns(t0, t1) / opsd);
    }

    std::cout << " done\n";
    auto s = compute_stats(samples, ops_per_iter);
    print_report(name, warmup_iters, measure_iters, s);
    return s;
}

} // namespace bench
