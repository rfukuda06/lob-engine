# Market-Making & Microstructure Lab Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the existing limit order book into a microstructure lab: a latent fair-value market with informed vs. noise flow, an inventory-aware market maker, and an analytics layer that measures adverse selection and PnL.

**Architecture:** All new work sits *above* the untouched `OrderBook`/`MatchingEngine` core as drivers/observers (the same slot `MarketSimulator` occupies today). Three new modules — `flow` (market model), `market_maker` (strategy), `analytics` (observer) — plus a thin `mm_sim` driver and a small post-only/IOC change to the engine's submit path.

**Tech Stack:** C++20, standard library only, CMake, the repo's hand-rolled `TEST`/`CHECK`/`CHECK_EQ` framework (`tests/test_framework.h`). Everything in namespace `lob`. Spec: `docs/superpowers/specs/2026-08-24-market-making-microstructure-lab-design.md`.

**Branch:** `feat/market-making-lab` (already created; the spec is committed there).

---

## Conventions used by every task

- **Build (configure):** `cmake -S . -B build` — run once, and again is harmless. `cmake --build build` auto-reconfigures when `CMakeLists.txt` changed.
- **Build:** `cmake --build build -j`
- **Run all tests:** `./build/orderbook_tests` — the framework has **no per-test filter**; it runs every registered test and prints one `PASS <name>` / `FAIL <name>` line each, then `N/M tests passed`. "Verify it fails/passes" means: build, run the binary, and look for that test's line.
- **TDD in C++ (compile-at-every-step):** because a test referencing a not-yet-existing symbol won't *compile* (not "fail"), each new unit is introduced as: (1) header + **stub** `.cpp` bodies that compile but return defaults, (2) wire into CMake, (3) write the failing test, (4) build+run → see `FAIL`, (5) fill in the real body, (6) build+run → see `PASS`, (7) commit.
- **Money units:** `Price`/`Quantity` are `std::int64_t` ticks/shares. The market maker's `cash_` is `long long` in **tick·share** units (e.g. cents·shares); convert to dollars only at display time.

## File structure (created/modified across the plan)

| File | Responsibility |
|---|---|
| `src/types.h` (modify) | Add `TimeInForce`; move `OrderRequest` here and extend with `tif` + `postOnly`. |
| `src/matching_engine.h/.cpp` (modify) | `SubmitResult.rejected`; post-only + IOC on `submitLimit`; `submit(OrderRequest)`. |
| `src/simulator.h` (modify) | Drop the local `OrderRequest` (now in `types.h`). |
| `src/flow.h/.cpp` (create) | `FairValue` random walk + `FlowModel` (noise + informed order flow). |
| `src/market_maker.h/.cpp` (create) | `MarketMaker`: quoting (inventory-skew → Avellaneda–Stoikov), inventory/cash accounting, risk cap. |
| `src/analytics.h/.cpp` (create) | `Analytics` observer: PnL, inventory, markout, spread/flow metrics, CSV. |
| `src/mm_sim.h/.cpp` (create) | `runMarketMakingSim(config)` driver + `MmSimConfig`/`Summary` wiring. |
| `src/main.cpp` (modify) | `--mm-sim` mode and flag parsing. |
| `tests/test_matching.cpp` (modify) | Order-type cases. |
| `tests/test_flow.cpp`, `tests/test_market_maker.cpp`, `tests/test_analytics.cpp`, `tests/test_mm_sim.cpp` (create) | Unit + end-to-end tests. |
| `CMakeLists.txt` (modify) | Add new sources to `CORE_SOURCES` and new tests to `orderbook_tests`. |
| `README.md` (modify) | Microstructure framing + results + pairs-bot differentiation. |

---

## Task 1: Order types — post-only, IOC, and `submit(OrderRequest)`

**Files:**
- Modify: `src/types.h`
- Modify: `src/matching_engine.h`, `src/matching_engine.cpp`
- Modify: `src/simulator.h` (remove its local `OrderRequest`)
- Test: `tests/test_matching.cpp` (append cases)

- [ ] **Step 1: Add `TimeInForce` and move/extend `OrderRequest` into `types.h`**

In `src/types.h`, after the `enum class OrderType { Limit, Market };` line, add:

```cpp
enum class TimeInForce { GTC, IOC };  // GTC: rest remainder; IOC: drop remainder
```

Then, before the closing `}  // namespace lob`, add the shared request type (moved from `simulator.h`). The two new fields are defaulted so existing `{type, side, price, qty}` initializers keep compiling:

```cpp
// A request to submit an order (no id yet — the engine assigns one).
struct OrderRequest {
    OrderType type;
    Side side;
    Price price;      // 0 for market orders
    Quantity quantity;
    TimeInForce tif = TimeInForce::GTC;
    bool postOnly = false;
};
```

- [ ] **Step 2: Remove the duplicate `OrderRequest` from `simulator.h`**

In `src/simulator.h`, delete the local definition (lines defining `struct OrderRequest { ... };`) so it is only defined in `types.h`. `simulator.h` already includes `matching_engine.h` → `order_book.h` → `types.h`, so the type stays visible to `simulator.cpp` and `benchmark.cpp`.

- [ ] **Step 3: Extend `SubmitResult` and `MatchingEngine` declarations**

In `src/matching_engine.h`, add a field to `SubmitResult`:

