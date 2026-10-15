#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lob {

// Forward-declare so Order can hold a back-pointer without a circular include.
class PriceLevel;

enum class Side : uint8_t { Buy, Sell };

// Exactly 64 bytes (one L1 cache line) and 64-byte aligned.
// - Data fields (id, ts, qty, price, side) first: keeps matching logic
// - prev/next: only accessed during doubly-linked list insertion and removal.
// - level: enables O(1) cancellation by skipping hash map / tree lookups.
// Note: adding fields past 64B causes alignas(64) to silently double the size
// to 128B; the static_assert below prevents this at compile time.

struct alignas(64) Order {
    uint64_t    id        = 0;
    uint64_t    timestamp = 0;   // Unix time in nanoseconds, set on arrival
    uint64_t    quantity  = 0;
    double      price     = 0.0;
    Side        side      = Side::Buy;

    Order*      prev      = nullptr;
    Order*      next      = nullptr;
    PriceLevel* level     = nullptr;  // which PriceLevel owns this order right now
};

// Compile-time hardware & memory layout guarantees:
static_assert(sizeof(Order)  == 64);
static_assert(alignof(Order) == 64);
static_assert(std::is_standard_layout_v<Order>); // C-compatible layout (safe for memcpy & fixed offsets)


// Factory helper to construct an unlinked Order instance:
// - [[nodiscard]]: warns at compile time if the returned order is dropped.
// - inline + noexcept: zeroes function call and stack-unwinding overhead.
// - leaves prev, next, and level as nullptr until the order is placed into a PriceLevel.
[[nodiscard]] inline Order make_order(uint64_t id, uint64_t ts,
                                      uint64_t qty, double price, Side side) noexcept {
    return Order{ id, ts, qty, price, side, nullptr, nullptr, nullptr };
}

}
