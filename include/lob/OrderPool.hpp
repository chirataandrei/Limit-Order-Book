#pragma once

#include "Order.hpp"

#include <cassert>
#include <cstddef>
#include <new>
#include <stdexcept>

namespace lob
{

	// Pre-allocated, zero-heap runtime pool using an intrusive LIFO free-list.
	class OrderPool
	{

	private:
		// Overlaid in-place onto inactive slots to avoid per-slot metadata overhead.
		struct FreeNode
		{
			FreeNode *next;
		};

		std::byte *storage_ = nullptr;
		FreeNode *free_head_ = nullptr;
		std::size_t allocated_ = 0;
		std::size_t capacity_;

		// Range and slot-boundary validation; used exclusively in assertions.
		[[nodiscard]] bool owns(const Order *ptr) const noexcept
		{
			const auto *raw = reinterpret_cast<const std::byte *>(ptr);
			return raw >= storage_ && raw < storage_ + sizeof(Order) * capacity_ && static_cast<std::size_t>(raw - storage_) % sizeof(Order) == 0;
		}

	public:
		explicit OrderPool(std::size_t capacity)
			: capacity_(capacity)
		{
			if (capacity == 0)
				throw std::invalid_argument("pool capacity must be > 0");

			// Over-aligned allocation to preserve Order's 64-byte cache line alignment.
			storage_ = static_cast<std::byte *>(
				::operator new(sizeof(Order) * capacity, std::align_val_t{alignof(Order)}));

			// Link in reverse so slot[0] is the first allocated (cache prefetcher friendly).
			for (std::size_t i = capacity; i-- > 0;)
			{
				auto *node = reinterpret_cast<FreeNode *>(storage_ + i * sizeof(Order));
				node->next = free_head_;
				free_head_ = node;
			}
		}

		OrderPool(const OrderPool &) = delete;
		OrderPool &operator=(const OrderPool &) = delete;

		~OrderPool()
		{
			::operator delete(storage_, std::align_val_t{alignof(Order)});
		}

		// Returns nullptr on exhaustion to avoid exception handling on the critical path.
		[[nodiscard]] Order *allocate() noexcept
		{
			if (!free_head_) [[unlikely]]
				return nullptr;

			FreeNode *slot = free_head_;
			free_head_ = slot->next;
			++allocated_;
			return ::new (static_cast<void *>(slot)) Order();
		}

		// Explicit destructor call omitted: Order is trivially destructible (standard layout).
		void deallocate(Order *ptr) noexcept
		{
			if (!ptr) [[unlikely]]
				return;
			assert(owns(ptr));

			auto *node = reinterpret_cast<FreeNode *>(ptr);
			node->next = free_head_;
			free_head_ = node;
			--allocated_;
		}

		[[nodiscard]] std::size_t allocated() const noexcept { return allocated_; }
	};

}