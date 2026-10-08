#pragma once

#include "Order.hpp"

#include <cassert>
#include <cstdint>

namespace lob {

// Intrusive doubly-linked list of orders at the same price.
// head = oldest (highest matching priority), tail = newest.
//
// The list is null-terminated rather than sentinel-based because PriceLevel
// itself isn't pooled – adding a dummy Order node would be awkward.
//
// The `level` back-pointer on each Order is kept in sync here so that
// OrderBook::cancel_order() can call remove() without knowing which tick slot
// the order belongs to.
class PriceLevel {
public:
    // Default-constructible so OrderBook can allocate the whole tick array in
    // one shot; init() stamps the price/tick once. Levels never move after
    // that (the array is fixed-size), so Order::level back-pointers stay valid.
    PriceLevel() noexcept = default;

    PriceLevel(const PriceLevel&)            = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;
    PriceLevel(PriceLevel&&)                 = delete;
    PriceLevel& operator=(PriceLevel&&)      = delete;

    void init(double price, uint32_t tick) noexcept {
        assert(empty());
        price_ = price;
        tick_  = tick;
    }

    void push_back(Order* o) noexcept {
        assert(o && !o->prev && !o->next && !o->level);
        o->prev  = tail_;
        o->next  = nullptr;
        o->level = this;
        if (tail_) tail_->next = o;
        else       head_       = o;
        tail_ = o;
        ++size_;
        total_volume_ += o->quantity;
    }

    void push_front(Order* o) noexcept {
        assert(o && !o->prev && !o->next && !o->level);
        o->next  = head_;
        o->prev  = nullptr;
        o->level = this;
        if (head_) head_->prev = o;
        else       tail_       = o;
        head_ = o;
        ++size_;
        total_volume_ += o->quantity;
    }

    void remove(Order* o) noexcept {
        assert(o && size_ > 0 && o->level == this);
        if (o->prev) o->prev->next = o->next;
        else         head_         = o->next;
        if (o->next) o->next->prev = o->prev;
        else         tail_         = o->prev;
        o->prev = o->next = nullptr;
        o->level = nullptr;
        --size_;
        total_volume_ -= o->quantity;
    }

    // Partial fill – caller must call remove() when quantity hits zero.
    void fill(Order* o, uint64_t qty) noexcept {
        assert(o && qty <= o->quantity);
        o->quantity   -= qty;
        total_volume_ -= qty;
    }

    [[nodiscard]] uint64_t get_total_volume() const noexcept { return total_volume_; }
    [[nodiscard]] uint32_t size()             const noexcept { return size_;         }
    [[nodiscard]] bool     empty()            const noexcept { return size_ == 0;    }
    [[nodiscard]] double   price()            const noexcept { return price_;        }
    [[nodiscard]] uint32_t tick()             const noexcept { return tick_;         }
    [[nodiscard]] Order*   front()            const noexcept { return head_;         }
    [[nodiscard]] Order*   back()             const noexcept { return tail_;         }

    struct iterator {
        Order* cur;
        Order*    operator*()  const noexcept { return cur; }
        iterator& operator++() noexcept { cur = cur->next; return *this; }
        bool operator!=(const iterator& r) const noexcept { return cur != r.cur; }
    };
    [[nodiscard]] iterator begin() const noexcept { return {head_};   }
    [[nodiscard]] iterator end()   const noexcept { return {nullptr}; }

private:
    Order*   head_         = nullptr;
    Order*   tail_         = nullptr;
    uint32_t size_         = 0;
    uint32_t tick_         = 0;   // index into the owning side's level array
    uint64_t total_volume_ = 0;
    double   price_        = 0.0;
};

static_assert(sizeof(PriceLevel) <= 64);

} // namespace lob
