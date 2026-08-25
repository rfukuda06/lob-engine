#include "flow.h"

#include <algorithm>
#include <cmath>

namespace lob {

FairValue::FairValue(double start, double vol) : value_(start), vol_(vol) {}

Price FairValue::step(std::mt19937& rng) {
    std::normal_distribution<double> bump(0.0, vol_);
    value_ += bump(rng);
    if (value_ < 1.0) value_ = 1.0;  // fair value stays positive
    return current();
}

Price FairValue::current() const {
    return std::max<Price>(1, static_cast<Price>(std::llround(value_)));
}

}  // namespace lob
