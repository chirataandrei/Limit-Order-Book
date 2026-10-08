#pragma once

#include "ObjectPool.hpp"
#include "FlatIdMap.hpp"
#include "Order.hpp"
#include "PriceLevel.hpp"
#include "TickBitmap.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace lob {

struct Trade {
    uint64_t bid_order_id;
    uint64_t ask_order_id;
    uint64_t quantity;
    double   price;      // execution at the resting (maker) side's price
    uint64_t timestamp;
    Side     aggressor;  // side of the incoming order that triggered the trade
};

// Price grid: level i sits at min_price + i * tick_size. Orders must land
// exactly on a tick inside [0, num_ticks); anything else is rejected.
struct BookConfig {
    double   min_price = 0.0;
    double   tick_size = 0.01;
    uint32_t num_ticks = 100'000;   // default grid: 0.00 – 999.99
};

class OrderBook {
public:
    // 1M orders in the pool = 64 MB. Enough headroom for most instruments.
    // TODO: make this a template parameter once we benchmark the tick-array replacement.
    static constexpr std::size_t POOL_CAPACITY = 1'000'000;

    explicit OrderBook(BookConfig cfg = {})
        : min_price_(cfg.min_price), tick_size_(cfg.tick_size), num_ticks_(cfg.num_ticks),
          order_map_(POOL_CAPACITY), bids_(cfg), asks_(cfg)
    {
        if (!(cfg.tick_size > 0.0) || cfg.num_ticks == 0 || !std::isfinite(cfg.min_price))
            throw std::invalid_argument("invalid BookConfig");
    }

    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&)                 = delete;
    OrderBook& operator=(OrderBook&&)      = delete;

    // Insert a limit order and attempt immediate matching.
    // Returns whatever trades were generated (may be empty if the order rests).
    [[nodiscard]]
    std::vector<Trade> add_order(uint64_t id, uint64_t quantity,
                                 double price, Side side, uint64_t timestamp = 0)
    {
        if (quantity == 0) [[unlikely]]
            throw std::invalid_argument("quantity must be > 0");
        const uint32_t tick = tick_of_(price);   // throws on off-grid / out-of-range
        if (order_map_.contains(id)) [[unlikely]]
            throw std::invalid_argument("duplicate order id");

        if (!timestamp) timestamp = now_ns();

        Order* o = pool_.allocate();
        if (!o) [[unlikely]] throw std::bad_alloc{};
        *o = make_order(id, timestamp, quantity, price, side);

        LevelArray& s = (side == Side::Buy) ? bids_ : asks_;
        PriceLevel& lvl = s.levels[tick];
        if (lvl.empty()) occupy_(s, tick, side == Side::Buy);
        lvl.push_back(o);

        order_map_.insert(id, o);

        std::vector<Trade> trades;
        match_(trades, side);
        return trades;
    }

    // O(1) cancel via the order_map lookup + intrusive list remove. If the
    // level goes empty, the bitmap update is O(log64 P) – at most one extra
    // descent when it was the best level.
    bool cancel_order(uint64_t id) noexcept {
        Order* o = order_map_.find(id);
        if (!o) return false;

        PriceLevel* lvl = o->level;
        assert(lvl);

        lvl->remove(o);
        order_map_.erase(id);

        if (lvl->empty()) {
            if (o->side == Side::Buy) release_(bids_, lvl->tick(), true);
            else                      release_(asks_, lvl->tick(), false);
        }

        pool_.deallocate(o);
        return true;
    }

    [[nodiscard]] const PriceLevel* best_bid() const noexcept {
        return bids_.count ? &bids_.levels[bids_.best] : nullptr;
    }
    [[nodiscard]] const PriceLevel* best_ask() const noexcept {
        return asks_.count ? &asks_.levels[asks_.best] : nullptr;
    }
    [[nodiscard]] double mid_price() const noexcept {
        if (!bids_.count || !asks_.count) return 0.0;
        return (bids_.levels[bids_.best].price() + asks_.levels[asks_.best].price()) * 0.5;
    }
    [[nodiscard]] double spread() const noexcept {
        if (!bids_.count || !asks_.count) return 0.0;
        return asks_.levels[asks_.best].price() - bids_.levels[bids_.best].price();
    }

    [[nodiscard]] std::size_t bid_levels()  const noexcept { return bids_.count;       }
    [[nodiscard]] std::size_t ask_levels()  const noexcept { return asks_.count;       }
    [[nodiscard]] std::size_t order_count() const noexcept { return order_map_.size(); }
    [[nodiscard]] std::size_t pool_used()   const noexcept { return pool_.allocated(); }

