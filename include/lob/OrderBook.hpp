#pragma once

#include "ObjectPool.hpp"
#include "Order.hpp"
#include "PriceLevel.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace lob {

struct Trade {
    uint64_t bid_order_id;
    uint64_t ask_order_id;
    uint64_t quantity;
    double   price;      // execution at the resting (maker) side's price
    uint64_t timestamp;
};

class OrderBook {
public:
    // 1M orders in the pool = 64 MB. Enough headroom for most instruments.
    // TODO: make this a template parameter once we benchmark the tick-array replacement.
    static constexpr std::size_t POOL_CAPACITY = 1'000'000;

    OrderBook() { order_map_.reserve(POOL_CAPACITY); }

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
        if (order_map_.contains(id)) [[unlikely]]
            throw std::invalid_argument("duplicate order id");

        if (!timestamp) timestamp = now_ns();

        Order* o = pool_.allocate();
        if (!o) [[unlikely]] throw std::bad_alloc{};
        *o = make_order(id, timestamp, quantity, price, side);

        if (side == Side::Buy)
            bids_.try_emplace(price, price).first->second.push_back(o);
        else
            asks_.try_emplace(price, price).first->second.push_back(o);

        order_map_.emplace(id, o);

        std::vector<Trade> trades;
        match_(trades);
        return trades;
    }

    // O(1) cancel via the order_map lookup + intrusive list remove.
    // The map erase (if the level goes empty) is O(log P) but unavoidable.
    bool cancel_order(uint64_t id) noexcept {
        auto it = order_map_.find(id);
        if (it == order_map_.end()) return false;

        Order*      o   = it->second;
        PriceLevel* lvl = o->level;
        assert(lvl);

        lvl->remove(o);
        order_map_.erase(it);

        if (lvl->empty()) {
            if (o->side == Side::Buy) bids_.erase(lvl->price());
            else                      asks_.erase(lvl->price());
        }

        pool_.deallocate(o);
        return true;
    }

    [[nodiscard]] const PriceLevel* best_bid() const noexcept {
        return bids_.empty() ? nullptr : &bids_.begin()->second;
    }
    [[nodiscard]] const PriceLevel* best_ask() const noexcept {
        return asks_.empty() ? nullptr : &asks_.begin()->second;
    }
    [[nodiscard]] double mid_price() const noexcept {
        if (bids_.empty() || asks_.empty()) return 0.0;
        return (bids_.begin()->first + asks_.begin()->first) * 0.5;
    }
    [[nodiscard]] double spread() const noexcept {
        if (bids_.empty() || asks_.empty()) return 0.0;
        return asks_.begin()->first - bids_.begin()->first;
    }

    [[nodiscard]] std::size_t bid_levels()  const noexcept { return bids_.size();      }
    [[nodiscard]] std::size_t ask_levels()  const noexcept { return asks_.size();      }
    [[nodiscard]] std::size_t order_count() const noexcept { return order_map_.size(); }
    [[nodiscard]] std::size_t pool_used()   const noexcept { return pool_.allocated(); }

private:
    // Bids sorted high→low, asks low→high, so begin() is always best price.
    using BidMap = std::map<double, PriceLevel, std::greater<double>>;
    using AskMap = std::map<double, PriceLevel, std::less<double>>;

    ObjectPool<Order, POOL_CAPACITY>     pool_;
    BidMap                               bids_;
    AskMap                               asks_;
    std::unordered_map<uint64_t, Order*> order_map_;

    void match_(std::vector<Trade>& out) {
        while (!bids_.empty() && !asks_.empty()) {
            auto  bid_it = bids_.begin();
            auto  ask_it = asks_.begin();

            if (bid_it->first < ask_it->first) break;  // no cross

            PriceLevel& blvl = bid_it->second;
            PriceLevel& alvl = ask_it->second;
            Order*      bo   = blvl.front();
            Order*      ao   = alvl.front();

            const uint64_t qty = std::min(bo->quantity, ao->quantity);

            // Execution price = resting ask (maker side).
            // If the sell was the aggressor, we'd want bid price instead –
            // for now this is fine since we don't track aggressor vs. resting.
            out.push_back({ bo->id, ao->id, qty, ask_it->first, now_ns() });

            if (bo->quantity == qty) {
                blvl.remove(bo);
                order_map_.erase(bo->id);
                pool_.deallocate(bo);
                if (blvl.empty()) bids_.erase(bid_it);
            } else {
                blvl.fill(bo, qty);
            }

            if (ao->quantity == qty) {
                alvl.remove(ao);
                order_map_.erase(ao->id);
                pool_.deallocate(ao);
                if (alvl.empty()) asks_.erase(ask_it);
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
