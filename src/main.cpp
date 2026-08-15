// main.cpp – integration tests + benchmarks for the full LOB stack
// Compile: g++ -std=c++20 -O2 -Wall -Wextra -o lob main.cpp

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

#include "OrderBook.hpp"

using namespace lob;

// ── helpers ──────────────────────────────────────────────────────────────────
static void print_trade(const Trade& t) {
    std::cout << "  TRADE  bid=" << t.bid_order_id
              << " ask="         << t.ask_order_id
              << " qty="         << t.quantity
              << " px="          << std::fixed << std::setprecision(2) << t.price
              << "\n";
}

static void print_top(const OrderBook& book) {
    auto* bid = book.best_bid();
    auto* ask = book.best_ask();
    std::cout << "  TOP  bid=";
    if (bid) std::cout << bid->price() << " vol=" << bid->get_total_volume();
    else     std::cout << "–";
    std::cout << "  ask=";
    if (ask) std::cout << ask->price() << " vol=" << ask->get_total_volume();
    else     std::cout << "–";
    std::cout << "  spread=" << book.spread()
              << "  mid="    << book.mid_price()
              << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    // ── 1. Full fill – one bid matches one ask completely ─────────────────────
    {
        std::cout << "── 1. Full fill ──\n";
        OrderBook book;

        auto t1 = book.add_order(1, 100, 100.0, Side::Sell);   // resting ask
        assert(t1.empty());                                      // no match yet
        print_top(book);

        auto t2 = book.add_order(2, 100, 100.0, Side::Buy);    // aggressive buy
        assert(t2.size() == 1);
        assert(t2[0].quantity == 100 && t2[0].price == 100.0);
        for (auto& t : t2) print_trade(t);

        assert(book.order_count() == 0);  // both orders consumed
        assert(book.bid_levels()  == 0);
        assert(book.ask_levels()  == 0);
        std::cout << "  orders_remaining=" << book.order_count() << " ✓\n\n";
    }

    // ── 2. Partial fill – bid qty > ask qty ──────────────────────────────────
    {
        std::cout << "── 2. Partial fill ──\n";
        OrderBook book;

        // Resting ask: 30 @ 99
        auto t1 = book.add_order(1, 30, 99.0, Side::Sell);
        assert(t1.empty());

        // Aggressive buy 100 @ 99 → fills 30, remaining 70 rests as bid
        auto t2 = book.add_order(2, 100, 99.0, Side::Buy);
        assert(t2.size()          == 1);
        assert(t2[0].quantity     == 30);
        assert(t2[0].ask_order_id == 1);
        assert(t2[0].bid_order_id == 2);
        for (auto& t : t2) print_trade(t);

        assert(book.ask_levels() == 0);          // ask fully consumed
        assert(book.bid_levels() == 1);          // bid level @ 99 with 70 remaining
        assert(book.best_bid()->get_total_volume() == 70);
        print_top(book);
        std::cout << "  remaining_bid_vol=" << book.best_bid()->get_total_volume() << " ✓\n\n";
    }

    // ── 3. Multi-level sweep – buy sweeps through three ask levels ────────────
    {
        std::cout << "── 3. Multi-level sweep ──\n";
        OrderBook book;

        // Resting asks at three levels
        book.add_order(1, 50,  100.0, Side::Sell);
        book.add_order(2, 50,  101.0, Side::Sell);
        book.add_order(3, 50,  102.0, Side::Sell);
        print_top(book);

        // Aggressive buy that sweeps all three levels
        auto trades = book.add_order(4, 150, 102.0, Side::Buy);
        assert(trades.size() == 3);
        assert(trades[0].price == 100.0);   // best ask first
        assert(trades[1].price == 101.0);
        assert(trades[2].price == 102.0);
        for (auto& t : trades) print_trade(t);

        assert(book.order_count() == 0);
        assert(book.ask_levels()  == 0);
        assert(book.bid_levels()  == 0);
        std::cout << "  swept 3 levels ✓\n\n";
    }

    // ── 4. Cancel order ───────────────────────────────────────────────────────
    {
        std::cout << "── 4. Cancel ──\n";
        OrderBook book;

        book.add_order(10, 200, 50.0, Side::Buy);    // level with 200
        book.add_order(11, 300, 50.0, Side::Buy);    // same level → 500 total
        book.add_order(12, 100, 50.0, Side::Buy);
        print_top(book);

        assert(book.cancel_order(11));               // remove middle order
        assert(book.order_count() == 2);
        assert(book.best_bid()->get_total_volume() == 300);  // 200 + 100
        std::cout << "  after cancel(11): vol=" << book.best_bid()->get_total_volume() << " ✓\n";

        assert(book.cancel_order(10));
        assert(book.cancel_order(12));
        assert(book.order_count() == 0);
        assert(book.bid_levels()  == 0);   // empty level pruned from map
        std::cout << "  all cancelled, bid_levels=" << book.bid_levels() << " ✓\n\n";
    }

    // ── 5. Cancel non-existent order returns false ────────────────────────────
    {
        std::cout << "── 5. Cancel non-existent ──\n";
        OrderBook book;
        assert(!book.cancel_order(999));
        std::cout << "  cancel_order(unknown)=false ✓\n\n";
    }

    // ── 6. FIFO matching – oldest order at a level fills first ───────────────
    {
        std::cout << "── 6. FIFO priority ──\n";
        OrderBook book;

        book.add_order(20, 40, 75.0, Side::Sell);   // order 20 is first (head)
        book.add_order(21, 60, 75.0, Side::Sell);   // order 21 is second

        auto trades = book.add_order(22, 40, 75.0, Side::Buy);
        // Should fill against order 20 first (FIFO)
        assert(trades.size() == 1);
        assert(trades[0].ask_order_id == 20);
        assert(trades[0].quantity     == 40);
        for (auto& t : trades) print_trade(t);

        // Order 21 should still be resting
        assert(book.ask_levels() == 1);
        assert(book.best_ask()->get_total_volume() == 60);
        assert(book.best_ask()->front()->id == 21);
        std::cout << "  FIFO: order 20 matched first, 21 still resting ✓\n\n";
    }

    // ── 7. Duplicate order ID rejected ───────────────────────────────────────
    {
        std::cout << "── 7. Duplicate ID rejection ──\n";
        OrderBook book;
        book.add_order(30, 100, 80.0, Side::Buy);

        bool threw = false;
        try { book.add_order(30, 50, 81.0, Side::Buy); }
        catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
        std::cout << "  duplicate id=30 rejected ✓\n\n";
    }

    // ── 8. Mid-price and spread ───────────────────────────────────────────────
    {
        std::cout << "── 8. Mid-price / spread ──\n";
        OrderBook book;
        book.add_order(40, 10, 99.0,  Side::Buy);
        book.add_order(41, 10, 101.0, Side::Sell);

        assert(book.spread()    == 2.0);
        assert(book.mid_price() == 100.0);
        std::cout << "  spread=" << book.spread()
                  << " mid="     << book.mid_price() << " ✓\n\n";
    }

    // ── 9. Throughput benchmark ───────────────────────────────────────────────
    {
        std::cout << "── 9. Throughput benchmark ──\n";
        OrderBook book;

        // ID space layout (no overlaps):
        //   [1 .. 100]          – 100 resting ask levels
        //   [100001 .. 200000]  – 100k bids for add/cancel bench
        //   [200001 .. 300000]  – 100k bids resting for match bench
        //   [300001 .. 400000]  – 100k aggressive sells for match bench
        constexpr uint64_t   ASK_BASE    =      1;
        constexpr uint64_t   ADD_BASE    = 100'001;
        constexpr uint64_t   BID_BASE    = 200'001;
        constexpr uint64_t   SELL_BASE   = 300'001;

        // Pre-populate 100 ask levels so the buy orders below don't cross.
        for (uint64_t i = 0; i < 100; ++i)
            book.add_order(ASK_BASE + i, 100, 200.0 + static_cast<double>(i), Side::Sell);

        constexpr std::size_t N = 100'000;

        // Benchmark: add N buy orders at price 100 (no match, they rest).
        auto t0 = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < N; ++i)
            book.add_order(ADD_BASE + i, 10, 100.0, Side::Buy);
        auto t1 = std::chrono::steady_clock::now();

        auto add_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        std::cout << "  add_order (no match): " << add_ns / N << " ns/op\n";

        // Benchmark: cancel all N bid orders.
        auto t2 = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < N; ++i)
            book.cancel_order(ADD_BASE + i);
        auto t3 = std::chrono::steady_clock::now();

        auto cancel_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
        std::cout << "  cancel_order:         " << cancel_ns / N << " ns/op\n";

        // Benchmark: matching sweep – 100k sells cross 100k pre-placed buys.
        for (uint64_t i = 0; i < N; ++i)
            book.add_order(BID_BASE + i, 1, 150.0, Side::Buy);   // rest bids @ 150

        std::size_t fill_count = 0;
        auto t4 = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < N; ++i) {
            auto trades = book.add_order(SELL_BASE + i, 1, 150.0, Side::Sell); // cross @ 150
            fill_count += trades.size();
        }
        auto t5 = std::chrono::steady_clock::now();

        auto match_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t5 - t4).count();
        std::cout << "  add+match (1 fill/op): " << match_ns / N << " ns/op"
                  << "  (total fills=" << fill_count << ")\n\n";

        assert(fill_count == N);
    }

    std::cout << "All assertions passed.\n";
    return 0;
}
