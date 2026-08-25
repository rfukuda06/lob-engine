#include <random>

#include "flow.h"
#include "matching_engine.h"
#include "test_framework.h"

TEST(fair_value_is_deterministic_for_a_seed) {
    std::mt19937 a(123), b(123);
    lob::FairValue fa(10000, 2.0), fb(10000, 2.0);
    for (int i = 0; i < 50; ++i) CHECK_EQ(fa.step(a), fb.step(b));
}

TEST(fair_value_moves_from_its_start) {
    std::mt19937 rng(7);
    lob::FairValue f(10000, 5.0);
    bool moved = false;
    for (int i = 0; i < 50; ++i) {
        if (f.step(rng) != 10000) { moved = true; break; }
    }
    CHECK(moved);
    CHECK(f.current() >= 1);  // never non-positive
}

TEST(informed_flow_lifts_offer_when_fair_is_above_the_ask) {
    lob::MatchingEngine engine;
    engine.submitLimit(lob::Side::Sell, 10000, 100);  // ask at 100.00
    lob::FlowParams p;
    p.informedFraction = 1.0;   // force informed
    p.ordersPerStep = 1;
    p.startPrice = 10050;       // fair well above the ask
    lob::FlowModel flow(1, p);
    auto reqs = flow.generate(engine.book());
    CHECK_EQ(reqs.size(), 1);
    CHECK(reqs[0].side == lob::Side::Buy);
    CHECK(reqs[0].type == lob::OrderType::Market);
}

TEST(noise_flow_posts_limits_near_the_mid) {
    lob::MatchingEngine engine;
    engine.submitLimit(lob::Side::Buy, 9995, 100);
    engine.submitLimit(lob::Side::Sell, 10005, 100);  // mid = 10000
    lob::FlowParams p;
    p.informedFraction = 0.0;      // force noise
    p.noiseMarketFraction = 0.0;   // force the passive-limit path for this test
    p.ordersPerStep = 20;
    p.noiseSpreadTicks = 5;
    lob::FlowModel flow(2, p);
    auto reqs = flow.generate(engine.book());
    CHECK(reqs.size() == 20);
    for (const auto& r : reqs) {
        CHECK(r.type == lob::OrderType::Limit);
        CHECK(r.price >= 10000 - 5 - 1);  // within a tick of the band
        CHECK(r.price <= 10000 + 5 + 1);
    }
}

TEST(noise_flow_sends_market_orders_when_configured) {
    lob::MatchingEngine engine;
    engine.submitLimit(lob::Side::Buy, 9995, 100);
    engine.submitLimit(lob::Side::Sell, 10005, 100);
    lob::FlowParams p;
    p.informedFraction = 0.0;      // no informed flow
    p.noiseMarketFraction = 1.0;   // force the marketable path
    p.ordersPerStep = 20;
    lob::FlowModel flow(3, p);
    auto reqs = flow.generate(engine.book());
    CHECK(reqs.size() == 20);
    for (const auto& r : reqs) CHECK(r.type == lob::OrderType::Market);
}

TEST(flow_generate_is_deterministic_for_a_seed) {
    lob::MatchingEngine e1, e2;
    e1.submitLimit(lob::Side::Buy, 9995, 100); e1.submitLimit(lob::Side::Sell, 10005, 100);
    e2.submitLimit(lob::Side::Buy, 9995, 100); e2.submitLimit(lob::Side::Sell, 10005, 100);
    lob::FlowParams p; p.ordersPerStep = 10;
    lob::FlowModel f1(99, p), f2(99, p);
    auto a = f1.generate(e1.book());
    auto b = f2.generate(e2.book());
    CHECK_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        CHECK(a[i].side == b[i].side);
        CHECK(a[i].type == b[i].type);
        CHECK_EQ(a[i].price, b[i].price);
        CHECK_EQ(a[i].quantity, b[i].quantity);
    }
}
