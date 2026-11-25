#include <gtest/gtest.h>

#include <memory>

#include "atom/mutex_queue.hpp"
#include "stress.hpp"

namespace {

using atom::MutexQueue;
using atom::test::Item;
using atom::test::kUnderTsan;

static_assert(atom::ConcurrentQueue<MutexQueue<Item>>);

TEST(MutexQueue, CapacityRoundsUpToAPowerOfTwo) {
    EXPECT_EQ(MutexQueue<int>(0).capacity(), 2u);
    EXPECT_EQ(MutexQueue<int>(5).capacity(), 8u);
    EXPECT_EQ(MutexQueue<int>(1024).capacity(), 1024u);
}

TEST(MutexQueue, FifoFullAndEmpty) {
    MutexQueue<int> q(4);
    int             out = 0;
    EXPECT_FALSE(q.try_pop(out));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(99));
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(q.try_pop(out));
}

TEST(MutexQueue, FailedPushLeavesTheItemWithTheCaller) {
    MutexQueue<std::unique_ptr<int>> q(2);
    EXPECT_TRUE(q.try_push(std::make_unique<int>(1)));
    EXPECT_TRUE(q.try_push(std::make_unique<int>(2)));
    auto keep = std::make_unique<int>(3);
    EXPECT_FALSE(q.try_push(std::move(keep)));
    ASSERT_NE(keep, nullptr);
    EXPECT_EQ(*keep, 3);
}

TEST(MutexQueue, Stress1P1C) {
    MutexQueue<Item> q(1024);
    atom::test::run_queue_stress(q, {.producers = 1, .consumers = 1, .per_producer = 200'000});
}

// The M1 milestone: 4 producers, 1 consumer, 1M items each.
TEST(MutexQueue, Stress4P1C) {
    MutexQueue<Item> q(1024);
    atom::test::run_queue_stress(
        q, {.producers = 4, .consumers = 1, .per_producer = kUnderTsan ? 250'000u : 1'000'000u});
}

TEST(MutexQueue, Stress4P4CSmallRing) {
    MutexQueue<Item> q(16);
    atom::test::run_queue_stress(q, {.producers = 4, .consumers = 4, .per_producer = 100'000});
}

}  // namespace
