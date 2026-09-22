#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace xiapl {
namespace internal {

// O(1) weighted sampler over combo masks (Walker alias method, Vose
// construction). Replaces per-draw binary search: one RNG draw, two loads,
// one compare per pick.
//
// Preconditions: n >= 1 (enforced with a hard throw below, not an assert —
// see the ctor); every weight is finite and > 0 — guaranteed by
// filter_combos, which drops non-finite and non-positive weights regardless
// of how the Range was constructed.
//
// Determinism: construction iterates in input order using index stacks in
// plain arrays; the table is a pure function of (masks, weights). This is
// part of the seeded-reproducibility contract.
class AliasPicker {
 public:
  AliasPicker(const std::uint64_t* masks, const double* weights,
              std::size_t n) {
    // Hard precondition, not an assert: this must hold in NDEBUG builds too.
    // n == 0 would read masks[0] out of bounds below and hand pick() an
    // empty table. calculate_range_equity screens empty ranges before
    // constructing a picker, so this is unreachable today and costs one
    // predictable branch in a non-hot path (two constructions per call).
    if (n == 0) {
      throw std::invalid_argument("AliasPicker: n must be >= 1");
    }
    n_ = static_cast<std::uint32_t>(n);
    first_mask_ = masks[0];

    // Uniform fast path: bit-equal weights (every unweighted range). Skips
    // the table build and samples with a single bounded() draw.
    uniform_ = true;
    for (std::size_t i = 1; i < n; ++i) {
      if (weights[i] != weights[0]) { uniform_ = false; break; }
    }
    if (uniform_) {
      uniform_masks_.assign(masks, masks + n);
      return;
    }

    // Max-normalize before summing: scaled[i] = (w[i]/max_w) * (n/W') with
    // W' = sum(w[i]/max_w). Normalizing by the raw sum overflows to inf for
    // weights near DBL_MAX, which silently degrades the sampler to uniform.
    double max_w = weights[0];
    for (std::size_t i = 1; i < n; ++i) {
      if (weights[i] > max_w) max_w = weights[i];
    }
    std::vector<double> scaled(n);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      scaled[i] = weights[i] / max_w;
      sum += scaled[i];
    }
    const double scale = static_cast<double>(n) / sum;
    for (std::size_t i = 0; i < n; ++i) scaled[i] *= scale;

    // Entries default to prob = 1, alias = self: leftovers on EITHER stack
    // after the pairing loop are already correct.
    entries_.resize(n + 1);
    for (std::size_t i = 0; i < n; ++i) {
      entries_[i].prob = 1.0;
      entries_[i].mask = masks[i];
      entries_[i].alias_mask = masks[i];
    }

    // Two stacks in one scratch array: small indices grow from the front,
    // large indices from the back. The regions never collide because every
    // live index sits in exactly one stack.
    std::vector<std::uint32_t> work(n);
    std::uint32_t small_top = 0;
    std::uint32_t large_bot = n_;
    for (std::uint32_t i = 0; i < n_; ++i) {
      if (scaled[i] < 1.0) work[small_top++] = i;
      else work[--large_bot] = i;
    }
    while (small_top > 0 && large_bot < n_) {
      const std::uint32_t s = work[--small_top];
      const std::uint32_t l = work[large_bot];
      entries_[s].prob = scaled[s];
      entries_[s].alias_mask = masks[l];
      scaled[l] = (scaled[l] + scaled[s]) - 1.0;
      if (scaled[l] < 1.0) {
        ++large_bot;
        work[small_top++] = l;
      }
    }

    // Sentinel: pick() computes i = (size_t)(u * n) with u < 1, which can
    // never reach n for any n in [1, 2^32) (proven: n*(1-2^-53) rounds up
    // to n only if n < 2^e <= n, a contradiction; exact powers of two are
    // representable). The sentinel makes that invariant harmless if it is
    // ever violated by a future refactor instead of undefined behavior.
    entries_[n_] = entries_[n_ - 1];
  }

  bool is_single() const { return n_ == 1; }
  // Callers taking the is_single() shortcut (bypassing pick() entirely for a
  // deterministic single-combo draw) must use first_mask() as the result.
  std::uint64_t first_mask() const { return first_mask_; }

  // Weight-proportional draw using a SINGLE RNG draw: the top 53 bits give
  // u in [0,1); i = floor(u*n) is the bucket, frac = u*n - i the coin.
  // Index-marginal deviation from 1/n is <= n/2^53 and the conditional
  // acceptance error is ~5e-13 — six orders below MC noise at any feasible
  // trial count (design-reviewed; a second draw buys nothing measurable).
  template <class Rng>
  std::uint64_t pick(Rng& rng) const {
    if (uniform_) {
      return uniform_masks_[rng.bounded(n_)];
    }
    const double u =
        static_cast<double>(rng.next() >> 11) * (1.0 / 9007199254740992.0);
    const double x = u * static_cast<double>(n_);
    const std::uint32_t i = static_cast<std::uint32_t>(x);
    const double frac = x - static_cast<double>(i);
    const Entry& e = entries_[i];
    return (frac < e.prob) ? e.mask : e.alias_mask;
  }

  // Test-only introspection.
  std::size_t size() const { return n_; }
  bool is_uniform() const { return uniform_; }
  double prob_at(std::size_t i) const { return entries_[i].prob; }
  std::uint64_t mask_at(std::size_t i) const { return entries_[i].mask; }
  std::uint64_t alias_mask_at(std::size_t i) const {
    return entries_[i].alias_mask;
  }

 private:
  struct Entry {
    double prob = 1.0;
    std::uint64_t mask = 0;
    std::uint64_t alias_mask = 0;
  };
  std::vector<Entry> entries_;          // n + 1 (sentinel), weighted mode only
  std::vector<std::uint64_t> uniform_masks_;  // uniform mode only
  std::uint64_t first_mask_ = 0;
  std::uint32_t n_ = 0;
  bool uniform_ = false;
};

}  // namespace internal
}  // namespace xiapl
