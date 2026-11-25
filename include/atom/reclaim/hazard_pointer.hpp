#pragma once

// Hazard pointers (Maged Michael, 2004).
//
// Before a thread dereferences a node it might share with a thread that could free it, it
// publishes the node's address in one of its hazard slots: "I am reading this, do not free
// it". A retired node is freed only when no slot anywhere holds its address.
//
// One process-wide domain: a fixed table of per-thread records, each with a couple of slots.
// A thread claims a record the first time it uses hazard pointers and gives it back when it
// exits, after freeing everything it retired (waiting, if it must, for readers to finish).
//
// Bounded memory: a stalled thread can keep at most kSlotsPerThread nodes alive, unlike
// epochs, where one stalled thread stops all freeing.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "atom/config.hpp"

namespace atom::hp {

inline constexpr std::size_t kMaxThreads     = 128;
inline constexpr std::size_t kSlotsPerThread = 2;
// Scan once this many nodes are waiting: about twice the number of slots, so each scan
// frees at least half of them and costs O(1) per retired node, amortised.
inline constexpr std::size_t kScanThreshold = 2 * kMaxThreads * kSlotsPerThread;

struct alignas(kCacheLine) Record {
    std::atomic<void*>        slots[kSlotsPerThread]{};
    std::atomic<std::int64_t> pending{0};  // retired, not yet freed; written by the owner only
    std::atomic<bool>         taken{false};
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
            // acquire: pairs with release_record, so the previous owner's last writes to
            // this record are visible to its new owner.
            if (r.taken.compare_exchange_strong(expected, true, std::memory_order_acquire,
                                                std::memory_order_relaxed))
                return &r;
        }
        std::fprintf(stderr, "atom::hp: more than %zu threads use hazard pointers\n", kMaxThreads);
        std::abort();
    }

    void release_record(Record* r) {
        for (auto& s : r->slots) s.store(nullptr, std::memory_order_release);
        r->taken.store(false, std::memory_order_release);  // hand the record over
    }

    // Frees every node in `list` that no slot protects and keeps the rest. `scratch` is the
    // caller's buffer for the hazards, reused between scans so a scan does not allocate.
    void scan(std::vector<Retired>& list, Record& owner, std::vector<void*>& scratch) {
        scratch.clear();
        for (Record& r : records_)
            for (auto& s : r.slots)
                // seq_cst: pairs with the seq_cst publish in Guard::protect (see there). Its
                // acquire half also orders the reader's last use of a node before our delete.
                if (void* p = s.load(std::memory_order_seq_cst)) scratch.push_back(p);
        std::sort(scratch.begin(), scratch.end());

        std::size_t kept = 0;
        for (Retired& r : list) {
            if (std::binary_search(scratch.begin(), scratch.end(), r.ptr)) {
                list[kept++] = r;
            } else {
                r.deleter(r.ptr);
            }
        }
        const auto freed = static_cast<std::int64_t>(list.size() - kept);
        list.resize(kept);
        // relaxed: a statistic, only read after the threads that write it have joined.
        owner.pending.store(owner.pending.load(std::memory_order_relaxed) - freed,
                            std::memory_order_relaxed);
    }

    // Nodes retired by any thread and not yet freed.
    std::int64_t pending() const {
        std::int64_t total = 0;
        for (const Record& r : records_) total += r.pending.load(std::memory_order_relaxed);
        return total;
    }

private:
    Record records_[kMaxThreads];
};

inline Domain& domain() {
    static Domain d;
    return d;
}

// Per-thread state: the claimed record, which of its slots are in use, and the list of nodes
// this thread retired that were still protected at its last scan.
class ThreadState {
public:
    ThreadState() : rec_(domain().acquire_record()) {}

    ~ThreadState() {
        drain();
        domain().release_record(rec_);
    }

    ThreadState(const ThreadState&)            = delete;
    ThreadState& operator=(const ThreadState&) = delete;

    std::atomic<void*>* take_slot() {
        for (std::size_t i = 0; i < kSlotsPerThread; ++i) {
            if ((used_ & (1u << i)) == 0) {
                used_ |= 1u << i;
                return &rec_->slots[i];
            }
        }
        std::fprintf(stderr, "atom::hp: more than %zu guards alive on one thread\n", kSlotsPerThread);
        std::abort();
    }

    void give_slot(std::atomic<void*>* slot) {
        // release: my reads of the node happen-before a scanner that sees the slot empty.
        slot->store(nullptr, std::memory_order_release);
        used_ &= ~(1u << static_cast<unsigned>(slot - rec_->slots));
    }

    void retire(void* p, void (*deleter)(void*)) {
        retired_.push_back({p, deleter});
        rec_->pending.store(rec_->pending.load(std::memory_order_relaxed) + 1,
                            std::memory_order_relaxed);
        if (retired_.size() >= kScanThreshold) domain().scan(retired_, *rec_, scratch_);
    }

    // Frees everything this thread retired, waiting for readers that still protect a node.
    void drain() {
        while (!retired_.empty()) {
            domain().scan(retired_, *rec_, scratch_);
            if (!retired_.empty()) std::this_thread::yield();
        }
    }

private:
    Record*              rec_;
    unsigned             used_ = 0;
    std::vector<Retired> retired_;
    // A member, not a separate thread_local: thread_locals are destroyed in reverse order of
    // construction, so a thread_local scratch vector created by the first scan died before
    // this object, and the scan in our destructor wrote into freed memory.
    std::vector<void*> scratch_;
};

inline ThreadState& local() {
    thread_local ThreadState state;
    return state;
}

// Owns one hazard slot for its lifetime.
class Guard {
public:
    Guard() : state_(local()), slot_(state_.take_slot()) {}
    ~Guard() { state_.give_slot(slot_); }

    Guard(const Guard&)            = delete;
    Guard& operator=(const Guard&) = delete;

    // Returns src's current value, published in this guard's slot so it cannot be freed
    // until the guard is reset or destroyed.
    template <class N>
    N* protect(const std::atomic<N*>& src) noexcept {
        N* p = src.load(std::memory_order_relaxed);  // a candidate; validated below
        for (;;) {
            // seq_cst on both: this is a store followed by a load of a *different* variable.
            // Acquire/release would let the load move before the store; then a scanner could
            // miss the hazard while our re-check still passes, and free p under us.
            slot_->store(p, std::memory_order_seq_cst);
            N* again = src.load(std::memory_order_seq_cst);
            if (again == p) return p;  // still reachable after the publish: safe
            p = again;
        }
    }

    // Stop protecting the node (it is unlinked and ours, or we no longer need it).
    void reset() noexcept {
        // release: my reads of the node happen-before a scanner that sees the slot empty.
        slot_->store(nullptr, std::memory_order_release);
    }

private:
    ThreadState&        state_;
    std::atomic<void*>* slot_;
};

template <class N>
void retire(N* p) {
    local().retire(p, [](void* q) { delete static_cast<N*>(q); });
}

inline void         drain_this_thread() { local().drain(); }
inline std::int64_t pending() { return domain().pending(); }

}  // namespace atom::hp

namespace atom {
// Reclamation policy for TreiberStack.
struct HazardPointers {
    using Guard = hp::Guard;
    template <class N>
    static void retire(N* p) {
        hp::retire(p);
    }
};
}  // namespace atom
