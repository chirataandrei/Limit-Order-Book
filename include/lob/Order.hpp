#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lob {

// Forward-declare so Order can hold a back-pointer without a circular include.
class PriceLevel;

enum class Side : uint8_t { Buy, Sell };

// One cache line exactly. The layout is deliberate:
//   - hot fields (id, qty, price, side) up front so partial reads stay in one line
//   - link pointers at the back; they're only touched by list surgery, not matching logic
//   - `level` back-pointer lets cancel_order() skip a map lookup entirely
//
// If you add fields and blow past 64B, alignment padding will silently grow the struct.
// The static_assert below will catch it.
struct alignas(64) Order {
    uint64_t    id        = 0;
    uint64_t    timestamp = 0;   // ns since epoch, set at submission
    uint64_t    quantity  = 0;
    double      price     = 0.0;
    Side        side      = Side::Buy;

    Order*      prev      = nullptr;
    Order*      next      = nullptr;
    PriceLevel* level     = nullptr;  // which PriceLevel owns this order right now
};

static_assert(sizeof(Order)  == 64);
static_assert(alignof(Order) == 64);
static_assert(std::is_standard_layout_v<Order>);
// pool free-list stores a pointer in free slots, so T must be pointer-sized
static_assert(sizeof(Order) >= sizeof(void*));

[[nodiscard]] inline Order make_order(uint64_t id, uint64_t ts,
                                      uint64_t qty, double price, Side side) noexcept {
    return Order{ id, ts, qty, price, side, nullptr, nullptr, nullptr };
}

} // namespace lob
