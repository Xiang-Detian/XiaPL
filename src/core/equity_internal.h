#pragma once

// Internal-only equity simulators, promoted from file-static linkage in
// src/core/equity_hands.cpp so tests/test_equity_agreement.cpp can call both
// on the same 2-player Hold'em spot and pin their agreement.
//
// Why this exists: equity_hands.cpp's dispatcher (see the
// `!use_plo && num_players == 2` condition inside compute_player_equities)
// routes every 2-player Hold'em call through calculate_equity to the
// HU-specialized simulate_heads_up; simulate_multiway (the
// generic multiway simulator) is therefore unreachable for that shape
// through any public API. The maintainer kept the HU specialization for its
// measured speed (1.13x-4.99x across workload shapes, largest on small exact
// boards) rather than unifying it onto the generic path, which left nothing
// pinning the two implementations against each other. This header exists
// purely to make that agreement testable; production code has no reason to
// call these directly (calculate_equity / compute_player_equities remain
// the only callers there).
//
// This is a linkage-only promotion (static -> xiapl::internal::). Both
// function bodies, semantics and threading contracts are exactly as defined
// in equity_hands.cpp -- see the comments above each definition there for the
// authoritative documentation.
//
// Like the rest of the internal:: surface (eval_internal.h), these skip the
// public-API validation calculate_equity performs on its callers' behalf:
// they trust `players_hole_masks` / `hero_mask` / `vill_mask` are already
// validated (see internal::build_used_mask in board_sample.h).

#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace xiapl::internal {

// Generic multiway equity simulator (Hold'em N > 2, or PLO any N; also the
// only path 2-player PLO reaches). Returns {winrates, equities, std_errors,
// chop_rate}, each per-player vector indexed like `players_hole_masks`.
//
// Monte Carlo runs in seed-derived 65536-trial chunks across `threads`
// workers, reduced in chunk-index order, so the result is bit-identical at
// every worker count for a given `master_seed` (see mc_chunking.h). Exact
// enumeration (iterations == 0, or the auto-fallback once iterations exceeds
// the board space) ignores `master_seed` / `threads` and runs serially.
// `out_trials`, if non-null, receives the number of board completions
// actually evaluated; `out_was_exact`, if non-null, reports which mode ran.
std::tuple<std::vector<double>, std::vector<double>, std::vector<double>,
          double>
simulate_multiway(
    const std::vector<std::uint64_t> &players_hole_masks, int iterations,
    std::uint64_t fixed_board_mask, bool use_plo, std::uint64_t master_seed,
    int threads, std::uint64_t *out_trials, bool *out_was_exact = nullptr);

// HU (2-player Hold'em)-specialized equity simulator. `deck_indices` must
// correspond to the remaining deck AFTER removing (hero_mask | vill_mask |
// fixed_board_mask) -- callers build it with internal::deck_indices_of
// (board_sample.h).
// Returns {hero_winrate, chop_rate}; the caller derives hero/villain equity
// and villain winrate from those two numbers (see compute_player_equities).
//
// Same threading contract as simulate_multiway above. `out_trials`
// / `out_was_exact` as above.
std::pair<double, double>
simulate_heads_up(std::uint64_t hero_mask, std::uint64_t vill_mask,
                  int iterations, std::uint64_t fixed_board_mask,
                  const std::vector<int> &deck_indices,
                  std::uint64_t master_seed, int threads,
                  std::uint64_t *out_trials = nullptr,
                  bool *out_was_exact = nullptr);

} // namespace xiapl::internal
