// Negative test: proves the stress test and TSan can catch the bug they claim to catch.
//
// This file is compiled with ATOM_SPSC_WEAK_PUBLISH, which turns the producer's
// `tail_.store(release)` into `relaxed`. The consumer's acquire-load of tail_ then no longer
// synchronises with anything, so its read of a slot races with the producer's write of that
// slot. Under the tsan preset ctest runs this with WILL_FAIL: the test passes only if TSan
// reports the race and stops the process (halt_on_error=1).

#ifndef ATOM_SPSC_WEAK_PUBLISH
#error "build this file with ATOM_SPSC_WEAK_PUBLISH defined"
#endif

#include <gtest/gtest.h>

#include "atom/spsc_queue.hpp"
#include "stress.hpp"

namespace {

TEST(NegativeSpsc, RelaxedPublishIsARace) {
    atom::SpscQueue<atom::test::Item> q(64);
    atom::test::run_queue_stress(q, {.producers = 1, .consumers = 1, .per_producer = 200'000});
}

}  // namespace