```cpp
struct SubmitResult {
    OrderId id = 0;
    std::vector<Trade> trades;
    Quantity restedQty = 0;      // remainder now resting (limit GTC only)
    Quantity cancelledQty = 0;   // remainder dropped (market / IOC / post-only reject)
    bool rejected = false;       // post-only order that would have crossed
};
```

Replace the `submitLimit` declaration and add `submit`:

```cpp
    SubmitResult submitLimit(Side side, Price limit, Quantity qty,
                             TimeInForce tif = TimeInForce::GTC,
                             bool postOnly = false);
    SubmitResult submitMarket(Side side, Quantity qty);
    SubmitResult submit(const OrderRequest& req);  // dispatch on req.type
    bool cancel(OrderId id);
```

- [ ] **Step 4: Write the failing tests**

Append to `tests/test_matching.cpp` (these reference the new behavior; the file already includes `matching_engine.h` and the test framework):

```cpp
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
```

Ensure `<optional>` is included in `tests/test_matching.cpp` (add `#include <optional>` at the top if not already present).

- [ ] **Step 5: Build and run — expect the new tests to FAIL**

Run: `cmake --build build -j && ./build/orderbook_tests`
Expected: existing tests still `PASS`; the four new tests appear (some `FAIL`, e.g. `FAIL post_only_rejects_when_it_would_cross`, because `submitLimit` ignores the new params and `submit` is unimplemented / returns default).

> If the build fails to link on `submit` (no definition yet), that is expected before Step 6 — add a stub `SubmitResult MatchingEngine::submit(const OrderRequest&) { return {}; }` in `matching_engine.cpp` so it links and the tests run and FAIL. Step 6 replaces it.

- [ ] **Step 6: Implement post-only, IOC, and `submit` in `matching_engine.cpp`**

Replace `MatchingEngine::submitLimit` with the version below, and add `submit`. `crosses` and `opposite` already exist in the anonymous namespace at the top of the file.

```cpp
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
```

The accountability invariant `sum(trade qty) + restedQty + cancelledQty == qty` still holds in every branch.

- [ ] **Step 7: Build and run — expect PASS**

Run: `cmake --build build -j && ./build/orderbook_tests`
Expected: all tests `PASS`, including the four new ones.

- [ ] **Step 8: Commit**

```bash
git add src/types.h src/simulator.h src/matching_engine.h src/matching_engine.cpp tests/test_matching.cpp
git commit -m "feat(engine): post-only + IOC order types and submit(OrderRequest)"
```

---

## Task 2: `FairValue` — the latent efficient price

**Files:**
- Create: `src/flow.h`, `src/flow.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_flow.cpp` (create)

- [ ] **Step 1: Create `src/flow.h` with the `FairValue` class**

```cpp
#pragma once
#include <random>

#include "types.h"

namespace lob {

// Exogenous efficient price V_t: a Gaussian random walk in ticks, independent
// of the book. Informed flow and analytics may read it; the market maker never
// does (that is what makes the MM adversely selectable).
class FairValue {
public:
    FairValue(double start, double vol);
    Price step(std::mt19937& rng);  // advance by N(0, vol^2); return rounded tick (>=1)
    Price current() const;          // current value rounded to a tick (>=1)

private:
    double value_;
    double vol_;
};

}  // namespace lob
```

- [ ] **Step 2: Create `src/flow.cpp` with a stub, then wire CMake**

```cpp
#include "flow.h"

#include <algorithm>
#include <cmath>

namespace lob {

FairValue::FairValue(double start, double vol) : value_(start), vol_(vol) {}

Price FairValue::step(std::mt19937&) { return current(); }  // stub — real body in Step 5

Price FairValue::current() const {
    return std::max<Price>(1, static_cast<Price>(std::llround(value_)));
}

}  // namespace lob
```

In `CMakeLists.txt`, add `src/flow.cpp` to `CORE_SOURCES`:

```cmake
set(CORE_SOURCES
    src/order_book.cpp
    src/matching_engine.cpp
    src/display.cpp
    src/simulator.cpp
    src/repl.cpp
    src/benchmark.cpp
    src/flow.cpp
)
```

And add the new test file to the `orderbook_tests` target's source list (after `tests/test_repl.cpp`):

```cmake
add_executable(orderbook_tests
    tests/test_main.cpp
    tests/test_order_book.cpp
    tests/test_matching.cpp
    tests/test_display.cpp
    tests/test_simulator.cpp
    tests/test_repl.cpp
    tests/test_flow.cpp
    ${CORE_SOURCES}
)
```

- [ ] **Step 3: Write the failing test in `tests/test_flow.cpp`**

```cpp
#include <random>

#include "flow.h"
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
```

- [ ] **Step 4: Build and run — expect FAIL**

Run: `cmake -S . -B build && cmake --build build -j && ./build/orderbook_tests`
Expected: `FAIL fair_value_moves_from_its_start` (the stub never moves).

- [ ] **Step 5: Implement `FairValue::step`**

Replace the stub in `src/flow.cpp`:

```cpp
Price FairValue::step(std::mt19937& rng) {
    std::normal_distribution<double> bump(0.0, vol_);
    value_ += bump(rng);
    if (value_ < 1.0) value_ = 1.0;  // fair value stays positive
    return current();
}
```

- [ ] **Step 6: Build and run — expect PASS**

Run: `cmake --build build -j && ./build/orderbook_tests`
Expected: both `FAIL`→`PASS`.

- [ ] **Step 7: Commit**

```bash
git add src/flow.h src/flow.cpp tests/test_flow.cpp CMakeLists.txt
git commit -m "feat(flow): FairValue latent efficient-price random walk"
```

