#include "mm_sim.h"
#include "test_framework.h"

TEST(mm_sim_is_deterministic_for_a_seed) {
    lob::MmSimConfig cfg;
    cfg.steps = 500;
    cfg.seed = 42;
    cfg.quiet = true;
    lob::Summary a = lob::runMarketMakingSim(cfg);
    lob::Summary b = lob::runMarketMakingSim(cfg);
    CHECK_EQ((long long)a.finalPnlFair, (long long)b.finalPnlFair);
    CHECK_EQ(a.fills, b.fills);
    CHECK_EQ((long long)a.maxAbsInventory, (long long)b.maxAbsInventory);
}

TEST(mm_sim_produces_fills_and_respects_inventory_cap) {
    lob::MmSimConfig cfg;
    cfg.steps = 1000;
    cfg.seed = 7;
    cfg.quiet = true;
    cfg.mm.maxInventory = 40;
    lob::Summary s = lob::runMarketMakingSim(cfg);
    CHECK(s.fills > 0);
    // The cap is soft (a single fill can exceed it), but inventory should stay near it.
    CHECK((long long)s.maxAbsInventory <= 40 + (long long)cfg.mm.quoteSize + (long long)cfg.flow.maxSize);
}

TEST(pure_noise_market_maker_captures_the_spread) {
    // No informed flow: the MM should earn the spread from uninformed marketable
    // orders. Marked at MID (so inventory drift doesn't mask capture) and averaged
    // over seeds so it is not a single-path fluke.
    double sum = 0.0;
    const int seeds = 5;
    for (unsigned s = 1; s <= (unsigned)seeds; ++s) {
        lob::MmSimConfig cfg;
        cfg.steps = 3000; cfg.seed = s; cfg.quiet = true;
        cfg.flow.informedFraction = 0.0;   // pure noise
        sum += lob::runMarketMakingSim(cfg).finalPnlMid;
    }
    CHECK(sum / seeds > 0.0);
}

TEST(inventory_skew_reduces_inventory_risk_under_toxic_flow) {
    // Toxic flow: a skewing maker should carry less inventory risk than a tight,
    // symmetric (zero-skew) maker. Averaged over seeds; same-machine. The cap is
    // set loose so inventory is driven by the strategy, not clamped by the cap.
    double tightInv = 0.0, skewInv = 0.0;
    const int seeds = 5;
    for (unsigned s = 1; s <= (unsigned)seeds; ++s) {
        lob::MmSimConfig tight;
        tight.steps = 3000; tight.seed = s; tight.quiet = true;
        tight.flow.informedFraction = 0.5;
        tight.mm.maxInventory = 500;       // loose cap
        tight.mm.inventorySkewK = 0.0;     // no inventory management
        tight.mm.baseHalfSpread = 2;

        lob::MmSimConfig skew = tight;
        skew.mm.inventorySkewK = 1.0;      // manage inventory

        tightInv += (double)lob::runMarketMakingSim(tight).maxAbsInventory;
        skewInv += (double)lob::runMarketMakingSim(skew).maxAbsInventory;
    }
    CHECK(skewInv / seeds <= tightInv / seeds);
}
