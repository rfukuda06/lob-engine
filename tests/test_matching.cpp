#include "test_framework.h"

#include <optional>
#include <random>

#include "matching_engine.h"

using namespace lob;

// Scenario 1: a resting limit order appears in the book.
TEST(s01_resting_order_appears) {
    MatchingEngine engine;
    SubmitResult r = engine.submitLimit(Side::Buy, 10000, 30);
    CHECK(r.trades.empty());
    CHECK_EQ(r.restedQty, 30);
    CHECK_EQ(r.cancelledQty, 0);
    CHECK(engine.book().bestBid() == 10000);
    CHECK(engine.book().bestAsk() == std::nullopt);
    CHECK_EQ(engine.book().depth(Side::Buy, 10)[0].totalQty, 30);
}

// Scenario 2: exact cross fully fills both orders.
TEST(s02_exact_cross_full_fill) {
    MatchingEngine engine;
    SubmitResult buy = engine.submitLimit(Side::Buy, 10000, 30);
    SubmitResult sell = engine.submitLimit(Side::Sell, 10000, 30);
    CHECK_EQ(sell.trades.size(), 1);
    CHECK_EQ(sell.trades[0].quantity, 30);
    CHECK_EQ(sell.trades[0].price, 10000);
    CHECK_EQ(sell.trades[0].makerId, buy.id);
    CHECK_EQ(sell.trades[0].takerId, sell.id);
    CHECK_EQ(sell.restedQty, 0);
    CHECK_EQ(engine.book().orderCount(), 0);
    CHECK(engine.book().bestBid() == std::nullopt);
}

// Scenario 3: incoming order partially filled; remainder rests at its limit.
TEST(s03_incoming_partial_fill_rests) {
    MatchingEngine engine;
    engine.submitLimit(Side::Sell, 10050, 10);
    SubmitResult buy = engine.submitLimit(Side::Buy, 10050, 25);
    CHECK_EQ(buy.trades.size(), 1);
    CHECK_EQ(buy.trades[0].quantity, 10);
    CHECK_EQ(buy.restedQty, 15);
    CHECK(engine.book().bestBid() == 10050);
    CHECK_EQ(engine.book().depth(Side::Buy, 1)[0].totalQty, 15);
    CHECK(engine.book().bestAsk() == std::nullopt);
}

// Scenario 4: a partially filled resting order keeps its queue position.
TEST(s04_resting_partial_keeps_position) {
    MatchingEngine engine;
    SubmitResult a = engine.submitLimit(Side::Sell, 10050, 30);
    SubmitResult b = engine.submitLimit(Side::Sell, 10050, 20);
    SubmitResult first = engine.submitLimit(Side::Buy, 10050, 10);
    CHECK_EQ(first.trades[0].makerId, a.id);      // A hit first (FIFO)
    SubmitResult second = engine.submitLimit(Side::Buy, 10050, 25);
    CHECK_EQ(second.trades.size(), 2);
    CHECK_EQ(second.trades[0].makerId, a.id);     // A still front after
    CHECK_EQ(second.trades[0].quantity, 20);      // partial fill: 30-10 left
    CHECK_EQ(second.trades[1].makerId, b.id);
    CHECK_EQ(second.trades[1].quantity, 5);
}

// Scenario 5: price improvement — taker fills at the maker's better price.
TEST(s05_price_improvement) {
    MatchingEngine engine;
    engine.submitLimit(Side::Sell, 10000, 40);
    SubmitResult buy = engine.submitLimit(Side::Buy, 10010, 40);
    CHECK_EQ(buy.trades.size(), 1);
    CHECK_EQ(buy.trades[0].price, 10000);  // maker's price, not 10010
    CHECK_EQ(buy.restedQty, 0);
}

// Scenario 6: market order sweeps levels (slippage); remainder cancelled.
TEST(s06_market_sweep_and_remainder_cancelled) {
    MatchingEngine engine;
    engine.submitLimit(Side::Sell, 10000, 30);
    engine.submitLimit(Side::Sell, 10010, 20);
    SubmitResult mkt = engine.submitMarket(Side::Buy, 100);
    CHECK_EQ(mkt.trades.size(), 2);
    CHECK_EQ(mkt.trades[0].price, 10000);  // best level first
    CHECK_EQ(mkt.trades[0].quantity, 30);
    CHECK_EQ(mkt.trades[1].price, 10010);  // slippage: worse second price
    CHECK_EQ(mkt.trades[1].quantity, 20);
    CHECK_EQ(mkt.cancelledQty, 50);        // unfilled remainder dropped
    CHECK_EQ(mkt.restedQty, 0);            // market orders never rest
    CHECK(engine.book().bestAsk() == std::nullopt);
}

// Scenario 7: FIFO time priority within a price level.
TEST(s07_fifo_time_priority) {
    MatchingEngine engine;
    SubmitResult a = engine.submitLimit(Side::Buy, 10000, 20);
    SubmitResult b = engine.submitLimit(Side::Buy, 10000, 30);
    SubmitResult s1 = engine.submitLimit(Side::Sell, 10000, 20);
    CHECK_EQ(s1.trades[0].makerId, a.id);
    SubmitResult s2 = engine.submitLimit(Side::Sell, 10000, 30);
    CHECK_EQ(s2.trades[0].makerId, b.id);
}

