#pragma once

// Fixed-size object pool for Order allocations.
//
// allocate/deallocate never touch the heap after construction. Memory is
// grabbed once up front via ::operator new (with correct alignment for
// over-aligned types like alignas(64)), then carved into an intrusive free
// list: a free slot stores its "next" pointer in-place, so there is zero
// metadata overhead per slot. Requirement: sizeof(T) >= sizeof(void*).

#include <cassert>
#include <cstddef>
#include <new>

namespace lob {

template <typename T, std::size_t Capacity>
class ObjectPool {
	static_assert(Capacity > 0);
    static_assert(sizeof(T) >= sizeof(void*),
	"T must be at least pointer-sized to store the free-list next pointer");
	
private:
	struct FreeNode { FreeNode* next; };
	
	std::byte*  storage_   = nullptr;
	FreeNode*   free_head_ = nullptr;
	std::size_t allocated_ = 0;
	
	[[nodiscard]] bool owns(const T* ptr) const noexcept {
		const auto* raw = reinterpret_cast<const std::byte*>(ptr);
		return raw >= storage_ && raw < storage_ + sizeof(T) * Capacity;
		}
public:
    ObjectPool() {
        storage_ = static_cast<std::byte*>(
            ::operator new(sizeof(T) * Capacity, std::align_val_t{alignof(T)}));

        // Chain highest-index first so slot[0] ends up at the head.
        // LIFO order means freshly-freed slots are reused immediately,
        // which keeps recently-touched memory hot in cache.
        for (std::size_t i = Capacity; i-- > 0; ) {
            auto* node = reinterpret_cast<FreeNode*>(storage_ + i * sizeof(T));
            node->next = free_head_;
            free_head_ = node;
        }
    }

    ObjectPool(const ObjectPool&)            = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&)                 = delete;
    ObjectPool& operator=(ObjectPool&&)      = delete;

    ~ObjectPool() {
        // Unsized form: clang doesn't enable -fsized-deallocation by default.
        ::operator delete(storage_, std::align_val_t{alignof(T)});
    }

    // Construct a default T in a free slot. Returns nullptr on exhaustion
    // rather than throwing – callers should check and handle gracefully.
    [[nodiscard]] T* allocate() noexcept {
        if (!free_head_) [[unlikely]] return nullptr;

        FreeNode* slot = free_head_;
        free_head_     = slot->next;
        ++allocated_;
        return ::new (static_cast<void*>(slot)) T();
    }

    void deallocate(T* ptr) noexcept {
        if (!ptr) [[unlikely]] return;
        assert(owns(ptr));

        ptr->~T();
        auto* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = free_head_;
        free_head_ = node;
        --allocated_;
    }

    [[nodiscard]] std::size_t allocated() const noexcept { return allocated_; }

};

} // namespace lob
