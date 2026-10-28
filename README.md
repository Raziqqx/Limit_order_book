# Limit Order Book & Matching Engine (C++17) with NASDAQ ITCH 5.0 replay

A low-latency limit order book with a price-time-priority matching engine. It is validated by rebuilding every NASDAQ order book from a full trading day of real TotalView-ITCH 5.0 data.

- **Orders:** limit (GTC), market and IOC, plus cancel and modify (keeps priority on a size-down, loses it on a reprice or size-up).
- **O(1) cancel by order ID:** an intrusive doubly linked FIFO per price level, plus an open-addressing ID index.
- **No allocation after startup:** orders, levels, the ID table and the price ladders all come from preallocated `mmap` arenas.
- **Flat price array:** levels are indexed by tick offset from a reference price, not a `std::map`. A two-level bitmap finds the next best level, and the window re-centers when the market drifts.
- **Full-day ITCH replay:** reports throughput and per-message latency percentiles, and cross-checks the books several independent ways.

See **[NOTES.txt](NOTES.txt)** for the full write-up: background, every design decision and the reason for it, the verification strategy, pitfalls, and interview Q&A.

## Results: full day, NASDAQ 2019-12-30

Measured on one pinned core of an Intel Core i5-14500HX under WSL2. Data: 8.25 GB, 268,744,780 messages, 8,906 instruments. Raw output is in [`results/`](results/).

| Metric | Value |
|---|---|
| Throughput (median of 3 runs, book processing only) | **6.72 M msg/s** (~149 ns/msg, whole day in ~40 s) |
| Latency, all messages: p50 / p90 / p99 / p99.9 | **56 / 220 / 668 / 1,290 ns** |
| Latency p50 by type: add / delete / execute / cancel / replace | 45 / 62 / 71 / 35 / 116 ns |
| Peak live orders / peak price levels | 1,924,078 / 784,060 |
| Matching-engine microbenchmark (20M synthetic ops) | 4.0 M ops/s, p50 66 ns, p99 485 ns |

The p99.99 (~24 µs) and the max come from the OS (WSL timer interrupts, page faults), not from the book code. Full per-type tables are in NOTES.txt section 7.

## Correctness evidence

| Check | Result |
|---|---|
| Unit and randomized differential tests vs a naive `std::map`/`std::list` engine (1M ops, 4 configs), also under ASan/UBSan | pass |
| Top of book vs an independent reference book, after every book-changing message | 263,241,937 comparisons, 0 mismatches |
| Full depth of every book plus structural invariants, at every checkpoint | 311,710 comparisons, 0 mismatches, 0 failures |
| Executions during continuous trading that hit our top of book (NASDAQ fills price-first) | 5,683,178 of 5,683,178 (100%) |
| Unknown order refs, over-executions, locate mismatches, bad messages | all 0 |
| Crossed or locked books during continuous trading | 0 of 115,504 observations |
| AAPL top of book from an independent Python parser (`tools/itch_tob.py`) | identical at all 35 checkpoints |

## Layout

```
include/lob/    order book, matching engine, pool, ID map, bitmap, arena, latency tools
include/itch/   zero-copy ITCH 5.0 decoder and the multi-book builder
src/            itch_replay (full-day tool), engine_bench (matching microbenchmark)
tests/          unit + differential tests
tools/          independent Python cross-checker
scripts/        get_data.sh (download a day), run_full_day.sh (all measurements)
results/        raw output of the full-day run
```

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/lob_tests
scripts/get_data.sh                                    # ~3.5 GB download -> ~/itch-data
./build/itch_replay ~/itch-data/12302019.NASDAQ_ITCH50 --prefault            # throughput
./build/itch_replay ~/itch-data/12302019.NASDAQ_ITCH50 --prefault --latency  # percentiles
./build/itch_replay ~/itch-data/12302019.NASDAQ_ITCH50 --verify              # reference check
./build/engine_bench                                   # matching-engine microbenchmark
scripts/run_full_day.sh                                # everything, saved to results/
```

The sample data comes from NASDAQ: <https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/>
