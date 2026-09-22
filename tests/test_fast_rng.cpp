#include "doctest.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <xiapl/detail/fast_rng.h>

// Contract tests for xiapl::FastRng. The MC board sampler in
// src/core/board_sample.h draws every board card through bounded(), so a bias
// or an out-of-range value here silently corrupts every Monte Carlo equity.
//
// ===========================================================================
// SEQUENCE INVARIANCE GUARD
// ===========================================================================
// xiapl::FastRng is the single generator behind every seeded code path in
// the library, so callers that record a seed and expect to replay the exact
// same result later depend on the generated sequence NEVER changing. A
// "harmless" edit to a SplitMix64 constant, a rotl amount or the state
// update order would silently invalidate every stored baseline while all
// self-consistency tests (a.next() == b.next()) stayed green.
//
// The reference-vector tests below pin the actual sequence with literal
// constants so any such edit fails loudly. The constants were measured from
// this implementation and cross-checked against the published xoshiro256++
// reference output.

namespace {

// Independent transcription of the published reference implementations
// splitmix64.c and xoshiro256plusplus.c (Blackman & Vigna, public domain,
// https://prng.di.unimi.it/). Written from the published algorithm rather
// than copied from include/xiapl/detail/fast_rng.h, so agreement below is a genuine
// cross-check that our class really is canonical xoshiro256++ seeded by
// canonical SplitMix64 -- not merely self-consistent.
struct ReferenceXoshiro256pp {
  std::uint64_t s[4];

  static std::uint64_t rotl(std::uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
  }

  explicit ReferenceXoshiro256pp(std::uint64_t seed) {
    std::uint64_t x = seed;
    for (int i = 0; i < 4; ++i) {
      std::uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
      s[i] = z ^ (z >> 31);
    }
  }

  std::uint64_t next() {
    const std::uint64_t result = rotl(s[0] + s[3], 23) + s[0];
    const std::uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
  }
};

} // namespace

TEST_CASE("FastRng matches the published xoshiro256++ reference") {
  // Sequence invariance guard, part 1: algorithm identity.
  for (std::uint64_t sd : {std::uint64_t{0}, std::uint64_t{7}, std::uint64_t{42},
                           std::uint64_t{20260723},
                           std::uint64_t{0xFFFFFFFFFFFFFFFFULL}}) {
    xiapl::FastRng ours;
    ours.seed(sd);
    ReferenceXoshiro256pp ref(sd);
    for (int i = 0; i < 20000; ++i) {
      REQUIRE(ours.next() == ref.next());
    }
  }
}

