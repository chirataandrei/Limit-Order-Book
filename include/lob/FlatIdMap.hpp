#pragma once

// Open-addressing hash table: order id -> Order*.
//
// Replaces std::unordered_map, which costs one heap node per entry and a
// pointer chase per lookup. Here every entry is 16 bytes in one flat array:
// a probe is a hash, a multiply-shift, and a short linear scan.
//
//   - Capacity is fixed at construction (power of two) and never grows, so
//     there is no rehash on the hot path. The caller sizes it so the load
//     factor stays <= 0.5; OrderBook gets that for free because the order
//     pool already caps live orders.
//   - Linear probing with backward-shift deletion: no tombstones, so erase
//     leaves the table exactly as if the key had never been inserted and
//     lookups never degrade with churn.
//   - An empty slot is val == nullptr. Order ids can use the full uint64 range.

#include "Order.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lob {

class FlatIdMap {
public:
    // `max_entries` is the most that will ever be live at once; the table is
    // sized to at least twice that.
    explicit FlatIdMap(std::size_t max_entries)
        : bits_(static_cast<unsigned>(std::bit_width(max_entries * 2 - 1))),
          mask_((std::size_t{1} << bits_) - 1),
          slots_(std::make_unique<Slot[]>(mask_ + 1)) {}

    [[nodiscard]] Order* find(uint64_t id) const noexcept {
        for (std::size_t i = home_(id);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (!s.val)       return nullptr;
            if (s.key == id)  return s.val;
        }
    }

    [[nodiscard]] bool contains(uint64_t id) const noexcept { return find(id) != nullptr; }

    // Caller guarantees `id` is not present and `o` is non-null.
    void insert(uint64_t id, Order* o) noexcept {
        std::size_t i = home_(id);
        while (slots_[i].val) i = (i + 1) & mask_;
        slots_[i] = { id, o };
        ++size_;
    }

    bool erase(uint64_t id) noexcept {
        std::size_t i = home_(id);
        for (;; i = (i + 1) & mask_) {
            if (!slots_[i].val)      return false;
            if (slots_[i].key == id) break;
        }

        // Backward shift: walk the cluster after the hole and pull back any
        // entry whose home slot is not strictly inside (hole, j].
        std::size_t hole = i;
        for (std::size_t j = (i + 1) & mask_; slots_[j].val; j = (j + 1) & mask_) {
            const std::size_t h = home_(slots_[j].key);
            if (((j - h) & mask_) >= ((j - hole) & mask_)) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole] = {};
        --size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    struct Slot {
        uint64_t key = 0;
        Order*   val = nullptr;
    };

    // Fibonacci hashing: multiply by 2^64/phi and keep the top bits. Robust for
    // any id pattern. An identity hash (id & mask) was tried: it keeps
    // sequential ids adjacent, but two id ranges that overlap modulo the table
    // size build giant probe clusters (the benchmark hit it: minutes per run).
    [[nodiscard]] std::size_t home_(uint64_t id) const noexcept {
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> (64 - bits_));
    }

    unsigned               bits_;
    std::size_t            mask_;
    std::unique_ptr<Slot[]> slots_;
    std::size_t            size_ = 0;
};

} // namespace lob
