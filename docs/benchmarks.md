# Benchmarks

Measured numbers for XiaPL, plus the head-to-head comparison against the two
best-known open-source 7-card evaluators. Everything here is a same-machine,
same-compiler, same-flags, single-thread measurement — no figure is quoted
from a third party's own README.

- [7-card evaluator head-to-head](#7-card-evaluator-head-to-head)
- [In-tree microbenchmarks](#in-tree-microbenchmarks)

---

## 7-card evaluator head-to-head

XiaPL is compared against
[PH Evaluator](https://github.com/HenryRLee/PokerHandEvaluator)
(`master` @ `6134a3b8`, project version 0.6.0, Apache-2.0) and
[OMPEval](https://github.com/zekyll/OMPEval)
(`master` @ `4aec210f`, MIT-style licence), both unmodified, both built from
source with the same optimization flags as XiaPL in every configuration below.

### What is being compared

The three libraries do not have the same output contract, so the table
publishes every row that is needed to read the comparison honestly rather
than a single cherry-picked one:

| Row | Contract |
|---|---|
| `xiapl::evaluate_mask()` | **the public API.** Takes a 52-bit card mask, validates the card count, and returns a decoded `HandValue` (category + tie-breaker ranks). |
| `xiapl::internal::eval_score7()` | the internal kernel behind it: mask in, packed comparable score out, no decode. **Not callable from outside the library** — it is listed because it is the like-for-like peer of a rank-returning evaluator, not as "XiaPL's speed". |
| PH Evaluator `evaluate_7cards()` | 7 card ids in, integer rank out (1..7462, lower is better). |
| OMPEval `evaluate()`, from 7 cards | builds an `omp::Hand` from 7 card ids, then evaluates. **This is the comparable row**: it is the only one that starts from loose cards like the other two libraries. |
| OMPEval `evaluate()`, `Hand` prebuilt | evaluation only, with `Hand` construction hoisted out of the timed region. OMPEval's API is built around an incrementally accumulated `Hand`, so an enumeration that reuses a shared board approaches this figure while a one-shot 7-card evaluation pays the build cost. The two OMPEval rows bracket its real cost. |

Two contexts are reported for every row, because they rank the libraries
differently:

- **batch** — independent evaluations in a tight loop; instruction-level
  parallelism is free. This is what a solver enumerating showdowns sees.
- **latency** — a dependent chain, where each evaluation must retire before
  the next one can start. This is the critical-path cost.

All numbers are **ns/eval, median of 15 reps**, lower is better.

### Apple M1 Pro (arm64)

macOS, Apple clang 17, all three libraries built `-O3 -DNDEBUG` with no
`-march` / `-mcpu`. Rebuilding everything with `-mcpu=apple-m1` moved every
median by less than the run-to-run spread and changed no ordering, so the
plain `-O3` figures — what a default build gives — are the ones published.

| library / entry point | batch | latency |
|---|---:|---:|
| **xiapl `evaluate_mask()`** (public API) | **10.55** | **18.08** |
| xiapl `eval_score7()` (internal kernel) | 8.17 | 12.87 |
| PH Evaluator `evaluate_7cards()` | 16.79 | 26.12 |
| OMPEval, from 7 cards | 2.55 | 14.72 |
| OMPEval, `Hand` prebuilt | 0.86 | 10.21 |

On arm64 OMPEval compiles to its portable scalar fallback: its SSE2/SSE4.1
paths are x86-only and are guarded out. Its numbers here are therefore a
valid same-machine measurement but not the path the library was written for —
which is what the second machine is for.

### Intel Xeon Silver 4416+ (x86_64, Sapphire Rapids)

Debian, clang 14, one thread pinned to a single core. Two configurations:
`baseline` = `-O3 -DNDEBUG` (a default build, no ISA flags — OMPEval takes
its SSE2 path), `native` = `-O3 -DNDEBUG -march=native`.

| library / entry point | batch (baseline) | batch (native) | latency (baseline) | latency (native) |
|---|---:|---:|---:|---:|
| **xiapl `evaluate_mask()`** (public API) | **20.72** | **19.63** | **29.31** | **21.89** |
| xiapl `eval_score7()` (internal kernel) | 15.11 | 10.15 | 21.40 | 15.94 |
| PH Evaluator `evaluate_7cards()` | 20.00 | 18.08 | 28.14 | 25.84 |
| OMPEval, from 7 cards | 2.82 | 3.03 | 17.89 | 16.43 |
| OMPEval, `Hand` prebuilt | 1.37 | 1.26 | 13.11 | 12.13 |

A third configuration compiling OMPEval on its SSE4.1 path (`-msse4.1`)
landed within the run-to-run spread of the SSE2 baseline — 1.38 batch /
13.32 latency prebuilt, 2.95 / 18.21 from 7 cards — so SSE4.1 versus SSE2
makes no material difference for OMPEval on this machine.

### AMD Ryzen 7 3700X (x86_64, Zen 2)

A third machine, and the first consumer desktop part on this page (the
other two are a laptop chip and a server chip) — included specifically to
check whether the POPCNT / ISA-baseline finding below generalizes past a
data-center CPU. WSL2 on Windows (kernel
`6.18.33.2-microsoft-standard-WSL2`), Ubuntu, clang 18.1.3, one thread
pinned with `taskset -c 2`, system load confirmed quiet before the run.
Measured under WSL2 rather than bare metal; the numbers are consistent with
bare-metal expectations, with roughly 6-10% more run-to-run spread on the
headline cells than the other two machines. Same two configurations as the
Xeon: `baseline` = `-O3 -DNDEBUG` (OMPEval's SSE2 path), `native` =
`-O3 -DNDEBUG -march=native`.

| library / entry point | batch (baseline) | batch (native) | latency (baseline) | latency (native) |
|---|---:|---:|---:|---:|
| **xiapl `evaluate_mask()`** (public API) | **21.12** | **13.51** | **23.37** | **15.01** |
| xiapl `eval_score7()` (internal kernel) | 16.39 | 9.07 | 18.84 | 11.04 |
| PH Evaluator `evaluate_7cards()` | 17.61 | 17.14 | 22.85 | 22.61 |
| OMPEval, from 7 cards | 3.07 | 3.09 | 13.03 | 13.05 |
| OMPEval, `Hand` prebuilt | 1.28 | 1.25 | 8.14 | 8.13 |

A third configuration compiling OMPEval on its SSE4.1 path again landed
within the run-to-run spread of the SSE2 baseline — 1.27 batch / 8.13
latency prebuilt, 3.07 / 13.03 from 7 cards — so SSE4.1 versus SSE2 is
immaterial on Zen 2 as well, the same conclusion as on the Xeon.

One observation worth stating plainly across all three machines on this
page: PH Evaluator is nearly machine-invariant (roughly 17-20 ns/eval batch
on the M1 Pro, the Xeon, and here), while XiaPL instead scales with ISA and
clock speed (16.39 ns baseline down to 9.07 ns native on this machine
alone) — so the XiaPL-vs-PHE batch gap ranges from about 1.07x here
(baseline) up to 2.1x on the M1 Pro.

### x86 note: the flush check and POPCNT

XiaPL's flush detection popcounts each suit lane. The generic `x86-64`
baseline target predates the POPCNT instruction, so a default `-O3` build
lowers those popcounts to a SWAR bit-twiddling sequence. A diagnostic
variant that replaces them with a byte lookup table (verified bit-identical
over the whole hand set) measured 11.14 ns/eval batch against 14.38 for the
unmodified kernel — so that lowering costs roughly 23% of the kernel's batch
time in the baseline configuration. Both figures come from one separate
diagnostic run in which the two variants were measured back-to-back, which
is why 14.38 is not exactly the 15.11 in the table above; only the paired
comparison is quoted. `-march=native` (or just `-mpopcnt` /
`-march=x86-64-v2`) emits the
real instruction and recovers most of that, which is visible in the `native`
column above. On arm64 the same diagnostic was within noise, popcount being
cheap there. If you build XiaPL for a specific x86 host, passing an ISA
baseline of `x86-64-v2` or newer is worth it.

The same diagnostic was repeated on the Ryzen 7 3700X (Zen 2, WSL2, clang
18): the byte-lookup-table kernel variant measured 12.94 ns/eval batch
against 16.13 for the unmodified kernel in the same run — a 20% cost, close
to the Xeon's 23% and confirming the x86-64-v2 wheel baseline is the right
floor on a consumer part too, not just a data-center one.

### Correctness cross-check

In every configuration, all three libraries were run over **1,000,000 random
7-card pairs** and compared on the *ordering* they induce (the score scales
differ: XiaPL's packed score and OMPEval's rank are higher-is-better, PH
Evaluator's is lower-is-better). **All three agreed on every pair**, in all
four build configurations, including all 758 exact ties — a tie in one
library and a strict order in another would have been counted as a mismatch.
All nine hand categories were exercised.

The same 1,000,000-pair, fixed-seed check was repeated on the Ryzen 7
3700X: all three libraries again agreed on every pair, in all four build
configurations measured there (baseline, SSE4.1, native, and the POPCNT
diagnostic), with the same 758 exact ties as the two machines above — the
hand set and tie count come from the fixed seed, not the host, so
reproducing them exactly is the expected outcome, not a coincidence.

### Method

- **Hand set**: 4,194,304 hands drawn uniformly from C(52,7) with a fixed
  seed (`0xC0FFEE12345678`), generated once in a neutral rank/suit space and
  converted to each library's native input representation **outside every
  timed region**.
- **Footprint**: each rep evaluates 4,194,304 hands as 32 passes over the
  first 131,072 of them, which keeps the inputs and every lookup table
  cache-resident and isolates evaluator cost from DRAM bandwidth. 131 k
  distinct hands is far past what a branch predictor can memorize, so the
  repeated passes do not flatter the branchy evaluators. On the M1 a
  streaming variant over all 4,194,304 distinct hands landed within a few
  percent of the cache-resident figures for every library.
- **Timing**: 3 warmup reps discarded, then 15 measured reps; median
  reported. Libraries are round-robin interleaved *within* each rep rather
  than run in blocks, so any drift or interference hits all of them alike.
- **Single thread throughout.** No figure on this page is a multi-thread
  number, and none of them is a scaling claim.
- **Identical flags per configuration** for all three libraries, verified by
  reading the generated build flags rather than assuming what CMake did
  (XiaPL's CMake target carries no `-O3` of its own — an untyped configure
  measures a `-O0` build).
- The latency loop derives the next hand's index from the previous result
  through a compiler-opaque launder, so no evaluation can start before the
  previous one retires. The extra chain cost is identical for all libraries,
  and the reported latency includes that small fixed harness overhead.

### How to read this

Against PH Evaluator, XiaPL's public entry point is clearly ahead on arm64
(1.6x in batch, 1.4x in latency) and the two trade places on x86: PH
Evaluator takes batch by a few percent, XiaPL takes latency once
`-march=native` is in play and gives up a few percent without it. Against
OMPEval, XiaPL loses batch throughput by a wide margin on both machines,
while the latency gap is small — on the M1 the internal kernel (12.87) is in
fact ahead of OMPEval's from-cards path (14.72). The reason is architectural
and worth stating plainly:
**PH Evaluator and OMPEval are table-driven** — hash the hand, one lookup
returns the final rank — **whereas XiaPL computes the hand category and
kickers arithmetically at run time**, through a branch tree and a
count-leading-zeros kicker loop. That buys a static table small enough to
stay resident next to everything else, `constexpr` tables with no runtime
initialization step, and a score that decodes directly into a `HandValue`;
it costs batch throughput. The batch-to-latency ratio is the fingerprint:
OMPEval overlaps several evaluations at once, XiaPL far fewer.

Evaluation speed is also not the whole cost of an equity query — board
enumeration, range iteration and RNG sit on the same path — which is what
the in-tree equity microbenchmark below measures.

---

## In-tree microbenchmarks

Two chrono-based microbenchmarks ship with the repository, so XiaPL's own
numbers are something you can re-measure on your machine rather than take on
faith:

```bash
cmake -S . -B build -DXIAPL_BUILD_BENCHMARKS=ON
cmake --build build -j
./build/benchmarks/bench_eval      # evaluation and winner determination
./build/benchmarks/bench_equity    # equity simulation
```

Both are compiled `-O3 -DNDEBUG` regardless of `CMAKE_BUILD_TYPE`, so a Debug
configure does not silently produce meaningless numbers.

`benchmarks/bench_eval.cpp` times 7-card evaluation plus heads-up winner
determination (`judge`) for Hold'em and PLO; `benchmarks/bench_equity.cpp`
times exact and Monte Carlo equity, both for fixed hands and range-vs-range.
Each prints wall-clock, ops/sec and ns/op for the machine it runs on.

The head-to-head harness behind the table above is deliberately not in this
repository: it has to fetch and build two third-party libraries, so shipping
it would put their sources (and licences) inside this tree. The Method
section above is the specification needed to rebuild it.
