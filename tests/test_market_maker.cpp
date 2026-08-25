#include "market_maker.h"
#include "matching_engine.h"
#include "test_framework.h"

// Build a book with a bid at 9995 and an ask at 10005 (mid = 10000).
static void seedBook(lob::MatchingEngine& engine) {
    engine.submitLimit(lob::Side::Buy, 9995, 100);
    engine.submitLimit(lob::Side::Sell, 10005, 100);
}

TEST(mm_buys_when_its_bid_is_hit) {
    lob::MMParams p;
    lob::MarketMaker mm(lob::MMPolicy::InventorySkew, p);
    lob::MatchingEngine engine;
    seedBook(engine);
    mm.requote(engine, engine.book(), 1, 1000);  // posts a bid + ask
    CHECK(mm.bidId().has_value());
    lob::OrderId bid = *mm.bidId();
    // Simulate the MM's bid getting hit for 5 at price 9997.
    std::vector<lob::Trade> trades = {{/*taker*/999, /*maker*/bid, 9997, 5}};
    mm.onTrades(trades, 1);
    CHECK_EQ(mm.inventory(), 5);                 // we bought 5
    CHECK_EQ(mm.cash(), -(long long)9997 * 5);   // paid 9997*5
    CHECK_EQ(mm.fillCount(), 1);
}

TEST(mm_sells_when_its_ask_is_lifted) {
    lob::MMParams p;
    lob::MarketMaker mm(lob::MMPolicy::InventorySkew, p);
    lob::MatchingEngine engine;
    seedBook(engine);
    mm.requote(engine, engine.book(), 1, 1000);
    CHECK(mm.askId().has_value());
    lob::OrderId ask = *mm.askId();
    std::vector<lob::Trade> trades = {{999, ask, 10003, 4}};
    mm.onTrades(trades, 1);
    CHECK_EQ(mm.inventory(), -4);                // we sold 4
    CHECK_EQ(mm.cash(), (long long)10003 * 4);
}

TEST(inventory_skew_quotes_lower_when_long) {
    lob::MMParams p;
    p.baseHalfSpread = 3;
    p.inventorySkewK = 1.0;
    p.quoteSize = 5;

    // Seed WIDE reference levels (bid 9900 / ask 10100, mid 10000) so the MM's own
    // quotes are unambiguously the best bid/ask and directly reflect its skew.
    auto seedWide = [](lob::MatchingEngine& e) {
        e.submitLimit(lob::Side::Buy, 9900, 100);
        e.submitLimit(lob::Side::Sell, 10100, 100);
    };

    lob::MatchingEngine e1; seedWide(e1);
    lob::MarketMaker flat(lob::MMPolicy::InventorySkew, p);
    flat.requote(e1, e1.book(), 1, 1000);
    lob::Price flatBid = *e1.book().bestBid();   // MM's bid at 9997

    // Long MM: fill its bid for +10, then requote on a fresh mid-10000 book.
    lob::MatchingEngine e2; seedWide(e2);
    lob::MarketMaker longMm(lob::MMPolicy::InventorySkew, p);
    longMm.requote(e2, e2.book(), 1, 1000);
    longMm.onTrades({{999, *longMm.bidId(), 9997, 10}}, 1);  // +10 inventory
    lob::MatchingEngine e3; seedWide(e3);
    longMm.requote(e3, e3.book(), 2, 1000);                  // cancels old ids, posts fresh
    lob::Price longBid = *e3.book().bestBid();   // MM's skewed bid at 9987

    CHECK_EQ(longMm.inventory(), 10);
    CHECK(longBid < flatBid);  // long inventory -> reservation shifts down -> lower bid
}
