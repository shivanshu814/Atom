#pragma once

#include <cstddef>
#include <thread>

namespace atom {

// Size of the block the CPU moves between cores. Two threads writing different variables on
// one line make that line bounce between their cores (false sharing), so anything one thread
// writes often gets a line of its own: alignas(kCacheLine).
//
// Not std::hardware_destructive_interference_size: Apple's libc++ reports 256 for it on
// M-series, GCC warns that its value can change between compiler versions, and we want a
// number we chose and can explain.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

// Hint for spin loops: "I am waiting, not working". Saves power and frees the core's shared
// resources for a sibling hyper-thread. It does not sleep and costs a few cycles.
inline void cpu_relax() noexcept {
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

// What a thread does when try_push / try_pop fails: spin briefly (the other side is usually
// a few nanoseconds away), then yield so a waiting thread does not starve the one it waits
// for when there are more threads than cores. Tests and benchmarks use this for every queue,
// so no queue gets a better waiting strategy than another.
class Backoff {
public:
    void pause() noexcept {
        if (spins_ < kSpinLimit) {
            ++spins_;
            cpu_relax();
        } else {
            std::this_thread::yield();
        }
    }
    void reset() noexcept { spins_ = 0; }

private:
    static constexpr unsigned kSpinLimit = 64;
    unsigned                  spins_     = 0;
};

}  // namespace atom
