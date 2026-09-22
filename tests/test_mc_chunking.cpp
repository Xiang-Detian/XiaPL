#include "doctest.h"

#include "../src/core/mc_chunking.h"
#include "test_env_util.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <xiapl/detail/fast_rng.h>

using xiapl::FastRng;
using xiapl::internal::derive_chunk_seed;
using xiapl::internal::kMcAutoThreadMinTrials;
using xiapl::internal::kMcChunkTrials;
using xiapl::internal::resolve_mc_threads;
using xiapl::internal::run_mc_chunks;

static_assert(kMcChunkTrials == 65536, "chunk size is a pinned contract");

namespace {

// Saves whatever XIAPL_NUM_THREADS the process already had on construction and
// restores it in the destructor, so this test can pin the env var for its
// decision-table rows without leaking an override into tests that run after
// it in the same process (tests/test_range_equity.cpp:27-47 precedent).
class XiaplNumThreadsEnvGuard {
 public:
    XiaplNumThreadsEnvGuard() {
        const char* raw = std::getenv("XIAPL_NUM_THREADS");
        had_env_ = (raw != nullptr);
        if (had_env_) saved_ = raw;
    }
    ~XiaplNumThreadsEnvGuard() {
        if (had_env_) {
            test_set_env("XIAPL_NUM_THREADS", saved_.c_str());
        } else {
            test_unset_env("XIAPL_NUM_THREADS");
        }
    }
    XiaplNumThreadsEnvGuard(const XiaplNumThreadsEnvGuard&) = delete;
    XiaplNumThreadsEnvGuard& operator=(const XiaplNumThreadsEnvGuard&) = delete;

 private:
    bool had_env_ = false;
    std::string saved_;
};

} // namespace

TEST_CASE("resolve_mc_threads decision table") {
    struct Row {
        int threads_option;
        std::uint64_t total_trials;
        std::uint64_t num_chunks;
        int expected;
        const char* why;
    };
    // XIAPL_NUM_THREADS is pinned so the auto rows are machine-independent.
    const XiaplNumThreadsEnvGuard env_guard;
    test_set_env("XIAPL_NUM_THREADS", "6");
    const Row rows[] = {
        {0, 1000, 1, 1, "single chunk is always serial"},
        {8, 1000, 1, 1, "explicit threads don't bypass single-chunk-is-serial"},
        {8, 1u << 20, 4, 4, "explicit threads capped at num_chunks"},
        {3, 1u << 20, 16, 3, "explicit threads honored below cap"},
        {-4, 1u << 20, 16, 1, "negative means serial by contract"},
        {0, kMcAutoThreadMinTrials - 1, 16, 1, "below auto threshold stays serial"},
        {0, kMcAutoThreadMinTrials, 16, 6, "at threshold auto engages (env=6)"},
        {0, kMcAutoThreadMinTrials, 2, 2, "auto also capped at num_chunks"},
    };
    for (const Row& r : rows) {
        CAPTURE(r.why);
        CHECK(resolve_mc_threads(r.threads_option, r.total_trials,
                                 r.num_chunks) == r.expected);
    }
}

TEST_CASE("derive_chunk_seed contract") {
    // Chunk 0 takes the master seed verbatim (single-chunk runs reproduce
    // the pre-chunking serial stream).
    CHECK(derive_chunk_seed(0x123456789abcdef0ULL, 0) == 0x123456789abcdef0ULL);
    // Chunks k >= 1: independent re-derivation of the SplitMix64 walk.
    const std::uint64_t s = 0x123456789abcdef0ULL;
    for (std::uint64_t k = 1; k <= 4; ++k) {
        std::uint64_t z = s + (k + 1) * 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        CHECK(derive_chunk_seed(s, k) == z);
        CHECK(derive_chunk_seed(s, k) != derive_chunk_seed(s, k - 1));
    }
}

// The three tests below cover run_mc_chunks itself — the single driver behind
// every Monte Carlo sampler in src/core/equity_hands.cpp and
// src/core/equity_range.cpp. They pin the three
// properties the samplers' bit-exactness rests on: how the trials are cut into
// chunks, which seed each chunk runs on, and the order the chunk results are
// folded together in.

