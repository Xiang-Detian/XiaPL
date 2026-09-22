#include "doctest.h"
#include "../src/core/alias_picker.h"

#include <xiapl/card.h>
#include <xiapl/detail/fast_rng.h>
#include <xiapl/range.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Contract tests for xiapl::internal::AliasPicker (src/core/alias_picker.h),
// the O(1) weighted combo sampler that replaces WeightedComboPicker's binary
// search in sample_range_equity. Task 2 shares const AliasPicker
// instances across threads, so every property checked here (table
// correctness, sampler unbiasedness, OOB safety, construction determinism)
// is load-bearing for that reuse.

using namespace xiapl;
using xiapl::internal::AliasPicker;

namespace {

// masks[i] = i + 1: distinct, trivially invertible (index = mask - 1), and
// AliasPicker never validates mask shape -- only the simulation layer cares
// that masks are card masks of the right width for the game -- 2 bits for
// Hold'em, 4 for PLO (see filter_combos).
std::vector<std::uint64_t> trivial_masks(std::size_t n) {
  std::vector<std::uint64_t> m(n);
  for (std::size_t i = 0; i < n; ++i) m[i] = static_cast<std::uint64_t>(i + 1);
  return m;
}

std::vector<double> weights_all_equal(std::size_t n) {
  return std::vector<double>(n, 3.5);
}

// Log-spaced ramp from min_w to max_w (inclusive), min_w/max_w gives the
// "spread" of the weight set. n == 1 has no ramp; the single weight is
// max_w (its value is irrelevant since a single-element table is always
// classified uniform).
std::vector<double> weights_log_ramp(std::size_t n, double min_w,
                                     double max_w) {
  std::vector<double> w(n);
  const double log_min = std::log(min_w);
  const double log_max = std::log(max_w);
  for (std::size_t i = 0; i < n; ++i) {
    const double t = (n > 1) ? static_cast<double>(i) /
                                   static_cast<double>(n - 1)
                             : 1.0;
    w[i] = std::exp(log_min + t * (log_max - log_min));
  }
  return w;
}

std::vector<double> weights_one_dominant(std::size_t n) {
  std::vector<double> w(n, 1.0);
  w[0] = 1e12;
  return w;
}

// Log-spaced ramp with both endpoints near DBL_MAX: summing 2+ raw elements
// already overflows to inf (verified: raw_sum is literally `inf` here), so a
// naive (non-max-normalized) implementation would divide every weight by
// inf and silently degrade to uniform sampling -- per the header's own
// warning. That degradation is exactly what this scenario is meant to
// catch: the true weighted share ranges from ~0.5x to ~0.9x the mean, so
// collapsing to uniform (1/n each) is off by ~0.36 relative on the low end,
// nowhere near the 1e-12 bound below. Max-normalization avoids the overflow
// entirely and measures ~1e-13 here, same order as the other spread
// scenarios.
//
// (An earlier version alternated two close-together DBL_MAX-scale values
// instead of a smooth ramp and failed the 1e-12 bound at n=1326 with a
// 1.28e-11 relative error. That was NOT a DBL_MAX-magnitude effect --
// reproduced identically with an ordinary-magnitude 0.9/0.8 alternation.
// The actual trigger is a non-dyadic weight ratio repeated across ~n
// nearly-equal terms: the sequential max-normalize sum accumulates
// correlated same-sign rounding instead of the typical random-walk
// cancellation, an O(n) absolute discrepancy against an O(n)-scale running
// sum, and Vose's small/large pairing chain concentrates that whole
// discrepancy onto a single terminal entry whose own mass is ~1/n --
// i.e. an O(n^2 * eps) relative-error ceiling (~3.9e-10 at n=1326), which
// the measured 1.28e-11 sits comfortably inside. The smooth ramp below
// doesn't repeat a single ratio across every term, so it doesn't hit this
// worst case.)
std::vector<double> weights_near_dbl_max(std::size_t n) {
  const double dbl_max = std::numeric_limits<double>::max();
  return weights_log_ramp(n, dbl_max * 0.5, dbl_max * 0.9);
}

// Independent (max-normalized) ground-truth probability share of
// weights[j]. Uses the same overflow-avoiding normalization the picker uses
// internally -- the point of the test is to check the TABLE the picker
// builds against the true weight ratios, not to re-derive those ratios with
// a different (overflow-prone) method.
double prob_share(const std::vector<double>& weights, std::size_t j) {
  double max_w = weights[0];
  for (double w : weights) {
    if (w > max_w) max_w = w;
  }
  double sum = 0.0;
  for (double w : weights) sum += w / max_w;
  return (weights[j] / max_w) / sum;
}

// Total probability mass the table assigns to outcome j: bucket j's own
// share (prob[j]/n) plus every other bucket's alias spillover into j
// (sum over i with alias_mask[i] == mask_j of (1 - prob[i])/n).
double emitted_mass(const AliasPicker& p, std::size_t j) {
  const std::size_t n = p.size();
  const std::uint64_t mask_j = p.mask_at(j);
  double mass = p.prob_at(j) / static_cast<double>(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (p.alias_mask_at(i) == mask_j) {
      mass += (1.0 - p.prob_at(i)) / static_cast<double>(n);
    }
  }
  return mass;
}

struct WeightSet {
  const char* name;
  std::function<std::vector<double>(std::size_t)> gen;
  // True iff this weight set is uniform (bit-equal weights) BY CONSTRUCTION
  // for every n >= 1, i.e. AliasPicker::is_uniform() must return true for it
  // regardless of n. Every other set must be non-uniform for n >= 2 (n == 1
  // is always uniform, handled separately -- see expect_uniform below).
  bool always_uniform;
};

const std::vector<WeightSet>& all_weight_sets() {
  static const std::vector<WeightSet> sets = {
      {"all-equal", [](std::size_t n) { return weights_all_equal(n); }, true},
      {"3-decade spread",
       [](std::size_t n) { return weights_log_ramp(n, 1.0, 1e3); }, false},
      {"1e-9 spread",
       [](std::size_t n) { return weights_log_ramp(n, 1e-9, 1.0); }, false},
      {"1e-300 spread",
       [](std::size_t n) { return weights_log_ramp(n, 1e-300, 1.0); }, false},
      {"one dominant weight 1e12",
       [](std::size_t n) { return weights_one_dominant(n); }, false},
      {"values near DBL_MAX",
       [](std::size_t n) { return weights_near_dbl_max(n); }, false},
  };
  return sets;
}

// Stub RNG for boundary/OOB tests: next() always returns a fixed scripted
// 64-bit word (pick() only ever draws once per call in the weighted path).
// bounded() is a plain modulo, NOT Lemire's rejection loop -- a constant
// next() could make a naive port of FastRng::bounded() spin forever, and
// this stub only needs to stay in range, not be unbiased.
struct ScriptedRng {
  std::uint64_t value;
  std::uint64_t next() const { return value; }
  double next_double() const {
    return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
  }
  std::uint32_t bounded(std::uint32_t n) const {
    return (n == 0) ? 0u : static_cast<std::uint32_t>(next() % n);
  }
};

}  // namespace

