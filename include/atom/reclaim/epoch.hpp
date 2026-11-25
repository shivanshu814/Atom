#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "atom/config.hpp"

namespace atom::ebr {

inline constexpr std::size_t kMaxThreads   = 128;
inline constexpr unsigned    kAdvanceEvery = 64;

struct alignas(kCacheLine) Record {
    std::atomic<std::uint64_t> state{0};    // (epoch << 1) | active, one word
    std::atomic<std::int64_t>  pending{0};  // owner writes only
    std::atomic<bool>          taken{false};
};

struct Retired {
    void* ptr;
    void (*deleter)(void*);
};

class Domain {
public:
    Record* acquire_record() {
        for (Record& r : records_) {
            if (r.taken.load(std::memory_order_relaxed)) continue;
            bool expected = false;
            if (r.taken.compare_exchange_strong(expected, true, std::memory_order_acquire,
                                                std::memory_order_relaxed))
                return &r;
        }
        std::fprintf(stderr, "atom::ebr: more than %zu threads\n", kMaxThreads);
        std::abort();
    }

    void release_record(Record* r) {
        r->state.store(0, std::memory_order_release);
        r->taken.store(false, std::memory_order_release);
    }

    std::uint64_t epoch() const noexcept { return epoch_.load(std::memory_order_seq_cst); }

    bool try_advance() noexcept {
        std::uint64_t e = epoch_.load(std::memory_order_seq_cst);
        for (Record& r : records_) {
            const std::uint64_t s = r.state.load(std::memory_order_seq_cst);
            if ((s & 1) != 0 && (s >> 1) != e) return false;
        }
        return epoch_.compare_exchange_strong(e, e + 1, std::memory_order_acq_rel,
                                              std::memory_order_relaxed);
    }

    std::int64_t pending() const {
        std::int64_t total = 0;
        for (const Record& r : records_) total += r.pending.load(std::memory_order_relaxed);
        return total;
    }

private:
    alignas(kCacheLine) std::atomic<std::uint64_t> epoch_{0};
    Record records_[kMaxThreads];
};

inline Domain& domain() {
    static Domain d;
    return d;
}

class ThreadState {
public:
    ThreadState() : rec_(domain().acquire_record()) {}

    ~ThreadState() {
        drain();
        domain().release_record(rec_);
    }

    ThreadState(const ThreadState&)            = delete;
    ThreadState& operator=(const ThreadState&) = delete;

    void enter() {
        if (depth_++ != 0) return;
        Domain&       d = domain();
        std::uint64_t e = d.epoch();
        for (;;) {
            // seq_cst: store then loads of shared pointers.
            rec_->state.store((e << 1) | 1, std::memory_order_seq_cst);
            const std::uint64_t now = d.epoch();
            if (now == e) break;
            e = now;
        }
        if (e != local_epoch_) {
            local_epoch_ = e;
            free_expired();
        }
    }

    void exit() {
        if (--depth_ != 0) return;
        rec_->state.store(local_epoch_ << 1, std::memory_order_release);
    }

    void retire(void* p, void (*deleter)(void*)) {
        // +1: the global epoch may already be one ahead of us.
        const std::uint64_t tag = local_epoch_ + 1;
        Bag&                bag = limbo_[tag % 3];
        bag.tag                 = tag;
        bag.items.push_back({p, deleter});
        rec_->pending.store(rec_->pending.load(std::memory_order_relaxed) + 1,
                            std::memory_order_relaxed);
        if (++since_advance_ >= kAdvanceEvery) {
            since_advance_ = 0;
            domain().try_advance();
        }
    }

    void drain() {
        while (!empty()) {
            domain().try_advance();
            enter();
            exit();
            if (!empty()) std::this_thread::yield();
        }
    }

private:
    struct Bag {
        std::uint64_t        tag = 0;
        std::vector<Retired> items;
    };

    bool empty() const {
        return limbo_[0].items.empty() && limbo_[1].items.empty() && limbo_[2].items.empty();
    }

    void free_expired() {
        std::int64_t freed = 0;
        for (Bag& bag : limbo_) {
            if (bag.items.empty() || bag.tag + 2 > local_epoch_) continue;
            for (Retired& r : bag.items) r.deleter(r.ptr);
            freed += static_cast<std::int64_t>(bag.items.size());
            bag.items.clear();
        }
        rec_->pending.store(rec_->pending.load(std::memory_order_relaxed) - freed,
                            std::memory_order_relaxed);
    }

    Record*       rec_;
    unsigned      depth_         = 0;
    unsigned      since_advance_ = 0;
    std::uint64_t local_epoch_   = 0;
    Bag           limbo_[3];
};

inline ThreadState& local() {
    thread_local ThreadState state;
    return state;
}

class Guard {
public:
    Guard() : state_(local()) { state_.enter(); }
    ~Guard() { state_.exit(); }

    Guard(const Guard&)            = delete;
    Guard& operator=(const Guard&) = delete;

    template <class N>
    N* protect(const std::atomic<N*>& src) noexcept {
        return src.load(std::memory_order_acquire);
    }

    void reset() noexcept {}

private:
    ThreadState& state_;
};

template <class N>
void retire(N* p) {
    local().retire(p, [](void* q) { delete static_cast<N*>(q); });
}

inline void         drain_this_thread() { local().drain(); }
inline std::int64_t pending() { return domain().pending(); }

}  // namespace atom::ebr

namespace atom {
struct EpochReclaim {
    using Guard = ebr::Guard;
    template <class N>
    static void retire(N* p) {
        ebr::retire(p);
    }
};
}  // namespace atom
