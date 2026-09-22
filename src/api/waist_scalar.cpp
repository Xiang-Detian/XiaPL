// C ABI (waist v1) -- scalar half: version, errors, card, mask utilities,
// hand evaluation, simulation options and canonicalization.
//
// Every function here is a mechanical projection of the public C++ API onto
// the frozen contract in <xiapl/c_api.h>: it validates only what that header
// says the waist validates (NULL pointers, negative counts, out-of-range ids,
// unknown enum values), delegates the actual work to include/xiapl/*, and lets
// waist::wrap classify whatever the C++ layer throws. It adds no behaviour of
// its own -- no sorting, no labelling, no extra checks, no repaired contracts.
// Where the C++ contract is odd (xiapl_card_from_id returning a sentinel
// instead of failing) the waist reproduces the oddity rather than fixing it.
//
// The handle-based half of the ABI (range, deck, equity, result handles) lives
// in a separate translation unit.

#include <xiapl/c_api.h>

#include "waist_error.h"
#include "waist_internal.h"

#include <xiapl/canonicalize.h>
#include <xiapl/card.h>
#include <xiapl/eval.h>
#include <xiapl/game_type.h>
#include <xiapl/hand_value.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>
#include <xiapl/version.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace xiapl;
using waist::emit_string;
using waist::invalid_argument;
using waist::load_options;
using waist::to_cards;
using waist::to_game_type;
using waist::wrap;

// Pin the POD layout in this translation unit as well as in the pure-C ABI
// test: a C++ consumer that compiles the waist must fail to BUILD, not merely
// fail a runtime check, if a struct drifts. tests/test_c_api_abi.c carries the
// per-field offsets (it is the file an FFI layer transcribes).
static_assert(sizeof(xiapl_hand_value_t) == 28, "xiapl_hand_value_t layout");
static_assert(sizeof(xiapl_sim_options_t) == 24, "xiapl_sim_options_t layout");
static_assert(sizeof(xiapl_equity_summary_t) == 24, "xiapl_equity_summary_t layout");
static_assert(sizeof(xiapl_range_equity_summary_t) == 40,
              "xiapl_range_equity_summary_t layout");

namespace {

// emit_string / to_game_type / load_options live in waist_internal.h: the
// handle half of the ABI needs the same three rules, and a second copy would
// let the two translation units drift. The two helpers below are used only
// here.

void store_hand_value(const HandValue& value, xiapl_hand_value_t* out) {
    out->category = static_cast<std::int32_t>(value.category);
    out->kicker_count = static_cast<std::int32_t>(value.kicker_count);
    // All five slots are copied verbatim; the evaluator already leaves the
    // slots past kicker_count at zero, so this is a copy, not a normalization.
    for (std::size_t i = 0; i < XIAPL_HAND_VALUE_MAX_KICKERS; ++i) {
        out->kickers[i] = static_cast<std::int32_t>(value.kickers[i]);
    }
}

void store_options(const SimulationOptions& options, xiapl_sim_options_t* out) {
    out->seed = options.seed;
    out->iterations = static_cast<std::int32_t>(options.iterations);
    out->deterministic = options.deterministic ? 1 : 0;
    out->threads = static_cast<std::int32_t>(options.threads);
    out->reserved = 0;
}

}  // namespace