// Scenario 8: price priority across levels.
TEST(s08_price_priority) {
    MatchingEngine engine;
    engine.submitLimit(Side::Buy, 10000, 10);
    SubmitResult better = engine.submitLimit(Side::Buy, 10010, 10);
    SubmitResult mkt = engine.submitMarket(Side::Sell, 10);
    CHECK_EQ(mkt.trades[0].makerId, better.id);  // higher bid fills first
    CHECK_EQ(mkt.trades[0].price, 10010);
}

// Scenario 9: cancellation semantics.
TEST(s09_cancel_semantics) {
    MatchingEngine engine;
    SubmitResult buy = engine.submitLimit(Side::Buy, 10000, 20);
    CHECK(engine.cancel(buy.id));
    CHECK(!engine.cancel(buy.id));   // second cancel fails
    CHECK(!engine.cancel(999999));   // unknown id fails
    // A cancelled order never matches:
    SubmitResult sell = engine.submitLimit(Side::Sell, 10000, 20);
    CHECK(sell.trades.empty());
    CHECK_EQ(sell.restedQty, 20);
}

// Scenario 10: a crossing limit order rests its leftover at its own limit.
TEST(s10_crossing_limit_rests_leftover) {
    MatchingEngine engine;
    engine.submitLimit(Side::Sell, 10000, 30);
    SubmitResult buy = engine.submitLimit(Side::Buy, 10000, 50);
    CHECK_EQ(buy.trades.size(), 1);
    CHECK_EQ(buy.trades[0].quantity, 30);
    CHECK_EQ(buy.restedQty, 20);
    CHECK(engine.book().bestBid() == 10000);  // leftover at its own limit
}

// Scenario 11: empty-book queries are well-defined.
TEST(s11_empty_book_queries) {
    MatchingEngine engine;
    CHECK(engine.book().bestBid() == std::nullopt);
    CHECK(engine.book().bestAsk() == std::nullopt);
    CHECK(engine.book().depth(Side::Buy, 10).empty());
    SubmitResult mkt = engine.submitMarket(Side::Buy, 50);  // no liquidity
    CHECK(mkt.trades.empty());
    CHECK_EQ(mkt.cancelledQty, 50);
}

// Scenario 12: seeded random burst — the book is never crossed.
TEST(s12_random_burst_never_crossed) {
    MatchingEngine engine;
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> kind(1, 100);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_int_distribution<Price> priceDist(10000 - 20, 10000 + 20);
    std::uniform_int_distribution<Quantity> qtyDist(1, 100);
    for (int i = 0; i < 10000; ++i) {
        Side side = sideDist(rng) == 0 ? Side::Buy : Side::Sell;
        if (kind(rng) <= 80) {
            engine.submitLimit(side, priceDist(rng), qtyDist(rng));
        } else {
            engine.submitMarket(side, qtyDist(rng));
        }
        auto bid = engine.book().bestBid();
        auto ask = engine.book().bestAsk();
        if (bid && ask) CHECK(*bid < *ask);
    }
}

TEST(post_only_rejects_when_it_would_cross) {
    lob::MatchingEngine engine;
    engine.submitLimit(lob::Side::Sell, 10005, 10);  // resting ask at 100.05
    // A post-only buy at 100.05 would cross the ask -> reject, no trade, no rest.
    lob::SubmitResult r = engine.submitLimit(lob::Side::Buy, 10005, 10,
                                             lob::TimeInForce::GTC, /*postOnly=*/true);
    CHECK(r.rejected);
    CHECK_EQ(r.trades.size(), 0);
    CHECK_EQ(r.restedQty, 0);
    CHECK_EQ(r.cancelledQty, 10);
}

TEST(post_only_rests_when_it_would_not_cross) {
    lob::MatchingEngine engine;
    engine.submitLimit(lob::Side::Sell, 10005, 10);  // ask at 100.05
    lob::SubmitResult r = engine.submitLimit(lob::Side::Buy, 10000, 10,
                                             lob::TimeInForce::GTC, /*postOnly=*/true);
    CHECK(!r.rejected);
    CHECK_EQ(r.trades.size(), 0);
    CHECK_EQ(r.restedQty, 10);
    CHECK(engine.book().bestBid() == std::optional<lob::Price>(10000));
}

TEST(ioc_matches_then_drops_remainder) {
    lob::MatchingEngine engine;
    engine.submitLimit(lob::Side::Sell, 10005, 4);  // only 4 available
    lob::SubmitResult r = engine.submitLimit(lob::Side::Buy, 10005, 10,
                                             lob::TimeInForce::IOC, /*postOnly=*/false);
    CHECK_EQ(r.trades.size(), 1);
    CHECK_EQ(r.trades[0].quantity, 4);
    CHECK_EQ(r.restedQty, 0);        // IOC never rests
    CHECK_EQ(r.cancelledQty, 6);     // remainder dropped
    CHECK(!engine.book().bestBid());
}

TEST(submit_dispatches_on_request_type) {
    lob::MatchingEngine engine;
    lob::OrderRequest limitReq{lob::OrderType::Limit, lob::Side::Buy, 10000, 5};
    lob::SubmitResult r = engine.submit(limitReq);
    CHECK_EQ(r.restedQty, 5);
    CHECK(engine.book().bestBid() == std::optional<lob::Price>(10000));
}
