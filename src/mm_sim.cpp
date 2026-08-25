#include "mm_sim.h"

#include <algorithm>

#include "matching_engine.h"
#include "order_book.h"

namespace lob {

namespace {

// Deterministic starting liquidity: 10 levels each side, at start +/- (5..14) ticks.
// The nearest level (+/-5) sits OUTSIDE the market maker's half-spread, so the MM's
// quotes rest at the touch from the first step and can actually be filled.
void seedLiquidity(MatchingEngine& engine, Price start) {
    for (Price k = 1; k <= 10; ++k) {
        engine.submitLimit(Side::Buy, start - (4 + k), 50);
        engine.submitLimit(Side::Sell, start + (4 + k), 50);
    }
}

double midOrFair(const OrderBook& book, Price fair) {
    auto bid = book.bestBid();
    auto ask = book.bestAsk();
    if (bid && ask) return (static_cast<double>(*bid) + static_cast<double>(*ask)) / 2.0;
    return static_cast<double>(fair);
}

}  // namespace

Summary runMarketMakingSim(const MmSimConfig& cfg) {
    MatchingEngine engine;
    FlowModel flow(cfg.seed, cfg.flow);
    MarketMaker mm(cfg.policy, cfg.mm);
    Analytics analytics;

    seedLiquidity(engine, cfg.flow.startPrice);

    for (long t = 1; t <= cfg.steps; ++t) {
        Price fair = flow.stepFairValue();          // latent value advances (MM can't see it)
        mm.requote(engine, engine.book(), t, cfg.steps);  // post fresh post-only quotes
        double stepMid = midOrFair(engine.book(), fair);

        std::vector<OrderRequest> arrivals = flow.generate(engine.book());
        for (const OrderRequest& req : arrivals) {
            SubmitResult res = engine.submit(req);
            analytics.onTrades(req.side, res.trades, stepMid, t);
            mm.onTrades(res.trades, t);
        }
        analytics.recordStep(engine.book(), fair, mm, t);
    }

    Summary summary = analytics.finalize(mm.fills());
    if (!cfg.csvPath.empty()) analytics.writeCsv(cfg.csvPath);
    if (!cfg.quiet) analytics.printSummary(summary);
    return summary;
}

}  // namespace lob
