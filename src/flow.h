#pragma once
#include <random>

#include "types.h"

namespace lob {

// Exogenous efficient price V_t: a Gaussian random walk in ticks, independent
// of the book. Informed flow and analytics may read it; the market maker never
// does (that is what makes the MM adversely selectable).
class FairValue {
public:
    FairValue(double start, double vol);
    Price step(std::mt19937& rng);  // advance by N(0, vol^2); return rounded tick (>=1)
    Price current() const;          // current value rounded to a tick (>=1)

private:
    double value_;
    double vol_;
};

}  // namespace lob