private:
    static constexpr uint32_t NO_TICK = std::numeric_limits<uint32_t>::max();

    // One side of the book: a dense array of levels indexed by tick, plus a
    // bitmap of which ticks are non-empty and a cached best tick
    // (highest for bids, lowest for asks).
    struct LevelArray {
        std::unique_ptr<PriceLevel[]> levels;
        TickBitmap                    occupied;
        std::size_t                   count = 0;
        uint32_t                      best  = NO_TICK;

        explicit LevelArray(const BookConfig& cfg)
            : levels(std::make_unique<PriceLevel[]>(cfg.num_ticks)),
              occupied(cfg.num_ticks)
        {
            for (uint32_t i = 0; i < cfg.num_ticks; ++i)
                levels[i].init(cfg.min_price + static_cast<double>(i) * cfg.tick_size, i);
        }
    };

    double   min_price_;
    double   tick_size_;
    uint32_t num_ticks_;

    ObjectPool<Order, POOL_CAPACITY> pool_;
    FlatIdMap                        order_map_;   // declared before bids_/asks_: matches ctor init order
    LevelArray                       bids_;
    LevelArray                       asks_;

    [[nodiscard]] uint32_t tick_of_(double price) const {
        const double raw = (price - min_price_) / tick_size_;
        const double r   = std::nearbyint(raw);
        // `!(…)` form also rejects NaN.
        if (!(r >= 0.0 && r < static_cast<double>(num_ticks_)) || std::fabs(raw - r) > 1e-6)
            [[unlikely]]
            throw std::invalid_argument("price outside tick grid");
        return static_cast<uint32_t>(r);
    }

    // Level just went empty → non-empty.
    static void occupy_(LevelArray& s, uint32_t tick, bool is_bid) noexcept {
        s.occupied.set(tick);
        ++s.count;
        if (s.best == NO_TICK || (is_bid ? tick > s.best : tick < s.best))
            s.best = tick;
    }

    // Level just went non-empty → empty. Re-derive best only if it was the best.
    static void release_(LevelArray& s, uint32_t tick, bool is_bid) noexcept {
        s.occupied.clear(tick);
        --s.count;
        if (s.count == 0)
            s.best = NO_TICK;
        else if (tick == s.best)
            s.best = static_cast<uint32_t>(is_bid ? s.occupied.highest()
                                                  : s.occupied.lowest());
    }

    void match_(std::vector<Trade>& out, Side aggressor) {
        while (bids_.count && asks_.count) {
            const uint32_t bt = bids_.best;
            const uint32_t at = asks_.best;

            if (bt < at) break;  // no cross

            PriceLevel& blvl = bids_.levels[bt];
            PriceLevel& alvl = asks_.levels[at];
            Order*      bo   = blvl.front();
            Order*      ao   = alvl.front();

            const uint64_t qty = std::min(bo->quantity, ao->quantity);

            // Execution price = the resting (maker) order's price. add_order()
            // matches after every insert, so the book is never crossed on
            // entry and the aggressor is always the order just added.
            const double px = (aggressor == Side::Buy) ? alvl.price() : blvl.price();
            out.push_back({ bo->id, ao->id, qty, px, now_ns(), aggressor });

            if (bo->quantity == qty) {
                blvl.remove(bo);
                order_map_.erase(bo->id);
                pool_.deallocate(bo);
                if (blvl.empty()) release_(bids_, bt, true);
            } else {
                blvl.fill(bo, qty);
            }

            if (ao->quantity == qty) {
                alvl.remove(ao);
                order_map_.erase(ao->id);
                pool_.deallocate(ao);
                if (alvl.empty()) release_(asks_, at, false);
            } else {
                alvl.fill(ao, qty);
            }
        }
    }

    static uint64_t now_ns() noexcept {
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }
};

} // namespace lob
