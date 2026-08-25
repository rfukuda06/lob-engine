#include "matching_engine.h"

#include <algorithm>
#include <cassert>

namespace lob {

namespace {

// Is a taker limited at `limit` willing to trade at maker price `makerPrice`?
bool crosses(Side takerSide, Price limit, Price makerPrice) {
    return takerSide == Side::Buy ? makerPrice <= limit : makerPrice >= limit;
}

Side opposite(Side side) {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

}  // namespace

Quantity MatchingEngine::matchAgainstBook(Side takerSide, OrderId takerId,
                                          Quantity qty,
                                          std::optional<Price> limit,
                                          std::vector<Trade>& trades) {
    Side makerSide = opposite(takerSide);
    Quantity remaining = qty;
    while (remaining > 0) {
        const Order* maker = book_.peekFront(makerSide);
        if (maker == nullptr) break;  // no liquidity left on that side
        if (limit && !crosses(takerSide, *limit, maker->price)) break;
        Quantity fillQty = std::min(remaining, maker->quantity);
        // Trades always execute at the maker's price: price improvement
        // goes to the incoming (taker) order.
        trades.push_back({takerId, maker->id, maker->price, fillQty});
        book_.fillFront(makerSide, fillQty);
        remaining -= fillQty;
    }
    return remaining;
}

SubmitResult MatchingEngine::submitLimit(Side side, Price limit, Quantity qty,
                                         TimeInForce tif, bool postOnly) {
    assert(limit > 0 && qty > 0);  // REPL/simulator validate before calling
    SubmitResult result;
    result.id = nextId_++;
    if (postOnly) {
        const Order* opp = book_.peekFront(opposite(side));
        if (opp != nullptr && crosses(side, limit, opp->price)) {
            result.cancelledQty = qty;   // would cross -> reject, nothing trades/rests
            result.rejected = true;
            return result;
        }
    }
    Quantity remaining =
        matchAgainstBook(side, result.id, qty, limit, result.trades);
    if (remaining > 0) {
        if (tif == TimeInForce::IOC) {
            result.cancelledQty = remaining;  // IOC: drop the remainder
        } else {
            book_.addOrder({result.id, side, OrderType::Limit, limit, remaining});
            result.restedQty = remaining;
        }
    }
    assertNotCrossed();
    return result;
}

SubmitResult MatchingEngine::submit(const OrderRequest& req) {
    return req.type == OrderType::Limit
               ? submitLimit(req.side, req.price, req.quantity, req.tif, req.postOnly)
               : submitMarket(req.side, req.quantity);
}

SubmitResult MatchingEngine::submitMarket(Side side, Quantity qty) {
    assert(qty > 0);
    SubmitResult result;
    result.id = nextId_++;
    Quantity remaining = matchAgainstBook(side, result.id, qty, std::nullopt,
                                          result.trades);
    result.cancelledQty = remaining;  // market remainders never rest
    assertNotCrossed();
    return result;
}

bool MatchingEngine::cancel(OrderId id) { return book_.cancel(id); }

void MatchingEngine::assertNotCrossed() const {
#ifndef NDEBUG
    // Invariant: after any submit completes, the book is never crossed —
    // matching ran to completion before anything rested.
    auto bid = book_.bestBid();
    auto ask = book_.bestAsk();
    assert(!(bid && ask) || *bid < *ask);
#endif
}

}  // namespace lob
