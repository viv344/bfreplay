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

Dependencies: a C++20 compiler, CMake 3.20 or later, libbz2, libzstd, GoogleTest.
