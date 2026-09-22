#pragma once

// Internal-only fast paths for src/core/equity_hands.cpp,
// src/core/equity_range.cpp and other in-tree hot loops.
// These bypass the public-API validation in eval.cpp
// (validate_player_masks, popcount + overlap re-check) so Monte-Carlo inner
// loops do not re-validate per trial. Callers MUST validate masks once up
// front.
//
// Everything here is a thin wrapper over the single evaluator in
// eval_core.h (mask -> packed uint32 score). Prefer calling eval_score7()
// directly and comparing the raw scores when the caller only needs an
// ordering: the packed score is order-isomorphic to HandValue, so
// decoding is pure overhead in that case.

#include <cstdint>
#include <vector>

#include <xiapl/hand_value.h>
#include <xiapl/utils.h>

#include "eval_core.h"

namespace xiapl::internal {

// ---- Allocation-free PLO scoring -----------------------------------------
//
// A PLO hand is the best five cards over EXACTLY two of the four hole cards
// and EXACTLY three of the board cards: C(4,2) x C(5,3) = 6 x 10 = 60
// candidates on a complete board. Everything below works on stack arrays
// only, so a Monte Carlo showdown loop can call it per trial with no heap
// traffic (the vector-returning winner APIs cost a malloc/free per call).

// C(5,3) = 10 three-card board subsets is the maximum, reached on a
// complete board.
constexpr int kPloMaxBoardTriples = 10;

// Card ids of `mask` in ascending order, into out[0..capacity). Returns how
// many were written; bits beyond `capacity` are silently ignored, so callers
// must size the buffer for the largest legal input (5 board / 4 hole cards).
inline int extract_card_ids(std::uint64_t mask, int* out, int capacity) {
    int n = 0;
    while (mask && n < capacity) {
        out[n++] = ctz64(mask);
        mask &= (mask - 1);
    }
    return n;
}

// Expands `board_mask` into its C(B,3) three-card sub-masks (B = popcount,
// read up to 5). Returns how many were written -- 0 when B < 3, i.e. when no
// legal PLO five-card hand exists yet. `out` must hold kPloMaxBoardTriples.
//
// This is split out of the scorer so a multi-player showdown builds the
// triples once instead of once per player: a two-player PLO showdown then
// costs 10 sub-mask builds + 120 five-card evaluations rather than 2 x 70.
inline int plo_board_triples(std::uint64_t board_mask, std::uint64_t* out) {
    int ids[5];
    const int b = extract_card_ids(board_mask, ids, 5);
    int n = 0;
    for (int i = 0; i < b; ++i) {
        for (int j = i + 1; j < b; ++j) {
            for (int k = j + 1; k < b; ++k) {
                out[n++] = (1ULL << ids[i]) | (1ULL << ids[j]) |
                           (1ULL << ids[k]);
            }
        }
    }
    return n;
}

// Best packed score for one PLO hand against a pre-expanded board triple set.
//
// Returns 0 when nothing could be evaluated (fewer than two hole cards, or
// n_triples == 0). A legal five-card hand always scores at least HighCard
// (category 1, so bits 23..20 are non-zero), which is what lets callers keep
// 0 as an unambiguous "not evaluated" / "cards conflict" sentinel.
//
// Precondition: hole_mask is within FULL_DECK_MASK and disjoint from the
// board (the caller validates once, up front).
inline std::uint32_t plo_score_triples(std::uint64_t hole_mask,
                                       const std::uint64_t* triples,
                                       int n_triples) {
    int ids[4];
    const int h = extract_card_ids(hole_mask, ids, 4);
    std::uint32_t best = 0;
    for (int i = 0; i < h; ++i) {
        for (int j = i + 1; j < h; ++j) {
            const std::uint64_t hole_two =
                (1ULL << ids[i]) | (1ULL << ids[j]);
            for (int t = 0; t < n_triples; ++t) {
                const std::uint32_t s = eval_score7(hole_two | triples[t]);
                if (s > best) best = s;
            }
        }
    }
    return best;
}

// Single-hand convenience form. Same contract as plo_score_triples; prefer
// the triple form when several hands share one board.
inline std::uint32_t plo_score(std::uint64_t hole_mask,
                               std::uint64_t board_mask) {
    std::uint64_t triples[kPloMaxBoardTriples];
    const int n = plo_board_triples(board_mask, triples);
    return plo_score_triples(hole_mask, triples, n);
}

// Player-count bounds for the bitmask winner forms below. These are the
// single source of truth for both the internal backstop (judge_holdem_mask /
// judge_plo_mask throw std::runtime_error past them) and the public up-front
// check calculate_equity performs before dispatching to either simulator
// (internal::validate_equity_input, board_sample.h) -- one pair of numbers,
// not two.
//
// kMaxSeats: 10 covers full-ring NLHE; judge_holdem_mask scores into a
// std::array<std::uint32_t, kMaxSeats>, so this is a real, load-bearing cap.
constexpr int kMaxSeats = 10;

// kMaxMaskPlayers: width of the uint32_t winner bitmask judge_plo_mask
// returns. In practice unreachable for PLO by card exhaustion (kMaxMaskPlayers
// four-card hands would need 4x as many cards as the deck holds) and it is
// well above kMaxSeats, so it is a pure representation bound rather than a
// realistic seat count -- but it is still the number judge_plo_mask itself
// enforces, so it is what the up-front PLO check uses too.
constexpr int kMaxMaskPlayers = 32;

// Bitmask forms: bit i set means player i holds (one of) the best hand(s);
// 0 means no player was evaluated. These own the scoring and tie logic and
// allocate nothing, which is why the Monte Carlo showdown loop in
// equity_hands.cpp uses them.
// Preconditions as above. At most kMaxSeats players (Hold'em) /
// kMaxMaskPlayers players (PLO); beyond that they throw std::runtime_error
// as a defensive backstop -- calculate_equity's up-front validation is meant
// to make that backstop unreachable through the public API.
std::uint32_t judge_holdem_mask(
    const std::vector<std::uint64_t>& player_hole_masks,
    std::uint64_t board_mask);

std::uint32_t judge_plo_mask(
    const std::vector<std::uint64_t>& player_hole_masks,
    std::uint64_t board_mask);

// Allocation-free Hold'em evaluator for tight inner loops.
// Writes the (category, kickers) result into `out`. Caller must guarantee:
//   - board and hole are within FULL_DECK_MASK
//   - they do not overlap
//   - popcount(board) in [3, 5], popcount(hole) == 2
inline void evaluate_holdem_fast(std::uint64_t board_mask,
                                 std::uint64_t hole_mask,
                                 HandValue& out) {
    out = decode_score(eval_score7(board_mask | hole_mask));
}

} // namespace xiapl::internal
