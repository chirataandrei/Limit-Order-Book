#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lob
{

	// Forward-declare so Order can hold a back-pointer without a circular include.
	class PriceLevel;

	enum class Side : uint8_t
	{
		Buy,
		Sell
	};

	// Sized and aligned to exactly one cache line (64 bytes) to prevent false sharing
	// and avoid crossing cache-line boundaries during matching.
	struct alignas(64) Order
	{
		uint64_t id = 0;
		uint64_t timestamp = 0; // Monotonic nanoseconds
		uint64_t quantity = 0;
		double price = 0.0;
		Side side = Side::Buy;

		// Intrusive list links within the owning PriceLevel
		Order *prev = nullptr;
		Order *next = nullptr;
		PriceLevel *level = nullptr;
	};

	static_assert(sizeof(Order) == 64);
	static_assert(alignof(Order) == 64);
	static_assert(std::is_standard_layout_v<Order>);

	[[nodiscard]] inline Order make_order(uint64_t id, uint64_t ts,
										  uint64_t qty, double price, Side side) noexcept
	{
		return Order{id, ts, qty, price, side, nullptr, nullptr, nullptr};
	}

}
