#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <random>
#include <thread>
#include <vector>

#include <xiapl/detail/fast_rng.h>
#include <xiapl/simulation.h>

#include "env_compat.h"

namespace xiapl {
namespace internal {

// ---- Chunked Monte Carlo substreams ----
//
// MC trials are partitioned into fixed-size chunks; each chunk runs its own
// RNG substream derived from the master seed, and the per-chunk accumulators
// are reduced in chunk-index order. Both the chunk size and the reduction
// order are constants rather than functions of the worker count, which is what
// makes the result bit-identical for every `threads` value including serial.
// Workers only decide *which* chunks they run, never what a chunk computes.

// Trials per chunk. Big enough that per-chunk setup (RNG seeding + deck copy,
// ~100 ns) is noise against ~65536 trials, small enough that a run at the
// auto-threading threshold still splits across several workers.
inline constexpr std::uint64_t kMcChunkTrials = 1ULL << 16;  // 65536

// Auto mode (threads == 0) only fans out at or above this trial count; below
// it, thread spin-up costs more than the work saved.
inline constexpr std::uint64_t kMcAutoThreadMinTrials = 200000;

// SplitMix64 finalizer — exactly the mixing step of FastRng::seed(), with the
// gamma increment left to the caller. Reused verbatim rather than approximated
// so the substream seeds have the same avalanche properties as the seeds
// FastRng already consumes.
inline std::uint64_t splitmix64_mix(std::uint64_t z) {
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// Seed for chunk k. Chunk 0 takes the master seed verbatim, so any run that
// fits in a single chunk reproduces the pre-chunking serial stream
// bit-for-bit (pinned by regression tests). Chunks k >= 1 walk the SplitMix64
// gamma sequence and mix, which is the standard splittable-generator
// construction: consecutive k values decorrelate fully.
inline std::uint64_t derive_chunk_seed(std::uint64_t s, std::uint64_t k) {
  return k == 0 ? s
                : splitmix64_mix(s + (k + 1) * 0x9E3779B97F4A7C15ULL);
}

// Worker count for auto mode. XIAPL_NUM_THREADS (a positive decimal integer,
// or its pre-rename XPL_NUM_THREADS spelling) overrides hardware
// concurrency; malformed or non-positive values are ignored
// silently because this is a deployment tuning hint, not user input to
// validate — and it can never change results, only speed.
inline int auto_mc_thread_count() {
  if (const char *env = getenv_compat("XIAPL_NUM_THREADS")) {
    errno = 0;
    char *end = nullptr;
    const long v = std::strtol(env, &end, 10);
    if (end != env && *end == '\0' && errno == 0 && v > 0 &&
        v <= static_cast<long>(std::numeric_limits<int>::max())) {
      return static_cast<int>(v);
    }
  }
  const unsigned hw = std::thread::hardware_concurrency();
  return hw > 0 ? static_cast<int>(hw) : 1;
}

// How many workers to run a chunked MC job with. Capped at num_chunks: extra
// workers would only spin up to find the queue empty.
inline int resolve_mc_threads(int threads_option, std::uint64_t total_trials,
                              std::uint64_t num_chunks) {
  if (num_chunks <= 1) return 1;
  long want;
  if (threads_option > 0) {
    want = threads_option;
  } else if (threads_option < 0) {
    want = 1;  // documented contract: negative means serial
  } else if (total_trials >= kMcAutoThreadMinTrials) {
    want = auto_mc_thread_count();
  } else {
    want = 1;
  }
  if (static_cast<std::uint64_t>(want) > num_chunks) {
    want = static_cast<long>(num_chunks);
  }
  return want < 1 ? 1 : static_cast<int>(want);
}

// Master seed for a chunked MC run — the single seed-resolution policy in
// the codebase (calculate_range_equity in equity_range.cpp reuses this
// directly for its caller-owned FastRng rather than duplicating it). An
// explicit deterministic seed is honoured verbatim (seed 0 included — the
// chunk RNGs are seeded through FastRng::seed(), which has no zero-guard,
// never through the FastRng(seed) ctor, which does). Non-deterministic runs
// compose all 64 bits from two random_device draws, once per call, so every
// chunk of one run still belongs to one coherent stream family.
inline std::uint64_t resolve_master_seed(const SimulationOptions &options) {
  if (options.deterministic) return options.seed;
  std::random_device rd;
  return (static_cast<std::uint64_t>(rd()) << 32) ^
         static_cast<std::uint64_t>(rd());
}

// Runs body(k) for every chunk index k in [0, num_chunks). With nthreads == 1
// the chunks run in index order on the calling thread; otherwise nthreads
// workers pull indices off one atomic counter, so which worker runs which
// chunk depends on timing. Work stealing is safe for reproducibility because
// `body` writes chunk k's result to slot k and the caller reduces in index
// order — completion order never enters the result.
//
// A worker whose chunk throws stops pulling work, but the run still
// terminates: the remaining chunks are picked up by the other workers, or (if
// every chunk throws) all workers exit. Exceptions are recorded by CHUNK
// INDEX, not by worker slot, so when several chunks throw different
// exceptions the one rethrown here is always the LOWEST chunk index —
// deterministic regardless of which worker happened to reach which chunk
// first. No exception may escape a worker — that would call std::terminate.
template <typename Body>
void run_chunks_parallel(std::uint64_t num_chunks, int nthreads,
                         Body &&body) {
  if (nthreads <= 1) {
    for (std::uint64_t k = 0; k < num_chunks; ++k) body(k);
    return;
  }

  std::atomic<std::uint64_t> next_chunk{0};
  std::vector<std::exception_ptr> chunk_errors(
      static_cast<std::size_t>(num_chunks));
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(nthreads));
  // If spawning worker t fails (resource exhaustion), the t-1 threads already
  // running still reference the caller's frame; they must be joined before the
  // exception leaves, or their destructors would call std::terminate on a
  // joinable thread and they would outlive `body`'s captures. Already-spawned
  // workers drain the remaining chunks, so the partial fan-out stays correct —
  // just narrower.
  try {
    for (int t = 0; t < nthreads; ++t) {
      workers.emplace_back([&]() {
        for (;;) {
          const std::uint64_t k =
              next_chunk.fetch_add(1, std::memory_order_relaxed);
          if (k >= num_chunks) break;
          try {
            body(k);
          } catch (...) {
            chunk_errors[static_cast<std::size_t>(k)] = std::current_exception();
            break;
          }
        }
      });
    }
  } catch (...) {
    for (std::thread &w : workers) {
      if (w.joinable()) w.join();
    }
    throw;
  }
  for (std::thread &w : workers) w.join();
  for (const std::exception_ptr &e : chunk_errors) {
    if (e) std::rethrow_exception(e);
  }
}

// ---- Chunked MC driver ----

// Runs one chunked Monte Carlo job end to end. This is the ONLY place the
// chunking arithmetic, the substream seeding and the reduction order live, so
// every MC sampler in the library gets the same bit-identical-at-any-thread-
// count guarantee for free.
//
// The driver owns:
//   * the chunk count, ceil(total_trials / kMcChunkTrials);
//   * each chunk's trial count, min(kMcChunkTrials, total_trials - k *
//     kMcChunkTrials) — full chunks except a possibly short last one;
//   * a FastRng per chunk, default-constructed then seeded (never via the
//     FastRng(seed) ctor, which has a zero-guard) with
//     derive_chunk_seed(master_seed, k);
//   * the worker fan-out via resolve_mc_threads + run_chunks_parallel;
//   * the per-chunk result slots and the ascending-index reduction.
//
// `run_chunk(chunk_trials, rng) -> Counters` computes one chunk and returns
// its accumulator. It is deliberately handed neither the chunk index nor the
// global trial offset, only how many trials to run: a chunk's output is then a
// function of (master_seed, k) alone by construction, which is what makes the
// work-stealing dispatch below safe. It may be called concurrently, so it must
// only touch its own locals and read-only shared state.
//
// `reduce(const Counters &)` is called once per chunk on the calling thread,
// in ASCENDING CHUNK INDEX ORDER, never in completion order. Floating-point
// addition is not associative, so the reduction order is part of the result
// contract, not an implementation detail.
//
// Result slots start default-constructed and are move-assigned the chunk's
// return value; pre-sizing them would only build accumulators for the
// move-assign to throw away. A chunk that throws leaves its slot default, but
// `reduce` never sees it — run_chunks_parallel rethrows before the reduction
// starts. A caller that wants an unfilled slot to be loud rather than
// contribute zeros can test for the default state at the top of `reduce`
// (simulate_multiway does).
template <typename Counters, typename ChunkFn, typename ReduceFn>
void run_mc_chunks(std::uint64_t total_trials, int threads,
                   std::uint64_t master_seed, ChunkFn &&run_chunk,
                   ReduceFn &&reduce) {
  const std::uint64_t num_chunks =
      (total_trials + kMcChunkTrials - 1) / kMcChunkTrials;
  std::vector<Counters> chunk_results(static_cast<std::size_t>(num_chunks));
  run_chunks_parallel(
      num_chunks, resolve_mc_threads(threads, total_trials, num_chunks),
      [&](std::uint64_t k) {
        FastRng chunk_rng;
        chunk_rng.seed(derive_chunk_seed(master_seed, k));
        const std::uint64_t begin = k * kMcChunkTrials;
        chunk_results[static_cast<std::size_t>(k)] =
            run_chunk(std::min(kMcChunkTrials, total_trials - begin),
                      chunk_rng);
      });
  for (const Counters &c : chunk_results) reduce(c);
}

} // namespace internal
} // namespace xiapl
