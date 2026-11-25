#pragma once

#include <cstddef>
#include <thread>

namespace atom {

// Apple Silicon moves 128-byte lines.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

inline void cpu_relax() noexcept {
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

// Spin briefly, then yield.
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
