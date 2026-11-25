#include <gtest/gtest.h>

#include <memory>

#include "atom/bounded_mpmc_queue.hpp"
#include "stress.hpp"

namespace {

using atom::BoundedMpmcQueue;
using atom::test::Item;

static_assert(atom::ConcurrentQueue<BoundedMpmcQueue<Item>>);

TEST(BoundedMpmcQueue, FifoFullAndEmpty) {
    BoundedMpmcQueue<int> q(4);
    int                   out = 0;
    EXPECT_FALSE(q.try_pop(out));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(99));
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(q.try_pop(out));
}

TEST(BoundedMpmcQueue, WrapsAroundManyLaps) {
    BoundedMpmcQueue<int> q(4);
    int                   out = 0;
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(q.try_push(i));
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i);
    }
}

TEST(BoundedMpmcQueue, FailedPushKeepsTheItem) {
    BoundedMpmcQueue<std::unique_ptr<int>> q(2);
    EXPECT_TRUE(q.try_push(std::make_unique<int>(1)));
    EXPECT_TRUE(q.try_push(std::make_unique<int>(2)));
    auto keep = std::make_unique<int>(3);
    EXPECT_FALSE(q.try_push(std::move(keep)));
    ASSERT_NE(keep, nullptr);
}

TEST(BoundedMpmcQueue, Stress1P1C) {
    BoundedMpmcQueue<Item> q(64);
    atom::test::run_queue_stress(q, {.producers = 1, .consumers = 1, .per_producer = 300'000});
}

TEST(BoundedMpmcQueue, Stress4P1C) {
    BoundedMpmcQueue<Item> q(1024);
    atom::test::run_queue_stress(q, {.producers = 4, .consumers = 1, .per_producer = 200'000});
}

// Per-producer order holds per consumer: one consumer can never take a producer's later
// item before that producer's earlier one, because positions are claimed in order.
TEST(BoundedMpmcQueue, Stress4P4C) {
    BoundedMpmcQueue<Item> q(1024);
    atom::test::run_queue_stress(q, {.producers = 4, .consumers = 4, .per_producer = 200'000});
}

TEST(BoundedMpmcQueue, Stress4P4CTinyRing) {
    BoundedMpmcQueue<Item> q(4);
    atom::test::run_queue_stress(q, {.producers = 4, .consumers = 4, .per_producer = 50'000});
}

}  // namespace
