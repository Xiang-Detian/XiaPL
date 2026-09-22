#pragma once

#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include <xiapl/detail/fast_rng.h>
#include <xiapl/utils.h>

#include "eval_internal.h"  // kMaxSeats / kMaxMaskPlayers (player-count caps)

namespace xiapl {
namespace internal {

// ---- Used-mask validation ----

// Validate player hole masks and build the combined used_mask.
// If expected_cards_per_hand > 0, checks each mask has exactly that many bits.
// Otherwise, only checks that the mask is non-empty.
inline std::uint64_t build_used_mask(
    const std::vector<std::uint64_t> &players_hole_masks,
    std::uint64_t board_mask, const char *caller_name,
    int expected_cards_per_hand = 0) {
  std::uint64_t used_mask = board_mask & FULL_DECK_MASK;
  for (int i = 0; i < static_cast<int>(players_hole_masks.size()); ++i) {
    std::uint64_t m = players_hole_masks[i] & FULL_DECK_MASK;
    if (expected_cards_per_hand > 0) {
      if (popcount64(m) != expected_cards_per_hand) {
        throw std::runtime_error(
            std::string(caller_name) + ": invalid hole mask card count");
      }
    } else {
      if (m == 0) {
        throw std::runtime_error(
            std::string(caller_name) + ": player hole mask is empty");
      }
    }
    if (used_mask & m) {
      throw std::runtime_error(
          std::string(caller_name) +
          ": overlapping cards between players or board");
    }
    used_mask |= m;
  }
  return used_mask;
}

// Board-mask contract shared by every equity entry point (calculate_equity,
// calculate_range_equity): 0 (preflop) or 3-5 (flop/turn/river) cards. Only
// {0, 3, 4, 5} are physically possible board sizes; 1 and 2 used to run
// silently with a meaningless num_to_draw. `caller_name` names the public
// entry point so the message teaches the caller which call rejected it.
inline void validate_board_count(std::uint64_t board_mask,
                                 const char *caller_name) {
  const int board_count = popcount64(board_mask & FULL_DECK_MASK);
  if (board_count != 0 && (board_count < 3 || board_count > 5)) {
    throw std::runtime_error(
        std::string(caller_name) +
        ": board must have 0 (preflop) or 3-5 (flop/turn/river) cards");
  }
}

// Full up-front input validation for calculate_equity: board card count
// (validate_board_count above), the player-count cap for the selected game
// (kMaxSeats for Hold'em / kMaxMaskPlayers for PLO -- the same bounds
// judge_holdem_mask / judge_plo_mask enforce as a defensive backstop, see
// eval_internal.h), and each hole mask's card count + pairwise overlap
// (build_used_mask above). Running all of it here, before either simulator
// dispatches a single Monte Carlo trial, is what makes every rejection a
// synchronous throw on the caller's own thread with a calculate_equity-named
// message -- never a thread-pool throw with an internal symbol name.
//
// Empty `players_hole_masks` is a no-op that returns board_mask unchanged:
// calculate_equity's long-standing contract is that zero players yields an
// empty EquityResult without validating board_mask at all, and this function
// preserves that exactly rather than introducing a new rejection.
//
// Returns the combined used_mask (board | every hole mask) so callers that
// need it downstream (the 2-player Hold'em HU dispatch builds its remaining
// deck from it) do not have to recompute it.
inline std::uint64_t validate_equity_input(
    const std::vector<std::uint64_t> &players_hole_masks,
    std::uint64_t board_mask, bool use_plo, const char *caller_name) {
  if (players_hole_masks.empty()) {
    return board_mask & FULL_DECK_MASK;
  }
  validate_board_count(board_mask, caller_name);

  const int num_players = static_cast<int>(players_hole_masks.size());
  const int max_players = use_plo ? kMaxMaskPlayers : kMaxSeats;
  if (num_players > max_players) {
    throw std::invalid_argument(
        std::string(caller_name) + ": at most " +
        std::to_string(max_players) + " players supported for " +
        (use_plo ? "PLO" : "Hold'em") + " (got " +
        std::to_string(num_players) + ")");
  }

  const int expected_hole = use_plo ? 4 : 2;
  return build_used_mask(players_hole_masks, board_mask, caller_name,
                         expected_hole);
}

// ---- Deck index / board-space arithmetic ----

inline std::vector<int> deck_indices_of(std::uint64_t remain_mask) {
  std::vector<int> deck_indices;
  deck_indices.reserve(52);
  std::uint64_t m = remain_mask;
  while (m) {
    int id = ctz64(m);
    deck_indices.push_back(id);
    m &= (m - 1);
  }
  return deck_indices;
}

// Compute C(n, k). Saturates to UINT64_MAX on overflow; never used here with
// n > 52 / k > 5 (max C(52, 5) = 2,598,960 fits in 32 bits).
inline std::uint64_t binomial_coefficient(int n, int k) noexcept {
  if (k < 0 || k > n) return 0;
  if (k == 0 || k == n) return 1;
  if (k > n - k) k = n - k;
  std::uint64_t result = 1;
  for (int i = 0; i < k; ++i) {
    // result * (n - i) cannot overflow for n <= 52, k <= 5
    result = result * static_cast<std::uint64_t>(n - i) /
             static_cast<std::uint64_t>(i + 1);
  }
  return result;
}

// Number of distinct board completions. num_to_draw == 0 (the board is
// already complete) counts as the single completion enumerate_boards emits.
inline std::uint64_t board_space_count(std::size_t deck_size, int num_to_draw) {
  if (num_to_draw <= 0) return 1;
  return binomial_coefficient(static_cast<int>(deck_size), num_to_draw);
}

// ---- Board enumeration / sampling ----

// Exact-vs-Monte-Carlo decision, shared by every board-driven path so they
// cannot drift apart. Exact when the caller asked for it (iterations == 0) or
// when sampling with replacement would run at least as many trials as the
// exhaustive enumeration has boards: past that point enumeration is both
// faster and exact, so we dispatch to it automatically (auto-fallback). The
// caller reports which one ran via out_was_exact / EquityResult::exact, so
// `exact` reflects what actually happened, not what was requested.
inline bool board_run_is_exact(int iterations, std::uint64_t space) {
  return iterations == 0 || static_cast<std::uint64_t>(iterations) >= space;
}

// Full enumeration over all combinations of num_to_draw cards, in
// combination order. Consumes no randomness and never threads.
//
// Precondition: num_to_draw >= 0 (every caller derives it from a board with
// 0 or 3-5 cards and rejects a negative value first). The `<= 0` branch
// exists for the "board already complete" case, num_to_draw == 0; a negative
// value reaching here would be a caller bug, not a supported input.
template <typename TrialFn>
void enumerate_boards(const std::vector<int> &deck_indices,
                      int num_to_draw, std::uint64_t fixed_board_mask,
                      TrialFn &&trial_fn) {
  if (num_to_draw <= 0) {
    trial_fn(fixed_board_mask);
    return;
  }
  const int deck_size = static_cast<int>(deck_indices.size());
  std::vector<int> comb(num_to_draw);
  std::iota(comb.begin(), comb.end(), 0);
  do {
    std::uint64_t board_mask = fixed_board_mask;
    for (int idx : comb) {
      board_mask |= (1ULL << deck_indices[idx]);
    }
    trial_fn(board_mask);
  } while (next_combination(comb, deck_size));
}

// Monte Carlo sampling of `iterations` boards via partial Fisher-Yates on a
// private copy of the deck.
//
// This is the only board sampler in the file: the chunked paths call it once
// per chunk (with that chunk's RNG and its own deck copy), so a chunk's board
// distribution is identical to a serial run's by construction.
//
// std::sample uses selection-sampling that consumes ~deck.size() RNG calls
// per trial; partial Fisher–Yates needs only num_to_draw RNG calls + swaps,
// giving ~10x speedup for typical (num_to_draw=1..5, deck=47..50).
//
// The draw index comes from FastRng::bounded (Lemire, debiased), not
// std::uniform_int_distribution: the distribution object has to be rebuilt
// for every draw because the range [k, deck_size-1] shrinks with k, and that
// construction dominated the per-trial cost (measured 63.5 ns of a 178 ns
// trial before this change).
//
// `shuffled` is intentionally NOT reset between iterations: it always holds a
// permutation of the same card multiset, so restarting the partial
// Fisher–Yates from k = 0 still yields a uniformly random num_to_draw-subset.
// The permutation therefore carries across the trials of one call — which is
// exactly why each chunk gets its own copy, so a chunk's output cannot depend
// on which chunks ran before it.
template <typename TrialFn>
void sample_boards(const std::vector<int> &deck_indices, int num_to_draw,
                   std::uint64_t iterations,
                   std::uint64_t fixed_board_mask, FastRng &rng,
                   TrialFn &&trial_fn) {
  std::vector<int> shuffled(deck_indices); // mutable copy
  int *const deck = shuffled.data();
  const std::uint32_t shuffled_deck_size =
      static_cast<std::uint32_t>(shuffled.size());
  for (std::uint64_t iter = 0; iter < iterations; ++iter) {
    std::uint64_t board_mask = fixed_board_mask;
    for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(num_to_draw);
         ++k) {
      // Uniform j in [k, shuffled_deck_size - 1].
      const std::uint32_t j = k + rng.bounded(shuffled_deck_size - k);
      const int picked = deck[j];
      deck[j] = deck[k];
      deck[k] = picked;
      board_mask |= (1ULL << picked);
    }
    trial_fn(board_mask);
  }
}

// Enumerate all board completions or sample via Monte Carlo, calling trial_fn
// for each completed board mask. Serial on both branches; used by the paths
// that consume a caller-owned RNG (calculate_range_equity's PerCombo board
// set). The chunked paths call board_run_is_exact + enumerate_boards /
// sample_boards directly so they can partition the MC branch.
template <typename TrialFn>
void enumerate_or_sample_boards(
    const std::vector<int> &deck_indices, int num_to_draw, int iterations,
    std::uint64_t fixed_board_mask, FastRng &rng, TrialFn &&trial_fn,
    bool *out_was_exact = nullptr) {
  const bool use_exact = board_run_is_exact(
      iterations, board_space_count(deck_indices.size(), num_to_draw));
  if (out_was_exact) *out_was_exact = use_exact;

  if (use_exact) {
    enumerate_boards(deck_indices, num_to_draw, fixed_board_mask, trial_fn);
  } else {
    sample_boards(deck_indices, num_to_draw,
                  static_cast<std::uint64_t>(iterations), fixed_board_mask, rng,
                  trial_fn);
  }
}

} // namespace internal
} // namespace xiapl
