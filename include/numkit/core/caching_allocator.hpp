// core/include/numkit/core/caching_allocator.hpp
//
// A `keel::Allocator` wrapper that recycles freed *large* blocks instead of
// returning them to the OS. Blocks of an identical (size, alignment) are pooled
// on a free list, so a later allocation of that size reuses warm, already-
// committed pages — eliminating repeated mmap/VirtualAlloc + demand-zero page
// faults that dominate big transient-array churn.
//
// Small allocations (< minCacheBytes) and any excess beyond maxCachedBytes pass
// straight through to the upstream allocator without locking.

#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/hash.h>
#include <keel/hash_map.h>
#include <keel/memory.h>

#include <mutex>

namespace numkit {

class CachingAllocator {
public:
    explicit CachingAllocator(
        keel::Allocator *upstream       = nullptr,
        keel::usize      minCacheBytes  = 256u * 1024u,          // cache >= 256 KiB
        keel::usize      maxCachedBytes = 1024u * 1024u * 1024u) // cap 1 GiB resident
        noexcept
        : upstream_(upstream ? upstream : keel::default_allocator()),
          minCache_(minCacheBytes),
          maxCached_(maxCachedBytes),
          free_(upstream_)
    {
        alloc_.alloc   = &CachingAllocator::allocCb;
        alloc_.realloc = &CachingAllocator::reallocCb;
        alloc_.free    = &CachingAllocator::freeCb;
        alloc_.ctx     = this;
    }

    ~CachingAllocator() { releaseCached(); }

    CachingAllocator(const CachingAllocator &)            = delete;
    CachingAllocator &operator=(const CachingAllocator &) = delete;

    [[nodiscard]] keel::Allocator *allocator() noexcept { return &alloc_; }

    // Return every pooled block to upstream (e.g. to reclaim memory at idle).
    void releaseCached() noexcept
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto entry : free_) {
            for (void *p : entry.value) {
                upstream_->free_bytes(p, entry.key.bytes, entry.key.align);
            }
        }
        free_.clear();
        cachedBytes_ = 0;
    }

private:
    struct BlockKey {
        keel::usize bytes;
        keel::usize align;
        constexpr bool operator==(const BlockKey &o) const noexcept
        {
            return bytes == o.bytes && align == o.align;
        }
    };

    struct BlockKeyHash {
        constexpr keel::u64 operator()(const BlockKey &k) const noexcept
        {
            return keel::hash_combine(keel::hash_u64(k.bytes), k.align);
        }
    };

    void *takeCached(keel::usize bytes, keel::usize align) noexcept
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (keel::Array<void *> *bucket = free_.get(BlockKey{bytes, align})) {
            if (!bucket->empty()) {
                void *p = bucket->back();
                bucket->pop_back();
                cachedBytes_ -= bytes;
                return p;
            }
        }
        return nullptr;
    }

    static void *allocCb(void *ctx, keel::usize bytes, keel::usize align) noexcept
    {
        if (bytes == 0) return nullptr;
        auto *self = static_cast<CachingAllocator *>(ctx);
        if (bytes >= self->minCache_) {
            if (void *p = self->takeCached(bytes, align)) return p;
        }
        return self->upstream_->try_alloc_bytes(bytes, align);
    }

    static void freeCb(void *ctx, void *p, keel::usize bytes, keel::usize align) noexcept
    {
        if (!p || bytes == 0) return;
        auto *self = static_cast<CachingAllocator *>(ctx);
        if (bytes >= self->minCache_) {
            try {
                std::lock_guard<std::mutex> lk(self->mu_);
                if (bytes <= self->maxCached_ && self->cachedBytes_ <= self->maxCached_ - bytes) {
                    BlockKey key{bytes, align};
                    keel::Array<void *> *bucket = self->free_.get(key);
                    if (!bucket) {
                        bucket = self->free_.try_insert(key, keel::Array<void *>(self->upstream_)).value;
                    }
                    bucket->push_back(p);
                    self->cachedBytes_ += bytes;
                    return;
                }
            } catch (...) {
                // Fall through to upstream free if free-list growth fails.
            }
        }
        self->upstream_->free_bytes(p, bytes, align);
    }

    static void *reallocCb(void *ctx, void *p, keel::usize oldBytes, keel::usize newBytes,
                           keel::usize align) noexcept
    {
        auto *self = static_cast<CachingAllocator *>(ctx);
        if (!p) return allocCb(ctx, newBytes, align);
        if (newBytes == 0) {
            freeCb(ctx, p, oldBytes, align);
            return nullptr;
        }
        if (oldBytes < self->minCache_ && newBytes < self->minCache_) {
            return self->upstream_->try_realloc_bytes(p, oldBytes, newBytes, align);
        }
        if (newBytes == oldBytes) return p;
        void *np = allocCb(ctx, newBytes, align);
        if (!np) return nullptr;
        if (oldBytes > 0) {
            keel::mem_copy(np, p, keel::min(oldBytes, newBytes));
        }
        freeCb(ctx, p, oldBytes, align);
        return np;
    }

    keel::Allocator                                            alloc_{};
    keel::Allocator                                           *upstream_;
    keel::usize                                                minCache_;
    keel::usize                                                maxCached_;
    std::mutex                                                 mu_;
    keel::HashMap<BlockKey, keel::Array<void *>, BlockKeyHash> free_;
    keel::usize                                                cachedBytes_ = 0;
};

// Process-wide cached allocator wrapping `keel::default_allocator()`. Used as
// the implicit allocator for default-constructed Engines so large transient
// arrays recycle their backing pages. Embedders passing their own Allocator*
// keep full control and are NOT wrapped.
inline keel::Allocator *defaultCachedAllocator() noexcept
{
    static CachingAllocator instance(keel::default_allocator());
    return instance.allocator();
}

} // namespace numkit
