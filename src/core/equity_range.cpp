#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <xiapl/detail/fast_rng.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include "alias_picker.h"
#include "board_sample.h"
#include "eval_internal.h"
#include "mc_chunking.h"
#include "range_parse.h"

namespace xiapl {

// Chunked Monte Carlo primitives (src/core/mc_chunking.h): the chunk sizing /
// substream seeding / index-ordered reduction that sample_range_equity below
// used to spell out itself now lives inside run_mc_chunks; run_chunks_parallel
// is used directly by the two non-MC (no RNG) partitions in the per-combo
// range path (the evaluation-cache fill and the pair loop, both below).
using internal::resolve_master_seed;
using internal::run_chunks_parallel;
using internal::run_mc_chunks;

// Human-readable game name for diagnostics (src/core/range_parse.h); shared
// with Range's set-algebra error messages (range_setops.cpp).
using internal::game_name;

// Board sampling / enumeration primitives (src/core/board_sample.h).
using internal::deck_indices_of;
using internal::enumerate_or_sample_boards;

// Unified equity-input validation (src/core/board_sample.h): one call per
// public entry point -- calculate_range_equity below. calculate_equity
// validates via compute_player_equities in equity_hands.cpp.
using internal::validate_board_count;

namespace {

// ---- Combo filtering ----

struct FilteredCombo {
  std::uint64_t mask;
  double weight;
};

// Drop combos whose mask is not a valid hand for the game (2 cards for
// Hold'em, 4 for PLO), overlaps the board, has non-positive weight, or
// duplicates another combo's mask (first occurrence kept).
//
// The unordered_set is a membership test only: the surviving combos are
// pushed in input order, so the output — and everything downstream that
// depends on it (alias-table construction, cache row order, pair-loop order)
// — is a pure function of the input sequence, not of any hash order.
static std::vector<FilteredCombo>
filter_combos(const std::vector<Combo> &combos, std::uint64_t board_mask,
              int cards_per_combo) {
  std::vector<FilteredCombo> out;
  out.reserve(combos.size());
  std::unordered_set<std::uint64_t> seen;
  seen.reserve(combos.size() * 2);
  for (const Combo &c : combos) {
    std::uint64_t m = c.mask & FULL_DECK_MASK;
    if (popcount64(m) != cards_per_combo) continue;
    if (m & board_mask) continue;
    // NaN fails every comparison, so `weight <= 0.0` would let NaN through.
    // Weights are unvalidated doubles reachable from Python (Combo.weight is
    // writable), so drop anything that is not a finite positive number.
    if (!std::isfinite(c.weight) || !(c.weight > 0.0)) continue;
    if (!seen.insert(m).second) continue;
    out.push_back({m, c.weight});
  }
  return out;
}

// ---- PerCombo enumeration (exact / bit-identical MC) ----

// Single-pass result for enumerate_range_equity: holds per-combo
// entries (hero + villain) and the pair-weighted aggregate numerator /
// denominator (caller divides). num_boards is the size of the global board
// sample set used (for SimulationResult::trials).
struct RangeEquityAccum {
  std::vector<RangeEquityEntry> hero;
  std::vector<RangeEquityEntry> villain;
  double pair_equity_weighted_sum = 0.0;
  double pair_weight_sum = 0.0;
  std::uint64_t num_boards = 0;
  // True when the board phase ran in exact-enumeration mode (either iterations
  // == 0 OR iterations >= C(remaining_deck, num_to_draw) auto-fallback).
  bool was_exact = false;
};

// Chunk COUNT cap (not chunk SIZE) for the PerCombo pair loop's hero-row
// partition. A fixed chunk SIZE (the original design here) caps parallelism
// at ceil(hero.size() / size): for a fixed size of 256, every realistic
// Hold'em range (44-1326 combos) collapses to 1 chunk, serializing the pair
// loop -- the dominant cost on mixed spots -- regardless of `threads`. A
// fixed chunk COUNT instead always offers up to kPerComboMaxRowChunks-way
// parallelism (more precisely min(hero.size(), kPerComboMaxRowChunks), since
// a chunk needs at least one row), while remaining a pure function of
// hero.size() alone -- never of the worker count -- which is what keeps
// PerCombo results bit-identical for every `threads` value including serial,
// and identical across machines with different core counts.
constexpr std::size_t kPerComboMaxRowChunks = 64;

// Single-pass range vs range equity.
//
// Builds the board sample set, unique-hand evaluation cache, and pair loop
// exactly once. The previous 2-pass implementation called the per-side helper
// twice (hero-as-hero and villain-as-hero), which doubled the dominant cost
// (cache fill ~= unique_hands * num_boards evaluations).
//
// Per pair (hero combo i, villain combo j) we accumulate:
//   hero_acc[i].equity_sum   += vc.weight * equity_hv
//   hero_acc[i].weight_sum   += vc.weight
//   vill_acc[j].equity_sum   += hc.weight * (1 - equity_hv)
//   vill_acc[j].weight_sum   += hc.weight
//   pair_equity_weighted_sum += hc.weight * vc.weight * equity_hv
//   pair_weight_sum          += hc.weight * vc.weight
//
// Per-combo equity in the emitted RangeEquityEntry is equity_sum / weight_sum.
// Pair aggregate hero_equity = pair_equity_weighted_sum / pair_weight_sum;
// villain aggregate = 1 - hero aggregate.
//
// `game` selects the scorer used to fill the evaluation cache; everything
// after the fill (the pair loop, the accumulators, the aggregate) compares
// packed scores and is game-independent by construction.
static RangeEquityAccum
enumerate_range_equity(const std::vector<FilteredCombo> &hero_combos,
                       const std::vector<FilteredCombo> &villain_combos,
                       std::uint64_t board_mask, int trials,
                       FastRng &rng, int num_threads_hint,
                       GameType game) {
  RangeEquityAccum result;

  const int board_count = popcount64(board_mask);
  const int num_to_draw = 5 - board_count;

  // Build global board sample set via the shared enumerator. trials==0 ->
  // exact, trials>0 -> MC. enumerate_or_sample_boards calls trial_fn once
  // per completed board mask.
  std::uint64_t remain_mask = FULL_DECK_MASK & ~board_mask;
  std::vector<int> deck_indices = deck_indices_of(remain_mask);
  std::vector<std::uint64_t> boards;
  if (trials > 0 && num_to_draw > 0) {
    boards.reserve(static_cast<std::size_t>(trials));
  }
  bool was_exact = false;
  enumerate_or_sample_boards(deck_indices, num_to_draw, trials, board_mask, rng,
                             [&](std::uint64_t b) { boards.push_back(b); },
                             &was_exact);

  const std::size_t num_boards = boards.size();
  result.num_boards = static_cast<std::uint64_t>(num_boards);
  result.was_exact = was_exact;

  // Collect unique hand masks (hero + villain) for evaluation cache
  std::vector<std::uint64_t> unique_hands;
  unique_hands.reserve(hero_combos.size() + villain_combos.size());
  for (const auto &c : hero_combos) unique_hands.push_back(c.mask);
  for (const auto &c : villain_combos) unique_hands.push_back(c.mask);
  std::sort(unique_hands.begin(), unique_hands.end());
  unique_hands.erase(std::unique(unique_hands.begin(), unique_hands.end()),
                     unique_hands.end());

  // Hard cap on the evaluation cache. Applies in BOTH exact and MC mode —
  // wide preflop exact (~13 GB at 1326 combos × C(52,5) boards) and large MC
  // (e.g. 1326 unique hands × 10M boards × 4 B ≈ 53 GB) both hit this.
  // Convert into a clean exception with a remediation hint instead of an
  // OOM kill.
  //
  // PLO gets its own remediation text. The cap is far easier to hit there —
  // a PLO range expands into 4-card combos, so even a modest-looking
  // notation item is hundreds or thousands of unique hands — and the answer
  // is not "lower iterations" (exact has none to lower) but "switch
  // estimator", so the message names the mode that scales.
  {
    constexpr std::size_t kCacheBytesLimit = 512ULL * 1024 * 1024;
    const std::size_t estimated_bytes =
        unique_hands.size() * num_boards * sizeof(std::uint32_t);
    if (estimated_bytes > kCacheBytesLimit) {
      const std::string size_text =
          "~" + std::to_string(estimated_bytes / (1024 * 1024)) +
          " MB of evaluation cache (limit " +
          std::to_string(kCacheBytesLimit / (1024 * 1024)) + " MB). ";
      if (game == GameType::Plo) {
        throw std::runtime_error(
            "calculate_range_equity: this PLO range-vs-range request would "
            "allocate " + size_text +
            "PLO hands are 4 cards, so a range expands into far more combos "
            "than the Hold'em equivalent and the per-combo engine does not "
            "scale to wide PLO ranges. Use RangeEquityMode::AggregateOnly "
            "with SimulationOptions::mc_seeded(iterations, seed), or narrow "
            "the range / fix more board cards.");
      }
      throw std::runtime_error(
          "calculate_range_equity: would allocate " + size_text +
          "Reduce SimulationOptions::iterations or narrow the range / "
          "fix more board cards.");
    }
  }

  // Pre-evaluate each unique hand on each global board (0 == invalid overlap).
  // Every (hand, board) cell is written by exactly one worker whichever way
  // the fill is cut below, so the fill is lock-free and has no shared mutable
  // state. We reuse the same thread-count decision as the pair loop below to
  // keep the policy consistent.
  std::vector<std::uint32_t> cache(unique_hands.size() * num_boards, 0);

  // A negative hint means serial on every path (see SimulationOptions::
  // threads); only an explicit 0 asks for auto. Auto here keys off this
  // path's own work-size heuristic and does NOT read XIAPL_NUM_THREADS —
  // that variable belongs to the Monte Carlo sampling paths, whose unit of
  // work (trials) is not comparable to pair-board evaluations.
  auto resolve_threads = [&](std::uint64_t work) -> int {
    const int hint = num_threads_hint;
    if (hint < 0) return 1;
    if (hint > 0) return hint;
    if (work < 10'000'000ULL) return 1;
    const unsigned hw = std::thread::hardware_concurrency();
    return hw > 0 ? static_cast<int>(hw) : 1;
  };

  // The cached value is the packed evaluator score itself: it is
  // order-isomorphic to HandValue (see eval_core.h) and never 0 for a real
  // hand, so 0 stays available as the "board conflicts with these holes"
  // sentinel used by the pair loop below. This holds for PLO too: the best
  // of the 60 (2-of-4 hole x 3-of-5 board) five-card hands is always at
  // least a high card, i.e. category 1 in bits 23..20.
  //
  // The game is decided ONCE, outside both loops, so the Hold'em fill keeps
  // exactly the loop body it had before PLO existed.
  const bool fill_plo = (game == GameType::Plo);
  // Fills the cache rectangle [h0, h1) x [b0, b1). Cells are independent, so
  // any partition of the rectangle produces a bit-identical cache.
  auto fill_block = [&](std::size_t h0, std::size_t h1, std::size_t b0,
                        std::size_t b1) {
    if (fill_plo) {
      // Boards are complete (5 cards) here, so every board expands to the
      // same 10 triples for every hand; they are rebuilt per board rather
      // than cached because the board loop is the inner one.
      for (std::size_t h = h0; h < h1; ++h) {
        const std::uint64_t hand = unique_hands[h];
        std::uint32_t *row = &cache[h * num_boards];
        for (std::size_t t = b0; t < b1; ++t) {
          const std::uint64_t b = boards[t];
          if (hand & b) continue; // leave 0 (invalid)
          row[t] = internal::plo_score(hand, b);
        }
      }
      return;
    }
    for (std::size_t h = h0; h < h1; ++h) {
      const std::uint64_t hand = unique_hands[h];
      std::uint32_t *row = &cache[h * num_boards];
      for (std::size_t t = b0; t < b1; ++t) {
        const std::uint64_t b = boards[t];
        if (hand & b) continue; // leave 0 (invalid)
        row[t] = internal::eval_score7(b | hand);
      }
    }
  };

  {
    const std::size_t num_hands = unique_hands.size();
    // The fill's unit of work is one cache CELL, but a cell is not the same
    // amount of work in both games: a Hold'em cell is one eval_score7 call,
    // a PLO cell is the best of 60 (C(4,2) hole pairs x C(5,3) board triples)
    // — so a PLO fill hits the auto-threading threshold at 1/60th of the cell
    // count Hold'em needs for the same real cost. Scale accordingly. (The
    // pair loop below is NOT scaled: its per-cell work is two cached-score
    // loads and a compare in both games.)
    const std::uint64_t evals_per_cell = fill_plo ? 60ULL : 1ULL;
    const std::uint64_t fill_work = static_cast<std::uint64_t>(num_hands) *
                                    static_cast<std::uint64_t>(num_boards) *
                                    evals_per_cell;
    int fill_threads = resolve_threads(fill_work);

    // Two ways to cut the rectangle, both lock-free because each (hand,
    // board) cell has exactly one owner. Rows first — they give each worker
    // a contiguous slab — but a narrow spot has fewer rows than cores, and
    // the row split silently capped the fill at unique_hands.size() threads
    // there. That is exactly the shape the docs steer PLO exact toward ("a
    // handful of combos, or a turn/river board"): two exact hands is 2 rows
    // and millions of boards, i.e. a hard 2x ceiling. Split by BOARD slices
    // instead when rows would starve the workers; each worker then owns a
    // disjoint board range across every row.
    const bool split_boards = static_cast<std::uint64_t>(fill_threads) > num_hands;
    const std::size_t axis = split_boards ? num_boards : num_hands;
    if (fill_threads > static_cast<int>(axis))
      fill_threads = static_cast<int>(axis);

    if (fill_threads <= 1) {
      fill_block(0, num_hands, 0, num_boards);
    } else {
      // Any partition of the (hand, board) rectangle is bit-identical (each
      // cell has one owner); run_chunks_parallel adds the exception-safe
      // join the manual spawn loop lacked (a mid-loop std::thread-ctor throw
      // used to leave already-spawned joinable threads to std::terminate).
      const std::size_t slice =
          (axis + static_cast<std::size_t>(fill_threads) - 1) /
          static_cast<std::size_t>(fill_threads);
      const std::uint64_t num_slices =
          static_cast<std::uint64_t>((axis + slice - 1) / slice);
      run_chunks_parallel(
          num_slices, fill_threads, [&](std::uint64_t s) {
            const std::size_t lo = static_cast<std::size_t>(s) * slice;
            const std::size_t hi = std::min(axis, lo + slice);
            if (split_boards) fill_block(0, num_hands, lo, hi);
            else fill_block(lo, hi, 0, num_boards);
          });
    }
  }

  // Precompute cache row index for each hero/villain combo so the pair loop
  // doesn't re-do binary search on every iteration.
  auto idx_of = [&](std::uint64_t hand) {
    auto it = std::lower_bound(unique_hands.begin(), unique_hands.end(), hand);
    return static_cast<std::size_t>(std::distance(unique_hands.begin(), it));
  };
  std::vector<std::size_t> hero_idx(hero_combos.size());
  std::vector<std::size_t> vill_idx(villain_combos.size());
  for (std::size_t i = 0; i < hero_combos.size(); ++i)
    hero_idx[i] = idx_of(hero_combos[i].mask);
  for (std::size_t j = 0; j < villain_combos.size(); ++j)
    vill_idx[j] = idx_of(villain_combos[j].mask);

  // Per-combo (equity_sum, weight_sum) accumulators.
  struct Accum {
    double equity_sum = 0.0;
    double weight_sum = 0.0;
  };

  // Pair-loop kernel. Each call processes hero combos in [i0, i1) and writes
  // to thread-local hero_acc / vill_acc / pair-aggregate accumulators. The
  // inner board loop is intentionally branchless so the compiler vectorizes
  // it (autovec on -O3 -march=native gives ~2x over the branchy version).
  auto run_hero_slice =
      [&](std::size_t i0, std::size_t i1, std::vector<Accum> &hero_acc_out,
          std::vector<Accum> &vill_acc_out, double &pair_eq_sum_out,
          double &pair_w_sum_out) {
        for (std::size_t i = i0; i < i1; ++i) {
          const auto &hc = hero_combos[i];
          const std::uint32_t *hero_scores = &cache[hero_idx[i] * num_boards];
          for (std::size_t j = 0; j < villain_combos.size(); ++j) {
            const auto &vc = villain_combos[j];
            if (hc.mask & vc.mask) continue; // overlap: villain shares cards
            const std::uint32_t *vill_scores = &cache[vill_idx[j] * num_boards];
            std::uint64_t wins = 0, chops = 0, valid = 0;
            for (std::size_t t = 0; t < num_boards; ++t) {
              const std::uint32_t hs = hero_scores[t];
              const std::uint32_t vs = vill_scores[t];
              // Branchless: both_valid is 1 iff neither score is 0
              // (board didn't conflict with hero/villain holes).
              const std::uint32_t both_valid =
                  static_cast<std::uint32_t>(hs != 0) &
                  static_cast<std::uint32_t>(vs != 0);
              valid += both_valid;
              wins += both_valid &
                      static_cast<std::uint32_t>(hs > vs);
              chops += both_valid &
                       static_cast<std::uint32_t>(hs == vs);
            }
            if (valid == 0) continue;
            const double equity_hv =
                (static_cast<double>(wins) + 0.5 * static_cast<double>(chops)) /
                static_cast<double>(valid);
            hero_acc_out[i].equity_sum += vc.weight * equity_hv;
            hero_acc_out[i].weight_sum += vc.weight;
            vill_acc_out[j].equity_sum += hc.weight * (1.0 - equity_hv);
            vill_acc_out[j].weight_sum += hc.weight;
            const double pair_weight = hc.weight * vc.weight;
            pair_eq_sum_out += pair_weight * equity_hv;
            pair_w_sum_out += pair_weight;
          }
        }
      };

  // Decide thread count. We pass `thread_count` via num_threads_hint from
  // the caller — it captures SimulationOptions::threads (0 = auto, 1 =
  // single, N = N).
  std::vector<Accum> hero_acc(hero_combos.size());
  std::vector<Accum> vill_acc(villain_combos.size());
  const std::uint64_t pair_work =
      static_cast<std::uint64_t>(hero_combos.size()) *
      static_cast<std::uint64_t>(villain_combos.size()) *
      static_cast<std::uint64_t>(num_boards);
  int nthreads = resolve_threads(pair_work);

  // Hero rows are partitioned into up to kPerComboMaxRowChunks chunks
  // (serial included -- determinism requires the SAME grouping at every
  // thread count, so there is no separate nthreads == 1 fast path here).
  // num_row_chunks and rows_per_chunk are pure functions of hero.size()
  // alone -- never of nthreads or of the machine's core count -- which is
  // what keeps the float-summation order, and thus the last bits of every
  // equity, identical for every `threads` value. hero_combos is non-empty
  // here (calculate_range_equity short-circuits the empty case before this
  // function is reached), so num_row_chunks >= 1 always.
  const std::size_t num_row_chunks =
      std::min(hero_combos.size(), kPerComboMaxRowChunks);
  const std::size_t rows_per_chunk =
      (hero_combos.size() + num_row_chunks - 1) / num_row_chunks;
  // Clamp the worker count to the chunk count: extra workers would only spin
  // up to find run_chunks_parallel's atomic counter already past
  // num_row_chunks (same reasoning as the fill's own thread clamp above).
  if (nthreads > static_cast<int>(num_row_chunks))
    nthreads = static_cast<int>(num_row_chunks);

  // Hero rows are owned by exactly one chunk, so workers write hero_acc
  // directly below (disjoint rows, no race). Villain columns and the pair
  // sums are touched by every chunk, so they get chunk-local accumulators,
  // reduced in ascending chunk-index order below -- that fixed order is what
  // makes the reduction itself independent of nthreads too.
  //
  // Memory bound: chunk_vill holds num_row_chunks x villain_combos.size() x
  // sizeof(Accum) (16 B) doubles. num_row_chunks is capped BY CONSTRUCTION at
  // kPerComboMaxRowChunks (64) regardless of how wide hero gets, so the bound
  // is 64 x villain.size() x 16 B. villain.size() is in turn bounded by the
  // game's combinatorial ceiling on whatever cards are NOT already on the
  // board -- and that ceiling is NOT tightest at a fully specified (river)
  // board. A turn board (4 board cards, num_boards == 48 river completions,
  // which comfortably survives the 512 MB evaluation-cache cap above at only
  // ~37 MB of cache) leaves 48 undealt cards instead of the river's 47, so
  // villain.size() <= C(48,4) = 194,580 for PLO (C(48,2) = 1,128 for Hold'em)
  // is the reachable worst case, not the river's C(47,4) = 178,365. That puts
  // the bound at 64 x 194,580 x 16 B =~ 199 MB (verified: 64 * 194580 * 16 =
  // 199,249,920 bytes), comfortably under the 512 MB evaluation-cache cap
  // above -- and well under what the pair loop's OLD fixed-256-row-SIZE
  // design would have used for the same PLO ceiling (ceil(194580/256) = 761
  // chunks, ~2.4 GB), since that design's chunk count grew with hero.size()
  // instead of being capped.
  std::vector<std::vector<Accum>> chunk_vill(
      num_row_chunks, std::vector<Accum>(villain_combos.size()));
  std::vector<double> chunk_pair_eq(num_row_chunks, 0.0);
  std::vector<double> chunk_pair_w(num_row_chunks, 0.0);
  run_chunks_parallel(num_row_chunks, nthreads, [&](std::uint64_t k) {
    const std::size_t ki = static_cast<std::size_t>(k);
    const std::size_t i0 = ki * rows_per_chunk;
    const std::size_t i1 = std::min(hero_combos.size(), i0 + rows_per_chunk);
    run_hero_slice(i0, i1, hero_acc, chunk_vill[ki], chunk_pair_eq[ki],
                   chunk_pair_w[ki]);
  });
  for (std::size_t k = 0; k < num_row_chunks; ++k) {
    for (std::size_t j = 0; j < villain_combos.size(); ++j) {
      vill_acc[j].equity_sum += chunk_vill[k][j].equity_sum;
      vill_acc[j].weight_sum += chunk_vill[k][j].weight_sum;
    }
    result.pair_equity_weighted_sum += chunk_pair_eq[k];
    result.pair_weight_sum += chunk_pair_w[k];
  }

  // Emit per-combo entries (only those with at least one compatible opposing
  // combo). Order matches the input hero_combos / villain_combos.
  result.hero.reserve(hero_combos.size());
  for (std::size_t i = 0; i < hero_combos.size(); ++i) {
    if (hero_acc[i].weight_sum > 0.0) {
      RangeEquityEntry e;
      e.combo_mask = hero_combos[i].mask;
      e.equity = hero_acc[i].equity_sum / hero_acc[i].weight_sum;
      e.weight = hero_combos[i].weight;
      result.hero.push_back(e);
    }
  }
  result.villain.reserve(villain_combos.size());
  for (std::size_t j = 0; j < villain_combos.size(); ++j) {
    if (vill_acc[j].weight_sum > 0.0) {
      RangeEquityEntry e;
      e.combo_mask = villain_combos[j].mask;
      e.equity = vill_acc[j].equity_sum / vill_acc[j].weight_sum;
      e.weight = villain_combos[j].weight;
      result.villain.push_back(e);
    }
  }

  return result;
}

// ---- AggregateOnly sampled-pair Monte Carlo ----------------------------
//
// The PerCombo engine above pays for a per-combo breakdown it does not need
// when the caller only wants the pair aggregate: it materialises a board
// sample set, an (unique hands x boards) evaluation cache, and an
// O(hero x villain x boards) pair loop. The estimator below samples the pair
// dimension instead of enumerating it, so the cost is O(iterations) with no
// cache at all.
//
// Estimand (identical to the PerCombo aggregate):
//   sum over card-compatible pairs of w_h * w_v * equity(h, v)
//   / sum over card-compatible pairs of w_h * w_v
// where equity(h, v) averages over boards drawn from the deck minus
// (fixed board | h | v). Each trial draws (h, v) with probability
// proportional to w_h * w_v restricted to the compatible set, then one board,
// so the per-trial equity is an unbiased draw from that estimand.

struct AggregateSample {
  double mean = 0.0;
  double std_error = 0.0;
  std::uint64_t trials = 0;
};

// Cap on consecutive pair rejections before we declare the ranges mutually
// blocking. Only ranges whose compatible-pair weight is ~0 can reach this
// (acceptance p gives (1-p)^10000, i.e. < 1e-4 already at p = 1e-3).
//
// PLO does not need a different cap even though its combos are twice as wide.
// Two independent 4-card hands share a card with probability
// 1 - C(48,4)/C(52,4) = 28.1%, so the worst case for a legitimate range pair
// is an acceptance rate around 0.72; the cap fires only after 10000
// consecutive rejections, i.e. with probability 0.281^10000, which is zero for
// any practical purpose. Narrow PLO ranges can of course collide far more
// often (AAKKds vs AAKKds accepts only 1 in 6), and 0.833^10000 is still ~0.
constexpr int kMaxConsecutivePairRejections = 10000;

// Preconditions (guaranteed by calculate_range_equity's input validation and
// by filter_combos, which drops non-positive weights regardless of how the
// Range was constructed): both combo vectors are non-empty and carry strictly
// positive weights, board_mask has 0 or 3-5 bits, and iterations > 0.
//
// kPlo selects the showdown rule. It is a template parameter rather than a
// runtime flag on purpose: the trial loop is the hot path, and this way the
// Hold'em instantiation contains no game test at all — the `if constexpr`
// below is resolved before codegen, so the Hold'em stream is exactly what it
// was before PLO existed. Everything else in the loop (weighted pair draw,
// pair rejection, board deal) is game-agnostic: AliasPicker treats the combo
// masks as opaque 64-bit values, and the dealer only needs `dead_mask` to
// have the right bits set — 8 for PLO instead of 4.
template <bool kPlo>
static AggregateSample sample_range_equity(
    const std::vector<FilteredCombo> &hero_combos,
    const std::vector<FilteredCombo> &villain_combos, std::uint64_t board_mask,
    int iterations, std::uint64_t master_seed, int num_threads_option) {
  std::vector<std::uint64_t> hero_masks_v, vill_masks_v;
  std::vector<double> hero_weights_v, vill_weights_v;
  hero_masks_v.reserve(hero_combos.size());
  hero_weights_v.reserve(hero_combos.size());
  for (const FilteredCombo& c : hero_combos) {
    hero_masks_v.push_back(c.mask);
    hero_weights_v.push_back(c.weight);
  }
  vill_masks_v.reserve(villain_combos.size());
  vill_weights_v.reserve(villain_combos.size());
  for (const FilteredCombo& c : villain_combos) {
    vill_masks_v.push_back(c.mask);
    vill_weights_v.push_back(c.weight);
  }
  const internal::AliasPicker hero_picker(hero_masks_v.data(),
                                          hero_weights_v.data(),
                                          hero_masks_v.size());
  const internal::AliasPicker vill_picker(vill_masks_v.data(),
                                          vill_weights_v.data(),
                                          vill_masks_v.size());

  // A side with exactly one compatible combo has a deterministic draw, so the
  // uniform double and the search are skipped and no RNG draw is consumed.
  // The distribution is trivially unchanged (n == 1 puts all mass on that
  // combo); only the random stream differs from the n > 1 case, which no
  // contract depends on (reproducibility is same-seed-same-result). The flags
  // are hoisted out of the trial loop so the test is free.
  // "single hand vs range" — the eval7-equivalent shape — takes this path.
  const bool hero_single = hero_picker.is_single();
  const bool vill_single = vill_picker.is_single();
  const std::uint64_t hero_only = hero_picker.first_mask();
  const std::uint64_t vill_only = vill_picker.first_mask();

  const int num_to_draw = 5 - popcount64(board_mask);

  // Canonical deck (deck minus the fixed board) in a fixed order, copied once
  // per chunk. The hero/villain hole cards change every trial, so they are
  // excluded by rejecting the draw (see the deal loop) rather than by
  // rebuilding this array per trial: the Fisher-Yates permutation
  // deliberately carries across trials WITHIN a chunk. It must NOT carry
  // across chunks, or a chunk's results would depend on which chunks happened
  // to precede it on the same worker.
  const std::vector<int> base_deck =
      deck_indices_of(FULL_DECK_MASK & ~board_mask);
  const std::uint32_t deck_size = static_cast<std::uint32_t>(base_deck.size());

  const std::uint64_t total = static_cast<std::uint64_t>(iterations);

  struct ChunkResult {
    double sum = 0.0;
    double sum_sq = 0.0;
  };

  double equity_sum = 0.0;
  double equity_sum_sq = 0.0;

  // Runs one chunk end to end. Everything it mutates is lambda-local (the
  // accumulators, the RNG the driver seeded for this chunk, the deck copy), so
  // concurrent chunks share nothing but read-only state (the two pickers, the
  // canonical deck). Accumulating into locals rather than straight into the
  // returned result also keeps the hot loop off a line other workers touch.
  auto run_chunk = [&](std::uint64_t chunk_trials, FastRng &rng) {
    std::vector<int> shuffled = base_deck;
    int *const deck = shuffled.data();
    double sum = 0.0;
    double sum_sq = 0.0;

    for (std::uint64_t iter = 0; iter < chunk_trials; ++iter) {
      // (a)+(b)+(c) Draw a hero combo and a villain combo, rejecting the WHOLE
      // pair when they share a card. Redrawing only the villain would
      // condition the villain on the hero while leaving the hero marginal at
      // its unconditional weights, which is not the pair distribution the
      // PerCombo aggregate uses.
      //
      // When both sides are single-combo and those two combos overlap, every
      // redraw returns the same blocked pair; the rejection counter still
      // terminates the loop (and throwing is the right answer there, since no
      // compatible pair exists at all).
      std::uint64_t hero_mask = 0;
      std::uint64_t vill_mask = 0;
      int rejections = 0;
      for (;;) {
        hero_mask = hero_single ? hero_only : hero_picker.pick(rng);
        vill_mask = vill_single ? vill_only : vill_picker.pick(rng);
        if ((hero_mask & vill_mask) == 0) break;
        if (++rejections >= kMaxConsecutivePairRejections) {
          throw std::runtime_error(
              "calculate_range_equity: no card-compatible hero/villain combo "
              "pair found after " +
              std::to_string(kMaxConsecutivePairRejections) +
              " draws; the two ranges appear to block each other");
        }
      }

      // (d) Deal the remaining board by partial Fisher-Yates over `shuffled`,
      // skipping the hole cards already drawn for both sides (4 total for
      // Hold'em, 8 for PLO -- dead_mask above). At step d, positions
      // [d, deck_size) hold exactly the not-yet-drawn cards, so rejecting
      // draws that land on a dead card yields a uniform pick among the live
      // ones; the accepted card is then swapped to position d as usual.
      // `shuffled` stays a permutation of the same multiset, so no reset is
      // needed between trials (within the chunk).
      const std::uint64_t dead_mask = hero_mask | vill_mask;
      std::uint64_t board = board_mask;
      for (std::uint32_t d = 0; d < static_cast<std::uint32_t>(num_to_draw);
           ++d) {
        std::uint32_t j;
        int picked;
        do {
          j = d + rng.bounded(deck_size - d);
          picked = deck[j];
        } while ((1ULL << picked) & dead_mask);
        deck[j] = deck[d];
        deck[d] = picked;
        board |= (1ULL << picked);
      }

      // (e) Packed scores are order-isomorphic to HandValue (see eval_core.h),
      // so the uint32 comparison is the HandValue ordering without the decode.
      // Both branches are allocation-free; the PLO one shares the ten 3-card
      // board subsets between the two players (60 five-card evaluations per
      // side) instead of rebuilding them twice.
      std::uint32_t hero_score;
      std::uint32_t vill_score;
      if constexpr (kPlo) {
        std::uint64_t triples[internal::kPloMaxBoardTriples];
        const int n_triples = internal::plo_board_triples(board, triples);
        hero_score = internal::plo_score_triples(hero_mask, triples, n_triples);
        vill_score = internal::plo_score_triples(vill_mask, triples, n_triples);
      } else {
        hero_score = internal::eval_score7(board | hero_mask);
        vill_score = internal::eval_score7(board | vill_mask);
      }
      const double equity = (hero_score > vill_score)
                                ? 1.0
                                : ((hero_score == vill_score) ? 0.5 : 0.0);
      sum += equity;
      sum_sq += equity * equity;
    }

    return ChunkResult{sum, sum_sq};
  };

  // Work stealing off one atomic counter (inside the driver) is safe for
  // reproducibility because results land by chunk index, not by completion
  // order. Chunks that throw — as with mutually blocking ranges, where every
  // chunk throws — surface as a rethrow after the join.
  //
  // The driver reduces in chunk-index order, never in completion order. Today
  // both accumulators are order-insensitive anyway: per-trial equities are 0,
  // 0.5 or 1.0 and their squares are 0, 0.25 or 1.0, so every partial sum is
  // an exact multiple of 0.25 bounded by the trial count (<= INT_MAX), which
  // keeps it far inside 2^53 — every addition is exact and any order gives the
  // same double. The fixed order is what keeps that true if the estimator ever
  // accumulates something less benign.
  run_mc_chunks<ChunkResult>(total, num_threads_option, master_seed, run_chunk,
                             [&](const ChunkResult &c) {
                               equity_sum += c.sum;
                               equity_sum_sq += c.sum_sq;
                             });

  AggregateSample out;
  out.trials = total;
  if (out.trials > 0) {
    const double n = static_cast<double>(out.trials);
    out.mean = equity_sum / n;
    double var = equity_sum_sq / n - out.mean * out.mean;
    if (var < 0.0) var = 0.0; // numerical guard
    out.std_error = std::sqrt(var / n);
  }
  return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API: calculate_range_equity (weighted Range x Range, reproducible)
// ---------------------------------------------------------------------------
RangeEquityResult calculate_range_equity(const Range &hero_range,
                                         const Range &villain_range,
                                         std::uint64_t board_mask,
                                         const SimulationOptions &options,
                                         RangeEquityMode mode) {
  // Game agreement is checked first, ahead of every other validation and
  // ahead of the empty-range shortcut: a Hold'em range paired with a PLO one
  // is a caller mistake whatever else is wrong with the call, and silently
  // answering it (e.g. with zeros, because the 2-card combos all fail the
  // 4-card filter) would be the worst possible outcome.
  if (hero_range.game() != villain_range.game()) {
    // Message text is duplicated in binding/core_simulation.cpp:require_same_game; keep both in sync (grep binding/ before editing).
    throw std::invalid_argument(
        std::string("calculate_range_equity: hero range is ") +
        game_name(hero_range.game()) + " but villain range is " +
        game_name(villain_range.game()) +
        "; both ranges must be the same game");
  }
  const GameType game = hero_range.game();

  if (options.iterations < 0) {
    throw std::runtime_error(
        "calculate_range_equity: iterations must be >= 0");
  }
  board_mask &= FULL_DECK_MASK;
  validate_board_count(board_mask, "calculate_range_equity");

  const int hole_cards = cards_per_hand(game);
  auto hero_combos =
      filter_combos(hero_range.combos(), board_mask, hole_cards);
  auto vill_combos =
      filter_combos(villain_range.combos(), board_mask, hole_cards);

  RangeEquityResult out;
  const bool exact_requested = (options.iterations == 0);

  if (hero_combos.empty() || vill_combos.empty()) {
    // Nothing to evaluate; exact flag tracks intent.
    out.exact = exact_requested;
    return out;
  }

  // AggregateOnly + Monte Carlo takes the sampled-pair fast path. Exact
  // (iterations == 0) keeps using the enumeration engine below and strips the
  // breakdowns afterwards, so exact results stay bit-identical across modes.
  //
  // No auto-fallback to exact here: even when the board is fully specified,
  // the pair dimension is still sampled, so the run is genuinely Monte Carlo
  // and `exact` stays false with trials == options.iterations.
  //
  // This branch owns its RNG seeding (one master seed fanned out into
  // per-chunk substreams), so it returns before the shared FastRng below is
  // built — constructing it here would burn two random_device draws for
  // nothing on non-deterministic runs.
  if (mode == RangeEquityMode::AggregateOnly && !exact_requested) {
    const std::uint64_t master_seed = resolve_master_seed(options);
    // One dispatch, outside the estimator: each instantiation compiles its
    // own showdown into the trial loop.
    const AggregateSample sampled =
        (game == GameType::Plo)
            ? sample_range_equity<true>(
                  hero_combos, vill_combos, board_mask, options.iterations,
                  master_seed, options.threads)
            : sample_range_equity<false>(
                  hero_combos, vill_combos, board_mask, options.iterations,
                  master_seed, options.threads);
    out.exact = false;
    out.trials = sampled.trials;
    out.hero_aggregate_equity = sampled.mean;
    out.villain_aggregate_equity = 1.0 - sampled.mean;
    out.aggregate_std_error = sampled.std_error;
    return out;
  }

  // Same seeding policy as resolve_master_seed (mc_chunking.h): a
  // deterministic run honours options.seed verbatim (seed 0 included, via
  // FastRng::seed()'s no-zero-guard); a non-deterministic run composes all 64
  // bits from two random_device draws. The deleted file-local
  // make_sampling_rng additionally forced the random_device branch whenever
  // exact_requested was true, even for a deterministic seed -- but that
  // resolved a seed this exact path never consumes (enumerate_or_sample_boards
  // dispatches straight to enumerate_boards, which takes no rng, whenever
  // iterations == 0). So the only observable difference is that an exact +
  // deterministic call here no longer burns two random_device draws for
  // nothing, which is the same contract-free consumption documented above at
  // calculate_equity's exact-request short-circuit.
  FastRng rng;
  rng.seed(resolve_master_seed(options));

  auto calc = enumerate_range_equity(hero_combos, vill_combos,
                                     board_mask, options.iterations,
                                     rng, options.threads, game);
  out.hero = std::move(calc.hero);
  out.villain = std::move(calc.villain);
  out.trials = calc.num_boards;
  // exact reflects what actually ran (auto-fallback when iterations >= board
  // space promotes the MC request to exact enumeration).
  out.exact = calc.was_exact;
  if (calc.pair_weight_sum > 0.0) {
    out.hero_aggregate_equity =
        calc.pair_equity_weighted_sum / calc.pair_weight_sum;
    out.villain_aggregate_equity = 1.0 - out.hero_aggregate_equity;
  }
  if (mode == RangeEquityMode::AggregateOnly) {
    // Exact enumeration only (the MC case returned above).
    out.hero.clear();
    out.villain.clear();
    // aggregate_std_error stays 0.0: exact enumeration has no sampling error.
  }
  return out;
}

} // namespace xiapl
