#pragma once

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
inline constexpr std::size_t kScanThreshold  = 2 * kMaxThreads * kSlotsPerThread;

struct alignas(kCacheLine) Record {
    std::atomic<void*>        slots[kSlotsPerThread]{};
    std::atomic<std::int64_t> pending{0};  // owner writes only
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
            if (r.taken.load(std::memory_order_relaxed)) continue;
            bool expected = false;
            if (r.taken.compare_exchange_strong(expected, true, std::memory_order_acquire,
                                                std::memory_order_relaxed))
                return &r;
        }
        std::fprintf(stderr, "atom::hp: more than %zu threads\n", kMaxThreads);
        std::abort();
    }

    void release_record(Record* r) {
        for (auto& s : r->slots) s.store(nullptr, std::memory_order_release);
        r->taken.store(false, std::memory_order_release);
    }

    void scan(std::vector<Retired>& list, Record& owner, std::vector<void*>& scratch) {
        scratch.clear();
        for (Record& r : records_)
            for (auto& s : r.slots)
                // seq_cst: pairs with the publish in Guard::protect.
                if (void* p = s.load(std::memory_order_seq_cst)) scratch.push_back(p);
        std::sort(scratch.begin(), scratch.end());

        std::size_t kept = 0;
        for (Retired& r : list) {
            if (std::binary_search(scratch.begin(), scratch.end(), r.ptr)) list[kept++] = r;
            else r.deleter(r.ptr);
        }
        const auto freed = static_cast<std::int64_t>(list.size() - kept);
        list.resize(kept);
        owner.pending.store(owner.pending.load(std::memory_order_relaxed) - freed,
                            std::memory_order_relaxed);
    }

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
        std::fprintf(stderr, "atom::hp: more than %zu guards on one thread\n", kSlotsPerThread);
        std::abort();
    }

    void give_slot(std::atomic<void*>* slot) {
        slot->store(nullptr, std::memory_order_release);
        used_ &= ~(1u << static_cast<unsigned>(slot - rec_->slots));
    }

    void retire(void* p, void (*deleter)(void*)) {
        retired_.push_back({p, deleter});
        rec_->pending.store(rec_->pending.load(std::memory_order_relaxed) + 1,
                            std::memory_order_relaxed);
        if (retired_.size() >= kScanThreshold) domain().scan(retired_, *rec_, scratch_);
    }

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
    std::vector<void*>   scratch_;  // a member: a thread_local would die before ~ThreadState
};

inline ThreadState& local() {
    thread_local ThreadState state;
    return state;
}

class Guard {
public:
    Guard() : state_(local()), slot_(state_.take_slot()) {}
    ~Guard() { state_.give_slot(slot_); }

    Guard(const Guard&)            = delete;
    Guard& operator=(const Guard&) = delete;

    template <class N>
    N* protect(const std::atomic<N*>& src) noexcept {
        N* p = src.load(std::memory_order_relaxed);
        for (;;) {
            // seq_cst: store then load of another variable; weaker lets the load go first.
            slot_->store(p, std::memory_order_seq_cst);
            N* again = src.load(std::memory_order_seq_cst);
            if (again == p) return p;
            p = again;
        }
    }

    void reset() noexcept { slot_->store(nullptr, std::memory_order_release); }

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
struct HazardPointers {
    using Guard = hp::Guard;
    template <class N>
    static void retire(N* p) {
        hp::retire(p);
    }
};
}  // namespace atom