---

## Task 3: `FlowModel` — noise + informed order flow

**Files:**
- Modify: `src/flow.h`, `src/flow.cpp`
- Test: `tests/test_flow.cpp` (append)

- [ ] **Step 1: Add `FlowParams` and `FlowModel` to `src/flow.h`**

Add `#include <vector>` and `#include "order_book.h"` to the includes, then add before the closing namespace brace:

```cpp
struct FlowParams {
    double informedFraction = 0.15;   // P(an arrival is informed)
    double noiseMarketFraction = 0.4; // P(a NOISE arrival is a marketable order vs a passive limit)
    double fairValueVol = 2.0;        // ticks/step for the fair-value walk
    Price startPrice = 10000;         // $100.00
    int ordersPerStep = 3;            // arrivals per step
    Quantity minSize = 1;
    Quantity maxSize = 10;
    Price noiseSpreadTicks = 5;       // noise limits placed within +/- this of mid
    Price informedEdgeTicks = 1;      // fair must beat the touch by this to trigger a take
};

// Mixes uninformed (noise) and informed order flow. Informed arrivals trade
// TOWARD the latent fair value, picking off stale quotes -> adverse selection.
class FlowModel {
public:
    FlowModel(unsigned seed, FlowParams params);
    Price fairValue() const;                    // current V_t
    Price stepFairValue();                      // advance V_t one step, return it
    std::vector<OrderRequest> generate(const OrderBook& book);  // this step's arrivals

private:
    Price referenceMid(const OrderBook& book) const;  // book mid, else startPrice

    FairValue fair_;
    std::mt19937 rng_;
    FlowParams p_;
};
```

- [ ] **Step 2: Add stub definitions to `src/flow.cpp`, then the failing test**

Append to `src/flow.cpp` (add `#include <vector>` at the top):

```cpp
FlowModel::FlowModel(unsigned seed, FlowParams params)
    : fair_(static_cast<double>(params.startPrice), params.fairValueVol),
      rng_(seed),
      p_(params) {}

Price FlowModel::fairValue() const { return fair_.current(); }
Price FlowModel::stepFairValue() { return fair_.step(rng_); }

Price FlowModel::referenceMid(const OrderBook& book) const {
    auto bid = book.bestBid();
    auto ask = book.bestAsk();
    if (bid && ask) return (*bid + *ask) / 2;
    return p_.startPrice;
}

std::vector<OrderRequest> FlowModel::generate(const OrderBook&) {
    return {};  // stub — real body in Step 4
}
```

Append the failing tests to `tests/test_flow.cpp` (add `#include "matching_engine.h"`):

```cpp
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
```

- [ ] **Step 3: Build and run — expect FAIL**

Run: `cmake --build build -j && ./build/orderbook_tests`
Expected: the three new `flow` tests `FAIL` (stub returns no requests).

- [ ] **Step 4: Implement `FlowModel::generate`**

Replace the stub in `src/flow.cpp` (add `#include <algorithm>` if not present):

```cpp
std::vector<OrderRequest> FlowModel::generate(const OrderBook& book) {
    std::vector<OrderRequest> out;
    std::bernoulli_distribution isInformed(p_.informedFraction);
    std::bernoulli_distribution noiseTakes(p_.noiseMarketFraction);
    std::uniform_int_distribution<Quantity> size(p_.minSize, p_.maxSize);
    std::uniform_int_distribution<int> coin(0, 1);
    std::uniform_int_distribution<Price> off(1, p_.noiseSpreadTicks);
    const Price fair = fair_.current();

    for (int i = 0; i < p_.ordersPerStep; ++i) {
        if (isInformed(rng_)) {
            // Informed: take TOWARD fair value when it beats the touch. Market
            // orders already drop any unfilled remainder, so no IOC flag is needed.
            auto ask = book.bestAsk();
            auto bid = book.bestBid();
            if (ask && fair > *ask + p_.informedEdgeTicks) {
                out.push_back({OrderType::Market, Side::Buy, 0, size(rng_)});
            } else if (bid && fair < *bid - p_.informedEdgeTicks) {
                out.push_back({OrderType::Market, Side::Sell, 0, size(rng_)});
            }
            // else: informed trader sees no edge this arrival -> posts nothing
        } else {
            Side side = coin(rng_) == 0 ? Side::Buy : Side::Sell;
            if (noiseTakes(rng_)) {
                // Uninformed MARKETABLE order: crosses the spread and lets the
                // market maker capture it. This is the flow the MM *earns* from.
                out.push_back({OrderType::Market, side, 0, size(rng_)});
            } else {
                // Uninformed passive limit a few ticks off mid: adds book depth.
                Price mid = referenceMid(book);
                Price px = side == Side::Buy ? mid - off(rng_) : mid + off(rng_);
                px = std::max<Price>(px, 1);
                out.push_back({OrderType::Limit, side, px, size(rng_)});
            }
        }
    }
    return out;
}
```

> **Why noise needs a marketable component:** a passive limit placed *around* the mid never crosses the market maker's quotes, so a limit-only noise stream would give the MM zero fills. Uninformed **market** orders are the benign flow the MM profits from (it captures the half-spread); informed market orders are the toxic flow that picks it off. The mix of the two is the whole economic point.

- [ ] **Step 5: Build and run — expect PASS**

Run: `cmake --build build -j && ./build/orderbook_tests`
Expected: all `flow` tests `PASS`.

