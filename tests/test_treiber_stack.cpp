#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "atom/reclaim/epoch.hpp"
#include "atom/reclaim/hazard_pointer.hpp"
#include "atom/treiber_stack.hpp"
#include "stress.hpp"

namespace {

using atom::EpochReclaim;
using atom::HazardPointers;
using atom::NoReclaim;
using atom::TreiberStack;
using atom::test::Item;
using atom::test::kUnderTsan;

struct Counted {
    static inline std::atomic<long> live{0};
    int                             v = 0;
    explicit Counted(int x) : v(x) { live.fetch_add(1, std::memory_order_relaxed); }
    Counted(const Counted& o) : v(o.v) { live.fetch_add(1, std::memory_order_relaxed); }
    Counted(Counted&& o) noexcept : v(o.v) { live.fetch_add(1, std::memory_order_relaxed); }
    ~Counted() { live.fetch_sub(1, std::memory_order_relaxed); }
};

template <class R>
void drain_main_thread() {
    if constexpr (std::is_same_v<R, HazardPointers>) atom::hp::drain_this_thread();
    if constexpr (std::is_same_v<R, EpochReclaim>) atom::ebr::drain_this_thread();
}

template <class R>
std::int64_t pending() {
    if constexpr (std::is_same_v<R, HazardPointers>) return atom::hp::pending();
    else if constexpr (std::is_same_v<R, EpochReclaim>) return atom::ebr::pending();
    else return 0;
}

template <class R>
class TreiberStackTest : public ::testing::Test {};
using Policies = ::testing::Types<NoReclaim, HazardPointers, EpochReclaim>;
TYPED_TEST_SUITE(TreiberStackTest, Policies);

TYPED_TEST(TreiberStackTest, LifoAndEmpty) {
    TreiberStack<int, TypeParam> s;
    EXPECT_TRUE(s.empty());
    EXPECT_FALSE(s.pop().has_value());
    for (int i = 0; i < 100; ++i) s.push(i);
    for (int i = 99; i >= 0; --i) {
        auto v = s.pop();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, i);
    }
    EXPECT_FALSE(s.pop().has_value());
    drain_main_thread<TypeParam>();
}

TYPED_TEST(TreiberStackTest, MoveOnlyValues) {
    TreiberStack<std::unique_ptr<int>, TypeParam> s;
    s.push(std::make_unique<int>(5));
    auto v = s.pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(**v, 5);
    drain_main_thread<TypeParam>();
}

TYPED_TEST(TreiberStackTest, Stress4Threads) {
    TreiberStack<Item, TypeParam> s;
    atom::test::run_stack_stress(s, 4, kUnderTsan ? 50'000u : 200'000u);
    drain_main_thread<TypeParam>();
}

TYPED_TEST(TreiberStackTest, Stress8Threads) {
    TreiberStack<Item, TypeParam> s;
    atom::test::run_stack_stress(s, 8, kUnderTsan ? 25'000u : 100'000u);
    drain_main_thread<TypeParam>();
}

TYPED_TEST(TreiberStackTest, EveryNodeIsFreed) {
    if constexpr (std::is_same_v<TypeParam, NoReclaim>) {
        GTEST_SKIP() << "NoReclaim leaks popped nodes by design";
    } else {
        const long live_before = Counted::live.load(std::memory_order_relaxed);
        {
            TreiberStack<Counted, TypeParam> s;
            std::vector<std::thread>         threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&s, t] {
                    for (int i = 0; i < 20'000; ++i) {
                        s.push(Counted(t * 100'000 + i));
                        (void)s.pop();
                    }
                });
            }
            for (auto& th : threads) th.join();
            while (s.pop()) {
            }
            drain_main_thread<TypeParam>();
        }
        EXPECT_EQ(Counted::live.load(std::memory_order_relaxed), live_before);
        EXPECT_EQ(pending<TypeParam>(), 0);
    }
}

template <class R>
std::int64_t pending_while_one_thread_stalls(int ops_per_worker) {
    TreiberStack<Item, R> s;
    for (std::uint64_t i = 0; i < 16; ++i) s.push(Item{0, 0, i});

    std::atomic<bool> stalled{false}, release_stall{false};
    std::thread       staller([&] {
        int               dummy = 0;
        std::atomic<int*> src{&dummy};
        typename R::Guard guard;
        [[maybe_unused]] int* held = guard.protect(src);
        stalled.store(true, std::memory_order_release);
        while (!release_stall.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    while (!stalled.load(std::memory_order_acquire)) std::this_thread::yield();

    const std::int64_t before = pending<R>();
    std::atomic<int>   done{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 2; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < ops_per_worker; ++i) {
                s.push(Item{static_cast<std::uint32_t>(t), 0, static_cast<std::uint64_t>(i)});
                (void)s.pop();
            }
            done.fetch_add(1, std::memory_order_release);
            // Epoch drain at thread exit waits for the staller, so measure first.
            while (!release_stall.load(std::memory_order_acquire)) std::this_thread::yield();
        });
    }
    while (done.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    const std::int64_t during = pending<R>() - before;

    release_stall.store(true, std::memory_order_release);
    staller.join();
    for (auto& w : workers) w.join();
    while (s.pop()) {
    }
    drain_main_thread<R>();
    EXPECT_EQ(pending<R>(), 0) << "everything is freed once the stalled thread leaves";
    return during;
}

TEST(Reclamation, EpochsGrowWhileOneThreadStalls) {
    const int          ops    = 20'000;
    const std::int64_t during = pending_while_one_thread_stalls<EpochReclaim>(ops);
    EXPECT_GE(during, 2 * ops - 200) << "almost every popped node stays unfreed";
}

TEST(Reclamation, HazardPointersStayBoundedWhileOneThreadStalls) {
    const int          ops    = 20'000;
    const std::int64_t during = pending_while_one_thread_stalls<HazardPointers>(ops);
    EXPECT_LE(during, static_cast<std::int64_t>(2 * atom::hp::kScanThreshold + 1));
}

}  // namespace
