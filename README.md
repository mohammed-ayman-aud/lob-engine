# ITCH order book and matching engine (C++20)

A full-depth limit order book rebuilt from the NASDAQ TotalView-ITCH 5.0 binary feed, plus a
price-time-priority matching engine on the same data structures and a lock-free SPSC queue that
connects the feed thread to the book thread. Correctness comes before speed:

* **Differential testing:** after replaying the same feed, the C++ book matches a deliberately
  simple Python reference book level for level at full depth (`tools/ref_book.py`).
* **Fuzzing:** libFuzzer with ASan and UBSan. After every input, the book's whole structure is
  re-validated (links, level totals, ladder order, leaked orders).
* **Unit tests** cover each component, including queue-priority rules and the SPSC queue under
  two threads.
* **Benchmarks** report p50/p99/p99.9 from per-operation timing, never just a mean, and every
  optimisation is measured against a baseline.

```
 ITCH file (mmap, zero-copy)
        │  [u16 len][msg][u16 len][msg]...
        ▼
 feed thread ── framing ──► SpscQueue<Frame> ──► book thread ── itch::decode ──► BookBuilder
   (CPU 0)                  (lock-free ring)        (CPU 1)                         │
                                                                     per instrument ▼
                                                    Book ─ bids: Side ─ price→Level map + sorted ladder
                                                         └ asks: Side    Level = FIFO of intrusive Orders
```

## Layout

| Path | What it is |
|---|---|
| `include/lob/itch.hpp` | Zero-copy ITCH 5.0 decoder. Frame lengths are checked against the spec size of every message type, so malformed input returns an error instead of reading past the frame. |
| `include/lob/book.hpp` | `Order` (48 B, hot fields first), `Level` (intrusive FIFO), and `Side`, which holds a price→level hash map plus a price-sorted ladder with the best price at the back. Includes `validate()`. |
| `include/lob/book_builder.hpp` | ITCH visitor that maintains every instrument's book: add, execute, cancel, delete and replace (replace loses time priority, per the spec). |
| `include/lob/matching.hpp` | Matching engine: limit, market and IOC orders and cancels; fills best price first, oldest first. |
| `include/lob/pool.hpp` | Arena / free-list object pool. No malloc on the hot path after warm-up. |
| `include/lob/flat_map.hpp` | Open-addressing hash map: linear probing, backward-shift deletion, no tombstones. |
| `include/lob/spsc.hpp` | Disruptor-style SPSC ring: acquire/release only, padded cache lines, cached peer indices. |
| `include/lob/histogram.hpp` | Log-linear (HdrHistogram-style) latency histogram, accurate to about 1.6%. |
| `src/replay.cpp` | CLI: throughput, per-message latency, two-thread pipeline, baseline, book dump. |
| `bench/` | Matching-engine latency (fast vs baseline) and SPSC vs `std::mutex` queue. |
| `fuzz/` | libFuzzer harness and minimised corpus (119 inputs). |
| `tools/gen_itch.py` | Generates spec-exact synthetic ITCH with realistic message ratios. |
| `tools/ref_book.py` | Python reference book used for the differential tests. |
| `tools/run_real_data.sh` | Downloads a real NASDAQ day and runs everything, including `perf stat` cache-miss counts. |

## Complexity

* **Execute, cancel and delete are O(1):** an order-ref hash lookup, then an unlink from an
  intrusive list.
* **Adding at an existing price is O(1):** a price→level hash lookup, then an append.
* **Creating or removing a price level** costs O(log L) to find its slot in the ladder, plus a shift
  of the levels better than it. That shift is short because activity clusters at the inside.

## Build and test

```bash
cmake -S . -B build && cmake --build build -j && ./build/unit_tests      # 44 checks
python3 tools/gen_itch.py data/small.itch --messages 200000 --stocks 50
./build/replay data/small.itch --dump cpp.txt --depth 100000
python3 tools/ref_book.py data/small.itch --depth 100000 > py.txt && cmp cpp.txt py.txt   # identical
```

Fuzzing needs clang:
```bash
clang++ -std=c++20 -Iinclude -g -O1 -fsanitize=fuzzer,address,undefined fuzz/fuzz_itch.cpp -o fuzz_itch
./fuzz_itch fuzz/corpus -max_total_time=600 -max_len=4096
```

## Results so far