- [ ] **Step 6: Commit**

```bash
git add src/flow.h src/flow.cpp tests/test_flow.cpp
git commit -m "feat(flow): FlowModel mixing informed and noise order flow"
```

---

## Task 4: `MarketMaker` — inventory-skew quoting, accounting, risk cap

**Files:**
- Create: `src/market_maker.h`, `src/market_maker.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_market_maker.cpp` (create)

- [ ] **Step 1: Create `src/market_maker.h`**

```cpp
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
```

- [ ] **Step 2: Create `src/market_maker.cpp` with stubs; wire CMake**

```cpp
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

MarketMaker::Quotes MarketMaker::computeQuotes(const OrderBook&, long, long) const {
    return {0, 0, false};  // stub — real body in Task 4 Step 5 (skew) / Task 5 (A-S)
}

void MarketMaker::requote(MatchingEngine&, const OrderBook&, long, long) {}   // stub
void MarketMaker::onTrades(const std::vector<Trade>&, long) {}                // stub

}  // namespace lob
```

In `CMakeLists.txt`, add `src/market_maker.cpp` to `CORE_SOURCES` and `tests/test_market_maker.cpp` to the `orderbook_tests` sources (same pattern as Task 2 Step 2).

- [ ] **Step 3: Write the failing tests in `tests/test_market_maker.cpp`**

```cpp
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
    lob::MatchingEngine engine;
    seedBook(engine);  // mid = 10000
    lob::MMParams p;
    p.baseHalfSpread = 3;
    p.inventorySkewK = 1.0;      // 1 tick per unit inventory
    p.quoteSize = 5;

    lob::MarketMaker flat(lob::MMPolicy::InventorySkew, p);
    flat.requote(engine, engine.book(), 1, 1000);
    lob::Price flatBid = *engine.book().bestBid();  // MM's bid is now the best bid

    // A long MM should shift its bid strictly lower than a flat MM's.
    lob::MatchingEngine engine2;
    seedBook(engine2);
    lob::MarketMaker longMm(lob::MMPolicy::InventorySkew, p);
    // give it +10 inventory via a simulated ask lift, then requote
    longMm.requote(engine2, engine2.book(), 1, 1000);
    longMm.onTrades({{999, *longMm.askId(), 10003, 10}}, 1);   // inventory -> -10? see note
    // NOTE: an ask lift makes us SHORT. To make us LONG, hit our bid instead:
    lob::MatchingEngine engine3;
    seedBook(engine3);
    lob::MarketMaker longMm2(lob::MMPolicy::InventorySkew, p);
    longMm2.requote(engine3, engine3.book(), 1, 1000);
    longMm2.onTrades({{999, *longMm2.bidId(), 9997, 10}}, 1);  // +10 inventory (long)
    // cancel old quotes implicitly by re-quoting on a fresh engine reflecting mid 10000
    lob::MatchingEngine engine4;
    seedBook(engine4);
    // Re-drive a long MM's quotes on a clean book to read its bid:
    lob::MarketMaker longMm3(lob::MMPolicy::InventorySkew, p);
    longMm3.onTrades({{999, 0, 9997, 10}}, 1);  // no id match -> use direct inventory set? see Step 4 note
    longMm3.requote(engine4, engine4.book(), 1, 1000);
    lob::Price longBid = *engine4.book().bestBid();
    CHECK(longBid < flatBid);
}
```

> **Step 4 note (simplify the skew test):** the fill-attribution juggling above is awkward because inventory can only change through matched `makerId`s. Replace the whole `inventory_skew_quotes_lower_when_long` body with the cleaner version below, which drives inventory through the MM's real bid and reads the resulting quote from a *fresh* requote on the same mid:

```cpp
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
```

Delete the earlier awkward version; keep only this one.

- [ ] **Step 5: Implement `requote`, `onTrades`, and the inventory-skew `computeQuotes`**

Replace the three stubs in `src/market_maker.cpp`:

```cpp
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
    } else {
        // Avellaneda-Stoikov — implemented in Task 5. Fall back to skew for now.
        reservation = mid - static_cast<double>(inventory_) * p_.inventorySkewK;
        halfSpread = static_cast<double>(p_.baseHalfSpread);
        (void)t; (void)T;
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
```

- [ ] **Step 6: Build and run — expect PASS**

Run: `cmake -S . -B build && cmake --build build -j && ./build/orderbook_tests`
Expected: the three `mm_*`/skew tests `PASS`.

- [ ] **Step 7: Commit**

```bash
git add src/market_maker.h src/market_maker.cpp tests/test_market_maker.cpp CMakeLists.txt
git commit -m "feat(mm): inventory-skew market maker with fill accounting and risk cap"
```

---

## Task 5: `MarketMaker` — Avellaneda–Stoikov policy

**Files:**
- Modify: `src/market_maker.cpp`
- Test: `tests/test_market_maker.cpp` (append)

- [ ] **Step 1: Write the failing test**

Append to `tests/test_market_maker.cpp` (add `#include <cmath>` at the top):