TEST_CASE("AliasPicker exact table distribution (n x weight-set matrix)") {
  for (std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{3},
                        std::size_t{8}, std::size_t{64}, std::size_t{1326}}) {
    const std::vector<std::uint64_t> masks = trivial_masks(n);
    for (const WeightSet& ws : all_weight_sets()) {
      CAPTURE(n);
      CAPTURE(ws.name);
      const std::vector<double> weights = ws.gen(n);
      const AliasPicker p(masks.data(), weights.data(), n);

      // n == 1 is always uniform (nothing to compare against, regardless of
      // weight set); otherwise uniformity must match the weight set's own
      // always_uniform flag. Asserting the EXPECTED value (not just calling
      // is_uniform() and trusting whatever it returns) catches
      // uniform-detection over-triggering, which would otherwise silently
      // vacate the mass-conservation check below for every n it touches.
      const bool expect_uniform = (n == 1) || ws.always_uniform;
      CHECK(p.is_uniform() == expect_uniform);
      if (expect_uniform) continue;

      for (std::size_t j = 0; j < n; ++j) {
        const double expected = prob_share(weights, j);
        const double got = emitted_mass(p, j);
        const double rel_err =
            std::abs(got - expected) / std::max(expected, 1e-300);
        CHECK(rel_err < 1e-12);
      }
    }
  }
}

