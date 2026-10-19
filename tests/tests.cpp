// tests.cpp – integration tests for the full LOB stack
// Compile: g++ -std=c++20 -O2 -Wall -Wextra -I include -I include/lob -o lob_tests tests/tests.cpp

#include <cassert>
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

// OrderBook::add_order appends trades to a caller-owned buffer; the tests want
// a fresh vector per call, so they go through this wrapper.
static std::vector<Trade> add(OrderBook &book, uint64_t id, uint64_t qty, double px, Side side)
{
    std::vector<Trade> trades;
    book.add_order(id, qty, px, side, trades);
    return trades;
}

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
    if (bid) std::cout << book.price_of(*bid) << " vol=" << bid->get_total_volume();
    else     std::cout << "–";
    std::cout << "  ask=";
    if (ask) std::cout << book.price_of(*ask) << " vol=" << ask->get_total_volume();
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

        auto t1 = add(book, 1, 100, 100.0, Side::Sell);   // resting ask
        assert(t1.empty());                                      // no match yet
        print_top(book);

        auto t2 = add(book, 2, 100, 100.0, Side::Buy);    // aggressive buy
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
        auto t1 = add(book, 1, 30, 99.0, Side::Sell);
        assert(t1.empty());

        // Aggressive buy 100 @ 99 → fills 30, remaining 70 rests as bid
        auto t2 = add(book, 2, 100, 99.0, Side::Buy);
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
        (void)add(book, 1, 50,  100.0, Side::Sell);
        (void)add(book, 2, 50,  101.0, Side::Sell);
        (void)add(book, 3, 50,  102.0, Side::Sell);
        print_top(book);

        // Aggressive buy that sweeps all three levels
        auto trades = add(book, 4, 150, 102.0, Side::Buy);
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

        (void)add(book, 10, 200, 50.0, Side::Buy);    // level with 200
        (void)add(book, 11, 300, 50.0, Side::Buy);    // same level → 500 total
        (void)add(book, 12, 100, 50.0, Side::Buy);
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

        (void)add(book, 20, 40, 75.0, Side::Sell);   // order 20 is first (head)
        (void)add(book, 21, 60, 75.0, Side::Sell);   // order 21 is second

        auto trades = add(book, 22, 40, 75.0, Side::Buy);
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
        (void)add(book, 30, 100, 80.0, Side::Buy);

        bool threw = false;
        try { (void)add(book, 30, 50, 81.0, Side::Buy); }
        catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
        std::cout << "  duplicate id=30 rejected ✓\n\n";
    }

    // ── 8. Mid-price and spread ───────────────────────────────────────────────
    {
        std::cout << "── 8. Mid-price / spread ──\n";
        OrderBook book;
        (void)add(book, 40, 10, 99.0,  Side::Buy);
        (void)add(book, 41, 10, 101.0, Side::Sell);

        assert(book.spread()    == 2.0);
        assert(book.mid_price() == 100.0);
        std::cout << "  spread=" << book.spread()
                  << " mid="     << book.mid_price() << " ✓\n\n";
    }

    // ── 10. Tick grid validation ─────────────────────────────────────────────
    {
        std::cout << "── 10. Tick grid validation ──\n";
        OrderBook book(BookConfig{ .min_price = 10.0, .tick_size = 0.25, .num_ticks = 100 });

        auto rejects = [&](double px) {
            try { (void)add(book, 1, 1, px, Side::Buy); }
            catch (const std::invalid_argument&) { return true; }
            return false;
        };
        assert(rejects(9.75));      // below grid
        assert(rejects(35.0));      // first price past the last tick (10 + 100*0.25)
        assert(rejects(10.1));      // off-tick
        assert(rejects(std::nan("")));
        assert(book.order_count() == 0 && book.pool_used() == 0);   // nothing leaked

        (void)add(book, 1, 1, 10.0,  Side::Buy);    // first tick
        (void)add(book, 2, 1, 34.75, Side::Sell);   // last tick
        assert(book.price_of(*book.best_bid()) == 10.0);
        assert(book.price_of(*book.best_ask()) == 34.75);
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
        for (double p : px) (void)add(book, id++, 1, p, Side::Buy);   // ids 1..7
        for (double p : px) (void)add(book, id++, 1, p, Side::Buy);   // ids 8..14, same levels
        assert(book.bid_levels() == 7 && book.order_count() == 14);

        // Walk the best bid down by cancelling both orders at each level.
        for (int i = 6; i >= 0; --i) {
            assert(book.price_of(*book.best_bid()) == px[i]);
            assert(book.cancel_order(1 + i));
            assert(book.price_of(*book.best_bid()) == px[i]);          // one order left
            assert(book.cancel_order(8 + i));
        }
        assert(book.best_bid() == nullptr && book.bid_levels() == 0);

        // Asks: best is the lowest tick; a sweep must advance it level by level.
        id = 100;
        for (double p : px) (void)add(book, id++, 1, p, Side::Sell);
        assert(book.price_of(*book.best_ask()) == 3);
        auto t = add(book, 200, 3, 64.0, Side::Buy);       // takes 3 and 64, rests 1
        assert(t.size() == 2 && t[0].price == 3 && t[1].price == 64);
        assert(book.price_of(*book.best_ask()) == 4095);
        assert(book.price_of(*book.best_bid()) == 64 && book.best_bid()->get_total_volume() == 1);
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
                assert(add(book, next_id, q, px, side).empty());
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
                assert(book.price_of(*book.best_bid()) == ref_bid.rbegin()->first);
                assert(book.best_bid()->get_total_volume() == ref_bid.rbegin()->second);
            }
            if (ref_ask.empty()) assert(!book.best_ask());
            else {
                assert(book.price_of(*book.best_ask()) == ref_ask.begin()->first);
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
            (void)add(book, 1, 10, 100.0, Side::Sell);
            auto t = add(book, 2, 10, 105.0, Side::Buy);
            assert(t.size() == 1 && t[0].price == 100.0 && t[0].aggressor == Side::Buy);
            assert(t[0].bid_order_id == 2 && t[0].ask_order_id == 1);
        }
        {   // sell aggressor hits a resting bid: trades at the bid (was the bug)
            OrderBook book;
            (void)add(book, 1, 10, 105.0, Side::Buy);
            auto t = add(book, 2, 10, 100.0, Side::Sell);
            assert(t.size() == 1 && t[0].price == 105.0 && t[0].aggressor == Side::Sell);
            assert(t[0].bid_order_id == 1 && t[0].ask_order_id == 2);
        }
        {   // sell sweeping several bids: each fill at that bid's price
            OrderBook book;
            (void)add(book, 1, 5, 103.0, Side::Buy);
            (void)add(book, 2, 5, 102.0, Side::Buy);
            auto t = add(book, 3, 10, 100.0, Side::Sell);
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

    // ── 15. Caller-owned trade buffer ────────────────────────────────────────
    {
        std::cout << "── 15. Trade buffer overload ──\n";
        OrderBook book;
        std::vector<Trade> buf;
        (void)add(book, 1, 5, 100.0, Side::Sell);
        (void)add(book, 2, 5, 101.0, Side::Sell);

        book.add_order(3, 8, 101.0, Side::Buy, buf); // sweeps both levels
        assert(buf.size() == 2 && buf[0].price == 100.0 && buf[1].price == 101.0);
        assert(buf[0].quantity == 5 && buf[1].quantity == 3);

        const auto cap = buf.capacity();
        buf.clear();
        book.add_order(4, 2, 101.0, Side::Buy, buf); // matches the last 2 of order 2
        assert(buf.size() == 1 && buf[0].ask_order_id == 2 && buf[0].quantity == 2);
        assert(buf.capacity() == cap); // reused, not reallocated

        book.add_order(5, 1, 50.0, Side::Buy, buf); // rests: nothing is appended
        assert(buf.size() == 1);
        std::cout << "  appends to caller's vector, capacity reused ✓\n\n";
    }

    std::cout << "All assertions passed.\n";
    return 0;
}
