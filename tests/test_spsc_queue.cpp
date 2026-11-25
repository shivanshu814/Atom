#include <gtest/gtest.h>

#include <memory>

#include "atom/spsc_queue.hpp"
#include "stress.hpp"

namespace {

using atom::test::Item;
using atom::test::kUnderTsan;

static_assert(atom::ConcurrentQueue<atom::SpscQueue<Item>>);
static_assert(atom::ConcurrentQueue<atom::NaiveSpscQueue<Item>>);

// The layout the design promises: three separate lines, and nothing after the last one.
static_assert(alignof(atom::SpscQueue<Item>) == atom::kCacheLine);
static_assert(sizeof(atom::SpscQueue<Item>) == 3 * atom::kCacheLine);

template <class Q>
class SpscQueueTest : public ::testing::Test {};
using SpscQueues = ::testing::Types<atom::NaiveSpscQueue<Item>, atom::SpscQueue<Item>>;
TYPED_TEST_SUITE(SpscQueueTest, SpscQueues);

template <class Q>
using Rebind = std::conditional_t<std::is_same_v<Q, atom::SpscQueue<Item>>,
                                  atom::SpscQueue<std::unique_ptr<int>>,
                                  atom::NaiveSpscQueue<std::unique_ptr<int>>>;

TYPED_TEST(SpscQueueTest, FifoFullAndEmpty) {
    TypeParam q(4);
    Item      out;
    EXPECT_FALSE(q.try_pop(out));
    for (std::uint64_t i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(Item{0, 0, i}));
    EXPECT_FALSE(q.try_push(Item{0, 0, 99}));
    for (std::uint64_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out.seq, i);
    }
    EXPECT_FALSE(q.try_pop(out));
}

// Pushes and pops past the end of the ring many times, so the counter & mask wrap is used.
TYPED_TEST(SpscQueueTest, WrapsAroundTheRing) {
    TypeParam q(8);
    Item      out;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        ASSERT_TRUE(q.try_push(Item{0, 0, i}));
        ASSERT_TRUE(q.try_push(Item{0, 0, i + 1'000'000}));
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out.seq, i);
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out.seq, i + 1'000'000);
    }
}

TYPED_TEST(SpscQueueTest, MoveOnlyItemsAndFailedPushKeepsTheItem) {
    Rebind<TypeParam> q(2);
    EXPECT_TRUE(q.try_push(std::make_unique<int>(1)));
    EXPECT_TRUE(q.try_push(std::make_unique<int>(2)));
    auto keep = std::make_unique<int>(3);
    EXPECT_FALSE(q.try_push(std::move(keep)));
    ASSERT_NE(keep, nullptr);
    std::unique_ptr<int> out;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(*out, 1);
}

// A tiny ring makes the queue flip between full and empty constantly: the cached indices
// are refreshed on almost every call, which is where an ordering bug would show.
TYPED_TEST(SpscQueueTest, StressTinyRing) {
    TypeParam q(2);
    atom::test::run_queue_stress(q, {.producers = 1, .consumers = 1, .per_producer = 300'000});
}

TYPED_TEST(SpscQueueTest, StressLargeRing) {
    TypeParam q(65536);
    atom::test::run_queue_stress(
        q, {.producers = 1, .consumers = 1, .per_producer = kUnderTsan ? 1'000'000u : 5'000'000u});
}

}  // namespace