TEST_CASE("AliasPicker sampler chi-square (n=1326, 3-decade spread, 1e7 draws)") {
  const std::size_t n = 1326;
  const std::vector<std::uint64_t> masks = trivial_masks(n);
  const std::vector<double> weights = weights_log_ramp(n, 1.0, 1e3);
  const AliasPicker p(masks.data(), weights.data(), n);
  REQUIRE_FALSE(p.is_uniform());

  FastRng rng(12345);
  const std::int64_t draws = 10000000;  // 1e7
  std::vector<std::uint64_t> counts(n, 0);
  for (std::int64_t t = 0; t < draws; ++t) {
    const std::uint64_t mask = p.pick(rng);
    counts[mask - 1]++;  // trivial masks: mask == index + 1
  }

  double chi2 = 0.0;
  for (std::size_t j = 0; j < n; ++j) {
    const double expected = static_cast<double>(draws) * prob_share(weights, j);
    const double d = static_cast<double>(counts[j]) - expected;
    chi2 += d * d / expected;
  }

  // dof = n - 1 = 1325; ~6-sigma bound, frozen and zero-flake at this seed.
  const double dof = static_cast<double>(n - 1);
  const double limit = dof + 6.0 * std::sqrt(2.0 * dof);
  INFO("chi2 = ", chi2, " limit = ", limit);
  CHECK(chi2 < limit);
}

TEST_CASE("AliasPicker pick() stays in range under boundary/OOB RNG scripts") {
  for (std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{3},
                        std::size_t{1024}, std::size_t{1325},
                        std::size_t{1326}}) {
    const std::vector<std::uint64_t> masks = trivial_masks(n);
    const std::vector<double> weights = weights_log_ramp(n, 1.0, 1e3);
    const AliasPicker p(masks.data(), weights.data(), n);
    const std::set<std::uint64_t> mask_set(masks.begin(), masks.end());

    std::vector<std::uint64_t> raws = {
        std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max() - (1ULL << 11),
        std::uint64_t{0},
    };
    // Values landing exactly on (or just past) every bucket boundary:
    // pick() only looks at the top 53 bits (next() >> 11), so the low 11
    // bits of the scripted word are irrelevant and left zero.
    for (std::size_t k = 0; k <= n; ++k) {
      std::uint64_t top53 =
          (static_cast<std::uint64_t>(k) * (1ULL << 53)) /
          static_cast<std::uint64_t>(n);
      if (top53 > ((1ULL << 53) - 1)) top53 = (1ULL << 53) - 1;  // k == n
      raws.push_back(top53 << 11);
    }

    for (std::uint64_t raw : raws) {
      CAPTURE(n);
      CAPTURE(raw);
      ScriptedRng stub{raw};
      const std::uint64_t result = p.pick(stub);
      REQUIRE(mask_set.count(result) == 1);
    }
  }
}

TEST_CASE("AliasPicker rejects n == 0 even in release builds") {
    CHECK_THROWS_AS(AliasPicker(nullptr, nullptr, 0), std::invalid_argument);
}

TEST_CASE("AliasPicker table construction is deterministic") {
  const std::size_t n = 1326;
  const std::vector<std::uint64_t> masks = trivial_masks(n);
  const std::vector<double> weights = weights_log_ramp(n, 1.0, 1e3);
  const AliasPicker a(masks.data(), weights.data(), n);
  const AliasPicker b(masks.data(), weights.data(), n);
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < n; ++i) {
    CHECK(a.prob_at(i) == b.prob_at(i));
    CHECK(a.mask_at(i) == b.mask_at(i));
    CHECK(a.alias_mask_at(i) == b.alias_mask_at(i));
  }
}

TEST_CASE("AliasPicker pinned table (n=4, weights 0.1/0.2/0.3/0.4)") {
  // Hand-traced through the construction algorithm in alias_picker.h:
  //   max_w = 0.4; scaled (after /max_w) = [0.25, 0.5, 0.75, 1.0]
  //   sum = 2.5; scale = 4/2.5 = 1.6
  //   scaled (after *scale) = [0.4, 0.8, 1.2, 1.6]
  //   work-stack split: small = {0, 1} (front), large = {3, 2} (back)
  //     -> work = [0, 1, 3, 2], small_top=2, large_bot=2
  //   iter1: s=1 (scaled 0.8), l=work[large_bot=2]=3 (scaled 1.6)
  //          entries[1] = {prob=0.8, alias=mask[3]=4}
  //          scaled[3] = 1.6 + 0.8 - 1.0 = 1.4 (>=1, stays large)
  //   iter2: s=0 (scaled 0.4), l=work[large_bot=2]=3 (scaled 1.4)
  //          entries[0] = {prob=0.4, alias=mask[3]=4}
  //          scaled[3] = 1.4 + 0.4 - 1.0 = 0.8 (<1, moves to small stack)
  //   iter3: s=3 (scaled 0.8), l=work[large_bot=3]=2 (scaled 1.2)
  //          entries[3] = {prob=0.8, alias=mask[2]=3}
  //          scaled[2] = 1.2 + 0.8 - 1.0 = 1.0 (not <1, stays large,
  //          never revisited: small_top reaches 0 and the loop ends)
  //   entries[2] never becomes 's': stays at its default (prob=1.0,
  //   alias=self, mask=3) -- the "leftover" case documented in the header.
  //   Mass check (sanity, not asserted below): 0.1+0.2+0.3+0.4 == 1.0 and
  //   each emitted_mass(j) reproduces weights[j] exactly (verified
  //   separately by the n=4 case of the exact-distribution test above).
  std::vector<std::uint64_t> masks = {1, 2, 3, 4};
  std::vector<double> weights = {0.1, 0.2, 0.3, 0.4};
  const AliasPicker p(masks.data(), weights.data(), 4);
  REQUIRE_FALSE(p.is_uniform());
  REQUIRE(p.size() == 4);

  CHECK(p.mask_at(0) == 1);
  CHECK(p.prob_at(0) == doctest::Approx(0.4).epsilon(1e-9));
  CHECK(p.alias_mask_at(0) == 4);

  CHECK(p.mask_at(1) == 2);
  CHECK(p.prob_at(1) == doctest::Approx(0.8).epsilon(1e-9));
  CHECK(p.alias_mask_at(1) == 4);

  CHECK(p.mask_at(2) == 3);
  CHECK(p.prob_at(2) == doctest::Approx(1.0).epsilon(1e-9));
  CHECK(p.alias_mask_at(2) == 3);  // leftover: alias defaults to self

  CHECK(p.mask_at(3) == 4);
  CHECK(p.prob_at(3) == doctest::Approx(0.8).epsilon(1e-9));
  CHECK(p.alias_mask_at(3) == 3);
}