extern "C" {

// ===========================================================================
// ABI revision, errors, version
// ===========================================================================

std::uint32_t xiapl_c_abi_version(void) {
    return XIAPL_C_ABI_VERSION;
}

const char* xiapl_last_error_message(void) {
    // Never NULL: an untouched slot is an empty std::string.
    return waist::g_last_error.c_str();
}

void xiapl_clear_last_error(void) {
    waist::g_last_error.clear();
}

std::int32_t xiapl_version(std::int32_t* out_major, std::int32_t* out_minor,
                           std::int32_t* out_patch) {
    if (out_major != nullptr) *out_major = XIAPL_VERSION_MAJOR;
    if (out_minor != nullptr) *out_minor = XIAPL_VERSION_MINOR;
    if (out_patch != nullptr) *out_patch = XIAPL_VERSION_PATCH;
    return XIAPL_OK;
}

const char* xiapl_version_string(void) {
    return XIAPL_VERSION_STRING;
}

// ===========================================================================
// Card
// ===========================================================================

std::int32_t xiapl_card_from_rank_suit(std::int32_t rank, std::int32_t suit,
                                       std::uint8_t* out_id) {
    return wrap([&]() -> std::int32_t {
        if (out_id == nullptr) {
            return invalid_argument("xiapl_card_from_rank_suit: out_id must not be NULL");
        }
        // Card(int, int) is the validator: it throws std::invalid_argument for
        // a rank or suit outside range, and its message is carried verbatim.
        *out_id = Card(static_cast<int>(rank), static_cast<int>(suit)).id;
        return XIAPL_OK;
    });
}

std::int32_t xiapl_card_from_string(const char* text, std::uint8_t* out_id) {
    return wrap([&]() -> std::int32_t {
        if (text == nullptr) {
            return invalid_argument("xiapl_card_from_string: text must not be NULL");
        }
        if (out_id == nullptr) {
            return invalid_argument("xiapl_card_from_string: out_id must not be NULL");
        }
        *out_id = Card::from_string(std::string(text)).id;
        return XIAPL_OK;
    });
}

std::int32_t xiapl_card_from_id(std::uint8_t id, std::uint8_t* out_id) {
    return wrap([&]() -> std::int32_t {
        if (out_id == nullptr) {
            return invalid_argument("xiapl_card_from_id: out_id must not be NULL");
        }
        // Card::from_id is the non-throwing factory: an id outside [0, 51]
        // yields XIAPL_CARD_INVALID_ID. Reproduced, not repaired.
        *out_id = Card::from_id(id).id;
        return XIAPL_OK;
    });
}

std::int32_t xiapl_card_rank(std::uint8_t id, std::int32_t* out_rank) {
    return wrap([&]() -> std::int32_t {
        if (out_rank == nullptr) {
            return invalid_argument("xiapl_card_rank: out_rank must not be NULL");
        }
        if (!is_valid_card_id(id)) {
            return invalid_argument("xiapl_card_rank: id must be in [0, 51]");
        }
        *out_rank = Card::from_id(id).rank();
        return XIAPL_OK;
    });
}

std::int32_t xiapl_card_suit(std::uint8_t id, std::int32_t* out_suit) {
    return wrap([&]() -> std::int32_t {
        if (out_suit == nullptr) {
            return invalid_argument("xiapl_card_suit: out_suit must not be NULL");
        }
        if (!is_valid_card_id(id)) {
            return invalid_argument("xiapl_card_suit: id must be in [0, 51]");
        }
        *out_suit = Card::from_id(id).suit();
        return XIAPL_OK;
    });
}

std::int32_t xiapl_card_to_string(std::uint8_t id, char* out_buf,
                                  std::int32_t buf_capacity,
                                  std::int32_t* out_length) {
    return wrap([&]() -> std::int32_t {
        if (out_length == nullptr) {
            return invalid_argument("xiapl_card_to_string: out_length must not be NULL");
        }
        // from_id keeps an out-of-range id out of Card's throwing constructor,
        // so it formats as "Invalid" exactly like a default-constructed Card.
        return emit_string(Card::from_id(id).to_string(), out_buf, buf_capacity,
                           out_length);
    });
}

std::int32_t xiapl_card_repr(std::uint8_t id, char* out_buf,
                             std::int32_t buf_capacity,
                             std::int32_t* out_length) {
    return wrap([&]() -> std::int32_t {
        if (out_length == nullptr) {
            return invalid_argument("xiapl_card_repr: out_length must not be NULL");
        }
        return emit_string(Card::from_id(id).repr(), out_buf, buf_capacity,
                           out_length);
    });
}

std::int32_t xiapl_try_rank_from_char(char c, std::int32_t* out_rank,
                                      std::int32_t* out_found) {
    return wrap([&]() -> std::int32_t {
        if (out_rank == nullptr) {
            return invalid_argument("xiapl_try_rank_from_char: out_rank must not be NULL");
        }
        if (out_found == nullptr) {
            return invalid_argument("xiapl_try_rank_from_char: out_found must not be NULL");
        }
        const std::optional<int> rank = try_rank_from_char(c);
        *out_rank = rank ? static_cast<std::int32_t>(*rank) : 0;
        *out_found = rank ? 1 : 0;
        return XIAPL_OK;
    });
}

std::int32_t xiapl_try_suit_from_char(char c, std::int32_t* out_suit,
                                      std::int32_t* out_found) {
    return wrap([&]() -> std::int32_t {
        if (out_suit == nullptr) {
            return invalid_argument("xiapl_try_suit_from_char: out_suit must not be NULL");
        }
        if (out_found == nullptr) {
            return invalid_argument("xiapl_try_suit_from_char: out_found must not be NULL");
        }
        const std::optional<int> suit = try_suit_from_char(c);
        *out_suit = suit ? static_cast<std::int32_t>(*suit) : 0;
        *out_found = suit ? 1 : 0;
        return XIAPL_OK;
    });
}

std::int32_t xiapl_rank_to_char(std::int32_t rank, char* out_char) {
    return wrap([&]() -> std::int32_t {
        if (out_char == nullptr) {
            return invalid_argument("xiapl_rank_to_char: out_char must not be NULL");
        }
        // xiapl::rank_to_char answers '?' outside [2, 14]; the waist contract
        // is an error there, so the range is checked before delegating.
        if (rank < 2 || rank > 14) {
            return invalid_argument("xiapl_rank_to_char: rank must be in [2, 14]");
        }
        *out_char = rank_to_char(static_cast<int>(rank));
        return XIAPL_OK;
    });
}

// ===========================================================================
// Mask utilities
// ===========================================================================

std::int32_t xiapl_card_to_mask(std::uint8_t id, std::uint64_t* out_mask) {
    return wrap([&]() -> std::int32_t {
        if (out_mask == nullptr) {
            return invalid_argument("xiapl_card_to_mask: out_mask must not be NULL");
        }
        // card_to_mask rejects any id outside [0, 51], XIAPL_CARD_INVALID_ID
        // included, with std::invalid_argument.
        *out_mask = card_to_mask(Card::from_id(id));
        return XIAPL_OK;
    });
}

std::int32_t xiapl_cards_to_mask(const std::uint8_t* ids, std::int32_t count,
                                 std::uint64_t* out_mask) {
    return wrap([&]() -> std::int32_t {
        if (out_mask == nullptr) {
            return invalid_argument("xiapl_cards_to_mask: out_mask must not be NULL");
        }
        if (count < 0) {
            return invalid_argument("xiapl_cards_to_mask: count must not be negative");
        }
        if (ids == nullptr && count > 0) {
            return invalid_argument("xiapl_cards_to_mask: ids must not be NULL when count > 0");
        }
        // cards_to_mask throws on an invalid id and collapses duplicates.
        *out_mask = cards_to_mask(to_cards(ids, count));
        return XIAPL_OK;
    });
}

std::int32_t xiapl_mask_to_ids(std::uint64_t mask, std::uint8_t* out_ids,
                               std::int32_t ids_capacity,
                               std::int32_t* out_total) {
    return wrap([&]() -> std::int32_t {
        if (out_total == nullptr) {
            return invalid_argument("xiapl_mask_to_ids: out_total must not be NULL");
        }
        // mask_to_ids scans ids 0..51 only, so bits above 51 are ignored.
        const std::vector<int> ids = mask_to_ids(mask);
        *out_total = static_cast<std::int32_t>(ids.size());
        if (out_ids == nullptr || ids_capacity <= 0) return XIAPL_OK;

        const std::size_t n =
            std::min(ids.size(), static_cast<std::size_t>(ids_capacity));
        for (std::size_t i = 0; i < n; ++i) {
            out_ids[i] = static_cast<std::uint8_t>(ids[i]);
        }
        return XIAPL_OK;
    });
}

std::int32_t xiapl_cards_per_hand(std::int32_t game, std::int32_t* out_count) {
    return wrap([&]() -> std::int32_t {
        if (out_count == nullptr) {
            return invalid_argument("xiapl_cards_per_hand: out_count must not be NULL");
        }
        GameType game_type = GameType::Holdem;
        if (!to_game_type(game, &game_type)) {
            return invalid_argument("xiapl_cards_per_hand: unknown game");
        }
        *out_count = static_cast<std::int32_t>(cards_per_hand(game_type));
        return XIAPL_OK;
    });
}

// ===========================================================================
// Hand evaluation
// ===========================================================================

std::int32_t xiapl_evaluate_cards(const std::uint8_t* ids, std::int32_t count,
                                  xiapl_hand_value_t* out_value) {
    return wrap([&]() -> std::int32_t {
        if (out_value == nullptr) {
            return invalid_argument("xiapl_evaluate_cards: out_value must not be NULL");
        }
        if (ids == nullptr) {
            return invalid_argument("xiapl_evaluate_cards: ids must not be NULL");
        }
        if (count < 0) {
            return invalid_argument("xiapl_evaluate_cards: count must not be negative");
        }
        // evaluate_cards owns both classifications the header documents: a bad
        // card count is std::runtime_error, a duplicate is
        // std::invalid_argument.
        store_hand_value(evaluate_cards(to_cards(ids, count)), out_value);
        return XIAPL_OK;
    });
}

std::int32_t xiapl_evaluate_mask(std::uint64_t card_mask,
                                 xiapl_hand_value_t* out_value) {
    return wrap([&]() -> std::int32_t {
        if (out_value == nullptr) {
            return invalid_argument("xiapl_evaluate_mask: out_value must not be NULL");
        }
        store_hand_value(evaluate_mask(card_mask), out_value);
        return XIAPL_OK;
    });
}

std::int32_t xiapl_evaluate_hand(std::uint64_t board_mask,
                                 std::uint64_t hole_mask, std::int32_t game,
                                 xiapl_hand_value_t* out_value) {
    return wrap([&]() -> std::int32_t {
        if (out_value == nullptr) {
            return invalid_argument("xiapl_evaluate_hand: out_value must not be NULL");
        }
        GameType game_type = GameType::Holdem;
        if (!to_game_type(game, &game_type)) {
            return invalid_argument("xiapl_evaluate_hand: unknown game");
        }
        store_hand_value(evaluate_hand(board_mask, hole_mask, game_type),
                         out_value);
        return XIAPL_OK;
    });
}

std::int32_t xiapl_judge(const std::uint64_t* player_hole_masks,
                         std::int32_t num_players, std::uint64_t board_mask,
                         std::int32_t game, std::int32_t* out_winners,
                         std::int32_t winners_capacity,
                         std::int32_t* out_count) {
    return wrap([&]() -> std::int32_t {
        if (out_count == nullptr) {
            return invalid_argument("xiapl_judge: out_count must not be NULL");
        }
        if (num_players < 0) {
            return invalid_argument("xiapl_judge: num_players must not be negative");
        }
        if (player_hole_masks == nullptr && num_players > 0) {
            return invalid_argument(
                "xiapl_judge: player_hole_masks must not be NULL when num_players > 0");
        }
        if (out_winners == nullptr && num_players > 0) {
            return invalid_argument("xiapl_judge: out_winners must not be NULL");
        }
        // Output cardinality is bounded by num_players, so there is no query
        // pass: a short buffer fails up front, before any evaluation.
        if (winners_capacity < num_players) {
            return invalid_argument("xiapl_judge: winners_capacity must be >= num_players");
        }
        GameType game_type = GameType::Holdem;
        if (!to_game_type(game, &game_type)) {
            return invalid_argument("xiapl_judge: unknown game");
        }
        std::vector<std::uint64_t> masks;
        masks.reserve(static_cast<std::size_t>(num_players));
        for (std::int32_t i = 0; i < num_players; ++i) {
            masks.push_back(player_hole_masks[i]);
        }
        // judge owns the rest of the contract (board size, hole popcount,
        // overlaps, seat caps, the empty player list), all std::runtime_error.
        const std::vector<int> winners = judge(masks, board_mask, game_type);
        *out_count = static_cast<std::int32_t>(winners.size());
        for (std::size_t i = 0; i < winners.size(); ++i) {
            out_winners[i] = static_cast<std::int32_t>(winners[i]);
        }
        return XIAPL_OK;
    });
}

std::int32_t xiapl_describe_hand(const xiapl_hand_value_t* value, char* out_buf,
                                 std::int32_t buf_capacity,
                                 std::int32_t* out_length) {
    return wrap([&]() -> std::int32_t {
        if (value == nullptr) {
            return invalid_argument("xiapl_describe_hand: value must not be NULL");
        }
        if (out_length == nullptr) {
            return invalid_argument("xiapl_describe_hand: out_length must not be NULL");
        }
        // kicker_count indexes a five-slot array on both sides of the
        // boundary, so it is range-checked here; describe_hand itself trusts
        // it. An unknown category is NOT checked: describe_hand answers
        // "Unknown" for one, and the waist carries that answer through.
        if (value->kicker_count < 0 ||
            value->kicker_count > XIAPL_HAND_VALUE_MAX_KICKERS) {
            return invalid_argument("xiapl_describe_hand: kicker_count must be in [0, 5]");
        }
        HandValue hand;
        hand.category = static_cast<HandCategory>(value->category);
        hand.kicker_count = static_cast<std::uint8_t>(value->kicker_count);
        for (std::size_t i = 0; i < XIAPL_HAND_VALUE_MAX_KICKERS; ++i) {
            // The C++ kickers are uint8_t. A caller that writes a value the
            // C++ type cannot hold gets the truncation the C++ type would
            // give it: the header documents no kicker-VALUE check, and
            // inventing one here would make the waist reject inputs the
            // library itself accepts.
            hand.kickers[i] = static_cast<std::uint8_t>(value->kickers[i]);
        }
        return emit_string(describe_hand(hand), out_buf, buf_capacity, out_length);
    });
}

std::int32_t xiapl_hand_category_name(std::int32_t category, char* out_buf,
                                      std::int32_t buf_capacity,
                                      std::int32_t* out_length) {
    return wrap([&]() -> std::int32_t {
        if (out_length == nullptr) {
            return invalid_argument("xiapl_hand_category_name: out_length must not be NULL");
        }
        // to_string(HandCategory) answers "Unknown" for an unknown value; the
        // waist contract is an error, so the range is checked before it.
        if (category < XIAPL_HAND_HIGH_CARD || category > XIAPL_HAND_STRAIGHT_FLUSH) {
            return invalid_argument("xiapl_hand_category_name: unknown hand category");
        }
        return emit_string(to_string(static_cast<HandCategory>(category)),
                           out_buf, buf_capacity, out_length);
    });
}

// ===========================================================================
// Simulation options
// ===========================================================================

std::int32_t xiapl_sim_options_exact(xiapl_sim_options_t* out_options) {
    return wrap([&]() -> std::int32_t {
        if (out_options == nullptr) {
            return invalid_argument("xiapl_sim_options_exact: out_options must not be NULL");
        }
        store_options(SimulationOptions::exact(), out_options);
        return XIAPL_OK;
    });
}

std::int32_t xiapl_sim_options_mc_random(std::int32_t iterations,
                                         xiapl_sim_options_t* out_options) {
    return wrap([&]() -> std::int32_t {
        if (out_options == nullptr) {
            return invalid_argument("xiapl_sim_options_mc_random: out_options must not be NULL");
        }
        store_options(SimulationOptions::mc_random(static_cast<int>(iterations)),
                      out_options);
        return XIAPL_OK;
    });
}

std::int32_t xiapl_sim_options_mc_seeded(std::int32_t iterations,
                                         std::uint64_t seed,
                                         xiapl_sim_options_t* out_options) {
    return wrap([&]() -> std::int32_t {
        if (out_options == nullptr) {
            return invalid_argument("xiapl_sim_options_mc_seeded: out_options must not be NULL");
        }
        store_options(
            SimulationOptions::mc_seeded(static_cast<int>(iterations), seed),
            out_options);
        return XIAPL_OK;
    });
}

std::int32_t xiapl_sim_options_effective_mode(const xiapl_sim_options_t* options,
                                              std::int32_t* out_mode) {
    return wrap([&]() -> std::int32_t {
        if (options == nullptr) {
            return invalid_argument("xiapl_sim_options_effective_mode: options must not be NULL");
        }
        if (out_mode == nullptr) {
            return invalid_argument("xiapl_sim_options_effective_mode: out_mode must not be NULL");
        }
        *out_mode = static_cast<std::int32_t>(load_options(*options).effective_mode());
        return XIAPL_OK;
    });
}

// ===========================================================================
// Canonicalization
//
// Version tags are NOT translated here. An int32_t is cast straight onto
// CanonVersion (a scoped enum with a fixed uint32_t underlying type, so every
// value is representable) and the C++ routine does the rejecting, which keeps
// both the message and the ORDER of the checks identical to C++: e.g.
// generate_canonical_situations validates board_size before it validates the
// version, and a waist-side version check would reverse that.
//
// That pass-through is also what makes XIAPL_CANON_LEGACY (1) an error as of
// ABI v4. The Legacy enumerator was deleted, so a cast-through 1
// reaches a switch with no case for it (canonicalize_hero_and_board_v) or a
// canon_version_from_u32 call that names the removed generation
// (generate_canonical_situations); both throw std::invalid_argument, which
// wrap() reports as XIAPL_ERR_INVALID_ARGUMENT carrying the C++ message. A
// waist-side check for 1 would duplicate that verdict and then drift from it.
// ===========================================================================

std::int32_t xiapl_canonicalize_hero_and_board(std::uint64_t hero_mask,
                                               std::uint64_t board_mask,
                                               std::uint64_t* out_hero_mask,
                                               std::uint64_t* out_board_mask) {
    return wrap([&]() -> std::int32_t {
        if (out_hero_mask == nullptr) {
            return invalid_argument(
                "xiapl_canonicalize_hero_and_board: out_hero_mask must not be NULL");
        }
        if (out_board_mask == nullptr) {
            return invalid_argument(
                "xiapl_canonicalize_hero_and_board: out_board_mask must not be NULL");
        }
        const std::pair<std::uint64_t, std::uint64_t> canon =
            canonicalize_hero_and_board(hero_mask, board_mask);
        *out_hero_mask = canon.first;
        *out_board_mask = canon.second;
        return XIAPL_OK;
    });
}

std::int32_t xiapl_canonicalize_hero_and_board_v(std::uint64_t hero_mask,
                                                 std::uint64_t board_mask,
                                                 std::int32_t version,
                                                 std::uint64_t* out_hero_mask,
                                                 std::uint64_t* out_board_mask) {
    return wrap([&]() -> std::int32_t {
        if (out_hero_mask == nullptr) {
            return invalid_argument(
                "xiapl_canonicalize_hero_and_board_v: out_hero_mask must not be NULL");
        }
        if (out_board_mask == nullptr) {
            return invalid_argument(
                "xiapl_canonicalize_hero_and_board_v: out_board_mask must not be NULL");
        }
        const std::pair<std::uint64_t, std::uint64_t> canon =
            canonicalize_hero_and_board_v(
                hero_mask, board_mask,
                static_cast<CanonVersion>(static_cast<std::uint32_t>(version)));
        *out_hero_mask = canon.first;
        *out_board_mask = canon.second;
        return XIAPL_OK;
    });
}

std::int32_t xiapl_canonicalize_board(std::uint64_t board_mask,
                                      std::uint64_t* out_board_mask) {
    return wrap([&]() -> std::int32_t {
        if (out_board_mask == nullptr) {
            return invalid_argument("xiapl_canonicalize_board: out_board_mask must not be NULL");
        }
        *out_board_mask = canonicalize_board(board_mask);
        return XIAPL_OK;
    });
}

std::int32_t xiapl_canonicalize_hand_mask(std::uint64_t hero_mask, char* out_buf,
                                          std::int32_t buf_capacity,
                                          std::int32_t* out_length) {
    return wrap([&]() -> std::int32_t {
        if (out_length == nullptr) {
            return invalid_argument("xiapl_canonicalize_hand_mask: out_length must not be NULL");
        }
        // canonicalize_hand_mask throws std::invalid_argument unless the mask
        // has exactly two bits set (bits above 51 are ignored by mask_to_cards
        // before the arity check).
        return emit_string(canonicalize_hand_mask(hero_mask), out_buf, buf_capacity,
                           out_length);
    });
}

std::int32_t xiapl_canonicalize_hand_ids(const std::uint8_t* ids,
                                         std::int32_t count, char* out_buf,
                                         std::int32_t buf_capacity,
                                         std::int32_t* out_length) {
    return wrap([&]() -> std::int32_t {
        if (out_length == nullptr) {
            return invalid_argument("xiapl_canonicalize_hand_ids: out_length must not be NULL");
        }
        if (count < 0) {
            return invalid_argument("xiapl_canonicalize_hand_ids: count must not be negative");
        }
        if (ids == nullptr && count > 0) {
            return invalid_argument(
                "xiapl_canonicalize_hand_ids: ids must not be NULL when count > 0");
        }
        // canonicalize_hand(vector<Card>) checks the arity and nothing else:
        // a DUPLICATED card is a legal input that answers with the pair label
        // ("AA" for [As, As]). That is precisely why this id form exists
        // alongside the mask form, which cannot express a duplicate at all.
        return emit_string(canonicalize_hand(to_cards(ids, count)), out_buf,
                           buf_capacity, out_length);
    });
}

}  // extern "C"
