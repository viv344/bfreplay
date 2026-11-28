# bfreplay: event-driven Betfair Exchange replay engine (C++20)

`bfreplay` replays Betfair Exchange historical stream data message by message through a
trading strategy. It simulates order placement, cancellation and latency, then settles at
Betfair Starting Price (BSP) using Betfair's net-per-market commission rule.

It was built as research infrastructure for testing pre-off horse racing strategies under
realistic execution assumptions: strategies only ever see data that has already arrived,
orders take time to reach the exchange, and settlement follows Betfair's rules.

The public repository ships an example strategy (`LayFavourite`: lay the favourite at BSP
when its price is inside a band). It demonstrates the interface and is not expected to be
profitable. The proprietary strategies the engine was built for are not included.

## Data

Betfair's BASIC historical stream (1-minute last-traded prices, market definitions, BSP and
results) for every GB/IE horse racing win market from 1 Nov 2024 to 25 Nov 2026. That is
644,094 market files, of which 26,436 are GB/IE win markets, carrying 11.8M messages. The data
is licensed by Betfair and is not redistributed here; download it from
historicdata.betfair.com.

## Performance

AMD Ryzen AI 7 350 laptop (8 cores / 16 threads). All runs produce identical output.

| Configuration | Two-year replay | Throughput |
|---|---|---|
| Python reference implementation (16 processes) | 56 s | 0.2M msg/s |
| C++, raw bzip2, full directory scan | 22 s | 0.5M msg/s |
| C++, bzip2, index cache (skip the 96% non-target files) | 5.5 s | 2.1M msg/s |
| **C++, zstd cache** (`bfconvert`, one-off 5.5 s, 3.5 GB to 289 MB) | **1.8 s** | **6.6M msg/s** |
| C++, zstd cache, single thread | 7.7 s | 1.5M msg/s |

- JSON parsing: **1.5 GB/s, 494 ns per message, single thread** (`bench_json`).
- Timing bzip2 decompression on its own (26.3 s of the 32 s single-thread bzip2 replay, about 82%)
  showed it dominated the run, which is what motivated the zstd cache.

## Design

```
            .bz2 / .zst market file
                     │  MarketFile: whole-file decompress, split into string_view lines;
                     │  .bz2 is rejected after the first 64 KB if the header is not a GB/IE WIN market
                     ▼
            json::Document (arena parser; zero allocations in steady state; views into input)
                     │
                     ▼
            replay_file(): apply marketDefinition / rc price updates to MarketView
                     │  detect the off (first in-play definition) and stop
                     │  after each message: strategy.on_update(view, pt) -> wanted orders
                     │  diff against resting orders -> place / cancel (counted)
                     ▼
            settle at BSP from the final market definition; latency cut-off;
            withdrawn runners void; commission on net market winnings
```

- **`include/bfr/json.hpp`:** a purpose-built JSON parser. It builds a flat node arena whose
  strings point back into the input buffer, so no string is ever copied. The arena is
  reused line after line. It accepts Python-style `NaN`/`Infinity` and stringly-typed
  numbers (`"bsp":"Infinity"`), which appear in Betfair's data.
- **`include/bfr/strategy.hpp`:** the `Strategy` interface plus the example `LayFavourite`.
  Strategies see only data that has already arrived, so look-ahead is impossible by
  construction. Further strategies can be added in an untracked `private/` directory, and
  CMake builds them in automatically when present.
- **`src/sim.cpp`:** the replay and settlement logic, matching Betfair's rules for lay-SP
  orders (sized by liability, matched at BSP, void if the runner is withdrawn).
- **`src/main.cpp`:** a lock-free work queue across threads (atomic index), the index cache,
  and reporting.
- **`tools/bfconvert.cpp`:** a parallel bzip2-to-zstd converter.

## Verification

- GoogleTest unit tests cover the parser (escapes, malformed input, arena reuse), the
  example strategy (band, eligibility, original placement time, switching) and end-to-end
  replay of synthetic `.bz2` markets (latency cut-off, settlement, net-market commission).
- **Differential test against an independent Python implementation** of the same simulator
  and a research strategy: over two years of data, all 3,417 bets match one-for-one on
  market, selection, BSP, result, timing and price. Stakes agree to within 5e-9, which is
  CSV print precision.

## Live trading: `bflive`

`bflive` runs the **same `MarketEngine` and `Strategy` code** against the live Betfair
Exchange Stream API. Historical files are recorded stream output, so the backtested logic
is exactly the logic that trades.

- Certificate (non-interactive) login, with session keep-alive and re-login.
- TLS Stream API client (OpenSSL): authentication, market-filter subscription, heartbeat
  timeout, and resubscription from the last `initialClk`/`clk` after a disconnect.
