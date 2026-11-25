#include <gtest/gtest.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "atom/config.hpp"

namespace{
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    static_assert(std::atomic<void*>::is_always_lock_free);

    TEST(Config, CacheLineIsAPowerOfTwoAndAtLeast64){
        EXPECT_GE(atom::kCacheLine, 64u);
        EXPECT_EQ(atom::kCacheLine & (atom::kCacheLine - 1), 0u);
    }

    TEST(Config, AlignedMembersLandOnSeperateLines){
        struct Padded {
            alignas(atom::kCacheLine) std::atomic<std::size_t> a{0};
            alignas(atom::kCacheLine) std::atomic<std::size_t> b{0};
        };
        Padded p;
        const auto addr_a = reinterpret_cast<std::uintptr_t>(&p.a);
        const auto addr_b = reinterpret_cast<std::uintptr_t>(&p.b);
        EXPECT_EQ(addr_a % atom::kCacheLine, 0u);
        EXPECT_EQ(addr_b % atom::kCacheLine, 0u);
        EXPECT_EQ(addr_a / atom::kCacheLine + 1, addr_b / atom::kCacheLine);
    }

    TEST(Config, CpuRelaxCanBeCalled) {
        for (int i = 0; i < 1000; ++i) atom::cpu_relax();
        SUCCEED();
    }
}