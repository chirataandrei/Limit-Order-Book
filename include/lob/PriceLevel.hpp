#pragma once

#include "Order.hpp"

#include <cassert>
#include <cstdint>

namespace lob
{

	// Intrusive doubly-linked FIFO queue of orders at a single price level.

	class PriceLevel
	{

	private:
		Order *head_ = nullptr;
		Order *tail_ = nullptr;
		uint32_t size_ = 0;
		uint32_t tick_ = 0; // index into the owning side's level array
		uint64_t total_volume_ = 0;

	public:
		PriceLevel() noexcept = default;

		PriceLevel(const PriceLevel &) = delete;
		PriceLevel &operator=(const PriceLevel &) = delete;
		PriceLevel(PriceLevel &&) = delete;
		PriceLevel &operator=(PriceLevel &&) = delete;

		void init(uint32_t tick) noexcept
		{
			assert(empty());
			tick_ = tick;
		}

		void push_back(Order *o) noexcept
		{
			assert(o && !o->prev && !o->next && !o->level);
			o->prev = tail_;
			o->next = nullptr;
			o->level = this;
			if (tail_)
				tail_->next = o;
			else
				head_ = o;
			tail_ = o;
			++size_;
			total_volume_ += o->quantity;
		}

		void remove(Order *o) noexcept
		{
			assert(o && size_ > 0 && o->level == this);
			if (o->prev)
				o->prev->next = o->next;
			else
				head_ = o->next;
			if (o->next)
				o->next->prev = o->prev;
			else
				tail_ = o->prev;
			o->prev = o->next = nullptr;
			o->level = nullptr;
			--size_;
			total_volume_ -= o->quantity;
		}

		// Caller must invoke remove(o) if remaining quantity reaches zero.
		void fill(Order *o, uint64_t qty) noexcept
		{
			assert(o && qty <= o->quantity);
			o->quantity -= qty;
			total_volume_ -= qty;
		}

		[[nodiscard]] uint64_t get_total_volume() const noexcept { return total_volume_; }
		[[nodiscard]] uint32_t size() const noexcept { return size_; }
		[[nodiscard]] bool empty() const noexcept { return size_ == 0; }
		[[nodiscard]] uint32_t tick() const noexcept { return tick_; }
		[[nodiscard]] Order *front() const noexcept { return head_; }
		[[nodiscard]] Order *back() const noexcept { return tail_; }

		struct iterator
		{
			Order *cur;
			Order *operator*() const noexcept { return cur; }
			iterator &operator++() noexcept
			{
				cur = cur->next;
				return *this;
			}
			bool operator!=(const iterator &r) const noexcept { return cur != r.cur; }
		};
		[[nodiscard]] iterator begin() const noexcept { return {head_}; }
		[[nodiscard]] iterator end() const noexcept { return {nullptr}; }
	};

	static_assert(sizeof(PriceLevel) <= 64);

}
