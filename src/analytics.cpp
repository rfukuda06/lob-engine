#include "analytics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lob {

void Analytics::onTrades(Side takerSide, const std::vector<Trade>& trades,
                         double mid, long t) {
    int sign = takerSide == Side::Buy ? 1 : -1;
    for (const Trade& tr : trades) {
        trades_.push_back({t, tr.price, mid, sign, tr.quantity});
        pendingSignedVolume_ += static_cast<Quantity>(sign) * tr.quantity;
    }
}

void Analytics::recordStep(const OrderBook& book, Price fair,
                           const MarketMaker& mm, long t) {
    StepRecord r;
    r.t = t;
    r.fair = fair;
    auto bid = book.bestBid();
    auto ask = book.bestAsk();
    r.bid = bid ? *bid : 0;
    r.ask = ask ? *ask : 0;
    r.mid = (bid && ask) ? (static_cast<double>(*bid) + static_cast<double>(*ask)) / 2.0
                         : static_cast<double>(fair);
    auto bidLevels = book.depth(Side::Buy, 1);
    auto askLevels = book.depth(Side::Sell, 1);
    r.bidSize = bidLevels.empty() ? 0 : bidLevels.front().totalQty;
    r.askSize = askLevels.empty() ? 0 : askLevels.front().totalQty;
    if (r.bidSize + r.askSize > 0 && bid && ask) {
        r.microprice = (static_cast<double>(r.bidSize) * static_cast<double>(*ask) +
                        static_cast<double>(r.askSize) * static_cast<double>(*bid)) /
                       static_cast<double>(r.bidSize + r.askSize);
    } else {
        r.microprice = r.mid;
    }
    r.inventory = mm.inventory();
    r.cash = mm.cash();
    r.pnlMid = mm.markToMarket(static_cast<Price>(std::llround(r.mid)));
    r.pnlFair = mm.markToMarket(fair);

    // Order-flow imbalance (Cont-Kukanov-Stoikov) vs the previous step's top of
    // book. Positive OFI = net buying pressure. Zero on the first recorded step.
    if (havePrev_) {
        Quantity eb = 0, ea = 0;
        if (r.bid >= prevBid_) eb += r.bidSize;      // demand added at/above prior bid
        if (r.bid <= prevBid_) eb -= prevBidSize_;   // demand withdrawn at/below prior bid
        if (r.ask <= prevAsk_) ea += r.askSize;      // supply added at/below prior ask
        if (r.ask >= prevAsk_) ea -= prevAskSize_;   // supply withdrawn at/above prior ask
        r.ofi = static_cast<double>(eb - ea);
    }
    prevBid_ = r.bid;
    prevAsk_ = r.ask;
    prevBidSize_ = r.bidSize;
    prevAskSize_ = r.askSize;
    havePrev_ = true;

    r.signedVolume = pendingSignedVolume_;
    pendingSignedVolume_ = 0;
    steps_.push_back(r);
}

namespace {

// fair value recorded at step time `t` (steps are 1-based and in order).
double fairAt(const std::vector<StepRecord>& steps, long t) {
    if (t < 1 || static_cast<std::size_t>(t) > steps.size()) return 0.0;
    return static_cast<double>(steps[static_cast<std::size_t>(t - 1)].fair);
}

double midAtStep(const std::vector<StepRecord>& steps, long t) {
    if (t < 1 || static_cast<std::size_t>(t) > steps.size()) return 0.0;
    return steps[static_cast<std::size_t>(t - 1)].mid;
}

double averageMarkout(const std::vector<StepRecord>& steps,
                      const std::vector<MmFill>& fills, long horizon) {
    double sum = 0.0;
    long n = 0;
    long maxT = static_cast<long>(steps.size());
    for (const MmFill& f : fills) {
        long tf = f.t + horizon;
        if (tf > maxT) continue;
        double futureFair = fairAt(steps, tf);
        // Buy: profit if value rose (fair - price). Sell: profit if value fell (price - fair).
        double m = (f.side == Side::Buy) ? (futureFair - f.price)
                                         : (f.price - futureFair);
        sum += m;
        ++n;
    }
    return n > 0 ? sum / n : 0.0;
}

}  // namespace

