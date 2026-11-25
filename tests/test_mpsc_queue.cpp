#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include "atom/mpsc_queue.hpp"
#include "stress.hpp"

namespace {

using atom::MpscQueue;
using atom::PopResult;
using atom::test::Item;
using atom::test::kUnderTsan;

static_assert(atom::ConcurrentQueue<MpscQueue<Item>>);

struct Counted {
    static inline std::atomic<long> live{0};
    int                             v = 0;
    Counted() { live.fetch_add(1, std::memory_order_relaxed); }
    explicit Counted(int x) : v(x) { live.fetch_add(1, std::memory_order_relaxed); }
    Counted(const Counted& o) : v(o.v) { live.fetch_add(1, std::memory_order_relaxed); }
    Counted& operator=(const Counted&) noexcept = default;
    ~Counted() { live.fetch_sub(1, std::memory_order_relaxed); }
};

TEST(MpscQueue, EmptyThenFifo) {
    MpscQueue<int> q;
    int            out = 0;
    EXPECT_EQ(q.try_pop_ex(out), PopResult::Empty);
    for (int i = 0; i < 100; ++i) q.push(i);
    for (int i = 0; i < 100; ++i) {
        ASSERT_EQ(q.try_pop_ex(out), PopResult::Item);
        EXPECT_EQ(out, i);
    }
    EXPECT_EQ(q.try_pop_ex(out), PopResult::Empty);
    EXPECT_FALSE(q.try_pop(out));
}

TEST(MpscQueue, MoveOnlyItems) {
    MpscQueue<std::unique_ptr<int>> q;
    q.push(std::make_unique<int>(7));
    std::unique_ptr<int> out;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(*out, 7);
}

TEST(MpscQueue, FreesEveryNode) {
    {
        MpscQueue<Counted> q;
        for (int i = 0; i < 10; ++i) q.push(Counted(i));
        Counted out;
        for (int i = 0; i < 4; ++i) ASSERT_TRUE(q.try_pop(out));
    }
    EXPECT_EQ(Counted::live.load(std::memory_order_relaxed), 0);
}

TEST(MpscQueue, Stress1P1C) {
    MpscQueue<Item> q;
    atom::test::run_queue_stress(q, {.producers = 1, .consumers = 1, .per_producer = 300'000});
}

TEST(MpscQueue, Stress4P1C) {
    MpscQueue<Item> q;
    atom::test::run_queue_stress(q, {.producers = 4, .consumers = 1, .per_producer = 250'000});
}

TEST(MpscQueue, Stress8P1C) {
    MpscQueue<Item> q;
    atom::test::run_queue_stress(
        q, {.producers = 8, .consumers = 1, .per_producer = kUnderTsan ? 100'000u : 250'000u});
}

}  // namespace
