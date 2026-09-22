#pragma once

// Public hand-evaluation API.
//
//   evaluate_cards / evaluate_mask  - loose cards, no hole/board split
//   evaluate_hand                   - one player's hand vs a board
//   judge                           - showdown winners
//
// The game-specific rules are selected by the trailing `game` parameter, not
// by picking a different function: there is exactly one entry point per
// concept, for every game.

#include <cstdint>
#include <vector>

#include <xiapl/card.h>
#include <xiapl/game_type.h>
#include <xiapl/hand_value.h>

namespace xiapl {

// Best five-card value out of 5 to 7 loose cards. Game-agnostic: there is no
// hole/board split, so every card is usable. Throws std::runtime_error on a
// bad card count and std::invalid_argument on duplicate cards.
HandValue evaluate_cards(const std::vector<Card>& cards);

// Same as evaluate_cards, taking a 52-bit card mask (5 to 7 bits set).
HandValue evaluate_mask(std::uint64_t card_mask);

// Best five-card value for one player's hole cards against a board, under
// `game`'s showdown rule:
//   Holdem - best five of (2 hole + 3..5 board), any mix.
//   Plo    - best five of exactly 2 hole + exactly 3 board cards.
// board_mask must have 3 to 5 bits set, hole_mask exactly
// cards_per_hand(game); board and hole must not overlap.
HandValue evaluate_hand(std::uint64_t board_mask, std::uint64_t hole_mask,
                        GameType game = GameType::Holdem);

// Showdown: the ascending indices of every player holding the best hand under
// `game`'s rule. More than one index means a chop. board_mask must have 3 to 5
// bits set and each player_hole_masks[i] must have cards_per_hand(game) bits
// set and must not overlap the board or another player. Throws
// std::runtime_error on any of those violations, including an empty player
// list -- there is no showdown without players. Holdem additionally throws
// std::runtime_error for more than 10 players (the full-ring seat cap).
std::vector<int> judge(const std::vector<std::uint64_t>& player_hole_masks,
                       std::uint64_t board_mask,
                       GameType game = GameType::Holdem);

} // namespace xiapl