TEST_CASE("run_mc_chunks chunk sizing") {
    const std::uint64_t c = kMcChunkTrials;
    struct Row {
        std::uint64_t total;
        std::vector<std::uint64_t> expected;  // per-chunk trial counts
        const char* why;
    };
    const std::vector<Row> rows = {
        {0, {}, "zero trials runs no chunk at all"},
        {1, {1}, "one trial is one short chunk"},
        {c - 1, {c - 1}, "just under a full chunk is still one chunk"},
        {c, {c}, "exact single chunk, no empty tail chunk"},
        {c + 1, {c, 1}, "one trial over spills into a 1-trial second chunk"},
        {3 * c, {c, c, c}, "exact multiple: every chunk is full"},
        {3 * c + 7, {c, c, c, 7}, "remainder lands in the last chunk"},
    };
    // Run each row serially and fanned out: the partition is a function of
    // total_trials alone, never of the worker count.
    for (const int threads : {1, 8}) {
        for (const Row& r : rows) {
            CAPTURE(threads);
            CAPTURE(r.why);
            std::vector<std::uint64_t> seen;
            run_mc_chunks<std::uint64_t>(
                r.total, threads, 0x0123456789abcdefULL,
                [](std::uint64_t chunk_trials, FastRng&) {
                    return chunk_trials;
                },
                [&](const std::uint64_t& t) { seen.push_back(t); });
            REQUIRE(seen.size() == r.expected.size());
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < seen.size(); ++i) {
                CAPTURE(i);
                CHECK(seen[i] == r.expected[i]);
                sum += seen[i];
            }
            // The chunks partition the trials: no trial run twice, none lost.
            CHECK(sum == r.total);
        }
    }
}

TEST_CASE("run_mc_chunks seeds chunk k and keeps its result at index k") {
    const std::uint64_t master_seed = 0xa5a5f00ddeadbeefULL;
    const std::uint64_t num_chunks = 16;
    // Short last chunk, so the tail case is exercised here too.
    const std::uint64_t total = (num_chunks - 1) * kMcChunkTrials + 3;

    // Reference stream, derived independently of the driver: chunk k must run
    // on a FastRng seeded (not constructed — FastRng(seed) has a zero-guard)
    // with derive_chunk_seed(master_seed, k), consumed from its first draw.
    std::vector<std::uint64_t> expected(static_cast<std::size_t>(num_chunks));
    for (std::uint64_t k = 0; k < num_chunks; ++k) {
        FastRng rng;
        rng.seed(derive_chunk_seed(master_seed, k));
        expected[static_cast<std::size_t>(k)] = rng.next();
    }

    // At every worker count the i-th value the reduction sees must be chunk
    // i's — which pins both the substream seeding and the ascending-index
    // order of the reduction (the draws are distinct, so a permuted reduction
    // could not pass).
    for (const int threads : {1, 4, 8}) {
        CAPTURE(threads);
        std::vector<std::uint64_t> seen;
        run_mc_chunks<std::uint64_t>(
            total, threads, master_seed,
            [](std::uint64_t, FastRng& rng) { return rng.next(); },
            [&](const std::uint64_t& v) { seen.push_back(v); });
        REQUIRE(seen.size() == expected.size());
        for (std::size_t i = 0; i < seen.size(); ++i) {
            CAPTURE(i);
            CHECK(seen[i] == expected[i]);
        }
    }
}

TEST_CASE("run_mc_chunks reduction is order-sensitive and index-ordered") {
    const std::uint64_t master_seed = 0x51ed5eedULL;
    const std::uint64_t num_chunks = 16;
    const std::uint64_t total = num_chunks * kMcChunkTrials;

    // Chunk 0 identifies itself by its first draw: derive_chunk_seed hands
    // chunk 0 the master seed verbatim.
    FastRng seed_probe;
    seed_probe.seed(derive_chunk_seed(master_seed, 0));
    const std::uint64_t chunk0_first_draw = seed_probe.next();

    // Deliberately order-sensitive contributions: chunk 0 gives 1.0 and the
    // other 15 give 2^-53 each. Added in ascending chunk order, every small
    // term lands exactly halfway to the next double and ties to even, i.e.
    // back to 1.0 — so the total is exactly 1.0. Any order that sums the small
    // terms with each other first carries 15 * 2^-53 into the 1.0 and lands on
    // a different double, so this pins the reduction order rather than merely
    // its self-consistency.
    const double small = 0x1p-53;
    CHECK(1.0 + small == 1.0);              // absorbed one at a time
    CHECK(1.0 + 15.0 * small != 1.0);       // not absorbed in bulk

    for (const int threads : {1, 4, 8}) {
        CAPTURE(threads);
        double acc = 0.0;
        run_mc_chunks<double>(
            total, threads, master_seed,
            [&](std::uint64_t, FastRng& rng) {
                return rng.next() == chunk0_first_draw ? 1.0 : small;
            },
            [&](const double& v) { acc += v; });
        CHECK(acc == 1.0);
    }
}