```cpp
TEST(avellaneda_stoikov_uses_the_optimal_spread_formula) {
    // Flat inventory, infinite horizon (horizonSteps = 0 => tau = 1):
    //   reservation = mid,  half = 0.5*gamma*sigma^2 + (1/gamma)*ln(1 + gamma/kappa)
    lob::MMParams p;
    p.gamma = 0.1; p.sigma = 2.0; p.kappa = 1.5; p.horizonSteps = 0;
    p.quoteSize = 5;
    lob::MarketMaker mm(lob::MMPolicy::AvellanedaStoikov, p);

    lob::MatchingEngine engine;
    engine.submitLimit(lob::Side::Buy, 9990, 100);
    engine.submitLimit(lob::Side::Sell, 10010, 100);  // mid = 10000
    mm.requote(engine, engine.book(), 1, 1000);

    double half = 0.5 * 0.1 * 2.0 * 2.0 + (1.0 / 0.1) * std::log(1.0 + 0.1 / 1.5);
    lob::Price expectedBid = (lob::Price)std::llround(10000.0 - half);
    lob::Price expectedAsk = (lob::Price)std::llround(10000.0 + half);
    CHECK_EQ(*engine.book().bestBid(), expectedBid);
    // best ask is the MM's ask (10010 resting ask is worse than expectedAsk near mid)
    CHECK_EQ(*engine.book().bestAsk(), expectedAsk);
}

TEST(avellaneda_stoikov_reservation_shifts_with_inventory) {
    lob::MMParams p;
    p.gamma = 0.1; p.sigma = 2.0; p.kappa = 1.5; p.horizonSteps = 0;
    lob::MarketMaker mm(lob::MMPolicy::AvellanedaStoikov, p);

    lob::MatchingEngine e1;
    e1.submitLimit(lob::Side::Buy, 9990, 100);
    e1.submitLimit(lob::Side::Sell, 10010, 100);
    mm.requote(e1, e1.book(), 1, 1000);
    mm.onTrades({{999, *mm.bidId(), 9998, 20}}, 1);  // long +20

    lob::MatchingEngine e2;
    e2.submitLimit(lob::Side::Buy, 9990, 100);
    e2.submitLimit(lob::Side::Sell, 10010, 100);
    mm.requote(e2, e2.book(), 2, 1000);
    double q = 20.0;
    double shift = q * 0.1 * 2.0 * 2.0;  // gamma*sigma^2*tau, tau=1
    lob::Price expectedAsk = (lob::Price)std::llround(
        10000.0 - shift + (0.5 * 0.1 * 2.0 * 2.0 + (1.0 / 0.1) * std::log(1.0 + 0.1 / 1.5)));
    CHECK_EQ(*e2.book().bestAsk(), expectedAsk);  // reservation pulled down by long inventory
}
```

- [ ] **Step 2: Build and run — expect FAIL**

Run: `cmake --build build -j && ./build/orderbook_tests`
Expected: the two `avellaneda_stoikov_*` tests `FAIL` (the A-S branch currently falls back to skew).

- [ ] **Step 3: Implement the Avellaneda–Stoikov branch**

In `src/market_maker.cpp`, replace the `else` branch inside `computeQuotes` (the A-S fallback) with:

```cpp
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
```

Remove the now-unused `(void)t; (void)T;` line from the previous stub.

- [ ] **Step 4: Build and run — expect PASS**

Run: `cmake --build build -j && ./build/orderbook_tests`
Expected: both A-S tests `PASS`.

- [ ] **Step 5: Commit**

```bash
git add src/market_maker.cpp tests/test_market_maker.cpp
git commit -m "feat(mm): Avellaneda-Stoikov quoting policy"
```

---

## Task 6: `Analytics` — recording, strategy metrics, markout, CSV

**Files:**
- Create: `src/analytics.h`, `src/analytics.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_analytics.cpp` (create)

- [ ] **Step 1: Create `src/analytics.h`**

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "market_maker.h"
#include "order_book.h"
#include "types.h"

namespace lob {

struct StepRecord {
    long t = 0;
    Price fair = 0;
    Price bid = 0;            // best bid (0 if none)
    Price ask = 0;            // best ask (0 if none)
    double mid = 0.0;         // (bid+ask)/2 if both present, else fair
    double microprice = 0.0;
    Quantity bidSize = 0;     // top-of-book queue sizes
    Quantity askSize = 0;
    double ofi = 0.0;         // order-flow imbalance vs the previous step (0 on step 1)
    Quantity inventory = 0;
    long long cash = 0;
    double pnlMid = 0.0;
    double pnlFair = 0.0;
    Quantity signedVolume = 0;  // sum of signedTaker*qty this step (filled in Task 7)
};

struct MarketTrade {          // one executed trade, market-wide (filled in Task 7)
    long t = 0;
    Price price = 0;
    double midAtTrade = 0.0;
    int signedTaker = 0;      // +1 buyer-initiated, -1 seller-initiated
    Quantity qty = 0;
};

struct Summary {
    double finalPnlMid = 0.0;
    double finalPnlFair = 0.0;
    long long fills = 0;
    Quantity maxAbsInventory = 0;
    double sharpe = 0.0;
    double markout1 = 0.0, markout5 = 0.0, markout20 = 0.0;
    double effectiveSpread = 0.0;   // Task 7
    double realizedSpread = 0.0;    // Task 7
    double adverseSelection = 0.0;  // Task 7
    double kyleLambda = 0.0;        // Task 7
    double vpin = 0.0;              // Task 7
};

// Read-only observer. The engine is never mutated here.
class Analytics {
public:
    // Called per aggressive submission this step (taker side known by the driver).
    void onTrades(Side takerSide, const std::vector<Trade>& trades, double mid, long t);
    // Called once per step after flow + MM updates are applied.
    void recordStep(const OrderBook& book, Price fair, const MarketMaker& mm, long t);
    // Produce all metrics. `mmFills` is the market maker's own fills (mm.fills()).
    Summary finalize(const std::vector<MmFill>& mmFills) const;
    void printSummary(const Summary& s) const;
    void writeCsv(const std::string& path) const;

