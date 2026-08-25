# Market-Making & Microstructure Lab — Design Spec

**Date:** 2026-08-24
**Status:** Draft for review
**Extends:** the existing limit-order-book & matching-engine core (`src/order_book.*`, `src/matching_engine.*`).

---

## 1. One-line summary

Turn the existing limit order book into a **microstructure lab**: a latent "fair value" market with **informed vs. noise order flow** creates real *adverse selection*, an **inventory-aware market maker** (inventory-skew → Avellaneda–Stoikov) quotes into it, and an **analytics observer** measures whether it survives — markout, effective-vs-realized spread, inventory, PnL — with a CSV time series for charting.

---

## 2. Context & motivation

The current project is a clean, single-threaded, zero-dependency C++20 order book with a pure `OrderBook` container, a `MatchingEngine` (price-time priority, full accountability via `SubmitResult`), a REPL, a seeded `MarketSimulator`, a display, and a throughput benchmark. Dependencies flow one way: the book depends only on `types.h`; the engine owns a book; the simulator/REPL/benchmark are *drivers* that call `engine.submit*()`.

The project currently models the *machinery* of an exchange but no one *trades* on it with intent, and its simulator's price is purely reflexive (drifts to the last trade), so it cannot generate adverse selection — the central risk in real market-making.

**This extension is additive and reuses the existing layering exactly:** we add two more *drivers* (`flow`, `market_maker`) and one *observer* (`analytics`) in the same slot the simulator already occupies, plus one small, contained change to the engine's submit path (post-only / IOC). The core `OrderBook` and `MatchingEngine` internals are **not** restructured.

### 2.1 Differentiation from the author's existing pairs-trading bot (explicit design driver)

The author has a separate **crypto pairs-trading bot** (Python: cointegration + z-score mean-reversion, bar-level, abstract fills via a PaperBroker, backtest + paper-trade parity, drawdown kill-switch). To avoid the two projects reading as redundant "trading bots," this project is deliberately positioned on the axis the pairs bot **cannot** touch — it has no order book at all:

| Dimension | Pairs bot (exists) | This project |
|---|---|---|
| Signal | Statistical **alpha** (cointegration, z-score) | **Market microstructure** (queue, spread, adverse selection) |
| Level | Bar-level (hours), **takes** liquidity | Order-level (ticks), **provides** liquidity |
| Market model | Abstract fills (OHLCV + slippage) | A real **limit order book** built from scratch |
| Core question | "Can I predict mean-reversion?" | "Can I quote without getting picked off?" |
| Stack / role | Python, stats → quant **researcher** | C++, data structures → quant **trader / MM desk** |

