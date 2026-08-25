#include <random>

#include "flow.h"
#include "test_framework.h"

TEST(fair_value_is_deterministic_for_a_seed) {
    std::mt19937 a(123), b(123);
    lob::FairValue fa(10000, 2.0), fb(10000, 2.0);
    for (int i = 0; i < 50; ++i) CHECK_EQ(fa.step(a), fb.step(b));
}

TEST(fair_value_moves_from_its_start) {
    std::mt19937 rng(7);
    lob::FairValue f(10000, 5.0);
    bool moved = false;
    for (int i = 0; i < 50; ++i) {
        if (f.step(rng) != 10000) { moved = true; break; }
    }
    CHECK(moved);
    CHECK(f.current() >= 1);  // never non-positive
}
