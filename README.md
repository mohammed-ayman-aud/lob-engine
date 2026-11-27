# ITCH order book and matching engine (C++20)

I developed this to investigate how an exchange feed is transformed into an order book and at what speed. It decodes the NASDAQ TotalView-ITCH 5.0 binary feed and re-creates full-depth order books for all stocks. Price-time priority matching is then performed on the same data structures. A lock-free single-producer single-consumer (SPSC) queue facilitates communication between the feed and book threads.

Prior to optimizing the order book I ensured that it was valid. Main effort therefore focused on validation:

* **Differential validation.** I constructed a highly simplified Python order book (`tools/ref_book.py`) and validated that the C++ book was identical to this at full depth after replaying the same feed.
* **Fuzzing.** libFuzzer, ASan and UBSan feed random input to the parser. After each input the whole book is re-validated (checking links, level totals, price ordering and ensuring no orders are leaked).
* **Unit tests for each component.** This includes the time-priority rules within each price level and dual-thread operation of the SPSC queue.
* **Benchmarking.** Rather than simply obtaining an average, each individual operation is timed and p50/p99/p99.9 values are derived. All optimizations are benchmarked against a base version.

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

## What's in here

| Path | Contents |
|---|---|
| `include/lob/itch.hpp` | Zero-copy ITCH 5.0 decoder. It verifies that each frame's length is as expected for its message type. Invalid data therefore causes an error rather than reading past the end of the frame. |
| `include/lob/book.hpp` | Contains an `Order` structure (48 bytes, with the most frequently accessed fields placed first), a `Level` (intrusive FIFO order list) and a `Side` (price-to-level hash map and a price-ordered ladder, with the best price at the back). Also contains `validate()`. |
| `include/lob/book_builder.hpp` | Processes ITCH messages and applies them to all stocks' books. These messages are for adding, executing, canceling, deleting and replacing. As the specification dictates, a replaced order does not retain its original position in the queue. |
| `include/lob/matching.hpp` | Matching engine for limit, market and IOC orders and cancellations. Orders are filled first by best price and then by oldest. |
| `include/lob/pool.hpp` | An object pool with a free list. Once warmed up, nothing on the hot path calls malloc. |
| `include/lob/flat_map.hpp` | Linear-probing open-addressing hash map. Deletion is via backward shifting (no tombstones). |
| `include/lob/spsc.hpp` | SPSC ring buffer inspired by the LMAX Disruptor, using acquire/release semantics only. Cache lines are padded and each thread caches the other thread's index. |
| `include/lob/histogram.hpp` | Self-implemented log-linear latency histogram. Based on HdrHistogram, with about 1.6% precision. |
| `src/replay.cpp` | Command-line tool for benchmarking throughput, per-message latency and the two-thread pipeline, along with the baseline build and book dump. |
| `bench/` | Benchmarks for the matching engine (fast vs baseline) and the SPSC queue vs a `std::mutex` queue. |
| `fuzz/` | libFuzzer harness and a minimized test corpus of 119 inputs. |
| `tools/gen_itch.py` | Synthetic ITCH generator that follows the specification exactly, with realistic message proportions. |
| `tools/ref_book.py` | Reference Python book for differential testing. |
| `tools/run_real_data.sh` | Downloads one real NASDAQ trading day and benchmarks everything on it, including `perf stat` cache-miss statistics. |

## Speeds

* **Order execution, cancellation and deletion are O(1).** This comprises a hash lookup of the order ID and then detaching the order from its level.
* **Adding at an existing price is also O(1).** This is a hash lookup of the price level, followed by an append to the level.
* **Creating or deleting a price level** costs O(log L) to locate its position on the price ladder, plus shifting the levels in front of it. Although in theory this shifting could be costly, in practice it is limited because most trading activity occurs near the best price.

## Building and testing

```bash
cmake -S . -B build && cmake --build build -j && ./build/unit_tests      # 44 checks
python3 tools/gen_itch.py data/small.itch --messages 200000 --stocks 50
./build/replay data/small.itch --dump cpp.txt --depth 100000
python3 tools/ref_book.py data/small.itch --depth 100000 > py.txt && cmp cpp.txt py.txt   # identical
```

