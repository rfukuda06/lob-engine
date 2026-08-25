#pragma once
#include <optional>
#include <vector>

#include "matching_engine.h"
#include "order_book.h"
#include "types.h"

namespace lob {

enum class MMPolicy { InventorySkew, AvellanedaStoikov };

struct MMParams {
    Quantity quoteSize = 5;
    Price baseHalfSpread = 3;      // ticks (inventory-skew policy)
    double inventorySkewK = 0.5;   // reservation shift (ticks) per unit inventory
    Quantity maxInventory = 50;    // hard risk cap
    double gamma = 0.1;            // Avellaneda-Stoikov risk aversion
    double sigma = 2.0;            // vol estimate (ticks/step)
    double kappa = 1.5;            // order-arrival intensity
    long horizonSteps = 0;         // T; 0 => infinite-horizon (constant) A-S
};

// A market-maker fill (from the MM's own resting quote).
struct MmFill {
    long t;
    Price price;
    Side side;      // Buy = our bid was hit (we bought); Sell = our ask lifted (we sold)
    Quantity qty;
};

// Passive two-sided quoter. Quotes off the OBSERVABLE book mid only — it never
// sees fair value, which is exactly why informed flow can pick it off.
class MarketMaker {
public:
    MarketMaker(MMPolicy policy, MMParams params);

    // Cancel stale quotes and post fresh post-only bid/ask (subject to risk cap).
    void requote(MatchingEngine& engine, const OrderBook& book, long t, long T);
    // Attribute this step's fills to the MM (matches trade.makerId to our ids).
    void onTrades(const std::vector<Trade>& trades, long t);

    Quantity inventory() const { return inventory_; }
    long long cash() const { return cash_; }
    long long fillCount() const { return static_cast<long long>(fills_.size()); }
    double markToMarket(Price mark) const;  // cash_ + inventory_ * mark, tick*share units
    const std::vector<MmFill>& fills() const { return fills_; }
    std::optional<OrderId> bidId() const { return bidId_; }
    std::optional<OrderId> askId() const { return askId_; }

private:
    struct Quotes { Price bid; Price ask; bool valid; };
    Quotes computeQuotes(const OrderBook& book, long t, long T) const;
    static std::optional<double> midOf(const OrderBook& book);

    MMPolicy policy_;
    MMParams p_;
    Quantity inventory_ = 0;
    long long cash_ = 0;
    std::vector<MmFill> fills_;
    std::optional<OrderId> bidId_;
    std::optional<OrderId> askId_;
};

}  // namespace lob