All numbers below were measured on a **shared 4-vCPU cloud VM** (Xeon @ 2.1 GHz) with no core
isolation, running **synthetic** ITCH (20M messages, 500 symbols). Raw output is in
`results/synthetic_vm.txt`. Run `tools/run_real_data.sh` on your own Linux box to get
real-NASDAQ numbers and `perf` cache counters.

**Correctness:**

| Check | Result |
|---|---|
| C++ vs Python reference, full depth, 200k messages | identical (fast, baseline and pipeline builds) |
| Fuzzing, 10 min, ASan + UBSan + `validate()` after every input | 904,481 inputs, 0 crashes, 0 invariant violations |
| Line / branch coverage of the parser and book headers, from the fuzz corpus | 96.6% / 91.2% (`llvm-cov`) |

**Book reconstruction (`replay`):**

| Build | Throughput | Per-message p50 / p99 / p99.9 |
|---|---|---|
| pool + flat map | **5.0 M msg/s** (199 ns/msg) | 339 / 847 / 4,287 ns |
| baseline: `new`/`delete` + `std::unordered_map` | 2.3 M msg/s | 599 / 1,631 / 10,623 ns |

Per-message latency includes about 20 ns of `rdtsc`/`lfence` overhead. That is why it reads higher
than the throughput figure, which is untimed.

**Matching engine (`bench_matching`):** 5M seeded operations, 2.2M fills, each one timed.

| Build | p50 | p99 | p99.9 |
|---|---|---|---|
| pool + flat map | 103 ns | 419 ns | 639 ns |
| baseline | 120 ns | 927 ns | 1,743 ns |
| **reduction** | 14% | **55%** | **63%** |

The tail is where allocation and node-based hashing hurt: the median barely moves, but p99 is cut
by more than half.

**SPSC ring vs `std::mutex` + `std::deque` (`bench_spsc`):**

| | SPSC ring | mutex queue |
|---|---|---|
| Throughput, back to back | 38 to 89 ns/item (varies run to run on this VM) | 239 to 322 ns/item |
| One-way latency p50, paced | 221 ns | 10,367 ns |

**Findings worth knowing:**

* **The two-thread pipeline is slower than single-threaded here** (3.35 vs 5.0 M msg/s). Framing a
  memory-mapped file costs almost nothing, so the hand-off costs more than it saves. The queue pays
  off when the feed thread has real work, such as socket reads, packet sequencing or gap recovery.
  Measuring this and saying so is the point.
* **A negative result:** I tried identity-hashing order refs, which the exchange assigns
  sequentially, to get cache locality. It made replay **24x slower** (`--identity-hash`): some
  orders rest all day, so the live refs span more than the table size, wrapped refs collide with
  those survivors, and linear probing builds huge clusters.
* **The VM is noisy:** max latencies in the millisecond range are the hypervisor descheduling the
  vCPU, not the code. Isolated cores (`isolcpus`, `taskset`) would tighten p99.9.

## Resume bullets (fill in after `run_real_data.sh`)

These match what the code actually does. Replace the `[real]` values with your own machine's numbers.

* Built a NASDAQ TotalView-ITCH 5.0 parser and full-depth order book reconstruction in C++20,
  sustaining [real] M msgs/sec on a full historical trading day (5.0 M/s on synthetic data on a
  shared VM). Execute, cancel and delete are O(1) via intrusive price levels.
* Validated the book by differential testing against a Python reference implementation (identical
  at full depth) and by fuzzing the zero-copy parser with libFuzzer, ASan and UBSan: 900k+ inputs
  with structural invariant checks and 96.6% line coverage.
* Implemented a price-time-priority matching engine. A custom arena allocator and an
  open-addressing hash map cut p99 latency by 55% and p99.9 by 63% vs. `new`/`std::unordered_map`,
  measured per operation with an HdrHistogram-style histogram on pinned cores.
* Connected the feed and book threads with a lock-free SPSC ring buffer (acquire/release, padded
  cache lines): about 3x the throughput of a `std::mutex` queue. Measured where the extra thread
  helps and where it doesn't.

Notes on the original bullets:
* The harness is self-written rather than Google Benchmark, and the histogram is HdrHistogram-style
  rather than the library itself. Either describe it that way or swap the libraries in.
* Fuzzing found **0** bugs in this run, so don't claim "fixed N edge cases". Claim what is true: the
  inputs run, the invariants checked and the coverage reached.
* Cache-miss claims need `perf stat` numbers from your own machine, which `run_real_data.sh` collects.
