#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace atom::bench {

// Log-linear latency histogram (the HdrHistogram idea). Values below 64 get a bucket each;
// above that, every power of two is split into 32 equal buckets, so a reported value is at
// most about 3% above the true one, at any magnitude, in a fixed 15 KB array.
//
// One per thread, merged at the end: sharing one between threads would add contention to
// exactly the path being measured.
class Histogram {
public:
    void record(std::uint64_t v) noexcept {
        ++counts_[index(v)];
        ++total_;
        max_ = std::max(max_, v);
    }

    void merge(const Histogram& o) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        total_ += o.total_;
        max_ = std::max(max_, o.max_);
    }

    // The smallest bucket upper bound that covers fraction q of the samples, capped at the
    // largest value seen.
    std::uint64_t percentile(double q) const noexcept {
        if (total_ == 0) return 0;
        const auto    target = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(total_)));
        std::uint64_t seen   = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= target) return std::min(upper(i), max_);
        }
        return max_;
    }

    std::uint64_t max() const noexcept { return max_; }
    std::uint64_t count() const noexcept { return total_; }

    static constexpr unsigned    kSubBits = 5;
    static constexpr std::size_t kSub     = std::size_t{1} << kSubBits;  // 32 per power of two
    static constexpr std::size_t kBuckets = 2 * kSub + (64 - kSubBits - 1) * kSub;

    static constexpr std::size_t index(std::uint64_t v) noexcept {
        if (v < 2 * kSub) return static_cast<std::size_t>(v);
        const unsigned e = static_cast<unsigned>(std::bit_width(v)) - (kSubBits + 1);  // >= 1
        return 2 * kSub + (e - 1) * kSub + static_cast<std::size_t>((v >> e) - kSub);
    }

    static constexpr std::uint64_t upper(std::size_t i) noexcept {
        if (i < 2 * kSub) return i;
        const std::size_t j = i - 2 * kSub;
        const auto        e = static_cast<unsigned>(j / kSub + 1);
        const std::uint64_t m = j % kSub + kSub;
        return ((m + 1) << e) - 1;
    }

private:
    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t                       total_ = 0;
    std::uint64_t                       max_   = 0;
};

static_assert(Histogram::index(63) == 63);
static_assert(Histogram::index(64) == 64 && Histogram::upper(64) == 65);
static_assert(Histogram::index(127) == 95 && Histogram::upper(95) == 127);
static_assert(Histogram::index(128) == 96);
static_assert(Histogram::index(~std::uint64_t{0}) == Histogram::kBuckets - 1);

}  // namespace atom::bench