    const std::vector<StepRecord>& steps() const { return steps_; }

private:
    std::vector<StepRecord> steps_;
    std::vector<MarketTrade> trades_;
    Quantity pendingSignedVolume_ = 0;  // accumulates within a step, flushed in recordStep
    // Previous-step top of book, for the OFI (order-flow imbalance) computation.
    Price prevBid_ = 0;
    Price prevAsk_ = 0;
    Quantity prevBidSize_ = 0;
    Quantity prevAskSize_ = 0;
    bool havePrev_ = false;
};

}  // namespace lob
```

- [ ] **Step 2: Create `src/analytics.cpp` with stubs; wire CMake**

```cpp
#include "analytics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lob {

void Analytics::onTrades(Side, const std::vector<Trade>&, double, long) {}  // stub (Task 7)

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

Summary Analytics::finalize(const std::vector<MmFill>&) const { return {}; }  // stub

void Analytics::printSummary(const Summary&) const {}   // stub
void Analytics::writeCsv(const std::string&) const {}   // stub

}  // namespace lob
```

Add `src/analytics.cpp` to `CORE_SOURCES` and `tests/test_analytics.cpp` to `orderbook_tests` in `CMakeLists.txt`.

- [ ] **Step 3: Write the failing tests in `tests/test_analytics.cpp`**

```cpp
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
```

> Note: `ofi_reflects_top_of_book_queue_changes` exercises `recordStep` (implemented in Step 2), so it will already PASS at Step 4 — only the three `finalize`/`writeCsv`-dependent tests FAIL there.

- [ ] **Step 4: Build and run — expect FAIL**

Run: `cmake -S . -B build && cmake --build build -j && ./build/orderbook_tests`
Expected: the three `analytics_*`/`markout_*`/`csv_*` tests `FAIL` (finalize/writeCsv are stubs).

- [ ] **Step 5: Implement `finalize` (strategy + markout), `printSummary`, `writeCsv`**

Replace the `finalize`, `printSummary`, and `writeCsv` stubs in `src/analytics.cpp`:

```cpp
namespace {

// fair value recorded at step time `t` (steps are 1-based and in order).
double fairAt(const std::vector<StepRecord>& steps, long t) {
    if (t < 1 || static_cast<std::size_t>(t) > steps.size()) return 0.0;
    return static_cast<double>(steps[static_cast<std::size_t>(t - 1)].fair);
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
```

- [ ] **Step 6: Build and run — expect PASS**

Run: `cmake --build build -j && ./build/orderbook_tests`
Expected: the three analytics tests `PASS`.

- [ ] **Step 7: Commit**

```bash
git add src/analytics.h src/analytics.cpp tests/test_analytics.cpp CMakeLists.txt
git commit -m "feat(analytics): step recording, PnL/inventory/markout metrics, CSV export"
```

---

## Task 7: `Analytics` — market microstructure metrics

**Files:**
- Modify: `src/analytics.cpp`
- Test: `tests/test_analytics.cpp` (append)

Implements `onTrades` (records market-wide trades + accumulates signed volume) and extends `finalize` to compute effective/realized spread, adverse selection, Kyle's λ, and VPIN.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_analytics.cpp`:

```cpp
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
```

- [ ] **Step 2: Build and run — expect FAIL**

Run: `cmake --build build -j && ./build/orderbook_tests`
Expected: `effective_spread_*`, `kyle_lambda_*`, `vpin_*` `FAIL` (metrics are 0 from Task 6).

- [ ] **Step 3: Implement `onTrades` and extend `finalize`**

Replace the `onTrades` stub in `src/analytics.cpp`:

```cpp
void Analytics::onTrades(Side takerSide, const std::vector<Trade>& trades,
                         double mid, long t) {
    int sign = takerSide == Side::Buy ? 1 : -1;
    for (const Trade& tr : trades) {
        trades_.push_back({t, tr.price, mid, sign, tr.quantity});
        pendingSignedVolume_ += static_cast<Quantity>(sign) * tr.quantity;
    }
}
```

Add these helpers inside the existing anonymous `namespace { ... }` block in `analytics.cpp`:

```cpp
double midAtStep(const std::vector<StepRecord>& steps, long t) {
    if (t < 1 || static_cast<std::size_t>(t) > steps.size()) return 0.0;
    return steps[static_cast<std::size_t>(t - 1)].mid;
}
```

Then, in `finalize`, just before `return s;`, insert the market-metric computation:

```cpp
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
```

- [ ] **Step 4: Build and run — expect PASS**

Run: `cmake --build build -j && ./build/orderbook_tests`
Expected: the market-metric tests `PASS` (and the earlier analytics tests still `PASS`).

- [ ] **Step 5: Commit**

```bash
git add src/analytics.cpp tests/test_analytics.cpp
git commit -m "feat(analytics): effective/realized spread, Kyle's lambda, VPIN"
```

---

## Task 8: `mm_sim` driver + `--mm-sim` mode

**Files:**
- Create: `src/mm_sim.h`, `src/mm_sim.cpp`
- Modify: `CMakeLists.txt`, `src/main.cpp`
- Test: `tests/test_mm_sim.cpp` (create)

- [ ] **Step 1: Create `src/mm_sim.h`**

```cpp
#pragma once
#include <string>

#include "analytics.h"
#include "flow.h"
#include "market_maker.h"

namespace lob {

struct MmSimConfig {
    long steps = 20000;
    unsigned seed = 42;
    MMPolicy policy = MMPolicy::InventorySkew;
    FlowParams flow;
    MMParams mm;
    std::string csvPath;   // empty => no CSV
    bool quiet = false;    // suppress summary printing (tests set this)
};

// Runs the market-making simulation and returns the computed summary.
Summary runMarketMakingSim(const MmSimConfig& config);

}  // namespace lob
```

- [ ] **Step 2: Create `src/mm_sim.cpp` with a stub; wire CMake**

```cpp
#include "mm_sim.h"

#include "matching_engine.h"
#include "order_book.h"

namespace lob {

Summary runMarketMakingSim(const MmSimConfig&) { return {}; }  // stub — real body in Step 5

}  // namespace lob
```

Add `src/mm_sim.cpp` to `CORE_SOURCES` and `tests/test_mm_sim.cpp` to `orderbook_tests` in `CMakeLists.txt`.

- [ ] **Step 3: Write the failing tests in `tests/test_mm_sim.cpp`**

```cpp
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
```

> **These two tests encode real market-making economics** (uninformed flow is profitable; inventory skew reduces risk under toxic flow). They are averaged over seeds and documented as same-machine. If either inequality fails when you run it, that signals a **model bug to investigate** (e.g. quotes never reaching the touch, accounting sign error) — tune the parameters (spread, informed fraction, sizes, step count) to surface the effect rather than deleting the test.

- [ ] **Step 4: Build and run — expect FAIL**

Run: `cmake -S . -B build && cmake --build build -j && ./build/orderbook_tests`
Expected: the three `mm_sim_*` tests `FAIL` (stub returns an empty `Summary`, so `fills == 0`).

- [ ] **Step 5: Implement `runMarketMakingSim`**

Replace the stub in `src/mm_sim.cpp`:

```cpp
#include "mm_sim.h"

#include <algorithm>

#include "matching_engine.h"
#include "order_book.h"

namespace lob {

namespace {

// Deterministic starting liquidity: 10 levels each side, at start +/- (5..14) ticks.
// The nearest level (+/-5) sits OUTSIDE the market maker's half-spread, so the MM's
// quotes rest at the touch from the first step and can actually be filled.
void seedLiquidity(MatchingEngine& engine, Price start) {
    for (Price k = 1; k <= 10; ++k) {
        engine.submitLimit(Side::Buy, start - (4 + k), 50);
        engine.submitLimit(Side::Sell, start + (4 + k), 50);
    }
}

double midOrFair(const OrderBook& book, Price fair) {
    auto bid = book.bestBid();
    auto ask = book.bestAsk();
    if (bid && ask) return (static_cast<double>(*bid) + static_cast<double>(*ask)) / 2.0;
    return static_cast<double>(fair);
}

}  // namespace

Summary runMarketMakingSim(const MmSimConfig& cfg) {
    MatchingEngine engine;
    FlowModel flow(cfg.seed, cfg.flow);
    MarketMaker mm(cfg.policy, cfg.mm);
    Analytics analytics;

    seedLiquidity(engine, cfg.flow.startPrice);

    for (long t = 1; t <= cfg.steps; ++t) {
        Price fair = flow.stepFairValue();          // latent value advances (MM can't see it)
        mm.requote(engine, engine.book(), t, cfg.steps);  // post fresh post-only quotes
        double stepMid = midOrFair(engine.book(), fair);

        std::vector<OrderRequest> arrivals = flow.generate(engine.book());
        for (const OrderRequest& req : arrivals) {
            SubmitResult res = engine.submit(req);
            analytics.onTrades(req.side, res.trades, stepMid, t);
            mm.onTrades(res.trades, t);
        }
        analytics.recordStep(engine.book(), fair, mm, t);
    }

    Summary summary = analytics.finalize(mm.fills());
    if (!cfg.csvPath.empty()) analytics.writeCsv(cfg.csvPath);
    if (!cfg.quiet) analytics.printSummary(summary);
    return summary;
}

}  // namespace lob
```

- [ ] **Step 6: Build and run — expect PASS**

Run: `cmake --build build -j && ./build/orderbook_tests`
Expected: the three `mm_sim_*` tests `PASS`.

- [ ] **Step 7: Wire the `--mm-sim` mode into `src/main.cpp`**

Add `#include "mm_sim.h"` to the includes. Extend `printUsage` text and add flag parsing + dispatch. Replace the usage string and add the mode. The minimal, self-contained change:

Update `printUsage`:

```cpp
void printUsage() {
    std::printf(
        "usage: orderbook [--benchmark [N]] [--mm-sim [N]] [--seed S]\n"
        "                 [--informed-frac F] [--policy inventory|as] [--out FILE]\n"
        "  (no args)         interactive REPL with a simulated market\n"
        "  --benchmark [N]   process N generated orders (default 1000000)\n"
        "  --mm-sim [N]      run the market-making lab for N steps (default 20000)\n"
        "  --seed S          RNG seed (default 42)\n"
        "  --informed-frac F fraction of informed flow, 0..100 percent (default 15)\n"
        "  --policy P        market-maker policy: inventory (default) or as\n"
        "  --out FILE        write the per-step CSV to FILE\n");
}
```

In `main`, add these locals near the existing ones:

```cpp
    bool mmSim = false;
    lob::MmSimConfig mmCfg;
```

Add these `else if` branches inside the arg loop, before the final `else`:

```cpp
        } else if (std::strcmp(argv[i], "--mm-sim") == 0) {
            mmSim = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                auto n = parseNumber(argv[++i]);
                if (!n || *n <= 0) { printUsage(); return 1; }
                mmCfg.steps = static_cast<long>(*n);
            }
        } else if (std::strcmp(argv[i], "--informed-frac") == 0 && i + 1 < argc) {
            auto v = parseNumber(argv[++i]);
            if (!v || *v > 100) { printUsage(); return 1; }
            mmCfg.flow.informedFraction = static_cast<double>(*v) / 100.0;
        } else if (std::strcmp(argv[i], "--policy") == 0 && i + 1 < argc) {
            std::string pol = argv[++i];
            if (pol == "inventory") mmCfg.policy = lob::MMPolicy::InventorySkew;
            else if (pol == "as") mmCfg.policy = lob::MMPolicy::AvellanedaStoikov;
            else { printUsage(); return 1; }
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            mmCfg.csvPath = argv[++i];
```

After the loop, before the existing `if (benchmark) { ... }`, add:

```cpp
    if (mmSim) {
        mmCfg.seed = seed;
        lob::runMarketMakingSim(mmCfg);
        return 0;
    }
```

(The existing `--seed` branch already sets `seed`; assigning it into `mmCfg.seed` here keeps one seed source.)

- [ ] **Step 8: Build and run the app end-to-end**

Run: `cmake --build build -j && ./build/orderbook --mm-sim 2000 --seed 42 --informed-frac 30 --policy as --out build/mm.csv`
Expected: prints the `=== Market-Making Summary ===` block; `build/mm.csv` exists with a header row and 2000 data rows. Confirm with: `head -1 build/mm.csv` (header) and `wc -l build/mm.csv` (2001 lines incl. header).

- [ ] **Step 9: Run the whole test suite**

Run: `./build/orderbook_tests`
Expected: `N/M tests passed` with all `PASS`.

- [ ] **Step 10: Commit**

```bash
git add src/mm_sim.h src/mm_sim.cpp src/main.cpp tests/test_mm_sim.cpp CMakeLists.txt
git commit -m "feat(mm-sim): --mm-sim driver mode wiring flow + market maker + analytics"
```

---

## Task 9: README — microstructure framing, results, differentiation

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add a "Market-Making & Microstructure Lab" section**

Add a new section to `README.md` (below the existing overview) with: (a) the one-line framing "*I built a limit order book, then studied market-making and adverse selection on it*"; (b) how to run it (`./build/orderbook --mm-sim 20000 --seed 42 --policy as --informed-frac 30 --out mm.csv`); (c) a short glossary of the metrics (markout, effective vs realized spread, Kyle's λ, VPIN) in one line each; (d) a placeholder for a charted result. Use this content:

```markdown
## Market-Making & Microstructure Lab

Beyond matching orders, this project runs a **market maker inside its own book** and
measures whether the strategy survives *adverse selection* — the core risk of
providing liquidity. A latent "fair value" drifts exogenously; **informed** traders
trade toward it and pick off stale quotes, while **noise** traders don't. The market
maker (inventory-skew, or the **Avellaneda–Stoikov** optimal model) quotes off the
observable book mid only — it never sees fair value, which is exactly why it can be
picked off.

```bash
./build/orderbook --mm-sim 20000 --seed 42 --policy as --informed-frac 30 --out mm.csv
```

Reported metrics:
- **Markout PnL** — a fill's PnL measured a few steps later vs. fair value; negative = adverse selection.
- **Effective vs. realized spread** — taker cost vs. maker capture net of impact; their gap *is* adverse selection.
- **Kyle's λ** — price impact per unit of signed order flow.
- **VPIN** — volume-bucketed order-flow toxicity.

The CSV (`mm.csv`) has one row per step (fair, mid, microprice, inventory, PnL, …) for charting a PnL / inventory curve.

### Why this is different from a "trading bot"
This is a **market-microstructure** study on a hand-built order book — passive quoting,
queue dynamics, and adverse selection — not a directional alpha strategy. It deliberately
keeps strategy/PnL plumbing thin; the focus is what only a real order book can show.
```

- [ ] **Step 2: Generate a real result to quote (optional but recommended)**

Run: `./build/orderbook --mm-sim 20000 --seed 42 --policy as --informed-frac 30 --out mm.csv`
Copy the printed summary numbers into the README where the charted result placeholder is, and (if charting externally) add a PnL/inventory image. Note in the README that numbers are seed- and machine-specific.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: market-making & microstructure lab section in README"
```

---

## Final verification (Definition of Done)

- [ ] `cmake -S . -B build && cmake --build build -j` — clean build, no warnings from new files (`-Wall -Wextra` is on).
- [ ] `./build/orderbook_tests` — every test `PASS`; final line shows `M/M tests passed`.
- [ ] `ctest --test-dir build` — the `all_tests` target passes.
- [ ] `./build/orderbook --mm-sim 20000 --seed 42 --policy as --informed-frac 30 --out build/mm.csv` — prints a summary; CSV has 20000 data rows.
- [ ] Determinism: running the same `--mm-sim` command twice prints identical summary numbers.
- [ ] The existing REPL and `--benchmark` modes still work unchanged.
- [ ] Merge `feat/market-making-lab` per the finishing-a-development-branch skill.
```