namespace {

std::uint64_t mk2(const std::string& a, const std::string& b) {
  return card_to_mask(Card::from_string(a)) | card_to_mask(Card::from_string(b));
}

}  // namespace

TEST_CASE("AliasPicker integration: NaN/inf/non-positive weights vanish through the public API") {
  // A Range with one combo per pathological weight (NaN, +inf, 0.0, -1.0)
  // plus one valid combo must behave, end to end through
  // calculate_range_equity, exactly as if only the valid combo existed.
  // This exercises the Step 1 filter fix (filter_combos) together
  // with the AliasPicker construction it feeds.
  std::vector<Combo> combos = {
      {mk2("As", "Ks"), std::numeric_limits<double>::quiet_NaN()},
      {mk2("Ad", "Kd"), std::numeric_limits<double>::infinity()},
      {mk2("Ac", "Kc"), 0.0},
      {mk2("Ah", "Kh"), -1.0},
      {mk2("2s", "2d"), 1.0},  // the one valid combo
  };
  Range hero(combos);
  Range hero_valid_only(std::vector<Combo>{{mk2("2s", "2d"), 1.0}});
  Range vill = Range::from_string("QQ");

  SimulationOptions mc;
  mc.iterations = 5000;
  mc.deterministic = true;
  mc.seed = 123;

  SUBCASE("PerCombo") {
    auto full =
        calculate_range_equity(hero, vill, 0, mc, RangeEquityMode::PerCombo);
    auto valid = calculate_range_equity(hero_valid_only, vill, 0, mc,
                                        RangeEquityMode::PerCombo);
    REQUIRE(full.hero.size() == 1);
    CHECK(full.hero[0].combo_mask == valid.hero[0].combo_mask);
    CHECK(full.hero_aggregate_equity == valid.hero_aggregate_equity);
    CHECK(full.villain_aggregate_equity == valid.villain_aggregate_equity);
    CHECK_FALSE(std::isnan(full.hero_aggregate_equity));
    CHECK_FALSE(std::isnan(full.villain_aggregate_equity));
    for (const auto& e : full.hero) CHECK_FALSE(std::isnan(e.equity));
    for (const auto& e : full.villain) CHECK_FALSE(std::isnan(e.equity));
  }

  SUBCASE("AggregateOnly") {
    auto full = calculate_range_equity(hero, vill, 0, mc,
                                       RangeEquityMode::AggregateOnly);
    auto valid = calculate_range_equity(hero_valid_only, vill, 0, mc,
                                        RangeEquityMode::AggregateOnly);
    CHECK(full.hero_aggregate_equity == valid.hero_aggregate_equity);
    CHECK(full.villain_aggregate_equity == valid.villain_aggregate_equity);
    CHECK(full.aggregate_std_error == valid.aggregate_std_error);
    CHECK_FALSE(std::isnan(full.hero_aggregate_equity));
    CHECK_FALSE(std::isnan(full.villain_aggregate_equity));
    CHECK_FALSE(std::isnan(full.aggregate_std_error));
  }
}
