#pragma once
#include <optional>
#include <vector>

#include "order_book.h"
#include "types.h"

namespace lob {

// Accounts for every share of a submitted order:
// traded + restedQty + cancelledQty == requested quantity.
struct SubmitResult {
    OrderId id = 0;
    std::vector<Trade> trades;
    Quantity restedQty = 0;      // remainder now resting (limit GTC only)
    Quantity cancelledQty = 0;   // remainder dropped (market / IOC / post-only reject)
    bool rejected = false;       // post-only order that would have crossed
};

// The algorithm layer: decides when orders trade. Owns the book.
class MatchingEngine {
public:
    SubmitResult submitLimit(Side side, Price limit, Quantity qty,
                             TimeInForce tif = TimeInForce::GTC,
                             bool postOnly = false);
    SubmitResult submitMarket(Side side, Quantity qty);
    SubmitResult submit(const OrderRequest& req);  // dispatch on req.type
    bool cancel(OrderId id);
    const OrderBook& book() const { return book_; }

private:
    // Fills against the opposite side while the taker is willing to trade.
    // `limit` empty = market order (no price bound). Returns unfilled qty.
    Quantity matchAgainstBook(Side takerSide, OrderId takerId, Quantity qty,
                              std::optional<Price> limit,
                              std::vector<Trade>& trades);
    void assertNotCrossed() const;

    OrderBook book_;
    OrderId nextId_ = 1;  // single monotonic counter: user + simulator orders
};

}  // namespace lob
