#pragma once

#include <cstdint>

namespace xiapl {

// Poker game variant. Determines hole-card count and hand-ranking rules
// throughout the library (Range validation, simulation, hand evaluation).
enum class GameType : std::uint8_t {
    Holdem = 0,
    Plo = 1,
};

// Number of hole cards dealt per player for the given game
// (2 for Hold'em, 4 for PLO).
constexpr int cards_per_hand(GameType game) {
    return game == GameType::Plo ? 4 : 2;
}

} // namespace xiapl
