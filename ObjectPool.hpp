#pragma once

// Fixed-size object pool for Order allocations.
//
// The whole point is that allocate/deallocate never touch the heap after
// construction. Memory is grabbed once up front via ::operator new (with
// correct alignment for over-aligned types like alignas(64)), then carved
// up into a free list.
//
// Free slots store their "next" pointer in-place (intrusive free list),
// so there's zero metadata overhead per slot. Requirement: sizeof(T) >= sizeof(void*).

#include <cassert>
#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace lob {

template <typename T, std::size_t Capacity>
    requires (Capacity > 0)
class ObjectPool {
public:
    using value_type = T;

    ObjectPool() {
        static_assert(sizeof(T) >= sizeof(void*),
            "T must be at least pointer-sized to store the free-list next pointer");
        build_free_list();
    }

    ObjectPool(const ObjectPool&)            = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&)                 = delete;
    ObjectPool& operator=(ObjectPool&&)      = delete;

    ~ObjectPool() {
        ::operator delete(storage_, sizeof(T) * Capacity,
                          std::align_val_t{alignof(T)});
    }

    // Construct a T in-place and return a pointer. Returns nullptr on exhaustion
    // rather than throwing – callers should check and handle gracefully.
    template <typename... Args>
    [[nodiscard]] T* allocate(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        if (__builtin_expect(!free_head_, 0)) [[unlikely]]
            return nullptr;

        FreeNode* slot = free_head_;
        free_head_     = slot->next;
        ++allocated_;
        return ::new (static_cast<void*>(slot)) T(std::forward<Args>(args)...);
    }

    void deallocate(T* ptr) noexcept {
        if (__builtin_expect(!ptr, 0)) [[unlikely]] return;
        assert(owns(ptr));

        ptr->~T();
        auto* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = free_head_;
        free_head_ = node;
        --allocated_;
    }

    [[nodiscard]] bool        owns(const T* ptr) const noexcept {
        const auto* raw = reinterpret_cast<const std::byte*>(ptr);
        return raw >= storage_ && raw < storage_ + sizeof(T) * Capacity;
    }

    [[nodiscard]] std::size_t allocated()  const noexcept { return allocated_; }
    [[nodiscard]] std::size_t available()  const noexcept { return Capacity - allocated_; }
    [[nodiscard]] bool        full()       const noexcept { return allocated_ == Capacity; }
    [[nodiscard]] bool        empty()      const noexcept { return allocated_ == 0; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    struct FreeNode { FreeNode* next; };

    std::byte*  storage_   = nullptr;
    FreeNode*   free_head_ = nullptr;
    std::size_t allocated_ = 0;

    void build_free_list() {
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
};

// RAII helper so callers don't have to remember to call pool.deallocate().
template <typename T, std::size_t Cap>
struct PoolDeleter {
    ObjectPool<T, Cap>* pool = nullptr;
    void operator()(T* ptr) const noexcept { if (pool) pool->deallocate(ptr); }
};

template <typename T, std::size_t Cap, typename... Args>
[[nodiscard]] auto make_pooled(ObjectPool<T, Cap>& pool, Args&&... args)
    -> std::unique_ptr<T, PoolDeleter<T, Cap>>
{
    T* ptr = pool.allocate(std::forward<Args>(args)...);
    if (!ptr) throw std::bad_alloc{};
    return { ptr, PoolDeleter<T, Cap>{ &pool } };
}

} // namespace lob
