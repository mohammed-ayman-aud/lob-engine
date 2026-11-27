// Fixed-size object pool (arena) with an intrusive free list.
// Objects are carved from large contiguous chunks, so hot orders stay close together in
// memory and allocation is a pointer pop: no malloc on the hot path after warm-up.
#pragma once
#include <cstddef>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace lob {

template <class T, size_t ChunkObjects = 1 << 16>
class Pool {
    union Slot { Slot* next; alignas(T) unsigned char storage[sizeof(T)]; };
    std::vector<std::unique_ptr<Slot[]>> chunks_;
    Slot* free_ = nullptr;
    size_t live_ = 0;

    void grow() {
        chunks_.emplace_back(new Slot[ChunkObjects]);
        Slot* c = chunks_.back().get();
        for (size_t i = 0; i < ChunkObjects; ++i) { c[i].next = free_; free_ = &c[i]; }
    }

public:
    Pool() = default;
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    template <class... A>
    T* make(A&&... a) {
        if (!free_) [[unlikely]] grow();
        Slot* s = free_;
        free_ = s->next;
        ++live_;
        return ::new (s->storage) T{std::forward<A>(a)...};
    }
    void destroy(T* p) {
        p->~T();
        Slot* s = reinterpret_cast<Slot*>(p);
        s->next = free_;
        free_ = s;
        --live_;
    }
    size_t live() const { return live_; }
};

// Same interface on top of new/delete: the baseline the pool is benchmarked against.
template <class T>
struct HeapAlloc {
    template <class... A> T* make(A&&... a) { return new T{std::forward<A>(a)...}; }
    void destroy(T* p) { delete p; }
};

}  // namespace lob
