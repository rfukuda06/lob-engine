#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "market_maker.h"
#include "order_book.h"
#include "types.h"

namespace lob {

struct StepRecord {
    long t = 0;
    Price fair = 0;
    Price bid = 0;            // best bid (0 if none)
    Price ask = 0;            // best ask (0 if none)
    double mid = 0.0;         // (bid+ask)/2 if both present, else fair
    double microprice = 0.0;
    Quantity bidSize = 0;     // top-of-book queue sizes
    Quantity askSize = 0;
    double ofi = 0.0;         // order-flow imbalance vs the previous step (0 on step 1)
    Quantity inventory = 0;
    long long cash = 0;
    double pnlMid = 0.0;
    double pnlFair = 0.0;
    Quantity signedVolume = 0;  // sum of signedTaker*qty this step (filled in Task 7)
};

struct MarketTrade {          // one executed trade, market-wide (filled in Task 7)
    long t = 0;
    Price price = 0;
    double midAtTrade = 0.0;
    int signedTaker = 0;      // +1 buyer-initiated, -1 seller-initiated
    Quantity qty = 0;
};

struct Summary {
    double finalPnlMid = 0.0;
    double finalPnlFair = 0.0;
    long long fills = 0;
    Quantity maxAbsInventory = 0;
    double sharpe = 0.0;
    double markout1 = 0.0, markout5 = 0.0, markout20 = 0.0;
    double effectiveSpread = 0.0;   // Task 7
    double realizedSpread = 0.0;    // Task 7
    double adverseSelection = 0.0;  // Task 7
    double kyleLambda = 0.0;        // Task 7
    double vpin = 0.0;              // Task 7
};

// Read-only observer. The engine is never mutated here.
class Analytics {
public:
    // Called per aggressive submission this step (taker side known by the driver).
    void onTrades(Side takerSide, const std::vector<Trade>& trades, double mid, long t);
    // Called once per step after flow + MM updates are applied.
    void recordStep(const OrderBook& book, Price fair, const MarketMaker& mm, long t);
    // Produce all metrics. `mmFills` is the market maker's own fills (mm.fills()).
    Summary finalize(const std::vector<MmFill>& mmFills) const;
    void printSummary(const Summary& s) const;
    void writeCsv(const std::string& path) const;

    const std::vector<StepRecord>& steps() const { return steps_; }

private:
    std::vector<StepRecord> steps_;
    std::vector<MarketTrade> trades_;
    Quantity pendingSignedVolume_ = 0;  // accumulates within a step, flushed in recordStep
    // Previous-step top of book, for the OFI (order-flow imbalance) computation.
    Price prevBid_ = 0;
    Price prevAsk_ = 0;
    Quantity prevBidSize_ = 0;
    Quantity prevAskSize_ = 0;
    bool havePrev_ = false;
};

}  // namespace lob
