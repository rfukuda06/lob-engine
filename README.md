# Limit Order Book & Matching Engine

This is a single-threaded C++20 implementation of the two pieces at the
heart of every electronic exchange: a limit order book that holds resting 
orders in price-time priority and a matching engine that crosses incoming 
orders against it. Limit orders rest in the book, market orders sweep the 
best available levels, and trades execute at the resting order's price with
earlier orders at the same price filling first.

There are two modes: an interactive REPL where you trade against a seeded 
market simulator, and a benchmark that measures raw engine throughput —
**16 million orders per second on a single core, about 62 ns per order.**
Standard library only, no external dependencies.

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

### Sample result: the adverse-selection gradient

Turning up the fraction of **informed** flow is the whole experiment. As stale quotes
get picked off more often, the market maker's edge erodes, adverse selection climbs, and
inventory is driven into its risk cap. Inventory-skew maker, 20 000 steps, seed 42:

| Informed flow | MM fills | Final PnL vs. fair (ticks·shares) | Adverse selection (ticks) | Max \|inventory\| |
|---|---|---|---|---|
| 0 % (pure noise) | 773 | **+4 179** | 0.17 | 6 |
| 15 % | 641 | **+431** | 14.33 | 52 |
| 30 % | 88 | **−3 030** | 24.14 | 51 |

With no informed flow the maker profitably captures the spread while barely carrying
inventory; as toxic flow rises, PnL flips negative and the position is pushed to the
cap. (Numbers are seed- and machine-specific; regenerate with `--informed-frac`.)

### Why this is different from a "trading bot"
This is a **market-microstructure** study on a hand-built order book — passive quoting,
queue dynamics, and adverse selection — not a directional alpha strategy. It deliberately
keeps strategy/PnL plumbing thin; the focus is what only a real order book can show.

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

## Build & run

    brew install cmake                # once
    cmake -B build && cmake --build build
    ./build/orderbook_tests           # engine + microstructure unit tests
    ./build/orderbook                 # interactive REPL
    ./build/orderbook --mm-sim 20000 --policy as --informed-frac 30 --out mm.csv

    # benchmark (use an optimized build for numbers)
    cmake -B build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release
    ./build-release/orderbook --benchmark

## Simplifying assumptions

Single security; strictly sequential order arrival; no latency, fees, or
persistence; no self-trade prevention; no order modification; no hidden 
liquidity; the simulator is naive random flow around a drifting reference 
price, not a market model; only limit and market orders.
