#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "atom/config.hpp"

namespace atom::test {

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
inline constexpr bool kUnderTsan = true;
#else
inline constexpr bool kUnderTsan = false;
#endif
#else
inline constexpr bool kUnderTsan = false;
#endif

// Field by field: TSan on macOS misses races through memcpy.
struct Item {
    std::uint32_t producer = 0;
    std::uint32_t pad      = 0;
    std::uint64_t seq      = 0;

    Item() = default;
    Item(std::uint32_t p, std::uint32_t pd, std::uint64_t s) noexcept : producer(p), pad(pd), seq(s) {}
    Item(const Item& o) noexcept : producer(o.producer), pad(o.pad), seq(o.seq) {}
    Item& operator=(const Item& o) noexcept {
        producer = o.producer;
        pad      = o.pad;
        seq      = o.seq;
        return *this;
    }
};

class Jitter {
public:
    Jitter(bool enabled, std::uint64_t seed) : enabled_(enabled), state_(seed * 0x9E3779B97F4A7C15ull + 1) {}

    void maybe_yield() noexcept {
        if (!enabled_) return;
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        if ((state_ & 1023) == 0) std::this_thread::yield();
    }

private:
    bool          enabled_;
    std::uint64_t state_;
};

struct QueueStress {
    unsigned      producers    = 1;
    unsigned      consumers    = 1;
    std::uint64_t per_producer = 100'000;
    bool          check_fifo   = true;
    bool          jitter       = true;
};

inline void check_items(const std::vector<std::vector<Item>>& received, unsigned producers,
                        std::uint64_t per_producer, bool check_fifo) {
    std::vector<std::uint8_t> seen(static_cast<std::size_t>(producers) * per_producer, 0);
    std::uint64_t garbage = 0, duplicates = 0, out_of_order = 0, total = 0;
    std::string   first_problem;

    for (const auto& list : received) {
        std::vector<std::int64_t> last(producers, -1);
        for (const Item& it : list) {
            ++total;
            if (it.producer >= producers || it.seq >= per_producer) {
                if (garbage++ == 0)
                    first_problem += "garbage item (" + std::to_string(it.producer) + ", " +
                                     std::to_string(it.seq) + "); ";
                continue;
            }
            std::uint8_t& s = seen[it.producer * per_producer + it.seq];
            if (s++ != 0 && duplicates++ == 0)
                first_problem += "duplicate (" + std::to_string(it.producer) + ", " +
                                 std::to_string(it.seq) + "); ";
            if (check_fifo) {
                const auto seq = static_cast<std::int64_t>(it.seq);
                if (seq <= last[it.producer] && out_of_order++ == 0)
                    first_problem += "producer " + std::to_string(it.producer) + " seq " +
                                     std::to_string(it.seq) + " after " +
                                     std::to_string(last[it.producer]) + "; ";
                last[it.producer] = seq;
            }
        }
    }

    std::uint64_t missing = 0;
    for (std::uint8_t s : seen) missing += (s == 0);

    EXPECT_EQ(garbage, 0u) << first_problem;
    EXPECT_EQ(duplicates, 0u) << first_problem;
    EXPECT_EQ(out_of_order, 0u) << first_problem;
    EXPECT_EQ(missing, 0u) << "items pushed but never popped";
    EXPECT_EQ(total, static_cast<std::uint64_t>(producers) * per_producer);
}

template <class Q>
void run_queue_stress(Q& q, const QueueStress& cfg) {
    const std::uint64_t total = cfg.producers * cfg.per_producer;

    std::atomic<bool>          go{false};
    std::atomic<std::uint64_t> consumed{0};
    std::vector<std::vector<Item>> received(cfg.consumers);
    std::vector<std::thread>       threads;

    for (unsigned p = 0; p < cfg.producers; ++p) {
        threads.emplace_back([&, p] {
            Jitter  jitter(cfg.jitter, p + 1);
            Backoff backoff;
            while (!go.load(std::memory_order_acquire)) cpu_relax();
            for (std::uint64_t s = 0; s < cfg.per_producer; ++s) {
                const Item it{p, 0, s};
                while (!q.try_push(it)) backoff.pause();
                backoff.reset();
                jitter.maybe_yield();
            }
        });
    }
    for (unsigned c = 0; c < cfg.consumers; ++c) {
        threads.emplace_back([&, c] {
            Jitter  jitter(cfg.jitter, 1000 + c);
            Backoff backoff;
            auto&   out = received[c];
            out.reserve(total / cfg.consumers + 1);
            while (!go.load(std::memory_order_acquire)) cpu_relax();
            Item it;
            while (consumed.load(std::memory_order_relaxed) < total) {
                if (q.try_pop(it)) {
                    out.push_back(it);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                    backoff.reset();
                    jitter.maybe_yield();
                } else {
                    backoff.pause();
                }
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    check_items(received, cfg.producers, cfg.per_producer, cfg.check_fifo);
    Item leftover;
    EXPECT_FALSE(q.try_pop(leftover)) << "queue not empty after every item was consumed";
}

template <class S>
void run_stack_stress(S& stack, unsigned threads_count, std::uint64_t per_thread, bool jitter_on = true) {
    std::atomic<bool>              go{false};
    std::vector<std::vector<Item>> received(threads_count + 1);
    std::vector<std::thread>       threads;

    for (unsigned t = 0; t < threads_count; ++t) {
        threads.emplace_back([&, t] {
            Jitter jitter(jitter_on, t + 1);
            auto&  out = received[t];
            out.reserve(per_thread);
            while (!go.load(std::memory_order_acquire)) cpu_relax();
            for (std::uint64_t s = 0; s < per_thread; ++s) {
                stack.push(Item{t, 0, s});
                jitter.maybe_yield();
                if (std::optional<Item> v = stack.pop()) out.push_back(*v);
                jitter.maybe_yield();
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    while (std::optional<Item> v = stack.pop()) received[threads_count].push_back(*v);
    check_items(received, threads_count, per_thread, /*check_fifo=*/false);
}

}  // namespace atom::test
