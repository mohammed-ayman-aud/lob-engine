#!/usr/bin/env bash
# Run the engine on a real NASDAQ TotalView-ITCH 5.0 day and collect the numbers for the README.
# Needs: Linux x86-64, cmake, a C++20 compiler, ~12 GB free disk, and optionally `perf`.
#   ./tools/run_real_data.sh [YYYYMMDD-file-stem]    (default: 01302019, one full trading day)
set -euo pipefail
cd "$(dirname "$0")/.."
DAY=${1:-01302019}
FILE="data/${DAY}.NASDAQ_ITCH50"
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${DAY}.NASDAQ_ITCH50.gz"

mkdir -p data results
if [[ ! -f "$FILE" ]]; then
  echo "downloading $URL (several GB)"
  curl -fL --retry 3 -o "$FILE.gz" "$URL"
  gunzip "$FILE.gz"
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j >/dev/null
./build/unit_tests

OUT="results/real_${DAY}.txt"
{
  echo "# $(date -u) $(uname -srm)"; lscpu | grep -E "Model name|^CPU\(s\)" || true
  echo; echo "## throughput, single thread";        ./build/replay "$FILE"
  echo; echo "## throughput, baseline";             ./build/replay "$FILE" --baseline
  echo; echo "## two-thread pipeline (SPSC)";       ./build/replay "$FILE" --pipeline
  echo; echo "## per-message latency";              ./build/replay "$FILE" --latency
  echo; echo "## per-message latency, baseline";    ./build/replay "$FILE" --latency --baseline
  if command -v perf >/dev/null; then
    for mode in "" "--baseline"; do
      echo; echo "## perf stat $mode"
      perf stat -e cycles,instructions,L1-dcache-load-misses,LLC-load-misses,LLC-loads ./build/replay "$FILE" $mode 2>&1 | tail -15
    done
  else
    echo; echo "(perf not installed: skip cache-miss counts; on Ubuntu: sudo apt install linux-tools-\$(uname -r))"
  fi
  echo; echo "## matching engine";                  ./build/bench_matching
  echo; echo "## spsc";                             ./build/bench_spsc 0 2
} | tee "$OUT"
echo "saved $OUT"