To enable fuzzing you need clang:
```bash
clang++ -std=c++20 -Iinclude -g -O1 -fsanitize=fuzzer,address,undefined fuzz/fuzz_itch.cpp -o fuzz_itch
./fuzz_itch fuzz/corpus -max_total_time=600 -max_len=4096
```

## Results so far

All data was generated using **synthetic** ITCH (20M messages, 500 stocks) on a **shared 4-core cloud VM** (Xeon @ 2.1 GHz) with no core isolation. The raw results can be seen in `results/synthetic_vm.txt`. Running `tools/run_real_data.sh` on a Linux machine produces real NASDAQ results and `perf stat` cache-miss statistics.

**Correctness:**

| Check | Result |
|---|---|
| C++ vs Python reference, full depth, 200k messages | identical (fast, baseline and pipeline builds) |
| Fuzzing for 10 min with ASan + UBSan + `validate()` after every input | 904,481 inputs, 0 crashes, 0 invariant violations |
| Line / branch coverage of the parser and book headers from the fuzz corpus | 96.6% / 91.2% (`llvm-cov`) |

**Book reconstruction (`replay`):**

| Build | Throughput | Per-message p50 / p99 / p99.9 |
|---|---|---|
| pool + flat map | **5.0 M msg/s** (199 ns/msg) | 339 / 847 / 4,287 ns |
| baseline: `new`/`delete` + `std::unordered_map` | 2.35 M msg/s | 599 / 1,631 / 10,623 ns |

Per-message latency incorporates approximately 20 ns of `rdtsc`/`lfence` timing overhead. Hence it reads higher than the throughput figure, which is not timed per message.

**Matching engine (`bench_matching`):** 5M seeded operations and 2.2M fills, all individually timed.

| Build | p50 | p99 | p99.9 |
|---|---|---|---|
| pool + flat map | 103 ns | 419 ns | 639 ns |
| baseline: `new`/`delete` + `std::unordered_map` | 120 ns | 927 ns | 1,743 ns |
| **reduction** | 14% | **55%** | **63%** |

While the median changed only slightly, p99 was more than halved. Allocation and node-based hash maps were most detrimental in the tail.

**SPSC ring vs `std::mutex` + `std::deque` (`bench_spsc`):**

| | SPSC ring | mutex queue |
|---|---|---|
| Back-to-back throughput, 20M items | 75.8 ns/item (this varied a lot between runs on this VM) | 238.7 ns/item |
| One-way p50 latency, paced, 1M items | 221 ns | 10,367 ns |

## What I learned

* **Dual threading was actually slower than single threading** (3.35 vs 5.0 M msg/s). While the cost of splitting frames from a memory-mapped file is negligible, handing them to another thread was more costly. The queue should prove valuable once the feed thread has real work to do, such as socket input, packet sequencing and gap recovery. I had anticipated the reverse and have therefore kept this result in.
* **A failed approach.** Order IDs are issued sequentially by the exchange, so I attempted to use the order ID's own value as its hash to boost cache locality. However, this slowed replay by about **23x** (0.22 vs 5.02 M msg/s) (still available with `--identity-hash`). Some orders remain on the book all day, so the live order IDs become spread more widely than the table. New IDs wrap around and land on those older orders, and combined with linear probing this causes massive clustering.
* **The VM is not particularly quiet.** The millisecond-level maximum latencies most likely reflect the hypervisor suspending the virtual CPU, not the code. Pinning threads to dedicated cores (`isolcpus`, `taskset`) should help to tighten p99.9.

## Next steps

* Benchmark a full NASDAQ trading day and obtain `perf stat` cache-miss statistics on my own machine.
* Read the feed from a socket rather than a file, so that the second thread has real work to do.
* Implement packet gap detection and recovery.

I am interested to see how much the tail latency can be reduced by switching to pinned cores, as opposed to a shared VM.
