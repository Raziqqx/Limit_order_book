#!/usr/bin/env bash
# Build, test, and run every measurement on a full ITCH day. Results go to results/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FILE="${1:-$HOME/itch-data/12302019.NASDAQ_ITCH50}"
OUT="$ROOT/results"
mkdir -p "$OUT"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/build" -j
"$ROOT/build/lob_tests" | tee "$OUT/tests.txt"
# 1. throughput (no per-message timing), memory prefaulted, pinned to one core
taskset -c 2 "$ROOT/build/itch_replay" "$FILE" --prefault --tob-csv "$OUT/tob_cpp.csv" | tee "$OUT/throughput.txt"
# 2. per-message latency percentiles
taskset -c 2 "$ROOT/build/itch_replay" "$FILE" --prefault --latency --symbols "" | tee "$OUT/latency.txt"
# 3. full verification against the reference book (slow)
"$ROOT/build/itch_replay" "$FILE" --verify --symbols "" | tee "$OUT/verify.txt"
# 4. independent Python cross-check of AAPL top of book
python3 "$ROOT/tools/itch_tob.py" "$FILE" AAPL 30 > "$OUT/tob_py_aapl.csv"
if diff <(grep -E '^time|AAPL' "$OUT/tob_cpp.csv") "$OUT/tob_py_aapl.csv"; then
  echo "AAPL top of book: C++ and Python agree at every checkpoint" | tee "$OUT/crosscheck.txt"
else
  echo "AAPL top of book: MISMATCH" | tee "$OUT/crosscheck.txt"
fi
# 5. matching-engine microbenchmark
taskset -c 2 "$ROOT/build/engine_bench" 20000000 | tee "$OUT/engine_bench.txt"
