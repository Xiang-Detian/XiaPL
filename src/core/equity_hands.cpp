#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <tuple>
#include <vector>

#include <xiapl/detail/fast_rng.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include "board_sample.h"
#include "equity_internal.h"
#include "eval_internal.h"
#include "mc_chunking.h"

namespace xiapl {

// Chunked Monte Carlo primitives (src/core/mc_chunking.h): the chunk sizing /
// substream seeding / index-ordered reduction that the two simulators below
// used to spell out themselves now live inside run_mc_chunks.
using internal::resolve_master_seed;
using internal::run_mc_chunks;

// Board sampling / enumeration primitives (src/core/board_sample.h).
using internal::board_run_is_exact;
using internal::board_space_count;
using internal::build_used_mask;
using internal::deck_indices_of;
using internal::enumerate_boards;
using internal::sample_boards;

// Unified equity-input validation (src/core/board_sample.h): one call per
// public entry point -- calculate_equity below, via compute_player_equities.
// calculate_range_equity (equity_range.cpp) validates directly there.
using internal::validate_equity_input;

// ---- Fixed-hand equity simulators (internal) ----

// Mask-based simulator with per-player equity + standard error (Monte Carlo
// only). Returns (pure winrates per player, equity per player, std_error per
// player, chop_rate).
// - winrates[i]: probability player i wins outright alone (does NOT include
// chops).
// - equity[i]: expected share of the pot for player i (includes chopped
// shares).
// - std_error[i]: standard error of equity[i] from Monte Carlo sampling; 0.0
// for full enumeration.
// Internal helper. Returns the per-player stats plus chop_rate. Also reports
// the number of board completions actually evaluated via `out_trials` so
// callers can build EquityResult::trials.
//
// Monte Carlo runs in seed-derived chunks across `threads` workers (see the
// chunked-substream block above); `master_seed` is the only randomness input,
// so the result is bit-identical at every worker count. Exact enumeration
// ignores both parameters and runs serially.
std::tuple<std::vector<double>, std::vector<double>, std::vector<double>,
          double>
internal::simulate_multiway(
    const std::vector<std::uint64_t> &players_hole_masks, int iterations,
    std::uint64_t fixed_board_mask, bool use_plo, std::uint64_t master_seed,
    int threads, std::uint64_t *out_trials, bool *out_was_exact) {
  int num_players = static_cast<int>(players_hole_masks.size());
  if (num_players == 0) {
    return {std::vector<double>{}, std::vector<double>{}, std::vector<double>{},
            0.0};
  }

  // --- Validate masks and build used_mask ---
  // Enforce expected hole-card count up front so the per-trial lambda never
  // throws from inside the MC loop.
  const int expected_hole = use_plo ? 4 : 2;
  std::uint64_t used_mask = build_used_mask(
      players_hole_masks, fixed_board_mask, "simulate_multiway",
      expected_hole);

  // Pre-strip player hole masks so the MC loop can call the unchecked
  // internal::judge_*_mask path directly, skipping per-trial popcount /
  // overlap re-validation inside the public wrapper.
  std::vector<std::uint64_t> stripped_holes(num_players);
  for (int i = 0; i < num_players; ++i) {
    stripped_holes[i] = players_hole_masks[i] & FULL_DECK_MASK;
  }

  int board_count = popcount64(fixed_board_mask & FULL_DECK_MASK);
  int num_to_draw = 5 - board_count;
  if (num_to_draw < 0) {
    return {std::vector<double>(num_players, 0.0),
            std::vector<double>(num_players, 0.0),
            std::vector<double>(num_players, 0.0), 0.0};
  }

  std::uint64_t remain_mask = FULL_DECK_MASK & ~used_mask;
  std::vector<int> deck_indices = deck_indices_of(remain_mask);

  // Counters (uint64_t to avoid signed-overflow UB at iterations >= 2^31).
  // One instance per chunk on the MC path, one for the whole run on the exact
  // path; the totals below are the chunk-index-ordered reduction of those.
  struct Counters {
    std::vector<std::uint64_t> win_counts;
    std::vector<double> equity_sum;
    std::vector<double> equity_sum_sq;
    std::uint64_t chop_count = 0;
    std::uint64_t trials = 0;
    explicit Counters(int n = 0)
        : win_counts(static_cast<std::size_t>(n), 0),
          equity_sum(static_cast<std::size_t>(n), 0.0),
          equity_sum_sq(static_cast<std::size_t>(n), 0.0) {}
  };

  // Winners come back as a bitmask rather than a std::vector<int>: the vector
  // form costs a malloc/free per trial (it lives in another TU, so nothing
  // elides it), which dominated this loop for cheap Hold'em showdowns.
  auto run_trial = [&](Counters &c, std::uint64_t board_mask) {
    const std::uint32_t winners =
        use_plo
            ? internal::judge_plo_mask(stripped_holes, board_mask)
            : internal::judge_holdem_mask(stripped_holes, board_mask);
    if (winners == 0)
      return;

    if ((winners & (winners - 1)) == 0) {
      // Exactly one bit set: outright win, no share arithmetic.
      const std::size_t w = static_cast<std::size_t>(ctz64(winners));
      c.win_counts[w] += 1;
      c.equity_sum[w] += 1.0;
      c.equity_sum_sq[w] += 1.0;
    } else {
      const double share = 1.0 / static_cast<double>(popcount64(winners));
      const double share_sq = share * share;
      for (std::uint32_t m = winners; m; m &= (m - 1)) {
        const std::size_t w = static_cast<std::size_t>(ctz64(m));
        c.equity_sum[w] += share;
        c.equity_sum_sq[w] += share_sq;
      }
      c.chop_count += 1;
    }

    c.trials += 1;
  };

  const bool was_exact = board_run_is_exact(
      iterations, board_space_count(deck_indices.size(), num_to_draw));
  if (out_was_exact) *out_was_exact = was_exact;

  Counters totals(num_players);
  if (was_exact) {
    enumerate_boards(deck_indices, num_to_draw, fixed_board_mask,
                     [&](std::uint64_t board_mask) {
                       run_trial(totals, board_mask);
                     });
  } else {
    // Chunk k depends only on (master_seed, k): the driver's RNG substream,
    // its own deck copy inside sample_boards, and its own accumulator. The
    // driver also owns the chunk-index-ordered reduction, which is what makes
    // the floating-point equity sums independent of the worker count.
    run_mc_chunks<Counters>(
        static_cast<std::uint64_t>(iterations), threads, master_seed,
        [&](std::uint64_t chunk_trials, FastRng &chunk_rng) {
          Counters local(num_players);
          sample_boards(deck_indices, num_to_draw, chunk_trials,
                        fixed_board_mask, chunk_rng,
                        [&](std::uint64_t board_mask) {
                          run_trial(local, board_mask);
                        });
          return local;
        },
        [&](const Counters &c) {
          // A slot still at its default size means its chunk never ran — a
          // driver bug, not a data condition. Fail loudly rather than fold in
          // zeros.
          if (c.win_counts.size() != static_cast<std::size_t>(num_players)) {
            throw std::runtime_error(
                "simulate_multiway: chunk result was never stored");
          }
          for (std::size_t i = 0; i < static_cast<std::size_t>(num_players);
               ++i) {
            totals.win_counts[i] += c.win_counts[i];
            totals.equity_sum[i] += c.equity_sum[i];
            totals.equity_sum_sq[i] += c.equity_sum_sq[i];
          }
          totals.chop_count += c.chop_count;
          totals.trials += c.trials;
        });
  }

  const std::vector<std::uint64_t> &win_counts = totals.win_counts;
  const std::vector<double> &equity_sum = totals.equity_sum;
  const std::vector<double> &equity_sum_sq = totals.equity_sum_sq;
  const std::uint64_t total_iterations = totals.trials;
  const std::uint64_t chop_count = totals.chop_count;

  std::vector<double> winrates(num_players, 0.0);
  std::vector<double> equities(num_players, 0.0);
  std::vector<double> std_errors(num_players, 0.0);

  if (total_iterations > 0) {
    for (int i = 0; i < num_players; ++i) {
      winrates[i] = static_cast<double>(win_counts[i]) /
                    static_cast<double>(total_iterations);
      equities[i] = equity_sum[i] / static_cast<double>(total_iterations);

      // Standard error of mean equity estimate (Monte Carlo only); 0.0 when the
      // run was exact (either iterations==0 OR iterations >= board space).
      if (!was_exact) {
        double mean = equities[i];
        double mean_sq =
            equity_sum_sq[i] / static_cast<double>(total_iterations);
        double var = mean_sq - mean * mean;
        if (var < 0.0)
          var = 0.0; // numerical guard
        std_errors[i] = std::sqrt(var / static_cast<double>(total_iterations));
      }
    }
  }

  double chop_rate = (total_iterations > 0)
                         ? static_cast<double>(chop_count) /
                               static_cast<double>(total_iterations)
                         : 0.0;

  if (out_trials) {
    *out_trials = total_iterations;
  }

  return {winrates, equities, std_errors, chop_rate};
}

// Internal core function for HU simulation.
// `deck_indices` must correspond to the remaining deck AFTER removing
// (hero|vill|fixed_board).
//
// Preconditions (validated once by the caller, see
// internal::validate_equity_input in board_sample.h, called from
// compute_player_equities below): iterations >= 0; fixed_board_mask has 0
// or 3-5 bits; hero_mask / vill_mask are disjoint 2-bit masks not overlapping
// the board; deck_indices has enough cards for num_to_draw. This function no
// longer re-checks any of that itself -- those checks used to live here, but
// compute_player_equities is this function's only production caller and it
// always validates first, so they could only ever fire from inside this
// function's own per-trial Monte Carlo loop on a worker thread, contradicting
// the "validate up front" contract. Direct callers outside the public API
// (e.g. tests/test_equity_agreement.cpp, via equity_internal.h) are documented
// as trusting pre-validated input, same as internal::judge_holdem_mask /
// internal::judge_plo_mask.
//
// Same threading contract as simulate_multiway: Monte Carlo runs
// in seed-derived chunks across `threads` workers and is bit-identical at
// every worker count; exact enumeration ignores `master_seed` / `threads`.
std::pair<double, double>
internal::simulate_heads_up(std::uint64_t hero_mask,
                            std::uint64_t vill_mask, int iterations,
                            std::uint64_t fixed_board_mask,
                            const std::vector<int> &deck_indices,
                            std::uint64_t master_seed, int threads,
                            std::uint64_t *out_trials,
                            bool *out_was_exact) {
  const int board_count = popcount64(fixed_board_mask);
  const int num_to_draw = 5 - board_count;

  // One instance per chunk on the MC path, one for the whole run on the exact
  // path. All three counters are integers, so the reduction below is exact
  // regardless of order — the fixed chunk order is kept anyway, so the
  // property survives any future non-integer counter.
  struct Counters {
    std::uint64_t hero_wins = 0;
    std::uint64_t chops = 0;
    std::uint64_t trials = 0;
  };

  // Compare packed scores directly: the score is order-isomorphic to
  // HandValue (see eval_core.h), so this is exactly the HandValue ordering
  // without the per-trial decode the public evaluate_hand path pays.
  auto run_trial = [&](Counters &c, std::uint64_t board_mask) {
    const std::uint32_t hero_score = internal::eval_score7(board_mask | hero_mask);
    const std::uint32_t vill_score = internal::eval_score7(board_mask | vill_mask);

    if (hero_score > vill_score) {
      c.hero_wins++;
    } else if (hero_score == vill_score) {
      c.chops++;
    }
    c.trials++;
  };

  const bool was_exact = board_run_is_exact(
      iterations, board_space_count(deck_indices.size(), num_to_draw));
  if (out_was_exact) *out_was_exact = was_exact;

  Counters totals;
  if (was_exact) {
    enumerate_boards(deck_indices, num_to_draw, fixed_board_mask,
                     [&](std::uint64_t board_mask) {
                       run_trial(totals, board_mask);
                     });
  } else {
    run_mc_chunks<Counters>(
        static_cast<std::uint64_t>(iterations), threads, master_seed,
        [&](std::uint64_t chunk_trials, FastRng &chunk_rng) {
          Counters local;
          sample_boards(deck_indices, num_to_draw, chunk_trials,
                        fixed_board_mask, chunk_rng,
                        [&](std::uint64_t board_mask) {
                          run_trial(local, board_mask);
                        });
          return local;
        },
        [&](const Counters &c) {
          totals.hero_wins += c.hero_wins;
          totals.chops += c.chops;
          totals.trials += c.trials;
        });
  }

  const std::uint64_t hero_wins = totals.hero_wins;
  const std::uint64_t chops = totals.chops;
  const std::uint64_t total_iterations = totals.trials;

  double hero_winrate = 0.0;
  double chop_rate = 0.0;
  if (total_iterations > 0) {
    hero_winrate =
        static_cast<double>(hero_wins) / static_cast<double>(total_iterations);
    chop_rate =
        static_cast<double>(chops) / static_cast<double>(total_iterations);
  }

  if (out_trials) {
    *out_trials = total_iterations;
  }

  return {hero_winrate, chop_rate};
}

// ---- Per-player equity dispatch ----

// Per-player core of calculate_equity: dispatches to the HU or the multiway
// simulator and reshapes their output. `master_seed` / `threads` are passed
// straight through to whichever one runs (see their threading contracts).
// `out_trials` (if non-null) receives the number of board completions evaluated.
static std::tuple<std::vector<std::tuple<std::uint64_t,
                                         std::vector<std::uint64_t>, double,
                                         double, double>>,
                  double>
compute_player_equities(
    const std::vector<std::uint64_t> &players_hole_masks, int iterations,
    std::uint64_t fixed_board_mask, bool use_plo, std::uint64_t master_seed,
    int threads, std::uint64_t *out_trials, bool *out_was_exact = nullptr) {
  int num_players = static_cast<int>(players_hole_masks.size());
  std::vector<std::tuple<std::uint64_t, std::vector<std::uint64_t>, double,
                         double, double>>
      results;

  if (num_players == 0) {
    if (out_trials) *out_trials = 0;
    return {results, 0.0};
  }

  // Single up-front validation pass for calculate_equity's whole input
  // contract: board card count, the player-count cap for `use_plo`'s game,
  // and every hole mask's card count + pairwise overlap. This runs entirely
  // on the caller's thread, before dispatch to either simulator below, so a
  // rejection here can never come from inside a Monte Carlo worker thread.
  // `used_mask` (board | every hole mask) is reused by the HU branch instead
  // of being recomputed.
  std::uint64_t used_mask = validate_equity_input(
      players_hole_masks, fixed_board_mask, use_plo, "calculate_equity");
  std::uint64_t board_mask = fixed_board_mask & FULL_DECK_MASK;

  results.reserve(num_players);

  // HU path uses the HU core (Hold'em only)
  if (!use_plo && num_players == 2) {
    std::uint64_t hero_mask = players_hole_masks[0] & FULL_DECK_MASK;
    std::uint64_t vill_mask = players_hole_masks[1] & FULL_DECK_MASK;

    std::vector<int> hu_deck =
        deck_indices_of(FULL_DECK_MASK & ~used_mask);

    std::uint64_t hu_trials = 0;
    bool hu_was_exact = false;
    auto [hero_winrate, chop_rate] = internal::simulate_heads_up(
        hero_mask, vill_mask, iterations, board_mask, hu_deck, master_seed,
        threads, &hu_trials, &hu_was_exact);
    if (out_was_exact) *out_was_exact = hu_was_exact;

    // HU equity = win + chop/2
    double hero_equity = hero_winrate + chop_rate / 2.0;
    double vill_equity = 1.0 - hero_equity;

    // villain pure winrate = 1 - hero - chop
    double vill_winrate = 1.0 - hero_winrate - chop_rate;
    if (vill_winrate < 0.0) {
      vill_winrate = std::max(0.0, vill_winrate);
    }

    // Monte Carlo standard error of equity estimate (0.0 for full enumeration,
    // including the auto-fallback case where iterations >= board space).
    double hero_std_error = 0.0;
    double vill_std_error = 0.0;
    if (!hu_was_exact && hu_trials > 0) {
      double mean = hero_equity;
      double mean_sq = hero_winrate + 0.25 * chop_rate;
      double var = mean_sq - mean * mean;
      if (var < 0.0)
        var = 0.0;
      hero_std_error = std::sqrt(var / static_cast<double>(hu_trials));
      vill_std_error = hero_std_error;
    }

    {
      std::vector<std::uint64_t> villains;
      villains.push_back(vill_mask);
      results.emplace_back(hero_mask, std::move(villains), hero_winrate,
                           hero_equity, hero_std_error);
    }
    {
      std::vector<std::uint64_t> villains;
      villains.push_back(hero_mask);
      results.emplace_back(vill_mask, std::move(villains), vill_winrate,
                           vill_equity, vill_std_error);
    }

    if (out_trials) *out_trials = hu_trials;
    return {results, chop_rate};
  }

  // Multiway / PLO / non-HU path uses the stats simulator
  std::uint64_t stats_trials = 0;
  bool stats_was_exact = false;
  auto [winrates, equities, std_errors, chop_rate] =
      internal::simulate_multiway(
          players_hole_masks, iterations, board_mask, use_plo, master_seed,
          threads, &stats_trials, &stats_was_exact);
  if (out_was_exact) *out_was_exact = stats_was_exact;

  for (int i = 0; i < num_players; ++i) {
    std::vector<std::uint64_t> villains;
    villains.reserve(num_players - 1);
    for (int j = 0; j < num_players; ++j) {
      if (i == j)
        continue;
      villains.push_back(players_hole_masks[j] & FULL_DECK_MASK);
    }

    double winrate_i =
        (i < static_cast<int>(winrates.size())) ? winrates[i] : 0.0;
    double equity_i =
        (i < static_cast<int>(equities.size())) ? equities[i] : 0.0;
    double se_i =
        (i < static_cast<int>(std_errors.size())) ? std_errors[i] : 0.0;

    results.emplace_back(players_hole_masks[i] & FULL_DECK_MASK,
                         std::move(villains), winrate_i, equity_i, se_i);
  }

  if (out_trials) *out_trials = stats_trials;
  return {results, chop_rate};
}

// ---------------------------------------------------------------------------
// Public API: SimulationOptions / calculate_equity
// ---------------------------------------------------------------------------
EquityResult calculate_equity(const std::vector<std::uint64_t> &hole_masks,
                              std::uint64_t board_mask,
                              const SimulationOptions &options,
                              GameType game) {
  if (options.iterations < 0) {
    throw std::runtime_error("calculate_equity: iterations must be >= 0");
  }

  // The master seed is only consumed on the Monte Carlo paths, so an outright
  // exact request skips it (and the two random_device draws a
  // non-deterministic run would otherwise pay for nothing). A run that
  // auto-falls back to exact resolves a seed it then never uses, which is
  // harmless: random_device consumption is not part of any contract.
  const bool exact_requested = (options.iterations == 0);
  const std::uint64_t master_seed =
      exact_requested ? 0 : resolve_master_seed(options);

  std::uint64_t trials = 0;
  bool was_exact = false;
  auto [per_player, chop_rate] = compute_player_equities(
      hole_masks, options.iterations, board_mask, game == GameType::Plo,
      master_seed, options.threads, &trials, &was_exact);

  EquityResult out;
  // result.exact reflects what actually ran, not what was requested: if the
  // caller passes iterations >= board space, board_run_is_exact promotes the
  // run to exact enumeration and was_exact is true.
  out.exact = was_exact;
  out.chop_rate = chop_rate;
  out.trials = trials;
  out.players.reserve(per_player.size());
  for (const auto &row : per_player) {
    PlayerEquity pe;
    pe.winrate = std::get<2>(row);
    pe.equity = std::get<3>(row);
    pe.std_error = std::get<4>(row);
    out.players.push_back(pe);
  }
  return out;
}

} // namespace xiapl