**Positioning rules that follow from this** (binding on the design):
- Lead with **order-book-native** concepts: post-only quoting, queue position, adverse selection, markout, effective-vs-realized spread.
- Keep generic "bot infrastructure" **thin**: PnL accounting exists only to *measure* the market maker. **No** PaperBroker abstraction, **no** backtest-vs-live parity framework, **no** drawdown kill-switch engine (all of that is the pairs bot's story). Risk control here is a single inventory cap.
- README narrative: *"I built a limit order book, then studied market-making and adverse selection on it,"* not *"a bot that makes money."*

---

## 3. Goals & non-goals

### Goals
1. Produce genuine **adverse selection** via a latent fair value + informed flow.
2. Implement an **inventory-aware market maker** with two selectable policies (simple inventory-skew, and canonical Avellaneda–Stoikov).
3. **Measure** microstructure and strategy quality: markout, effective/realized spread, order-flow imbalance (OFI), microprice, Kyle's λ, VPIN, inventory & PnL paths.
4. Add the minimal order-type support the MM needs: **post-only** and **IOC**.
5. Ship a batch **`--mm-sim`** run mode that prints a summary table and writes a CSV time series.
6. Preserve the project's virtues: zero external dependencies, C++20, deterministic (same-machine) given a seed, explainable code.

### Non-goals (YAGNI — explicitly out of scope)
- Multi-threading, lock-free structures, or any low-latency optimization (that is the quant-*dev* axis; deliberately not this project).
- Live-refreshing TUI dashboard.
- Additional order types beyond post-only + IOC (no FOK, stop, iceberg, pro-rata matching).
- FIX/ITCH protocols or real market-data ingestion.
- Any shipped Python/plotting code — CSV export only (author charts externally).
- A broker abstraction, backtest-vs-live parity harness, or drawdown kill-switch (avoids echoing the pairs bot).
- Cross-platform determinism (remains same-machine only, as today, because `std::uniform_*_distribution` output is implementation-defined).
- Real-money execution.

---

## 4. Architecture

```
        ┌────────────── main.cpp  (new mode: --mm-sim) ──────────────┐
        │  parses flags, wires components, runs the step loop, reports │
        └───────┬───────────────┬───────────────┬────────────────────┘
                ▼               ▼                ▼
      ┌───────────────┐ ┌───────────────┐ ┌──────────────────┐
      │  FlowModel     │ │ MarketMaker    │ │   Analytics       │
      │ (flow.h/.cpp)  │ │(market_maker.*)│ │  (analytics.*)    │
      │ fair value V_t │ │ quotes bid/ask │ │ observer only:    │
      │ noise+informed │ │ inventory/cash │ │ strategy + market │
      │ → OrderRequests│ │ post-only/IOC  │ │ metrics, CSV      │
      └───────┬────────┘ └───────┬────────┘ └────────┬─────────┘
              │ submit*()        │ submit/cancel      │ reads book snapshots
              └────────┬─────────┴────────┬───────────┘ + trade stream (no writes)
                       ▼                  ▼
                 MatchingEngine  ──owns──►  OrderBook   (existing core;
                 + post-only / IOC on submit path        internals unchanged)
```

**New files** (each one job, matching existing style):
- `src/flow.h`, `src/flow.cpp` — latent fair value + noise/informed flow generation.
- `src/market_maker.h`, `src/market_maker.cpp` — the quoting strategy + inventory/PnL accounting.
- `src/analytics.h`, `src/analytics.cpp` — read-only metrics observer + reporting/CSV.

**Edited files:**
- `src/types.h` — add `enum class TimeInForce { GTC, IOC };`; add a shared `OrderRequest` struct (if not already centralized) carrying `{ Side, OrderType, std::optional<Price>, Quantity, TimeInForce, bool postOnly }`.
- `src/matching_engine.h/.cpp` — extend the limit submit path to honor `TimeInForce` and `postOnly`; add a reject signal to `SubmitResult`.
- `src/main.cpp` — add the `--mm-sim` mode and its flags.
- `CMakeLists.txt` — add the three new `.cpp` files to `CORE_SOURCES`, and the new test files to the test target.

**Untouched in spirit:** `order_book.*`, REPL, display, existing `simulator.*` and `benchmark.*` (the simulator remains the simple REPL demo; the new `flow` module is separate, per the approved decision).

---

## 5. Component designs

### 5.1 Order-type support (engine, minimal)

`types.h`:
```cpp
enum class TimeInForce { GTC, IOC };  // GTC = rest remainder (current behavior); IOC = drop remainder
```

`MatchingEngine::submitLimit` gains two defaulted parameters so all existing callers/tests compile unchanged:
```cpp
SubmitResult submitLimit(Side side, Price limit, Quantity qty,
                         TimeInForce tif = TimeInForce::GTC,
                         bool postOnly = false);
```

Semantics:
- **post-only:** before matching, peek the best opposite order; if the new order *would* cross (`crosses(side, limit, bestOppositePrice)`), **reject** it — no match, nothing rests. Returns `SubmitResult{ id, trades:{}, restedQty:0, cancelledQty:qty, rejected:true }`.
- **IOC:** match as normal, but never rest the remainder → remainder goes to `cancelledQty` (same terminal path `submitMarket` already uses).
- **GTC (default):** unchanged behavior.

`SubmitResult` gains `bool rejected = false;`. The accountability invariant is preserved: `sum(trade qty) + restedQty + cancelledQty == requested qty` in all cases (a post-only reject counts the full qty as `cancelledQty`).

*Rationale:* post-only keeps the market maker strictly passive (it never pays the spread / accidentally takes); IOC lets it (or informed flow) take aggressively. Reject-on-cross (rather than reprice/slide) is the simplest correct semantics; repricing is a noted possible extension, not in scope.

### 5.2 `flow.h/.cpp` — the market model (the engine of the whole thing)

**Fair value** — an exogenous random walk representing the efficient price, independent of the book:
```cpp
class FairValue {
    double value_;      // current V_t, kept fractional internally (ticks)
    double vol_;        // sigma per step (ticks)
public:
    FairValue(double start, double vol);
    Price step(std::mt19937& rng);  // advance by N(0, vol^2), return rounded tick
    Price current() const;          // rounded tick
};
```

**FlowModel** — mixes noise and informed order flow, driving the engine each step:
```cpp
struct FlowParams {
    double informedFraction = 0.15;   // P(an arriving order is informed)
    double noiseMarketFraction = 0.4; // P(a noise arrival is marketable vs a passive limit)
    double fairValueVol     = 2.0;    // ticks/step
    Price  startPrice       = 10000;  // $100.00 in ticks (integer, matches existing sim reference)
    int    ordersPerStep    = 3;      // arrival count per step
    Quantity minSize = 1, maxSize = 10;
    Price  noiseSpreadTicks = 5;      // how far around mid noise limits are placed
    Price  informedEdgeTicks = 1;     // fair must beat the touch by this to trigger informed take
};

class FlowModel {
    FairValue fair_;
    std::mt19937 rng_;
    FlowParams p_;
public:
    FlowModel(unsigned seed, FlowParams p);
    Price fairValue() const;                          // current V_t
    Price stepFairValue();                            // advance the latent price one step
    std::vector<OrderRequest> generate(const OrderBook& book); // this step's arrivals
};
```

Generation logic per arriving order:
- With probability `informedFraction`, it is **informed**: compare `fair_` to the book. If `fair > bestAsk + informedEdgeTicks` → **buy** to lift the offer (IOC/market, size drawn from `[minSize,maxSize]`). If `fair < bestBid − informedEdgeTicks` → **sell** to hit the bid. Otherwise the informed trader is indifferent and posts nothing (or a passive order). Informed flow trades *toward* true value, picking off stale quotes → **this is the adverse selection.**
- Otherwise it is **noise**: random side; with probability `noiseMarketFraction` a **marketable** order that crosses the spread, else a passive limit a few ticks around mid (`noiseSpreadTicks`). The marketable component is essential — a limit-only noise stream never crosses the maker's quotes, so the MM would get zero fills. Uninformed marketable flow is where the market maker *earns* the spread.

Determinism: all draws from a single seeded `std::mt19937`; the fair-value walk advances every step regardless of book state (so seeded runs stay in lockstep).

### 5.3 `market_maker.h/.cpp` — the strategy

```cpp
enum class MMPolicy { InventorySkew, AvellanedaStoikov };

struct MMParams {
    Quantity quoteSize      = 5;
    Price    baseHalfSpread = 3;    // ticks (inventory-skew policy)
    double   inventorySkewK = 0.5;  // ticks of reservation shift per unit inventory
    Quantity maxInventory   = 50;   // hard risk cap
    // Avellaneda–Stoikov params:
    double   gamma = 0.1;           // risk aversion
    double   sigma = 2.0;           // vol estimate (ticks/step)
    double   kappa = 1.5;           // order-arrival intensity
    long     horizonSteps = 0;      // T; 0 ⇒ infinite-horizon (constant) A-S variant
};

class MarketMaker {
    MMParams   p_;
    MMPolicy   policy_;
    Quantity   inventory_ = 0;
    long long  cash_      = 0;      // units of tick·qty (e.g. cents·shares)
    std::optional<OrderId> bidId_, askId_;
public:
    MarketMaker(MMPolicy policy, MMParams p);
    // Each step: cancel stale quotes and post fresh post-only quotes.
    // NOTE: deliberately does NOT receive fair value — the MM quotes off the book mid only.
    void requote(MatchingEngine& engine, const OrderBook& book, long t, long T);
    // Attribute fills from this step's trade stream (matches makerId ∈ our ids);
    // `t` stamps each fill for markout analysis.
    void onTrades(const std::vector<Trade>& trades, long t);
    Quantity  inventory() const { return inventory_; }
    long long cash() const { return cash_; }
    double markToMarket(Price mark) const;  // cash + inventory*mark, in tick·qty units
    std::optional<OrderId> bidId() const;
    std::optional<OrderId> askId() const;
};
```

> **Critical invariant — the MM is blind to fair value.** `requote` takes only the `OrderBook` (and `t`/`T`), never `fair`. The market maker quotes around the *observable book mid* `= (bestBid + bestAsk)/2`. In Avellaneda–Stoikov the reference price `s` is instantiated as this book mid, **not** the latent `V_t`. This is exactly why the MM can be adversely selected: informed flow, which *does* know `V_t`, moves the book before the MM re-prices. If the MM were handed `V_t`, adverse selection would vanish and the lab would be pointless.

**Quoting policies** (both quote one bid + one ask, `postOnly`, size `quoteSize`; `mid` = observable book mid):
- **InventorySkew:** reservation `r = mid − inventory·inventorySkewK`; `bid = r − baseHalfSpread`, `ask = r + baseHalfSpread`. Long inventory shifts both quotes down (lean to sell). Simple, intuitive.
- **AvellanedaStoikov:** reservation `r = mid − q·γ·σ²·(T−t)`; optimal half-spread `δ = ½·γσ²(T−t) + (1/γ)·ln(1 + γ/κ)`; `bid = r − δ`, `ask = r + δ`. With `horizonSteps = 0`, drop the `(T−t)` term for the constant-spread infinite-horizon variant. Quotes are rounded to ticks; if rounding would cross, widen by one tick.

**Risk control:** when `|inventory| ≥ maxInventory`, suppress the side that would worsen inventory (stop quoting the bid when very long, the ask when very short).

**Accounting (must be exact):** on a trade where `makerId` is our `bidId_`, we *bought* (someone sold into our bid): `inventory_ += q; cash_ -= q * price`. On our `askId_` lifted: `inventory_ -= q; cash_ += q * price`. Partial fills reduce the resting quote; `requote` cancels leftovers and re-posts next step. Mark-to-market PnL = `cash_ + inventory_ * mark`; reported both at **mid** (live view) and at **fair value** (true economic PnL).

### 5.4 `analytics.h/.cpp` — the observer

Read-only. Subscribes to the step loop: the trade stream + a per-step snapshot (book best levels, fair value, MM state). Two metric families.

**Strategy metrics (the market maker):**
- PnL time series (marked at mid and at fair value); realized spread-capture vs. inventory mark-to-market components.
- Inventory path; max inventory; time at cap.
- Fill count, fill rate, average edge captured per fill.
- **Markout PnL** at horizon Δ: for each MM fill at time `t`, price `p`, side `s`, compute `signed(s)·(fair_{t+Δ} − p)`; average across fills. Negative ⇒ adverse selection. Reported for a few Δ (e.g. 1, 5, 20 steps). *This is the headline adverse-selection measurement.*
- Sharpe of per-step PnL increments.

**Market metrics (the flow/book, independent of the MM):**
- **Order-flow imbalance (OFI)** (Cont–Kukanov–Stoikov): per-step signed change in best bid/ask queue sizes.
- **Microprice** (Stoikov): imbalance-weighted mid `= (bidSize·ask + askSize·bid)/(bidSize+askSize)`.
- **Effective spread** per trade `= 2·|p − mid_at_trade|`; **realized spread** `= 2·signed·(mid_{t+Δ} − p)`; **adverse selection = effective − realized**.
- **Kyle's λ:** OLS slope of `Δmid` on signed trade volume over the run (price impact per unit flow).
- **VPIN** (Easley–López de Prado–O'Hara): volume-bucketed `|buyVol − sellVol|/bucketVol`, averaged — order-flow toxicity.

**Interface & reporting:**
```cpp
class Analytics {
public:
    // takerSide is needed to sign order flow; the driver knows it per submission.
    void onTrades(Side takerSide, const std::vector<Trade>& trades, double mid, long t);
    void recordStep(const OrderBook& book, Price fair, const MarketMaker& mm, long t);
    Summary finalize(const std::vector<MmFill>& mmFills) const;  // markout, spread, λ, VPIN, OFI recorded per-step
    void printSummary(const Summary& s) const;
    void writeCsv(const std::string& path) const;   // per-step series incl. an `ofi` column
};
```

> Note: OFI is an inherently per-step series (recorded in `recordStep`, exported in the CSV `ofi` column), not a single summary scalar. The other market metrics reduce to summary scalars in `finalize`.
CSV columns (one row/step): `t, fair, bid, ask, mid, microprice, ofi, mm_inventory, mm_cash, pnl_mid, pnl_fair`, plus a trades side-table or fill markers sufficient to chart the PnL and inventory paths.

### 5.5 `main.cpp` — the `--mm-sim` mode & CLI

New mode dispatched from `main()` alongside the existing REPL/benchmark modes:
```
./orderbook --mm-sim [N]        # run N steps (default e.g. 20000)
  --seed S                      # determinism
  --informed-frac F             # 0..1 toxicity of the flow
  --policy inventory|as         # market-maker policy
  --gamma G --sigma V --kappa K # Avellaneda–Stoikov params
  --half-spread H --skew-k K    # inventory-skew params
  --max-inventory M             # risk cap
  --out results.csv             # CSV path (optional)
```
Flag parsing follows the existing `--benchmark`/`--seed` validation style (bounded digits, friendly errors at the boundary; engine trusts validated input). The mode builds a `MatchingEngine`, `FlowModel`, `MarketMaker`, `Analytics`, seeds initial liquidity, runs the loop (§6), then prints the summary and writes the CSV.

---

## 6. The step loop (data flow)

```
seed initial resting liquidity around startPrice
for t in 1..N:
    flow.stepFairValue()                       # exogenous true value advances (MM cannot see it)
    mm.requote(engine, book, t, N)             # cancel stale quotes; post fresh post-only bid/ask (off book mid)
    stepMid = mid(book) or fair                # observable mid captured before flow
    for req in flow.generate(book):            # noise + informed arrivals hit the book
        res = engine.submit(req)               # limit / market / IOC as specified
        analytics.onTrades(req.side, res.trades, stepMid, t)
        mm.onTrades(res.trades, t)             # attribute any fills to the MM
    analytics.recordStep(book, fair, mm, t)    # per-step snapshot
summary = analytics.finalize(mm.fills())
analytics.printSummary(summary); analytics.writeCsv(out)
```

**Ordering rationale:** the MM quotes *before* flow arrives, so informed flow can pick off the MM's now-stale quotes when `fair` has moved — reproducing the real adverse-selection dynamic. The MM re-quotes each step, so it continuously reprices toward the (unobserved-by-it) fair value using only the book it can see.

---

## 7. Testing strategy (TDD)

Written test-first, using the existing hand-rolled `TEST`/`CHECK` framework; new files `tests/test_flow.cpp`, `tests/test_market_maker.cpp`, `tests/test_analytics.cpp`, plus additions to `tests/test_matching.cpp` for order types.

**Deterministic / unit:**
- **Order types:** post-only that would cross is rejected (nothing trades/rests; `rejected==true`; qty in `cancelledQty`); post-only that would not cross rests normally; IOC matches then drops remainder to `cancelledQty`; accountability invariant holds in every case.
- **Fair value:** identical sequence for a fixed seed; mean/variance of increments roughly match `vol` over many steps.
- **Flow:** informed trader buys when `fair > bestAsk+edge` and sells when `fair < bestBid−edge`; noise stays near mid; whole `generate` sequence deterministic for a seed.
- **MM accounting:** hand-built fill scenarios → known inventory, cash, and mark-to-market (bid hit ⇒ inventory↑/cash↓; ask lifted ⇒ inventory↓/cash↑); partial fills; inventory cap suppresses the correct side.
- **Metrics:** effective spread, microprice, and OFI on tiny hand-constructed books match hand computation; markout sign is negative in a constructed pick-off scenario; Kyle's λ ≈ known slope on a synthetic linear-impact series.

**Economic / behavioral (directional, seed-fixed):**
- Pure noise (`informedFraction = 0`) ⇒ MM PnL ≥ ~0 (captures spread).
- Toxic flow (high `informedFraction`) ⇒ a tight *symmetric* quoter (skew-k = 0) loses money (adverse selection), and enabling inventory-skew / Avellaneda–Stoikov **reduces** the loss and **reduces** inventory variance. Asserted as inequalities on a fixed seed (documented as same-machine).

**Determinism:** the entire `--mm-sim` run reproduces identical summary numbers for a fixed seed on the same machine.

---

## 8. Design decisions & rationale (for the README / interview)

- **Latent fair value + informed flow** is the mechanism that manufactures *adverse selection* — the core risk of market-making — which the old reflexive simulator could not produce.
- **Two MM policies (skew → Avellaneda–Stoikov):** the progression from an intuitive linear skew to the canonical academic model *is* the story; naming and correctly implementing A–S is the credibility signal.
- **Markout & effective-vs-realized spread** are the industry-standard ways to *quantify* adverse selection — measurement, not just a strategy.
- **Post-only** keeps the maker passive (never pays the spread); **IOC** enables aggressive takes. Minimal set that the strategy actually needs.
- **Observer-based analytics** keep the engine pure — analytics only reads book snapshots and the trade stream, mirroring the existing book/engine separation.
- **Thin infrastructure by design** (no broker, no backtest-vs-live parity, no kill-switch) — a conscious choice to differentiate from the author's pairs-trading bot and keep the focus on microstructure.

---

## 9. Rough implementation order (preview; full plan via writing-plans)

1. Order types: `TimeInForce` + `postOnly` in `types.h`/engine, `SubmitResult.rejected`, tests. *(Smallest, unblocks the MM.)*
2. `flow` module: `FairValue`, then `FlowModel` (noise, then informed), tests.
3. `market_maker` module: inventory-skew policy + accounting + risk cap, tests; then Avellaneda–Stoikov policy.
4. `analytics` module: recording + strategy metrics (PnL, inventory, markout), then market metrics (OFI, microprice, effective/realized spread, Kyle's λ, VPIN); summary + CSV.
5. `--mm-sim` mode wiring + flags in `main.cpp`; end-to-end determinism test.
6. README: microstructure framing, a charted PnL/inventory/markout result, differentiation note.

---

## 10. Open questions (resolve during planning)

- **A–S horizon:** default to infinite-horizon constant-spread variant (`horizonSteps = 0`) for a steady-state lab, or use finite `T` (spread shrinks toward end)? *Leaning: infinite-horizon default, finite as a flag.*
- **Arrivals per step:** fixed `ordersPerStep` vs. Poisson-distributed. *Leaning: fixed for simplicity, note Poisson as an option.*
- **CSV granularity:** per-step snapshot only, or also a per-fill side-table? *Leaning: per-step + fill markers sufficient to chart.*
- **Cash units:** keep integer `tick·qty` throughout and format to dollars only at display time (avoids floating-point PnL drift). *Leaning: yes.*
