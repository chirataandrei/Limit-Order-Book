// main.cpp – integration tests + benchmarks for the full LOB stack
// Compile: g++ -std=c++20 -O2 -Wall -Wextra -o lob main.cpp

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <unordered_map>
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
        (void)book.add_order(1, 50,  100.0, Side::Sell);
        (void)book.add_order(2, 50,  101.0, Side::Sell);
        (void)book.add_order(3, 50,  102.0, Side::Sell);
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

        (void)book.add_order(10, 200, 50.0, Side::Buy);    // level with 200
        (void)book.add_order(11, 300, 50.0, Side::Buy);    // same level → 500 total
        (void)book.add_order(12, 100, 50.0, Side::Buy);
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

        (void)book.add_order(20, 40, 75.0, Side::Sell);   // order 20 is first (head)
        (void)book.add_order(21, 60, 75.0, Side::Sell);   // order 21 is second

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
        (void)book.add_order(30, 100, 80.0, Side::Buy);

        bool threw = false;
        try { (void)book.add_order(30, 50, 81.0, Side::Buy); }
        catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
        std::cout << "  duplicate id=30 rejected ✓\n\n";
    }

    // ── 8. Mid-price and spread ───────────────────────────────────────────────
    {
        std::cout << "── 8. Mid-price / spread ──\n";
        OrderBook book;
        (void)book.add_order(40, 10, 99.0,  Side::Buy);
        (void)book.add_order(41, 10, 101.0, Side::Sell);

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
            (void)book.add_order(ASK_BASE + i, 100, 200.0 + static_cast<double>(i), Side::Sell);

        constexpr std::size_t N = 100'000;

        // Benchmark: add N buy orders at price 100 (no match, they rest).
        auto t0 = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < N; ++i)
            (void)book.add_order(ADD_BASE + i, 10, 100.0, Side::Buy);
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
            (void)book.add_order(BID_BASE + i, 1, 150.0, Side::Buy);   // rest bids @ 150

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

    // ── 10. Tick grid validation ─────────────────────────────────────────────
    {
        std::cout << "── 10. Tick grid validation ──\n";
        OrderBook book(BookConfig{ .min_price = 10.0, .tick_size = 0.25, .num_ticks = 100 });

        auto rejects = [&](double px) {
            try { (void)book.add_order(1, 1, px, Side::Buy); }
            catch (const std::invalid_argument&) { return true; }
            return false;
        };
        assert(rejects(9.75));      // below grid
        assert(rejects(35.0));      // first price past the last tick (10 + 100*0.25)
        assert(rejects(10.1));      // off-tick
        assert(rejects(std::nan("")));
        assert(book.order_count() == 0 && book.pool_used() == 0);   // nothing leaked

        (void)book.add_order(1, 1, 10.0,  Side::Buy);    // first tick
        (void)book.add_order(2, 1, 34.75, Side::Sell);   // last tick
        assert(book.best_bid()->price() == 10.0);
        assert(book.best_ask()->price() == 34.75);
        std::cout << "  off-grid / out-of-range rejected, edges accepted ✓\n\n";
    }

    // ── 11. Best price tracks cancels and fills across bitmap words ──────────
    {
        std::cout << "── 11. Best-price tracking ──\n";
        // 1M ticks → 4-level bitmap; the ticks below straddle word and
        // super-word boundaries (64, 4096, 262144).
        OrderBook book(BookConfig{ .min_price = 0.0, .tick_size = 1.0, .num_ticks = 1'000'000 });
        const double px[] = { 3, 64, 4095, 4096, 262143, 262144, 999'999 };

        uint64_t id = 1;
        for (double p : px) (void)book.add_order(id++, 1, p, Side::Buy);   // ids 1..7
        for (double p : px) (void)book.add_order(id++, 1, p, Side::Buy);   // ids 8..14, same levels
        assert(book.bid_levels() == 7 && book.order_count() == 14);

        // Walk the best bid down by cancelling both orders at each level.
        for (int i = 6; i >= 0; --i) {
            assert(book.best_bid()->price() == px[i]);
            assert(book.cancel_order(1 + i));
            assert(book.best_bid()->price() == px[i]);          // one order left
            assert(book.cancel_order(8 + i));
        }
        assert(book.best_bid() == nullptr && book.bid_levels() == 0);

        // Asks: best is the lowest tick; a sweep must advance it level by level.
        id = 100;
        for (double p : px) (void)book.add_order(id++, 1, p, Side::Sell);
        assert(book.best_ask()->price() == 3);
        auto t = book.add_order(200, 3, 64.0, Side::Buy);       // takes 3 and 64, rests 1
        assert(t.size() == 2 && t[0].price == 3 && t[1].price == 64);
        assert(book.best_ask()->price() == 4095);
        assert(book.best_bid()->price() == 64 && book.best_bid()->get_total_volume() == 1);
        std::cout << "  best bid/ask correct across word boundaries ✓\n\n";
    }

    // ── 12. Randomised differential test vs. std::map reference ──────────────
    {
        std::cout << "── 12. Random add/cancel vs. reference ──\n";
        OrderBook book(BookConfig{ .min_price = 0.0, .tick_size = 1.0, .num_ticks = 20'000 });
        std::mt19937_64 rng(12345);

        // Bids live in [0, 9999], asks in [10000, 19999] so nothing ever crosses
        // and the reference model needs no matching logic.
        struct Live { double px; uint64_t qty; Side side; };
        std::map<double, uint64_t> ref_bid, ref_ask;     // price → resting qty
        std::map<uint64_t, Live>   live;
        uint64_t next_id = 1;

        for (int step = 0; step < 200'000; ++step) {
            if (live.empty() || rng() % 100 < 55) {
                const Side     side = (rng() & 1) ? Side::Buy : Side::Sell;
                const double   px   = static_cast<double>(rng() % 10'000) + (side == Side::Sell ? 10'000 : 0);
                const uint64_t q    = 1 + rng() % 50;
                assert(book.add_order(next_id, q, px, side).empty());
                (side == Side::Buy ? ref_bid : ref_ask)[px] += q;
                live[next_id++] = { px, q, side };
            } else {
                auto it = live.begin();
                std::advance(it, static_cast<std::ptrdiff_t>(rng() % live.size()));
                const Live l = it->second;
                assert(book.cancel_order(it->first));
                auto& ref = (l.side == Side::Buy) ? ref_bid : ref_ask;
                if ((ref[l.px] -= l.qty) == 0) ref.erase(l.px);
                live.erase(it);
            }

            assert(book.order_count() == live.size());
            assert(book.bid_levels()  == ref_bid.size());
            assert(book.ask_levels()  == ref_ask.size());
            if (ref_bid.empty()) assert(!book.best_bid());
            else {
                assert(book.best_bid()->price() == ref_bid.rbegin()->first);
                assert(book.best_bid()->get_total_volume() == ref_bid.rbegin()->second);
            }
            if (ref_ask.empty()) assert(!book.best_ask());
            else {
                assert(book.best_ask()->price() == ref_ask.begin()->first);
                assert(book.best_ask()->get_total_volume() == ref_ask.begin()->second);
            }
        }
        std::cout << "  200k random ops, book == reference at every step ✓\n\n";
    }

    // ── 13. Execution price is the resting order's price ─────────────────────
    {
        std::cout << "── 13. Aggressor / execution price ──\n";
        {   // buy aggressor lifts a resting ask: trades at the ask
            OrderBook book;
            (void)book.add_order(1, 10, 100.0, Side::Sell);
            auto t = book.add_order(2, 10, 105.0, Side::Buy);
            assert(t.size() == 1 && t[0].price == 100.0 && t[0].aggressor == Side::Buy);
            assert(t[0].bid_order_id == 2 && t[0].ask_order_id == 1);
        }
        {   // sell aggressor hits a resting bid: trades at the bid (was the bug)
            OrderBook book;
            (void)book.add_order(1, 10, 105.0, Side::Buy);
            auto t = book.add_order(2, 10, 100.0, Side::Sell);
            assert(t.size() == 1 && t[0].price == 105.0 && t[0].aggressor == Side::Sell);
            assert(t[0].bid_order_id == 1 && t[0].ask_order_id == 2);
        }
        {   // sell sweeping several bids: each fill at that bid's price
            OrderBook book;
            (void)book.add_order(1, 5, 103.0, Side::Buy);
            (void)book.add_order(2, 5, 102.0, Side::Buy);
            auto t = book.add_order(3, 10, 100.0, Side::Sell);
            assert(t.size() == 2 && t[0].price == 103.0 && t[1].price == 102.0);
        }
        std::cout << "  trades print at the maker's price for both aggressor sides ✓\n\n";
    }

    // ── 14. FlatIdMap vs. std::unordered_map ─────────────────────────────────
    {
        std::cout << "── 14. FlatIdMap differential test ──\n";
        // Tiny table (16 slots) and a small key space force long probe clusters
        // and plenty of backward-shift deletions that wrap around the end.
        constexpr int MAX_LIVE = 8;
        FlatIdMap map(MAX_LIVE);
        std::unordered_map<uint64_t, Order*> ref;
        std::vector<Order> store(64);
        std::mt19937_64 rng(777);

        for (int step = 0; step < 500'000; ++step) {
            const uint64_t key = rng() % 40 + ((rng() & 1) ? 0 : (uint64_t{1} << 63));
            Order* const   val = &store[rng() % store.size()];
            if (ref.count(key)) {
                if (rng() & 1) { assert(map.erase(key)); ref.erase(key); }
            } else if (ref.size() < MAX_LIVE) {
                map.insert(key, val);
                ref[key] = val;
            } else {
                assert(!map.erase(key));
            }
            assert(map.size() == ref.size());
            for (uint64_t k = 0; k < 40; ++k)
                for (uint64_t hi : {uint64_t{0}, uint64_t{1} << 63}) {
                    auto it = ref.find(k + hi);
                    assert(map.find(k + hi) == (it == ref.end() ? nullptr : it->second));
                }
        }
        std::cout << "  500k ops, every key agrees with the reference ✓\n\n";
    }

    std::cout << "All assertions passed.\n";
    return 0;
}
