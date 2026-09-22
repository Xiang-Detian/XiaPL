#pragma once

#include <cstdint>

namespace xiapl {

// Street of the game (mirrors the Python Street Enum)
enum class Street : std::uint8_t {
    PREFLOP = 0,
    FLOP,
    TURN,
    RIVER,
    SHOWDOWN
};

} // namespace xiapl
