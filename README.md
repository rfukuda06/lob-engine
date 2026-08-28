# Limit Order Book & Matching Engine

This is a single-threaded C++20 implementation of the two pieces at the
heart of every electronic exchange: a limit order book that holds resting 
orders in price-time priority and a matching engine that crosses incoming 
orders against it. Limit orders rest in the book, market orders sweep the 
best available levels, and trades execute at the resting order's price with
earlier orders at the same price filling first.

You can drive it three ways: an interactive REPL to trade against a seeded
market by hand, a benchmark that clocks the engine at **16 million orders per
second on a single core** (about 62 ns per order), and a market-making
simulation. Standard library only, no external dependencies.

## Interactive REPL

Running `./build/orderbook` drops you into a REPL against a simulated market
(seed 42, so this exact book is reproducible):

    ================ ORDER BOOK ================
            ASKS
       100.09 |     70  (2)
       100.08 |    109  (2)
       100.07 |    114  (2)
       100.05 |     42  (1)
       100.03 |     95  (2)
       100.02 |     13  (1)
    --------------------------------------------
     Best Ask: 100.02   Mid: 99.99   Spread: 0.06
     Best Bid: 99.96
    --------------------------------------------
        99.96 |     73  (1)
        99.95 |    163  (3)
        99.94 |     11  (1)
        99.93 |     61  (1)
        99.92 |     33  (1)
        99.91 |     48  (1)
        99.90 |    100  (2)
            BIDS
    ============================================

Each row is a price level: aggregate resting quantity and (order count).
Asks print worst-first so the best ask sits nearest the spread; your fills
are tagged `(you)` in the trade tape.

## Commands

    buy 50 @ 100.10   limit buy      step [N]    run N sim events
    sell 25 @ 100.50  limit sell     book [N]    top N levels/side
    buy 50            market buy     trades [N]  last N trades
    sell 25           market sell    help / quit
    cancel 12         cancel by id

## Performance

The matching engine sustains **16 million orders per second on a single
core — roughly 62 nanoseconds per order** — including matching, resting,
and all cancel-index bookkeeping:

    Orders processed:  1000000
    Execution time:    0.062 seconds
    Throughput:        16.05 million orders/sec
    Trades executed:   152723
    Resting orders:    845416

(Apple M-series, Release build, seed 42.) Order generation is pre-computed
so the timed loop measures the engine alone, and identical seeds give
identical trade/resting counts on every run.

## Design

Prices are int64 ticks (1 tick = $0.01). This fixed-point representation 
avoids floating-point rounding errors and enables exact price comparison 
and reliable order-book indexing.

The `OrderBook` is a pure data structure; the `MatchingEngine` is the
algorithm that runs against it:

- Bids: `std::map<Price, PriceLevel, std::greater<>>` — best bid is `begin()`
- Asks: `std::map<Price, PriceLevel, std::less<>>` — best ask is `begin()`
- Each `PriceLevel`: `std::list<Order>` in FIFO arrival order + cached total
- Cancel index: `unordered_map<OrderId, {side, price, list iterator}>` —
  `std::list` iterators stay valid under other insertions/erasures

| Operation | Complexity (L = price levels/side) |
|---|---|
| Add resting order | O(log L) |
| Best bid / best ask | O(1) |
| One fill during matching | O(1) amortized (+O(log L) when a level empties) |
| Cancel | O(1) average (+O(log L) when a level empties) |
| Depth snapshot, top N | O(N) |

Matching rules: trades execute at the resting (maker) order's price, so price
improvement goes to the incoming order; partially filled resting orders keep
their queue position; an unfilled market-order remainder is cancelled, never
rested. Invariant: after any submit completes the book is never crossed.

## Market-Making & Microstructure Lab

Because a real order book has queues, a spread, and passive fills, you can run a
market maker inside it and measure *adverse selection*, the core risk of providing
liquidity. That is something an abstract backtest cannot show; the order book is
the foundation, and this lab is what it lets you study.

Here is how it works. A hidden "fair value" drifts over time; **informed** traders
can see it and pick off stale quotes, while **noise** traders cannot. The maker
(inventory-skew, or the **Avellaneda-Stoikov** optimal model) quotes off the visible
book mid only, never the fair value, which is exactly what leaves it exposed.

```bash
# strategy knobs (gamma/sigma/kappa/skew-k/half-spread/max-inventory) and --json
# output are all set from the CLI; see --help.
./build/orderbook --mm-sim 20000 --seed 42 --policy as --informed-frac 30 --out mm.csv
```

Turning up the fraction of **informed** flow is the whole experiment. Averaged
over 12 seeds (inventory-skew maker, 20000 steps; ± is standard error):

| Informed flow | MM fills | PnL vs. fair (ticks·shares) | Adverse selection (ticks) | Max \|inventory\| |
|---|---|---|---|---|
| 0% (pure noise) | 872 ± 34 | **+4024 ± 175** | 0.2 ± 0.0 | 6 ± 0 |
| 15% | 934 ± 248 | -827 ± 3009 | 17.2 ± 4.1 | 44 ± 5 |
| 30% | 182 ± 33 | +518 ± 2762 | 49.3 ± 11.7 | 50 ± 2 |

Adverse selection climbs sharply and reliably with informed flow. Under pure
noise the maker cleanly earns the spread; under toxic flow its PnL is swamped by
inventory risk (the error bars dwarf the mean), which is exactly why quoting into
informed traders is dangerous.

![Market-making lab overview](docs/mm_overview.png)

One run at light informed flow. The whole idea is in the top-left panel: fair
value wanders, the observable mid is far stickier, and the maker only ever sees
the mid, so that gap is its information disadvantage.

It reports markout PnL, effective vs. realized spread (their gap *is* adverse
selection), Kyle's λ (price impact per unit of signed flow), and VPIN (order-flow
toxicity). `scripts/plot_mm.py` charts the per-step CSV and `scripts/sweep.py`
regenerates the table above.

## Build & run

    brew install cmake                # once
    cmake -B build && cmake --build build
    ./build/orderbook_tests           # engine + microstructure tests
    ./build/orderbook                 # interactive REPL
    ./build/orderbook --mm-sim 20000 --policy as --informed-frac 30 --out mm.csv

    # benchmark (use an optimized build for numbers)
    cmake -B build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release
    ./build-release/orderbook --benchmark

The C++ core has no dependencies. The analysis scripts in `scripts/`
(`sweep.py`, `plot_mm.py`) need Python 3 with `pandas` and `matplotlib`.

## Simplifying assumptions

Single security; strictly sequential order arrival; no latency, fees, or
persistence; no self-trade prevention; no order modification; no hidden
liquidity; only limit and market orders. The REPL's flow is naive random order
generation, not a market model. In the lab the maker reposts every step (so it
never keeps queue priority), and deep seeded liquidity anchors the observable
mid, making it stickier than the fair value; both are areas to make more realistic.
