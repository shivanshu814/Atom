// Built with ATOM_SPSC_WEAK_PUBLISH: TSan must report a race.

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
