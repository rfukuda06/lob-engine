#pragma once
#include <random>
#include <vector>

#include "matching_engine.h"
#include "types.h"

namespace lob {

// Naive random order flow around a reference price. Not a market model —
// its only job is to make the book move plausibly (documented assumption).
class MarketSimulator {
public:
    static constexpr unsigned kDefaultSeed = 42;
    static constexpr Price kInitialReference = 10000;  // $100.00

    MarketSimulator(MatchingEngine& engine, unsigned seed = kDefaultSeed);

    // 10 bids and 10 asks spread over 1-10 ticks around the reference.
    void seedInitialLiquidity();

    // One event: ~85% limit order, ~10% market order, ~5% cancel one of the
    // simulator's own resting orders. Returns any trades that resulted.
    std::vector<Trade> step();

    // Random limit (90%) or market (10%) order around the current reference,
    // without applying it. Used by the benchmark to pre-generate load.
    OrderRequest drawOrderRequest();

    Price referencePrice() const { return referencePrice_; }

private:
    std::vector<Trade> submitRequest(const OrderRequest& req);
    void cancelRandomOwnOrder();

    MatchingEngine& engine_;
    std::mt19937 rng_;
    Price referencePrice_ = kInitialReference;
    std::vector<OrderId> restingIds_;  // sim-owned resting orders (lazily
                                       // pruned: filled ids drop when picked)
};

}  // namespace lob