- Raw stream recording (`live_data/stream-YYYYMMDD.jsonl`) for later replay and audit.
- **Dry run by default:** intended orders are logged and settled on paper at BSP when each
  market closes. `--live` places real `MARKET_ON_CLOSE` lay orders through a REST worker
  thread, so the stream reader never blocks on HTTP.
- Acts only inside a trade window before the scheduled start (`--window-min`, default 30).
  The window changes no backtest result, because only orders standing at the off are matched.
- Warm-up: no orders until `--warmup-min` (default 60) of continuous data has been seen,
  after start-up and after any data gap longer than 5 minutes (resting orders are cancelled
  on a gap). Gaps are measured on the wall clock, so machine suspends are caught.
- `run_bflive.sh` keeps the machine from sleeping while the bot runs (`systemd-inhibit`).
- Hard limits: maximum liability per order, orders per day and liability per day. A kill
  switch (`touch <config dir>/STOP`) cancels all open orders and halts. Orders are also
  cancelled on shutdown.

```bash
./build/bflive --config ~/.config/bflive/config.ini --strategy lay-favourite          # dry run
./build/bflive --config ~/.config/bflive/config.ini --strategy lay-favourite --live \
               --max-liability 20 --max-orders-per-day 40 --max-daily-liability 800
```

Order-book recording (no strategy, no orders): `--record-only` with `--fields` chooses the
Stream API data fields, e.g. the full ladder, traded volume per price and projected SP:

```bash
./build/bflive --record-only --record-dir book_data --log rec_log.csv \
  --fields '["EX_MARKET_DEF","EX_ALL_OFFERS","EX_TRADED","EX_TRADED_VOL","EX_LTP","SP_PROJECTED","SP_TRADED"]'
```

`bfkeys` lists the account's delayed and live app keys and whether each is active.

## Market making: queue-aware maker simulator (`bfmm`)

`include/bfr/mm/` simulates passive limit orders (quotes) against recorded order-book streams:

- **Prices as tick indices** on Betfair's 350-step ladder (`ticks.hpp`), so "one tick better"
  is exact.
- **Order book per runner** (`book.hpp`) from `atb`/`atl`/`trd`/`spn` deltas. Full images reset the
  book without counting their volume as new trades.
- **Fill model** (`sim.hpp`):
  - New orders join the back of the displayed queue as it stands when they arrive (after latency).
  - Matched volume at our price consumes the queue ahead first. Betfair reports traded volume as
    backer's stake x 2, so `--traded-scale` defaults to 0.5.
  - Cancellations shrink the queue ahead pro-rata, or only after the queue behind us is gone
    (`--cancel behind`, pessimistic), or by a power-law share (`--cancel power`).
  - A trade at a worse price, or the opposite side crossing our price, fills us completely.
  - Latency applies before an order can fill, and cancelled orders stay fillable until the cancel
    lands (`--cancel-latency-ms`).
  - No fills while the market is suspended. A non-runner with a reduction factor of 2.5% or more
    cancels unmatched orders and reduces matched prices; bets on the removed runner are void.
  - Minimum stake and instructions per clock hour (transaction charges) are tracked.
- **BSP reconciliation at the off** (Betfair SP rules): an order still resting with persistence
  LAPSE is matched at its own price only when that is worse than BSP for us (lay P >= BSP,
  back P <= BSP), otherwise it lapses. TAKE_SP orders become SP bets at BSP (a lay keeps its
  liability and lapses below £10; a back keeps its stake and lapses below £2). Changing
  persistence keeps the queue place, as `updateOrders` does. Orders still in flight at the off
  are not included. `--no-reconcile` restores plain lapsing; `--take-sp` quotes with TAKE_SP.
  - Post-only: quotes never cross the book or our own orders.
  - Orders lapse at the off.
- **Settlement:** P&L on the result, P&L with exposure closed at BSP (low variance), and the
  edge of every fill against BSP. Commission is charged on net market winnings.
- **Example strategy** (`join_touch.hpp`): joins or improves the touch in wide spreads when the
  price is no worse than projected SP, and skews away from accumulated exposure.

```bash
./build/bfmm --cancel behind --cancel-latency-ms 300 --markets markets.csv book_data/*.jsonl
```

Recordings made with a delayed app key are conflated (one update per ~3 minutes), which is
fine for spread and depth studies but too coarse for fill modelling. Use live-key recordings for that.

## Build and run

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build
./build/bfreplay DATA/BASIC                                # full scan; writes the index cache
./build/bfconvert DATA/BASIC DATA/zst                      # one-off zstd cache
./build/bfreplay DATA/zst --strategy lay-favourite --lo 1.5 --hi 4 --out bets.csv
```

Options: `--lo`, `--hi`, `--liability £`, `--threads N`, `--out FILE`. The output reports
P&L at 2% and 6% commission, split at 1 Nov 2025.

Dependencies: a C++20 compiler, CMake 3.20 or later, libbz2, libzstd, OpenSSL, libcurl, GoogleTest.
