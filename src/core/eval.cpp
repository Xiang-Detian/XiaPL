#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <xiapl/eval.h>
#include <xiapl/utils.h>

#include "eval_internal.h"   // pulls in eval_core.h (eval_score7 / decode_score)

namespace xiapl {

// Every function here is a thin shell around internal::eval_score7():
// validate the input, evaluate one or more 5-7 card masks, keep the best
// packed score, and decode once at the end.
//
// The packed score is order-isomorphic to HandValue: bits 23..20 hold the
// HandCategory and bits 19..0 hold kickers[0..4] as one nibble each (0 =
// unused). HandValue::operator< compares category first, then the whole
// 5-element kickers array lexicographically, then kicker_count; since
// decode_score() derives kicker_count from the kicker nibbles, that last
// tiebreak can never separate two equal-nibble scores. So comparing raw
// uint32 scores is exactly equivalent to comparing decoded HandValues,
// and the ordering-only paths below never decode.

namespace {

// ---- Validation ----

void validate_player_masks(
    const std::vector<std::uint64_t>& player_hole_masks,
    std::uint64_t board_mask,
    int expected_hole_cards,
    const char* caller_name
) {
    int board_count = popcount64(board_mask & FULL_DECK_MASK);
    if (board_count < 3 || board_count > 5) {
        throw std::runtime_error(std::string(caller_name) + " requires 3 to 5 board cards");
    }
    if (player_hole_masks.empty()) {
        throw std::runtime_error(std::string(caller_name) + " requires at least one player");
    }

    std::uint64_t used = board_mask & FULL_DECK_MASK;
    for (std::size_t i = 0; i < player_hole_masks.size(); ++i) {
        std::uint64_t hole = player_hole_masks[i] & FULL_DECK_MASK;
        if (popcount64(hole) != expected_hole_cards) {
            throw std::runtime_error(std::string(caller_name) + ": invalid hole card count");
        }
        if (used & hole) {
            throw std::runtime_error(std::string(caller_name) + ": overlapping cards");
        }
        used |= hole;
    }
}

// ---- Winner-mask scoring core ----
//
// PLO scoring (best 5 of "exactly 2 of 4 hole" x "exactly 3 of B board")
// lives in eval_internal.h as internal::plo_score / plo_score_triples: the
// Monte Carlo showdown loop in equity_range.cpp needs the same rule with zero
// allocation per trial, and one definition is the only way the two paths
// cannot drift apart.

// Winners = every index holding the top score, as a bitmask (bit i set == i
// is a winner). `best == 0` means no player was evaluated at all, which
// yields an empty winner set (mask 0).
//
// This is the ONLY place the "highest score, ties included" comparison is
// written. The vector-returning winner API is an adapter over this mask (see
// mask_to_indices), so the two forms cannot disagree by construction.
// Allocation-free, so Monte Carlo inner loops can call it per trial.
std::uint32_t winner_mask_from_scores(const std::uint32_t* scores, int n) {
    std::uint32_t best = 0;
    for (int i = 0; i < n; ++i) {
        if (scores[i] > best) best = scores[i];
    }
    if (best == 0) return 0;
    std::uint32_t winners = 0;
    for (int i = 0; i < n; ++i) {
        if (scores[i] == best) winners |= (1u << i);
    }
    return winners;
}

// Adapter: winner bitmask -> ascending winner indices.
std::vector<int> mask_to_indices(std::uint32_t winners) {
    std::vector<int> out;
    if (winners == 0) return out;
    out.reserve(static_cast<std::size_t>(popcount64(winners)));
    for (std::uint32_t m = winners; m; m &= (m - 1)) {
        out.push_back(ctz64(m));
    }
    return out;
}

// Score every player's best 7-card hand into out_scores[0..n). Shared by the
// vector and bitmask winner forms so the two can never disagree on the
// scoring itself. `caller` only names the thrower.
int score_players_holdem(const std::vector<std::uint64_t>& player_hole_masks,
                         std::uint64_t board_mask, std::uint32_t* out_scores,
                         int capacity, const char* caller) {
    const int num_players = static_cast<int>(player_hole_masks.size());
    if (num_players > capacity) {
        throw std::runtime_error(std::string(caller) + ": too many players");
    }
    for (int i = 0; i < num_players; ++i) {
        out_scores[i] = internal::eval_score7(board_mask | player_hole_masks[i]);
    }
    return num_players;
}

// Same for PLO (max over 2-of-4 hole x 3-of-B board per player).
void score_players_plo(const std::vector<std::uint64_t>& player_hole_masks,
                       std::uint64_t board_mask, std::uint32_t* out_scores) {
    // Stay inside the 52-card universe even on this unchecked path: the
    // helpers' index arrays are sized for a legal board / PLO hand.
    // The board triples are shared by every player, so they are built once.
    std::uint64_t triples[internal::kPloMaxBoardTriples];
    const int n_triples =
        internal::plo_board_triples(board_mask & FULL_DECK_MASK, triples);
    const int num_players = static_cast<int>(player_hole_masks.size());
    for (int p = 0; p < num_players; ++p) {
        out_scores[p] = internal::plo_score_triples(
            player_hole_masks[p] & FULL_DECK_MASK, triples, n_triples);
    }
}

// ---- Per-game judge mask layer ----
//
// Strip-and-validate wrappers over internal::judge_holdem_mask /
// internal::judge_plo_mask (the bitmask scoring core shared with the Monte
// Carlo showdown loop in equity_hands.cpp -- see eval_internal.h). judge_holdem
// / judge_plo below adapt the bitmask result to the public vector-of-indices
// form via mask_to_indices.

std::vector<int> judge_holdem_masks(
    const std::vector<std::uint64_t>& player_hole_masks,
    std::uint64_t board_mask
) {
    int num_players = static_cast<int>(player_hole_masks.size());
    if (num_players == 0) return {};

    // Strip board mask to the 52-card universe so callers passing stray
    // high bits get the same answer as the public validator. Then verify.
    board_mask &= FULL_DECK_MASK;
    int board_count = popcount64(board_mask);
    if (board_count < 3 || board_count > 5) {
        throw std::runtime_error("judge_holdem_masks expects 3 to 5 board cards");
    }
    std::vector<std::uint64_t> stripped(num_players);
    for (int i = 0; i < num_players; ++i) {
        std::uint64_t hole_mask = player_hole_masks[i] & FULL_DECK_MASK;
        if (hole_mask == 0) {
            throw std::runtime_error("judge_holdem_masks: player hole mask is empty");
        }
        stripped[i] = hole_mask;
    }
    return mask_to_indices(internal::judge_holdem_mask(stripped, board_mask));
}

std::vector<int> judge_plo_masks(
    const std::vector<std::uint64_t>& player_hole_masks,
    std::uint64_t board_mask
) {
    int num_players = static_cast<int>(player_hole_masks.size());
    if (num_players == 0) return {};

    board_mask &= FULL_DECK_MASK;
    int board_count = popcount64(board_mask);
    if (board_count < 3 || board_count > 5) {
        throw std::runtime_error("judge_plo_masks expects 3 to 5 board cards");
    }

    std::vector<std::uint64_t> stripped(num_players);
    for (int p = 0; p < num_players; ++p) {
        std::uint64_t hole_mask = player_hole_masks[p] & FULL_DECK_MASK;
        if (popcount64(hole_mask) != 4) {
            throw std::runtime_error(
                "judge_plo_masks: each player must have exactly 4 hole cards in mask");
        }
        stripped[p] = hole_mask;
    }
    return mask_to_indices(internal::judge_plo_mask(stripped, board_mask));
}

// ---- Per-game arms: evaluate_hand / judge ----
//
// They used to be four separate public functions (evaluate_holdem / evaluate_plo /
// determine_winners_holdem / determine_winners_plo); the surface is now one
// entry point per concept with a trailing `game`, so these are file-local.
// Their error text names the public entry point the caller actually used.
HandValue evaluate_hand_holdem(std::uint64_t board_mask, std::uint64_t hole_mask) {
    int board_count = popcount64(board_mask & FULL_DECK_MASK);
    int hole_count = popcount64(hole_mask & FULL_DECK_MASK);
    if (board_count < 3 || board_count > 5) {
        throw std::runtime_error("evaluate_hand requires 3 to 5 board cards");
    }
    if (hole_count != 2) {
        throw std::runtime_error("evaluate_hand: Hold'em requires exactly 2 hole cards");
    }
    if (board_mask & hole_mask) {
        throw std::runtime_error("evaluate_hand: board and hole overlap");
    }
    // eval_score7 reads only the low 52 bits, so stray high bits are ignored.
    return internal::decode_score(internal::eval_score7(board_mask | hole_mask));
}

HandValue evaluate_hand_plo(std::uint64_t board_mask, std::uint64_t hole_mask) {
    int board_count = popcount64(board_mask & FULL_DECK_MASK);
    int hole_count = popcount64(hole_mask & FULL_DECK_MASK);
    if (board_count < 3 || board_count > 5) {
        throw std::runtime_error("evaluate_hand requires 3 to 5 board cards");
    }
    if (hole_count != 4) {
        throw std::runtime_error("evaluate_hand: PLO requires exactly 4 hole cards");
    }
    if (board_mask & hole_mask) {
        throw std::runtime_error("evaluate_hand: board and hole overlap");
    }

    const std::uint32_t best = internal::plo_score(
        hole_mask & FULL_DECK_MASK, board_mask & FULL_DECK_MASK);
    if (best == 0) {
        throw std::runtime_error("evaluate_hand: no hand evaluated");
    }
    return internal::decode_score(best);
}

std::vector<int> judge_holdem(
    const std::vector<std::uint64_t>& player_hole_masks,
    std::uint64_t board_mask
) {
    validate_player_masks(player_hole_masks, board_mask, 2, "judge (Hold'em)");
    return judge_holdem_masks(player_hole_masks, board_mask);
}

std::vector<int> judge_plo(
    const std::vector<std::uint64_t>& player_hole_masks,
    std::uint64_t board_mask
) {
    validate_player_masks(player_hole_masks, board_mask, 4, "judge (PLO)");
    return judge_plo_masks(player_hole_masks, board_mask);
}

} // namespace

// ---- internal:: bitmask winner API -----------------------------------
//
// Shared scoring core: judge_holdem_masks / judge_plo_masks above call these
// after validating and stripping input, and the Monte Carlo showdown loop in
// equity_hands.cpp calls them directly on pre-validated masks (see the
// unchecked-fast-path contract in eval_internal.h). One definition means the
// two paths cannot disagree on who wins.
namespace internal {
std::uint32_t judge_holdem_mask(
    const std::vector<std::uint64_t>& player_hole_masks,
    std::uint64_t board_mask
) {
    if (player_hole_masks.empty()) return 0;
    std::array<std::uint32_t, kMaxSeats> scores{};
    const int n = score_players_holdem(player_hole_masks, board_mask,
                                       scores.data(), kMaxSeats, "judge_holdem_mask");
    return winner_mask_from_scores(scores.data(), n);
}

std::uint32_t judge_plo_mask(
    const std::vector<std::uint64_t>& player_hole_masks,
    std::uint64_t board_mask
) {
    const int num_players = static_cast<int>(player_hole_masks.size());
    if (num_players == 0) return 0;
    if (num_players > kMaxMaskPlayers) {
        throw std::runtime_error(std::string("judge_plo_mask") + ": too many players");
    }
    std::array<std::uint32_t, kMaxMaskPlayers> scores{};
    score_players_plo(player_hole_masks, board_mask, scores.data());
    return winner_mask_from_scores(scores.data(), num_players);
}
} // namespace internal

HandValue evaluate_cards(const std::vector<Card>& cards) {
    if (cards.size() < 5 || cards.size() > 7) {
        throw std::runtime_error("evaluate_cards requires 5 to 7 cards");
    }
    // cards_to_mask validates each Card.id; an invalid card throws there.
    std::uint64_t mask = cards_to_mask(cards);
    // Reject duplicate Cards: cards_to_mask silently ORs duplicates into the
    // same bit, which would otherwise produce a meaningless evaluation
    // (e.g. {As,As,As,As,As} -> 1-bit mask -> bogus HighCard).
    if (popcount64(mask) != static_cast<int>(cards.size())) {
        throw std::invalid_argument("evaluate_cards: duplicate cards in input");
    }
    return internal::decode_score(internal::eval_score7(mask));
}

HandValue evaluate_mask(std::uint64_t card_mask) {
    card_mask &= FULL_DECK_MASK;
    int card_count = popcount64(card_mask);
    if (card_count < 5 || card_count > 7) {
        throw std::runtime_error("evaluate_mask requires 5 to 7 cards");
    }
    return internal::decode_score(internal::eval_score7(card_mask));
}

HandValue evaluate_hand(std::uint64_t board_mask, std::uint64_t hole_mask,
                        GameType game) {
    return game == GameType::Plo
               ? evaluate_hand_plo(board_mask, hole_mask)
               : evaluate_hand_holdem(board_mask, hole_mask);
}

std::vector<int> judge(
    const std::vector<std::uint64_t>& player_hole_masks,
    std::uint64_t board_mask,
    GameType game
) {
    return game == GameType::Plo
               ? judge_plo(player_hole_masks, board_mask)
               : judge_holdem(player_hole_masks, board_mask);
}

} // namespace xiapl
