#pragma once

// Boundary conventions shared by every translation unit of the C ABI
// (waist v1). Private to src/api/.
//
// These four helpers encode rules that <xiapl/c_api.h> states ONCE and that
// every function then inherits, so they must have exactly one implementation:
//   * emit_string   -- the string buffer protocol (convention 7),
//   * to_game_type  -- the int32_t game tag -> GameType validation
//                      (convention 10: enum parameters are int32_t),
//   * load_options  -- xiapl_sim_options_t -> SimulationOptions.
//   * to_cards      -- a uint8_t id array -> std::vector<Card>.
// A second copy of any of them would let the scalar and the handle halves of
// the ABI drift apart on truncation, on NUL termination or on what counts as
// a known game.
//
// Error plumbing lives next door in waist_error.h.

#include <xiapl/c_api.h>

#include <xiapl/card.h>
#include <xiapl/game_type.h>
#include <xiapl/simulation.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace xiapl::waist {

// String output (convention 7): `*out_length` is always the byte length
// excluding the NUL. A NULL buffer or a non-positive capacity is the QUERY
// call and is never an error; otherwise at most `buf_capacity` bytes are
// written and the result is always NUL-terminated, truncating if it does not
// fit. `out_length` is required and is checked by the caller.
inline std::int32_t emit_string(const std::string& text, char* out_buf,
                                std::int32_t buf_capacity,
                                std::int32_t* out_length) {
    *out_length = static_cast<std::int32_t>(text.size());
    if (out_buf == nullptr || buf_capacity <= 0) return XIAPL_OK;

    const std::size_t room = static_cast<std::size_t>(buf_capacity) - 1;
    const std::size_t n = std::min(room, text.size());
    std::memcpy(out_buf, text.data(), n);
    out_buf[n] = '\0';
    return XIAPL_OK;
}

// Enum-valued parameters are int32_t (convention 10), so the game tag is
// validated here rather than by a cast into GameType.
inline bool to_game_type(std::int32_t game, GameType* out) {
    switch (game) {
    case XIAPL_GAME_HOLDEM: *out = GameType::Holdem; return true;
    case XIAPL_GAME_PLO:    *out = GameType::Plo;    return true;
    default:                return false;
    }
}

// Options are carried verbatim; `reserved` is not read. A zero-initialized
// block therefore lands on the C++ defaults, which is the permanent
// "zero-initialized IS the library default" invariant.
inline SimulationOptions load_options(const xiapl_sim_options_t& in) {
    SimulationOptions options;
    options.seed = in.seed;
    options.iterations = static_cast<int>(in.iterations);
    options.deterministic = in.deterministic != 0;
    options.threads = static_cast<int>(in.threads);
    return options;
}

// An id array becomes Cards the same way xiapl_card_from_id does: an id
// outside [0, 51] becomes Card::INVALID_ID, and the C++ routine that receives
// it owns the verdict (Deck::set_cards rejects it, Deck::remove_cards skips
// it, canonicalize_hand accepts it, cards_to_mask/evaluate_cards throw).
// Every caller rejects a negative count before reaching here; the guard keeps
// that from being load-bearing, since a negative count would otherwise
// reserve a nonsense size.
inline std::vector<Card> to_cards(const std::uint8_t* ids, std::int32_t count) {
    if (count <= 0) return {};
    std::vector<Card> cards;
    cards.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i) {
        cards.push_back(Card::from_id(ids[i]));
    }
    return cards;
}

}  // namespace xiapl::waist
