#include <cstdio>
#include <string>
#include <vector>

#include "analytics.h"
#include "market_maker.h"
#include "matching_engine.h"
#include "test_framework.h"

TEST(analytics_records_pnl_and_inventory) {
    lob::MatchingEngine engine;
    engine.submitLimit(lob::Side::Buy, 9995, 100);
    engine.submitLimit(lob::Side::Sell, 10005, 100);  // mid = 10000
    lob::MarketMaker mm(lob::MMPolicy::InventorySkew, {});
    mm.requote(engine, engine.book(), 1, 100);
    mm.onTrades({{999, *mm.bidId(), 9997, 10}}, 1);   // bought 10 @ 9997

    lob::Analytics an;
    an.recordStep(engine.book(), 10000, mm, 1);
    lob::Summary s = an.finalize(mm.fills());
    // pnl at fair 10000: cash(-99970) + inv(10)*10000 = 30 (ticks*shares)
    CHECK_EQ((long long)s.finalPnlFair, 30);
    CHECK_EQ(s.fills, 1);
    CHECK_EQ((long long)s.maxAbsInventory, 10);
}

TEST(markout_is_negative_when_the_mm_is_picked_off) {
    // MM buys at 10000 at t=1; fair then rises to 10050 by t=3 -> it bought too high
    // relative to where value was heading? Markout for a BUY = fair[t+d] - price.
    // Construct the OPPOSITE (adverse) case: MM SELLS at 10000, fair rises to 10050,
    // so selling was bad -> markout (for a sell = price - fair[t+d]) is negative.
    lob::Analytics an;
    lob::MatchingEngine engine;  // not used for values; we drive records directly
    lob::MarketMaker mm(lob::MMPolicy::InventorySkew, {});
    // record fair path: t=1..3 with fair rising 10000 -> 10025 -> 10050
    lob::MatchingEngine e; e.submitLimit(lob::Side::Buy, 9995, 100); e.submitLimit(lob::Side::Sell, 10005, 100);
    an.recordStep(e.book(), 10000, mm, 1);
    an.recordStep(e.book(), 10025, mm, 2);
    an.recordStep(e.book(), 10050, mm, 3);

    std::vector<lob::MmFill> fills = {{1, 10000, lob::Side::Sell, 5}};  // sold at 10000 at t=1
    lob::Summary s = an.finalize(fills);
    // markout at horizon 1: sold @10000, fair@t2 = 10025 -> (price - fair) = -25 < 0
    CHECK(s.markout1 < 0.0);
}

TEST(csv_is_written_with_header_and_rows) {
    lob::MatchingEngine engine;
    engine.submitLimit(lob::Side::Buy, 9995, 100);
    engine.submitLimit(lob::Side::Sell, 10005, 100);
    lob::MarketMaker mm(lob::MMPolicy::InventorySkew, {});
    lob::Analytics an;
    an.recordStep(engine.book(), 10000, mm, 1);
    an.recordStep(engine.book(), 10001, mm, 2);

    std::string path = "build/test_analytics_out.csv";
    an.writeCsv(path);
    std::FILE* f = std::fopen(path.c_str(), "r");
    CHECK(f != nullptr);
    if (f) {
        char line[256];
        char* header = std::fgets(line, sizeof(line), f);
        CHECK(header != nullptr);
        CHECK(std::string(header).rfind("t,", 0) == 0);  // header starts with "t,"
        int rows = 0;
        while (std::fgets(line, sizeof(line), f)) ++rows;
        CHECK_EQ(rows, 2);
        std::fclose(f);
    }
}

TEST(ofi_reflects_top_of_book_queue_changes) {
    lob::MarketMaker mm(lob::MMPolicy::InventorySkew, {});
    lob::Analytics an;
    lob::MatchingEngine e;
    e.submitLimit(lob::Side::Buy, 9995, 100);
    e.submitLimit(lob::Side::Sell, 10005, 100);
    an.recordStep(e.book(), 10000, mm, 1);         // first step: ofi = 0 (no prev)
    e.submitLimit(lob::Side::Buy, 9995, 50);        // bid queue grows 100 -> 150 at same price
    an.recordStep(e.book(), 10000, mm, 2);
    // eb = 150 (>= prev bid) - 100 (<= prev bid) = 50; ea = 0 -> ofi = 50.
    CHECK_EQ((long long)an.steps().back().ofi, 50);
}

TEST(effective_spread_measures_taker_cost) {
    lob::Analytics an;
    lob::MatchingEngine e; e.submitLimit(lob::Side::Buy, 9995, 100); e.submitLimit(lob::Side::Sell, 10005, 100);
    lob::MarketMaker mm(lob::MMPolicy::InventorySkew, {});
    // A buyer-initiated trade at 10005 when mid was 10000: effective = 2*|10005-10000| = 10.
    an.onTrades(lob::Side::Buy, {{999, 111, 10005, 10}}, 10000.0, 1);
    an.recordStep(e.book(), 10000, mm, 1);
    lob::Summary s = an.finalize(mm.fills());
    CHECK(s.effectiveSpread > 9.0 && s.effectiveSpread < 11.0);
}

TEST(kyle_lambda_is_positive_when_signed_volume_moves_price) {
    // Kyle's lambda = OLS slope of delta-mid on per-step signed volume. The
    // regressor must VARY (constant signed volume -> zero variance -> slope 0),
    // and the mid must move by an amount proportional to that step's signed volume.
    lob::Analytics an;
    lob::MarketMaker mm(lob::MMPolicy::InventorySkew, {});
    long midLevel = 10000;
    for (long t = 1; t <= 20; ++t) {
        lob::Quantity q = (t % 2 == 0) ? 5 : 15;   // signed buy volume alternates -> variance
        an.onTrades(lob::Side::Buy, {{999, 111, midLevel, q}}, (double)midLevel, t);
        midLevel += q;                             // price impact: mid rises by that volume
        lob::MatchingEngine et;
        et.submitLimit(lob::Side::Buy, midLevel - 5, 100);
        et.submitLimit(lob::Side::Sell, midLevel + 5, 100);  // mid = midLevel
        an.recordStep(et.book(), midLevel, mm, t);
    }
    // Each step's delta-mid equals its signed volume -> slope ~ 1 > 0.
    lob::Summary s = an.finalize(mm.fills());
    CHECK(s.kyleLambda > 0.0);
}

TEST(vpin_is_high_for_one_sided_flow) {
    lob::Analytics an;
    lob::MatchingEngine e; e.submitLimit(lob::Side::Buy, 9995, 100); e.submitLimit(lob::Side::Sell, 10005, 100);
    lob::MarketMaker mm(lob::MMPolicy::InventorySkew, {});
    for (long t = 1; t <= 50; ++t) {
        an.onTrades(lob::Side::Buy, {{999, 111, 10000, 10}}, 10000.0, t);  // all buys
        an.recordStep(e.book(), 10000, mm, t);
    }
    lob::Summary s = an.finalize(mm.fills());
    CHECK(s.vpin > 0.8);  // fully one-sided flow -> toxicity near 1
}