TEST_CASE("FastRng reference vectors (sequence invariance guard)") {
  // Sequence invariance guard, part 2: literal pins.
  //
  // These constants were measured from this implementation AND verified to
  // equal the published xoshiro256++ reference (see the cross-check above).
  // If one of these CHECKs fails, the RNG stream changed and every seeded
  // result recorded before the change is invalidated. Do NOT "fix" the
  // constants without a deliberate, reviewed decision to re-baseline the
  // affected artefacts.

  SUBCASE("seed 7") {
    xiapl::FastRng r;
    r.seed(7);
    // state_hash() before any draw pins the SplitMix64 seeding path itself.
    CHECK(r.state_hash() == 0xd8d7c903ce53e0bcULL);
    CHECK(r.next() == 0x0e2c1a002aae913dULL);
    CHECK(r.next() == 0x2c0fc8ddfa4e9e14ULL);
    CHECK(r.next() == 0xb7b311b3b0d45872ULL);
    CHECK(r.next() == 0x6d5d9f6a6318013cULL);
  }

  SUBCASE("seed 42") {
    xiapl::FastRng r;
    r.seed(42);
    CHECK(r.state_hash() == 0xd3cd0f5e6ef2700aULL);
    CHECK(r.next() == 0xd0764d4f4476689fULL);
    CHECK(r.next() == 0x519e4174576f3791ULL);
  }

  SUBCASE("seed 0") {
    // seed 0 is the SimulationOptions default and a legal deterministic
    // seed (resolve_master_seed / calculate_range_equity's caller-owned
    // FastRng both seed via FastRng::seed(), deliberately bypassing the
    // FastRng(0) ctor zero-guard, to keep it reproducible). Pin its stream
    // too.
    xiapl::FastRng r;
    r.seed(0);
    CHECK(r.state_hash() == 0x2d7687863ff823e5ULL);
    CHECK(r.next() == 0x53175d61490b23dfULL);
    CHECK(r.next() == 0x61da6f3dc380d507ULL);
  }

  SUBCASE("long run trajectory") {
    // Guards the state update (a wrong rotl/shift can still reproduce the
    // first few words while diverging later).
    xiapl::FastRng r;
    r.seed(7);
    for (int i = 0; i < 1000; ++i) (void)r.next();
    CHECK(r.state_hash() == 0xaa7df2c0024dd283ULL);
    CHECK(r.next() == 0x724f73cb5d988155ULL);
  }

  SUBCASE("default constructor") {
    // FastRng() seeds with 0xDEADBEEFCAFEBABE; callers may draw from the
    // default-constructed stream before an explicit reseed.
    xiapl::FastRng r;
    CHECK(r.next() == 0xbc4c9fe9190b4de0ULL);
  }

  SUBCASE("bounded(52) draw sequence") {
    // Pins what the MC board sampler actually consumes: a change in the
    // bounded() derivation moves every Monte Carlo equity even when next()
    // is untouched.
    xiapl::FastRng r;
    r.seed(20260723);
    const std::uint32_t expected[8] = {39, 7, 45, 35, 18, 41, 3, 44};
    for (int i = 0; i < 8; ++i) {
      CHECK(r.bounded(52) == expected[i]);
    }
  }
}

TEST_CASE("FastRng::seed is deterministic (including seed 0)") {
  // mc_chunking.h's chunk seeding (and equity_range.cpp's direct
  // FastRng::seed(resolve_master_seed(...)) call) uses seed(), not the ctor,
  // precisely so that SimulationOptions::seed == 0 stays reproducible. Pin
  // that.
  for (std::uint64_t s : {std::uint64_t{0}, std::uint64_t{1},
                          std::uint64_t{7}, std::uint64_t{0xFFFFFFFFFFFFFFFFULL}}) {
    xiapl::FastRng a;
    xiapl::FastRng b;
    a.seed(s);
    b.seed(s);
    for (int i = 0; i < 64; ++i) {
      CHECK(a.next() == b.next());
    }
  }
}

TEST_CASE("FastRng distinct seeds diverge") {
  xiapl::FastRng a;
  xiapl::FastRng b;
  a.seed(0);
  b.seed(1);
  bool differs = false;
  for (int i = 0; i < 16 && !differs; ++i) {
    if (a.next() != b.next()) differs = true;
  }
  CHECK(differs);
}

TEST_CASE("FastRng::bounded stays in range") {
  xiapl::FastRng rng;
  rng.seed(20260723);
  // 1..52 covers every deck size the partial Fisher-Yates sampler asks for.
  for (std::uint32_t n = 1; n <= 52; ++n) {
    for (int i = 0; i < 2000; ++i) {
      const std::uint32_t v = rng.bounded(n);
      REQUIRE(v < n);
    }
  }
  // n == 0 must return 0 rather than dividing by zero / looping forever.
  CHECK(rng.bounded(0) == 0);
  // n == 1 is the degenerate single-outcome case.
  for (int i = 0; i < 100; ++i) {
    CHECK(rng.bounded(1) == 0);
  }
}

