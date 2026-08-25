#include "market_maker.h"

#include <algorithm>
#include <cmath>

namespace lob {

MarketMaker::MarketMaker(MMPolicy policy, MMParams params)
    : policy_(policy), p_(params) {}

std::optional<double> MarketMaker::midOf(const OrderBook& book) {
    auto bid = book.bestBid();
    auto ask = book.bestAsk();
    if (bid && ask) return (static_cast<double>(*bid) + static_cast<double>(*ask)) / 2.0;
    return std::nullopt;
}

double MarketMaker::markToMarket(Price mark) const {
    return static_cast<double>(cash_) + static_cast<double>(inventory_) * mark;
}

MarketMaker::Quotes MarketMaker::computeQuotes(const OrderBook& book, long t,
                                               long T) const {
    auto midOpt = midOf(book);
    if (!midOpt) return {0, 0, false};
    double mid = *midOpt;

    double reservation;
    double halfSpread;
    if (policy_ == MMPolicy::InventorySkew) {
        reservation = mid - static_cast<double>(inventory_) * p_.inventorySkewK;
        halfSpread = static_cast<double>(p_.baseHalfSpread);
        (void)t; (void)T;
    } else {  // MMPolicy::AvellanedaStoikov
        double tau = 1.0;
        if (p_.horizonSteps > 0) {
            tau = static_cast<double>(p_.horizonSteps - t) / static_cast<double>(p_.horizonSteps);
            if (tau < 0.0) tau = 0.0;
        }
        double s2 = p_.sigma * p_.sigma;
        reservation = mid - static_cast<double>(inventory_) * p_.gamma * s2 * tau;
        halfSpread = 0.5 * p_.gamma * s2 * tau + (1.0 / p_.gamma) * std::log(1.0 + p_.gamma / p_.kappa);
    }

    Price bid = static_cast<Price>(std::llround(reservation - halfSpread));
    Price ask = static_cast<Price>(std::llround(reservation + halfSpread));
    if (bid >= ask) bid = ask - 1;      // keep at least a one-tick spread
    if (bid < 1) bid = 1;
    if (ask <= bid) ask = bid + 1;
    return {bid, ask, true};
}

void MarketMaker::requote(MatchingEngine& engine, const OrderBook& book, long t,
                          long T) {
    if (bidId_) { engine.cancel(*bidId_); bidId_.reset(); }
    if (askId_) { engine.cancel(*askId_); askId_.reset(); }

    Quotes q = computeQuotes(book, t, T);
    if (!q.valid) return;

    bool wantBid = inventory_ < p_.maxInventory;    // stop bidding when too long
    bool wantAsk = inventory_ > -p_.maxInventory;   // stop offering when too short
    if (wantBid) {
        SubmitResult r = engine.submitLimit(Side::Buy, q.bid, p_.quoteSize,
                                            TimeInForce::GTC, /*postOnly=*/true);
        if (r.restedQty > 0) bidId_ = r.id;
    }
    if (wantAsk) {
        SubmitResult r = engine.submitLimit(Side::Sell, q.ask, p_.quoteSize,
                                            TimeInForce::GTC, /*postOnly=*/true);
        if (r.restedQty > 0) askId_ = r.id;
    }
}

void MarketMaker::onTrades(const std::vector<Trade>& trades, long t) {
    for (const Trade& tr : trades) {
        if (bidId_ && tr.makerId == *bidId_) {
            inventory_ += tr.quantity;
            cash_ -= static_cast<long long>(tr.price) * tr.quantity;
            fills_.push_back({t, tr.price, Side::Buy, tr.quantity});
        } else if (askId_ && tr.makerId == *askId_) {
            inventory_ -= tr.quantity;
            cash_ += static_cast<long long>(tr.price) * tr.quantity;
            fills_.push_back({t, tr.price, Side::Sell, tr.quantity});
        }
    }
}

}  // namespace lob
