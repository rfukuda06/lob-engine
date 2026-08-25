#pragma once
#include <random>
#include <vector>

#include "order_book.h"
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

struct FlowParams {
    double informedFraction = 0.15;   // P(an arrival is informed)
    double noiseMarketFraction = 0.4; // P(a NOISE arrival is a marketable order vs a passive limit)
    double fairValueVol = 2.0;        // ticks/step for the fair-value walk
    Price startPrice = 10000;         // $100.00
    int ordersPerStep = 3;            // arrivals per step
    Quantity minSize = 1;
    Quantity maxSize = 10;
    Price noiseSpreadTicks = 5;       // noise limits placed within +/- this of mid
    Price informedEdgeTicks = 1;      // fair must beat the touch by this to trigger a take
};

// Mixes uninformed (noise) and informed order flow. Informed arrivals trade
// TOWARD the latent fair value, picking off stale quotes -> adverse selection.
class FlowModel {
public:
    FlowModel(unsigned seed, FlowParams params);
    Price fairValue() const;                    // current V_t
    Price stepFairValue();                      // advance V_t one step, return it
    std::vector<OrderRequest> generate(const OrderBook& book);  // this step's arrivals

private:
    Price referenceMid(const OrderBook& book) const;  // book mid, else startPrice

    FairValue fair_;
    std::mt19937 rng_;
    FlowParams p_;
};

}  // namespace lob