TEST_CASE("FastRng::bounded is uniform (chi-square)") {
  xiapl::FastRng rng;
  rng.seed(0xC0FFEE);

  // Deck sizes actually used by the board sampler: 52 down to 43 (5 board
  // cards drawn from a 47-card deck at most). 47 and 52 are the hot ones.
  for (std::uint32_t n : {2u, 3u, 47u, 48u, 52u}) {
    const int kPerBucket = 4000;
    const std::uint64_t draws = static_cast<std::uint64_t>(n) * kPerBucket;
    std::vector<std::uint64_t> counts(n, 0);
    for (std::uint64_t i = 0; i < draws; ++i) {
      counts[rng.bounded(n)]++;
    }
    const double expected = static_cast<double>(kPerBucket);
    double chi2 = 0.0;
    for (std::uint32_t k = 0; k < n; ++k) {
      const double d = static_cast<double>(counts[k]) - expected;
      chi2 += d * d / expected;
    }
    // Under uniformity chi2 ~ chi-square with (n-1) df, mean n-1 and
    // variance 2(n-1). Allow ~5 sigma: a fixed-seed test must not flake.
    const double df = static_cast<double>(n) - 1.0;
    const double limit = df + 5.0 * std::sqrt(2.0 * df);
    INFO("n = ", n, " chi2 = ", chi2, " limit = ", limit);
    CHECK(chi2 < limit);
  }
}

TEST_CASE("FastRng::bounded modulo-bias sentinel") {
  // A naive `next32() % n` is biased when n does not divide 2^32: the
  // outcomes below (2^32 mod n) get one extra preimage each. Pick n so that
  // the excess mass is large enough to measure -- n = 3 * 2^30 gives
  // split = 2^32 - n = 2^30 outcomes with double weight, i.e. `%` would put
  // ~1/2 of the draws below `split` where uniform puts exactly 1/3.
  //
  // (Counter-example for the record: n = 2^31 + 1 looks like a worse case
  // because the max per-outcome ratio is also 2x, but only 2 outcomes are
  // affected, so both implementations score 1.00000 here and the test would
  // be vacuous.)
  const std::uint32_t n = 3u * (1u << 30);
  const std::uint32_t split = static_cast<std::uint32_t>((1ULL << 32) - n);
  xiapl::FastRng rng;
  rng.seed(4242);

  const int draws = 200000;
  int below = 0;
  for (int i = 0; i < draws; ++i) {
    const std::uint32_t v = rng.bounded(n);
    REQUIRE(v < n);
    if (v < split) below++;
  }
  // Uniform expectation is split / n = 1/3 exactly; binomial sd of the
  // fraction is sqrt((1/3)(2/3)/draws) ~= 0.00105, so these bounds are ~6
  // sigma (fixed seed, must not flake). Naive `%` measures ~0.4996 here.
  const double frac = static_cast<double>(below) / draws;
  INFO("below-split fraction = ", frac, " (uniform expects 0.33333)");
  CHECK(frac > 0.327);
  CHECK(frac < 0.340);
}

TEST_CASE("FastRng::next_double lies in [0, 1)") {
  xiapl::FastRng rng;
  rng.seed(99);
  double sum = 0.0;
  const int draws = 100000;
  for (int i = 0; i < draws; ++i) {
    const double d = rng.next_double();
    REQUIRE(d >= 0.0);
    REQUIRE(d < 1.0);
    sum += d;
  }
  // Mean 0.5, sd of the mean = sqrt(1/12 / draws) ~= 0.00091; 5 sigma ~= 0.005.
  CHECK(std::abs(sum / draws - 0.5) < 0.005);
}

TEST_CASE("FastRng state_hash tracks state and does not consume a draw") {
  xiapl::FastRng a;
  xiapl::FastRng b;
  a.seed(31337);
  b.seed(31337);

  const std::uint64_t h0 = a.state_hash();
  CHECK(a.state_hash() == h0);   // pure observer
  CHECK(b.state_hash() == h0);   // same seed -> same state

  (void)a.next();
  CHECK(a.state_hash() != h0);
  (void)b.next();
  CHECK(a.state_hash() == b.state_hash());
}
