#include "flow.h"

#include <algorithm>
#include <cmath>
#include <vector>

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

FlowModel::FlowModel(unsigned seed, FlowParams params)
    : fair_(static_cast<double>(params.startPrice), params.fairValueVol),
      rng_(seed),
      p_(params) {}

Price FlowModel::fairValue() const { return fair_.current(); }
Price FlowModel::stepFairValue() { return fair_.step(rng_); }

Price FlowModel::referenceMid(const OrderBook& book) const {
    auto bid = book.bestBid();
    auto ask = book.bestAsk();
    if (bid && ask) return (*bid + *ask) / 2;
    return p_.startPrice;
}

std::vector<OrderRequest> FlowModel::generate(const OrderBook& book) {
    std::vector<OrderRequest> out;
    std::bernoulli_distribution isInformed(p_.informedFraction);
    std::bernoulli_distribution noiseTakes(p_.noiseMarketFraction);
    std::uniform_int_distribution<Quantity> size(p_.minSize, p_.maxSize);
    std::uniform_int_distribution<int> coin(0, 1);
    std::uniform_int_distribution<Price> off(1, p_.noiseSpreadTicks);
    const Price fair = fair_.current();

    for (int i = 0; i < p_.ordersPerStep; ++i) {
        if (isInformed(rng_)) {
            // Informed: take TOWARD fair value when it beats the touch. Market
            // orders already drop any unfilled remainder, so no IOC flag is needed.
            auto ask = book.bestAsk();
            auto bid = book.bestBid();
            if (ask && fair > *ask + p_.informedEdgeTicks) {
                out.push_back({OrderType::Market, Side::Buy, 0, size(rng_)});
            } else if (bid && fair < *bid - p_.informedEdgeTicks) {
                out.push_back({OrderType::Market, Side::Sell, 0, size(rng_)});
            }
            // else: informed trader sees no edge this arrival -> posts nothing
        } else {
            Side side = coin(rng_) == 0 ? Side::Buy : Side::Sell;
            if (noiseTakes(rng_)) {
                // Uninformed MARKETABLE order: crosses the spread and lets the
                // market maker capture it. This is the flow the MM *earns* from.
                out.push_back({OrderType::Market, side, 0, size(rng_)});
            } else {
                // Uninformed passive limit a few ticks off mid: adds book depth.
                Price mid = referenceMid(book);
                Price px = side == Side::Buy ? mid - off(rng_) : mid + off(rng_);
                px = std::max<Price>(px, 1);
                out.push_back({OrderType::Limit, side, px, size(rng_)});
            }
        }
    }
    return out;
}

}  // namespace lob