Summary Analytics::finalize(const std::vector<MmFill>& mmFills) const {
    Summary s;
    if (!steps_.empty()) {
        s.finalPnlMid = steps_.back().pnlMid;
        s.finalPnlFair = steps_.back().pnlFair;
    }
    s.fills = static_cast<long long>(mmFills.size());

    for (const StepRecord& r : steps_) {
        Quantity a = r.inventory < 0 ? -r.inventory : r.inventory;
        if (a > s.maxAbsInventory) s.maxAbsInventory = a;
    }

    // Sharpe of per-step pnlFair increments.
    if (steps_.size() >= 2) {
        std::vector<double> d;
        d.reserve(steps_.size() - 1);
        for (std::size_t i = 1; i < steps_.size(); ++i)
            d.push_back(steps_[i].pnlFair - steps_[i - 1].pnlFair);
        double mean = 0.0;
        for (double x : d) mean += x;
        mean /= static_cast<double>(d.size());
        double var = 0.0;
        for (double x : d) var += (x - mean) * (x - mean);
        var /= static_cast<double>(d.size());
        double sd = std::sqrt(var);
        s.sharpe = sd > 0.0 ? mean / sd : 0.0;
    }

    s.markout1 = averageMarkout(steps_, mmFills, 1);
    s.markout5 = averageMarkout(steps_, mmFills, 5);
    s.markout20 = averageMarkout(steps_, mmFills, 20);

    // --- Market microstructure metrics ---
    // Effective spread (taker cost) and realized spread (net of impact at horizon d).
    const long kRealizedHorizon = 5;
    if (!trades_.empty()) {
        double effSum = 0.0, realSum = 0.0;
        long realN = 0;
        for (const MarketTrade& mt : trades_) {
            effSum += 2.0 * mt.signedTaker * (static_cast<double>(mt.price) - mt.midAtTrade);
            long tf = mt.t + kRealizedHorizon;
            if (static_cast<std::size_t>(tf) <= steps_.size()) {
                double midFuture = midAtStep(steps_, tf);
                realSum += 2.0 * mt.signedTaker * (static_cast<double>(mt.price) - midFuture);
                ++realN;
            }
        }
        s.effectiveSpread = effSum / static_cast<double>(trades_.size());
        s.realizedSpread = realN > 0 ? realSum / static_cast<double>(realN) : 0.0;
        s.adverseSelection = s.effectiveSpread - s.realizedSpread;
    }

    // Kyle's lambda: OLS slope of delta-mid on signed volume across steps.
    if (steps_.size() >= 2) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        long n = 0;
        for (std::size_t i = 1; i < steps_.size(); ++i) {
            double x = static_cast<double>(steps_[i].signedVolume);
            double y = steps_[i].mid - steps_[i - 1].mid;
            sx += x; sy += y; sxx += x * x; sxy += x * y; ++n;
        }
        double denom = n * sxx - sx * sx;
        if (denom != 0.0) s.kyleLambda = (n * sxy - sx * sy) / denom;
    }

    // VPIN: bucket trades by cumulative volume; average |buy-sell|/bucketVol.
    if (!trades_.empty()) {
        Quantity totalVol = 0;
        for (const MarketTrade& mt : trades_) totalVol += mt.qty;
        const int kBuckets = 20;
        Quantity bucketVol = totalVol / kBuckets;
        if (bucketVol > 0) {
            double vpinSum = 0.0;
            int buckets = 0;
            Quantity acc = 0, buy = 0, sell = 0;
            for (const MarketTrade& mt : trades_) {
                if (mt.signedTaker > 0) buy += mt.qty; else sell += mt.qty;
                acc += mt.qty;
                if (acc >= bucketVol) {
                    Quantity diff = buy > sell ? buy - sell : sell - buy;
                    vpinSum += static_cast<double>(diff) / static_cast<double>(acc);
                    ++buckets;
                    acc = 0; buy = 0; sell = 0;
                }
            }
            s.vpin = buckets > 0 ? vpinSum / buckets : 0.0;
        }
    }
    return s;
}

void Analytics::printSummary(const Summary& s) const {
    std::printf("\n=== Market-Making Summary ===\n");
    std::printf("Final PnL (mark-to-mid):   %.2f ticks*shares\n", s.finalPnlMid);
    std::printf("Final PnL (mark-to-fair):  %.2f ticks*shares\n", s.finalPnlFair);
    std::printf("MM fills:                  %lld\n", s.fills);
    std::printf("Max |inventory|:           %lld\n", (long long)s.maxAbsInventory);
    std::printf("Sharpe (per-step, fair):   %.4f\n", s.sharpe);
    std::printf("Markout @1/@5/@20:         %.2f / %.2f / %.2f ticks\n",
                s.markout1, s.markout5, s.markout20);
    std::printf("Effective spread:          %.2f ticks\n", s.effectiveSpread);
    std::printf("Realized spread:           %.2f ticks\n", s.realizedSpread);
    std::printf("Adverse selection:         %.2f ticks\n", s.adverseSelection);
    std::printf("Kyle's lambda:             %.5f ticks/share\n", s.kyleLambda);
    std::printf("VPIN:                      %.4f\n", s.vpin);
}

void Analytics::writeCsv(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "t,fair,bid,ask,mid,microprice,bid_size,ask_size,ofi,"
                    "inventory,cash,pnl_mid,pnl_fair,signed_volume\n");
    for (const StepRecord& r : steps_) {
        std::fprintf(f, "%ld,%lld,%lld,%lld,%.2f,%.2f,%lld,%lld,%.2f,%lld,%lld,%.2f,%.2f,%lld\n",
                     r.t, (long long)r.fair, (long long)r.bid, (long long)r.ask, r.mid,
                     r.microprice, (long long)r.bidSize, (long long)r.askSize, r.ofi,
                     (long long)r.inventory, (long long)r.cash, r.pnlMid, r.pnlFair,
                     (long long)r.signedVolume);
    }
    std::fclose(f);
}

}  // namespace lob
