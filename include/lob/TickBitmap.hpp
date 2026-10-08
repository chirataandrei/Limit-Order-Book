#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

// Hierarchical occupancy bitmap over [0, n). Level 0 has one bit per tick;
// every higher level has one bit per 64-bit word of the level below ("any bit
// set in there?"), up to a single root word. lowest()/highest() descend from
// the root in one ctz/clz per level – 4 steps for 1M ticks, 6 for 2^36 –
// so finding the next best price after a level empties is O(log64 n), not O(n).
class TickBitmap {
public:
    explicit TickBitmap(std::size_t n) {
        assert(n > 0);
        std::size_t bits = n;
        do {
            const std::size_t words = (bits + 63) / 64;
            levels_.emplace_back(words, 0ULL);
            bits = words;
        } while (bits > 1);
    }

    void set(std::size_t i) noexcept {
        for (auto& lvl : levels_) {
            lvl[i >> 6] |= 1ULL << (i & 63);
            i >>= 6;
        }
    }

    void clear(std::size_t i) noexcept {
        for (auto& lvl : levels_) {
            lvl[i >> 6] &= ~(1ULL << (i & 63));
            if (lvl[i >> 6]) break;   // word still non-empty → parents stay set
            i >>= 6;
        }
    }

    [[nodiscard]] bool any() const noexcept { return levels_.back()[0] != 0; }

    // Both require any().
    [[nodiscard]] std::size_t lowest() const noexcept {
        assert(any());
        std::size_t i = 0;
        for (std::size_t l = levels_.size(); l-- > 0; )
            i = (i << 6) | static_cast<std::size_t>(std::countr_zero(levels_[l][i]));
        return i;
    }

    [[nodiscard]] std::size_t highest() const noexcept {
        assert(any());
        std::size_t i = 0;
        for (std::size_t l = levels_.size(); l-- > 0; )
            i = (i << 6) | static_cast<std::size_t>(63 - std::countl_zero(levels_[l][i]));
        return i;
    }

private:
    std::vector<std::vector<uint64_t>> levels_;   // levels_[0] = finest
};

} // namespace lob
