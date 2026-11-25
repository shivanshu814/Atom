#pragma once

// Epoch-based reclamation (Keir Fraser, 2004).
//
// Instead of protecting each pointer, a thread protects a whole critical section: it
// announces the global epoch it entered in, and nodes it could have seen stay alive until
// every thread has moved on. Cheaper per access than hazard pointers (one announce per
// operation, nothing per node), but one thread stalled inside a critical section stops all
// freeing, so memory is unbounded.
//
// The rules, with E the global epoch:
//   - An active thread announces an epoch that is E or E - 1 (E only advances when every
//     active thread has announced E).
//   - A node retired by a thread announced at L is tagged L + 1, not L. While that thread is
//     active, E is at most L + 1, and a reader that entered at E may already hold the node.
//     Tagged L the node would be freed at E = L + 2, which a reader still at L + 1 does not
//     block. Tagged L + 1 it is freed at E = L + 3, which needs that reader to have left.
//   - A node tagged t is freed once its owner observes E >= t + 2.
//
// Each thread frees only its own limbo lists, so no list is shared and none needs a lock.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "atom/config.hpp"

namespace atom::ebr {

inline constexpr std::size_t kMaxThreads = 128;
// Retires between attempts to advance the epoch. Each attempt reads every record, so this
// keeps that cost small per node while still freeing memory regularly.
inline constexpr unsigned kAdvanceEvery = 64;

struct alignas(kCacheLine) Record {
    // (epoch << 1) | active, one word, so nobody can see "active" with an epoch that has
    // not been written yet (as two stores could be seen).
    std::atomic<std::uint64_t> state{0};
    std::atomic<std::int64_t>  pending{0};  // retired, not yet freed; written by the owner only
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
            if (r.taken.load(std::memory_order_relaxed)) continue;  // cheap pre-check
            bool expected = false;
            // acquire: pairs with release_record, so the previous owner's writes are visible.
            if (r.taken.compare_exchange_strong(expected, true, std::memory_order_acquire,
                                                std::memory_order_relaxed))
                return &r;
        }
        std::fprintf(stderr, "atom::ebr: more than %zu threads use epochs\n", kMaxThreads);
        std::abort();
    }

    void release_record(Record* r) {
        r->state.store(0, std::memory_order_release);      // inactive
        r->taken.store(false, std::memory_order_release);  // hand the record over
    }

    std::uint64_t epoch() const noexcept {
        // seq_cst: part of the store-then-load handshake in ThreadState::enter.
        return epoch_.load(std::memory_order_seq_cst);
    }

    // Moves the epoch forward if every active thread has announced the current one.
    bool try_advance() noexcept {
        std::uint64_t e = epoch_.load(std::memory_order_seq_cst);
        for (Record& r : records_) {
            // seq_cst: pairs with the seq_cst announce in enter(). Its acquire half also
            // orders every read a thread did before exit() before the advance below, and so
            // before the frees that the advance allows.
            const std::uint64_t s = r.state.load(std::memory_order_seq_cst);
            if ((s & 1) != 0 && (s >> 1) != e) return false;
        }
        // acq_rel: release carries the observations above to whoever sees e + 1 and frees.
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
        if (depth_++ != 0) return;  // nested: already announced
        Domain&       d = domain();
        std::uint64_t e = d.epoch();
        for (;;) {
            // seq_cst: a store followed by loads of shared pointers. Weaker orderings would
            // let those loads run before the announce is visible, and try_advance could then
            // move on without seeing us.
            rec_->state.store((e << 1) | 1, std::memory_order_seq_cst);
            const std::uint64_t now = d.epoch();
            if (now == e) break;  // announced the epoch that is still current
            e = now;
        }
        if (e != local_epoch_) {
            local_epoch_ = e;
            free_expired();
        }
    }

    void exit() {
        if (--depth_ != 0) return;
        // release: everything read inside the critical section happens-before an advance
        // that sees us inactive.
        rec_->state.store(local_epoch_ << 1, std::memory_order_release);
    }

    // Only inside enter() / exit().
    void retire(void* p, void (*deleter)(void*)) {
        const std::uint64_t tag = local_epoch_ + 1;  // see the header comment
        Bag&                bag = limbo_[tag % 3];
        bag.tag                 = tag;  // any older contents were freed when the epoch moved
        bag.items.push_back({p, deleter});
        rec_->pending.store(rec_->pending.load(std::memory_order_relaxed) + 1,
                            std::memory_order_relaxed);
        if (++since_advance_ >= kAdvanceEvery) {
            since_advance_ = 0;
            domain().try_advance();
        }
    }

    // Frees everything this thread retired. Waits for the epoch to move far enough, which
    // needs every other thread to leave its critical section eventually.
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
        // relaxed: a statistic, only read after the threads that write it have joined.
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

// A critical section: nodes read inside it are not freed until it ends.
class Guard {
public:
    Guard() : state_(local()) { state_.enter(); }
    ~Guard() { state_.exit(); }

    Guard(const Guard&)            = delete;
    Guard& operator=(const Guard&) = delete;

    template <class N>
    N* protect(const std::atomic<N*>& src) noexcept {
        // acquire: pairs with the release that published the node, so its fields are visible.
        // Nothing else is needed: being inside the critical section is the protection.
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
// Reclamation policy for TreiberStack.
struct EpochReclaim {
    using Guard = ebr::Guard;
    template <class N>
    static void retire(N* p) {
        ebr::retire(p);
    }
};
}  // namespace atom
