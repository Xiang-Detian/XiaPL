// C ABI (waist v1) -- handle half: Range, Deck, fixed-hand equity,
// range-vs-range equity and canonical-situation enumeration.
//
// Like the scalar half, every function here is a mechanical projection of the
// public C++ API onto the frozen contract in <xiapl/c_api.h>. The waist
// validates only what that header says it validates -- NULL pointers where one
// is required, negative counts, unknown enum values, and the two capacity
// rules the header spells out (judge-style bounded output, and dealing, which
// mutates and therefore checks BEFORE touching the deck). Everything else is
// delegated, so the C++ exception type and its what() are what the caller
// sees.
//
// Handles are thin owning wrappers around the C++ value type. `xiapl_range`,
// `xiapl_range_equity` and `xiapl_canonical_situations` are immutable after
// creation (convention 11: safe to read concurrently); `xiapl_deck` is mutable
// and is not internally synchronized.
//
// Internal worker threads need no special treatment: src/core/mc_chunking.h
// stores each chunk's exception and rethrows it ON THE CALLING THREAD after
// the join, so wrap() at this boundary already satisfies convention 8(a).

#include <xiapl/c_api.h>

#include "waist_error.h"
#include "waist_internal.h"

#include <xiapl/canonicalize.h>
#include <xiapl/card.h>
#include <xiapl/deck.h>
#include <xiapl/game_type.h>
#include <xiapl/range.h>
#include <xiapl/simulation.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace xiapl;
using waist::emit_string;
using waist::invalid_argument;
using waist::load_options;
using waist::to_cards;
using waist::to_game_type;
using waist::wrap;

// ---------------------------------------------------------------------------
// Handle definitions
//
// One C++ value per handle, nothing else: the handle adds no state the C++
// type does not have, so there is no second source of truth to keep in sync.
// ---------------------------------------------------------------------------
struct xiapl_range {
    Range impl;
};

struct xiapl_deck {
    Deck impl;
};

struct xiapl_range_equity {
    RangeEquityResult impl;
};

struct xiapl_canonical_situations {
    std::vector<HeroBoardMask> impl;
};

namespace {

// ---- Output helpers (boundary convention 4) -------------------------------

// Parallel columns: `*out_total` is always the full count, each column pointer
// may independently be NULL, and a short buffer is not an error.
std::int32_t emit_combos(const std::vector<Combo>& combos,
                         std::uint64_t* out_masks, double* out_weights,
                         std::int32_t combos_capacity, std::int32_t* out_total) {
    *out_total = static_cast<std::int32_t>(combos.size());
    if ((out_masks == nullptr && out_weights == nullptr) || combos_capacity <= 0) {
        return XIAPL_OK;
    }

    const std::size_t n =
        std::min(combos.size(), static_cast<std::size_t>(combos_capacity));
    for (std::size_t i = 0; i < n; ++i) {
        if (out_masks != nullptr) out_masks[i] = combos[i].mask;
        if (out_weights != nullptr) out_weights[i] = combos[i].weight;
    }
    return XIAPL_OK;
}

std::int32_t emit_entries(const std::vector<RangeEquityEntry>& entries,
                          std::uint64_t* out_masks, double* out_equities,
                          double* out_weights, std::int32_t entries_capacity,
                          std::int32_t* out_total) {
    *out_total = static_cast<std::int32_t>(entries.size());
    if ((out_masks == nullptr && out_equities == nullptr && out_weights == nullptr) ||
        entries_capacity <= 0) {
        return XIAPL_OK;
    }

    const std::size_t n =
        std::min(entries.size(), static_cast<std::size_t>(entries_capacity));
    for (std::size_t i = 0; i < n; ++i) {
        if (out_masks != nullptr) out_masks[i] = entries[i].combo_mask;
        if (out_equities != nullptr) out_equities[i] = entries[i].equity;
        if (out_weights != nullptr) out_weights[i] = entries[i].weight;
    }
    return XIAPL_OK;
}

std::int32_t emit_ids(const std::vector<int>& ids, std::uint8_t* out_ids,
                      std::int32_t ids_capacity, std::int32_t* out_total) {
    *out_total = static_cast<std::int32_t>(ids.size());
    if (out_ids == nullptr || ids_capacity <= 0) return XIAPL_OK;

    const std::size_t n =
        std::min(ids.size(), static_cast<std::size_t>(ids_capacity));
    for (std::size_t i = 0; i < n; ++i) {
        out_ids[i] = static_cast<std::uint8_t>(ids[i]);
    }
    return XIAPL_OK;
}

// ---- Type conversion ------------------------------------------------------

bool to_range_equity_mode(std::int32_t mode, RangeEquityMode* out) {
    switch (mode) {
    case XIAPL_RANGE_EQUITY_PER_COMBO:      *out = RangeEquityMode::PerCombo;      return true;
    case XIAPL_RANGE_EQUITY_AGGREGATE_ONLY: *out = RangeEquityMode::AggregateOnly; return true;
    default:                                return false;
    }
}

// to_cards (id array -> std::vector<Card>) is shared with waist_scalar.cpp;
// its single implementation lives in waist_internal.h.

void store_range_equity_summary(const RangeEquityResult& result,
                                xiapl_range_equity_summary_t* out) {
    out->hero_aggregate_equity = result.hero_aggregate_equity;
    out->villain_aggregate_equity = result.villain_aggregate_equity;
    out->aggregate_std_error = result.aggregate_std_error;
    out->trials = result.trials;
    out->exact = result.exact ? 1 : 0;
    out->reserved = 0;
}

}  // namespace

extern "C" {

// ===========================================================================
// Range
// ===========================================================================

std::int32_t xiapl_range_create(xiapl_range_t** out_range) {
    return wrap([&]() -> std::int32_t {
        if (out_range == nullptr) {
            return invalid_argument("xiapl_range_create: out_range must not be NULL");
        }
        // Every handle-producing function clears the out slot first, so that
        // `rc == XIAPL_OK && *out != NULL` is a valid success test on all of
        // them, not only on xiapl_try_parse_range where the header names it.
        *out_range = nullptr;
        *out_range = new xiapl_range{Range()};
        return XIAPL_OK;
    });
}

std::int32_t xiapl_range_create_from_combos(const std::uint64_t* masks,
                                            const double* weights,
                                            std::int32_t count,
                                            std::int32_t game,
                                            xiapl_range_t** out_range) {
    return wrap([&]() -> std::int32_t {
        if (out_range == nullptr) {
            return invalid_argument("xiapl_range_create_from_combos: out_range must not be NULL");
        }
        *out_range = nullptr;
        if (count < 0) {
            return invalid_argument("xiapl_range_create_from_combos: count must not be negative");
        }
        if (masks == nullptr && count > 0) {
            return invalid_argument(
                "xiapl_range_create_from_combos: masks must not be NULL when count > 0");
        }
        GameType game_type = GameType::Holdem;
        if (!to_game_type(game, &game_type)) {
            return invalid_argument("xiapl_range_create_from_combos: unknown game");
        }
        std::vector<Combo> combos;
        combos.reserve(static_cast<std::size_t>(count));
        for (std::int32_t i = 0; i < count; ++i) {
            // A NULL weight column means weight 1.0 for every combo, which is
            // also Combo's own default.
            Combo combo;
            combo.mask = masks[i];
            if (weights != nullptr) combo.weight = weights[i];
            combos.push_back(combo);
        }
        // The GameType-tagged constructor validates the popcount of every
        // mask and names the offending one.
        *out_range = new xiapl_range{Range(std::move(combos), game_type)};
        return XIAPL_OK;
    });
}

std::int32_t xiapl_range_create_from_string(const char* text, std::int32_t game,
                                            xiapl_range_t** out_range) {
    return wrap([&]() -> std::int32_t {
        if (out_range == nullptr) {
            return invalid_argument("xiapl_range_create_from_string: out_range must not be NULL");
        }
        *out_range = nullptr;
        if (text == nullptr) {
            return invalid_argument("xiapl_range_create_from_string: text must not be NULL");
        }
        GameType game_type = GameType::Holdem;
        if (!to_game_type(game, &game_type)) {
            return invalid_argument("xiapl_range_create_from_string: unknown game");
        }
        *out_range = new xiapl_range{
            Range::from_string(std::string_view(text), game_type)};
        return XIAPL_OK;
    });
}

std::int32_t xiapl_range_create_all(std::int32_t game, xiapl_range_t** out_range) {
    return wrap([&]() -> std::int32_t {
        if (out_range == nullptr) {
            return invalid_argument("xiapl_range_create_all: out_range must not be NULL");
        }
        *out_range = nullptr;
        GameType game_type = GameType::Holdem;
        if (!to_game_type(game, &game_type)) {
            return invalid_argument("xiapl_range_create_all: unknown game");
        }
        *out_range = new xiapl_range{Range::all(game_type)};
        return XIAPL_OK;
    });
}

std::int32_t xiapl_try_parse_range(const char* text, xiapl_range_t** out_range) {
    return wrap([&]() -> std::int32_t {
        if (out_range == nullptr) {
            return invalid_argument("xiapl_try_parse_range: out_range must not be NULL");
        }
        *out_range = nullptr;
        if (text == nullptr) {
            return invalid_argument("xiapl_try_parse_range: text must not be NULL");
        }
        // "This is not a range" is an ANSWER: OK plus a NULL handle, and the
        // error slot is deliberately left alone -- nothing failed. Non-parser
        // failures (std::bad_alloc) still propagate out of try_parse_range and
        // are classified by wrap().
        std::optional<Range> parsed = try_parse_range(std::string_view(text));
        if (!parsed) return XIAPL_OK;
        *out_range = new xiapl_range{std::move(*parsed)};
        return XIAPL_OK;
    });
}

void xiapl_range_destroy(xiapl_range_t* range) {
    delete range;
}

std::int32_t xiapl_range_size(const xiapl_range_t* range, std::int32_t* out_size) {
    return wrap([&]() -> std::int32_t {
        if (range == nullptr) {
            return invalid_argument("xiapl_range_size: range must not be NULL");
        }
        if (out_size == nullptr) {
            return invalid_argument("xiapl_range_size: out_size must not be NULL");
        }
        *out_size = static_cast<std::int32_t>(range->impl.size());
        return XIAPL_OK;
    });
}

std::int32_t xiapl_range_empty(const xiapl_range_t* range, std::int32_t* out_empty) {
    return wrap([&]() -> std::int32_t {
        if (range == nullptr) {
            return invalid_argument("xiapl_range_empty: range must not be NULL");
        }
        if (out_empty == nullptr) {
            return invalid_argument("xiapl_range_empty: out_empty must not be NULL");
        }
        *out_empty = range->impl.empty() ? 1 : 0;
        return XIAPL_OK;
    });
}

std::int32_t xiapl_range_game(const xiapl_range_t* range, std::int32_t* out_game) {
    return wrap([&]() -> std::int32_t {
        if (range == nullptr) {
            return invalid_argument("xiapl_range_game: range must not be NULL");
        }
        if (out_game == nullptr) {
            return invalid_argument("xiapl_range_game: out_game must not be NULL");
        }
        *out_game = static_cast<std::int32_t>(range->impl.game());
        return XIAPL_OK;
    });
}

std::int32_t xiapl_range_combos(const xiapl_range_t* range,
                                std::uint64_t* out_masks, double* out_weights,
                                std::int32_t combos_capacity,
                                std::int32_t* out_total) {
    return wrap([&]() -> std::int32_t {
        if (range == nullptr) {
            return invalid_argument("xiapl_range_combos: range must not be NULL");
        }
        if (out_total == nullptr) {
            return invalid_argument("xiapl_range_combos: out_total must not be NULL");
        }
        return emit_combos(range->impl.combos(), out_masks, out_weights,
                           combos_capacity, out_total);
    });
}

std::int32_t xiapl_range_valid_combos(const xiapl_range_t* range,
                                      std::uint64_t dead_mask,
                                      std::uint64_t* out_masks,
                                      double* out_weights,
                                      std::int32_t combos_capacity,
                                      std::int32_t* out_total) {
    return wrap([&]() -> std::int32_t {
        if (range == nullptr) {
            return invalid_argument("xiapl_range_valid_combos: range must not be NULL");
        }
        if (out_total == nullptr) {
            return invalid_argument("xiapl_range_valid_combos: out_total must not be NULL");
        }
        return emit_combos(range->impl.valid_combos(dead_mask), out_masks,
                           out_weights, combos_capacity, out_total);
    });
}

std::int32_t xiapl_range_total_weight(const xiapl_range_t* range,
                                      std::uint64_t dead_mask,
                                      double* out_weight) {
    return wrap([&]() -> std::int32_t {
        if (range == nullptr) {
            return invalid_argument("xiapl_range_total_weight: range must not be NULL");
        }
        if (out_weight == nullptr) {
            return invalid_argument("xiapl_range_total_weight: out_weight must not be NULL");
        }
        *out_weight = range->impl.total_weight(dead_mask);
        return XIAPL_OK;
    });
}

// The three set operations are pure: they read both operands through const
// references and hand back a NEW handle, so neither operand can be observed to
// change. Game-tag mismatch and duplicated masks are the C++ routine's
// verdicts, carried through verbatim.
std::int32_t xiapl_range_union(const xiapl_range_t* a, const xiapl_range_t* b,
                               xiapl_range_t** out_range) {
    return wrap([&]() -> std::int32_t {
        if (out_range == nullptr) {
            return invalid_argument("xiapl_range_union: out_range must not be NULL");
        }
        *out_range = nullptr;
        if (a == nullptr || b == nullptr) {
            return invalid_argument("xiapl_range_union: operands must not be NULL");
        }
        *out_range = new xiapl_range{range_union(a->impl, b->impl)};
        return XIAPL_OK;
    });
}

std::int32_t xiapl_range_intersection(const xiapl_range_t* a,
                                      const xiapl_range_t* b,
                                      xiapl_range_t** out_range) {
    return wrap([&]() -> std::int32_t {
        if (out_range == nullptr) {
            return invalid_argument("xiapl_range_intersection: out_range must not be NULL");
        }
        *out_range = nullptr;
        if (a == nullptr || b == nullptr) {
            return invalid_argument("xiapl_range_intersection: operands must not be NULL");
        }
        *out_range = new xiapl_range{range_intersection(a->impl, b->impl)};
        return XIAPL_OK;
    });
}

std::int32_t xiapl_range_difference(const xiapl_range_t* a,
                                    const xiapl_range_t* b,
                                    xiapl_range_t** out_range) {
    return wrap([&]() -> std::int32_t {
        if (out_range == nullptr) {
            return invalid_argument("xiapl_range_difference: out_range must not be NULL");
        }
        *out_range = nullptr;
        if (a == nullptr || b == nullptr) {
            return invalid_argument("xiapl_range_difference: operands must not be NULL");
        }
        *out_range = new xiapl_range{range_difference(a->impl, b->impl)};
        return XIAPL_OK;
    });
}

// ===========================================================================
// Top-percent starting hands
// ===========================================================================

std::int32_t xiapl_rank_starting_hands(double top_percent, std::int32_t game,
                                       char* out_labels,
                                       std::int32_t labels_capacity,
                                       std::int32_t* out_total) {
    return wrap([&]() -> std::int32_t {
        if (out_total == nullptr) {
            return invalid_argument("xiapl_rank_starting_hands: out_total must not be NULL");
        }
        GameType game_type = GameType::Holdem;
        if (!to_game_type(game, &game_type)) {
            return invalid_argument("xiapl_rank_starting_hands: unknown game");
        }
        // top_percent and the "Hold'em only in 0.1" rule are the C++
        // routine's, so both messages arrive verbatim.
        const std::vector<std::string> labels =
            rank_starting_hands(top_percent, game_type);
        *out_total = static_cast<std::int32_t>(labels.size());
        if (out_labels == nullptr || labels_capacity <= 0) return XIAPL_OK;

        const std::size_t n =
            std::min(labels.size(), static_cast<std::size_t>(labels_capacity));
        for (std::size_t i = 0; i < n; ++i) {
            char* slot = out_labels + i * XIAPL_STARTING_HAND_LABEL_SIZE;
            const std::size_t room = XIAPL_STARTING_HAND_LABEL_SIZE - 1;
            const std::size_t length = std::min(room, labels[i].size());
            std::memcpy(slot, labels[i].data(), length);
            // NUL-fill the rest of the slot, not just the terminator, so the
            // fixed-stride buffer has no byte the caller cannot account for.
            std::memset(slot + length, 0, XIAPL_STARTING_HAND_LABEL_SIZE - length);
        }
        return XIAPL_OK;
    });
}

std::int32_t xiapl_generate_top_percent_range(double top_percent,
                                              std::int32_t game,
                                              xiapl_range_t** out_range) {
    return wrap([&]() -> std::int32_t {
        if (out_range == nullptr) {
            return invalid_argument("xiapl_generate_top_percent_range: out_range must not be NULL");
        }
        *out_range = nullptr;
        GameType game_type = GameType::Holdem;
        if (!to_game_type(game, &game_type)) {
            return invalid_argument("xiapl_generate_top_percent_range: unknown game");
        }
        *out_range =
            new xiapl_range{generate_top_percent_range(top_percent, game_type)};
        return XIAPL_OK;
    });
}

// ===========================================================================
// Equity: fixed hands vs a board
// ===========================================================================

std::int32_t xiapl_calculate_equity(const std::uint64_t* hole_masks,
                                    std::int32_t num_players,
                                    std::uint64_t board_mask,
                                    const xiapl_sim_options_t* options,
                                    std::int32_t game, double* out_winrate,
                                    double* out_equity, double* out_std_error,
                                    std::int32_t players_capacity,
                                    xiapl_equity_summary_t* out_summary) {
    return wrap([&]() -> std::int32_t {
        if (options == nullptr) {
            return invalid_argument("xiapl_calculate_equity: options must not be NULL");
        }
        if (num_players < 0) {
            return invalid_argument("xiapl_calculate_equity: num_players must not be negative");
        }
        if (hole_masks == nullptr && num_players > 0) {
            return invalid_argument(
                "xiapl_calculate_equity: hole_masks must not be NULL when num_players > 0");
        }
        GameType game_type = GameType::Holdem;
        if (!to_game_type(game, &game_type)) {
            return invalid_argument("xiapl_calculate_equity: unknown game");
        }
        // Output cardinality equals num_players, so there is no query pass. A
        // column the caller does not want is NULL; any column it DOES want has
        // to have room, and that is checked before any trial runs. When all
        // three columns are NULL the capacity is irrelevant.
        const bool wants_columns = out_winrate != nullptr ||
                                   out_equity != nullptr ||
                                   out_std_error != nullptr;
        if (wants_columns && players_capacity < num_players) {
            return invalid_argument(
                "xiapl_calculate_equity: players_capacity must be >= num_players");
        }
        std::vector<std::uint64_t> masks;
        masks.reserve(static_cast<std::size_t>(num_players));
        for (std::int32_t i = 0; i < num_players; ++i) {
            masks.push_back(hole_masks[i]);
        }
        // calculate_equity owns the rest of the contract: board popcount, hole
        // popcount, overlaps and the per-game seat caps.
        const EquityResult result =
            calculate_equity(masks, board_mask, load_options(*options), game_type);

        if (wants_columns) {
            const std::size_t n = std::min(
                result.players.size(),
                static_cast<std::size_t>(std::max(0, players_capacity)));
            for (std::size_t i = 0; i < n; ++i) {
                if (out_winrate != nullptr) out_winrate[i] = result.players[i].winrate;
                if (out_equity != nullptr) out_equity[i] = result.players[i].equity;
                if (out_std_error != nullptr) {
                    out_std_error[i] = result.players[i].std_error;
                }
            }
        }
        if (out_summary != nullptr) {
            out_summary->chop_rate = result.chop_rate;
            out_summary->trials = result.trials;
            out_summary->exact = result.exact ? 1 : 0;
            out_summary->reserved = 0;
        }
        return XIAPL_OK;
    });
}

// ===========================================================================
// Range-vs-range equity
// ===========================================================================

std::int32_t xiapl_calculate_range_equity(const xiapl_range_t* hero_range,
                                          const xiapl_range_t* villain_range,
                                          std::uint64_t board_mask,
                                          const xiapl_sim_options_t* options,
                                          std::int32_t mode,
                                          xiapl_range_equity_t** out_result) {
    return wrap([&]() -> std::int32_t {
        if (out_result == nullptr) {
            return invalid_argument("xiapl_calculate_range_equity: out_result must not be NULL");
        }
        *out_result = nullptr;
        if (hero_range == nullptr || villain_range == nullptr) {
            return invalid_argument("xiapl_calculate_range_equity: ranges must not be NULL");
        }
        if (options == nullptr) {
            return invalid_argument("xiapl_calculate_range_equity: options must not be NULL");
        }
        RangeEquityMode equity_mode = RangeEquityMode::PerCombo;
        if (!to_range_equity_mode(mode, &equity_mode)) {
            return invalid_argument("xiapl_calculate_range_equity: unknown mode");
        }
        // Game-tag mismatch, the 512 MB exact-cache cap and the blocking-range
        // guard are all the C++ routine's verdicts.
        RangeEquityResult result =
            calculate_range_equity(hero_range->impl, villain_range->impl,
                                   board_mask, load_options(*options), equity_mode);
        *out_result = new xiapl_range_equity{std::move(result)};
        return XIAPL_OK;
    });
}

void xiapl_range_equity_destroy(xiapl_range_equity_t* result) {
    delete result;
}

std::int32_t xiapl_range_equity_summary(const xiapl_range_equity_t* result,
                                        xiapl_range_equity_summary_t* out_summary) {
    return wrap([&]() -> std::int32_t {
        if (result == nullptr) {
            return invalid_argument("xiapl_range_equity_summary: result must not be NULL");
        }
        if (out_summary == nullptr) {
            return invalid_argument("xiapl_range_equity_summary: out_summary must not be NULL");
        }
        store_range_equity_summary(result->impl, out_summary);
        return XIAPL_OK;
    });
}

std::int32_t xiapl_range_equity_hero(const xiapl_range_equity_t* result,
                                     std::uint64_t* out_masks,
                                     double* out_equities, double* out_weights,
                                     std::int32_t entries_capacity,
                                     std::int32_t* out_total) {
    return wrap([&]() -> std::int32_t {
        if (result == nullptr) {
            return invalid_argument("xiapl_range_equity_hero: result must not be NULL");
        }
        if (out_total == nullptr) {
            return invalid_argument("xiapl_range_equity_hero: out_total must not be NULL");
        }
        return emit_entries(result->impl.hero, out_masks, out_equities,
                            out_weights, entries_capacity, out_total);
    });
}

std::int32_t xiapl_range_equity_villain(const xiapl_range_equity_t* result,
                                        std::uint64_t* out_masks,
                                        double* out_equities,
                                        double* out_weights,
                                        std::int32_t entries_capacity,
                                        std::int32_t* out_total) {
    return wrap([&]() -> std::int32_t {
        if (result == nullptr) {
            return invalid_argument("xiapl_range_equity_villain: result must not be NULL");
        }
        if (out_total == nullptr) {
            return invalid_argument("xiapl_range_equity_villain: out_total must not be NULL");
        }
        return emit_entries(result->impl.villain, out_masks, out_equities,
                            out_weights, entries_capacity, out_total);
    });
}

// ===========================================================================
// Deck
// ===========================================================================

std::int32_t xiapl_deck_create(xiapl_deck_t** out_deck) {
    return wrap([&]() -> std::int32_t {
        if (out_deck == nullptr) {
            return invalid_argument("xiapl_deck_create: out_deck must not be NULL");
        }
        *out_deck = nullptr;
        // The default Deck is sorted, not shuffled: deterministic by default.
        *out_deck = new xiapl_deck{Deck()};
        return XIAPL_OK;
    });
}

std::int32_t xiapl_deck_create_from_cards(const std::uint8_t* ids,
                                          std::int32_t count,
                                          xiapl_deck_t** out_deck) {
    return wrap([&]() -> std::int32_t {
        if (out_deck == nullptr) {
            return invalid_argument("xiapl_deck_create_from_cards: out_deck must not be NULL");
        }
        *out_deck = nullptr;
        if (count < 0) {
            return invalid_argument("xiapl_deck_create_from_cards: count must not be negative");
        }
        if (ids == nullptr && count > 0) {
            return invalid_argument(
                "xiapl_deck_create_from_cards: ids must not be NULL when count > 0");
        }
        // The Deck constructor delegates to set_cards, which rejects an
        // invalid id and a duplicate before it stores anything.
        *out_deck = new xiapl_deck{Deck(to_cards(ids, count))};
        return XIAPL_OK;
    });
}

void xiapl_deck_destroy(xiapl_deck_t* deck) {
    delete deck;
}

std::int32_t xiapl_deck_shuffle(xiapl_deck_t* deck) {
    return wrap([&]() -> std::int32_t {
        if (deck == nullptr) {
            return invalid_argument("xiapl_deck_shuffle: deck must not be NULL");
        }
        deck->impl.shuffle();
        return XIAPL_OK;
    });
}

std::int32_t xiapl_deck_shuffle_seeded(xiapl_deck_t* deck, std::uint64_t seed) {
    return wrap([&]() -> std::int32_t {
        if (deck == nullptr) {
            return invalid_argument("xiapl_deck_shuffle_seeded: deck must not be NULL");
        }
        // Seed 0 is an ordinary seed here; Deck::shuffle(seed) seeds FastRng
        // explicitly to keep it from being remapped to random_device.
        deck->impl.shuffle(seed);
        return XIAPL_OK;
    });
}

std::int32_t xiapl_deck_deal(xiapl_deck_t* deck, std::int32_t n,
                             std::uint8_t* out_ids, std::int32_t ids_capacity,
                             std::int32_t* out_count) {
    return wrap([&]() -> std::int32_t {
        if (deck == nullptr) {
            return invalid_argument("xiapl_deck_deal: deck must not be NULL");
        }
        if (out_count == nullptr) {
            return invalid_argument("xiapl_deck_deal: out_count must not be NULL");
        }
        // Dealing MUTATES, so a query pass would consume cards: the header
        // requires a real buffer with room for n, unconditionally, and both
        // checks happen before the deck is touched.
        if (out_ids == nullptr) {
            return invalid_argument("xiapl_deck_deal: out_ids must not be NULL");
        }
        if (ids_capacity < n) {
            return invalid_argument("xiapl_deck_deal: ids_capacity must be >= n");
        }
        // Deck::deal stops early when the deck runs out (not an error) and
        // deals nothing for n <= 0.
        const std::vector<Card> dealt = deck->impl.deal(static_cast<int>(n));
        *out_count = static_cast<std::int32_t>(dealt.size());
        for (std::size_t i = 0; i < dealt.size(); ++i) {
            out_ids[i] = dealt[i].id;
        }
        return XIAPL_OK;
    });
}

std::int32_t xiapl_deck_deal_mask(xiapl_deck_t* deck, std::int32_t n,
                                  std::uint64_t* out_mask) {
    return wrap([&]() -> std::int32_t {
        if (deck == nullptr) {
            return invalid_argument("xiapl_deck_deal_mask: deck must not be NULL");
        }
        if (out_mask == nullptr) {
            return invalid_argument("xiapl_deck_deal_mask: out_mask must not be NULL");
        }
        *out_mask = deck->impl.deal_mask(static_cast<int>(n));
        return XIAPL_OK;
    });
}

std::int32_t xiapl_deck_burn(xiapl_deck_t* deck, std::int32_t n) {
    return wrap([&]() -> std::int32_t {
        if (deck == nullptr) {
            return invalid_argument("xiapl_deck_burn: deck must not be NULL");
        }
        deck->impl.burn(static_cast<int>(n));
        return XIAPL_OK;
    });
}

std::int32_t xiapl_deck_reset(xiapl_deck_t* deck, std::int32_t shuffle) {
    return wrap([&]() -> std::int32_t {
        if (deck == nullptr) {
            return invalid_argument("xiapl_deck_reset: deck must not be NULL");
        }
        deck->impl.reset(shuffle != 0);
        return XIAPL_OK;
    });
}

std::int32_t xiapl_deck_remove_cards(xiapl_deck_t* deck,
                                     const std::uint8_t* ids,
                                     std::int32_t count) {
    return wrap([&]() -> std::int32_t {
        if (deck == nullptr) {
            return invalid_argument("xiapl_deck_remove_cards: deck must not be NULL");
        }
        if (count < 0) {
            return invalid_argument("xiapl_deck_remove_cards: count must not be negative");
        }
        if (ids == nullptr && count > 0) {
            return invalid_argument(
                "xiapl_deck_remove_cards: ids must not be NULL when count > 0");
        }
        // Ids that are not in the deck -- including an id outside [0, 51],
        // which can never be in it -- are skipped silently, per C++.
        deck->impl.remove_cards(to_cards(ids, count));
        return XIAPL_OK;
    });
}

std::int32_t xiapl_deck_set_cards(xiapl_deck_t* deck, const std::uint8_t* ids,
                                  std::int32_t count) {
    return wrap([&]() -> std::int32_t {
        if (deck == nullptr) {
            return invalid_argument("xiapl_deck_set_cards: deck must not be NULL");
        }
        if (count < 0) {
            return invalid_argument("xiapl_deck_set_cards: count must not be negative");
        }
        if (ids == nullptr && count > 0) {
            return invalid_argument(
                "xiapl_deck_set_cards: ids must not be NULL when count > 0");
        }
        // Deck::set_cards validates the whole list before it clears the deck,
        // so a rejected list leaves the deck as it was.
        deck->impl.set_cards(to_cards(ids, count));
        return XIAPL_OK;
    });
}

std::int32_t xiapl_deck_get_cards(const xiapl_deck_t* deck,
                                  std::uint8_t* out_ids,
                                  std::int32_t ids_capacity,
                                  std::int32_t* out_total) {
    return wrap([&]() -> std::int32_t {
        if (deck == nullptr) {
            return invalid_argument("xiapl_deck_get_cards: deck must not be NULL");
        }
        if (out_total == nullptr) {
            return invalid_argument("xiapl_deck_get_cards: out_total must not be NULL");
        }
        // Front to back: the next card dealt is the LAST entry.
        return emit_ids(deck->impl.get_card_ids(), out_ids, ids_capacity, out_total);
    });
}

std::int32_t xiapl_deck_size(const xiapl_deck_t* deck, std::int32_t* out_size) {
    return wrap([&]() -> std::int32_t {
        if (deck == nullptr) {
            return invalid_argument("xiapl_deck_size: deck must not be NULL");
        }
        if (out_size == nullptr) {
            return invalid_argument("xiapl_deck_size: out_size must not be NULL");
        }
        *out_size = static_cast<std::int32_t>(deck->impl.size());
        return XIAPL_OK;
    });
}

std::int32_t xiapl_deck_empty(const xiapl_deck_t* deck, std::int32_t* out_empty) {
    return wrap([&]() -> std::int32_t {
        if (deck == nullptr) {
            return invalid_argument("xiapl_deck_empty: deck must not be NULL");
        }
        if (out_empty == nullptr) {
            return invalid_argument("xiapl_deck_empty: out_empty must not be NULL");
        }
        *out_empty = deck->impl.empty() ? 1 : 0;
        return XIAPL_OK;
    });
}

std::int32_t xiapl_deck_has_cards(const xiapl_deck_t* deck, std::int32_t n,
                                  std::int32_t* out_has) {
    return wrap([&]() -> std::int32_t {
        if (deck == nullptr) {
            return invalid_argument("xiapl_deck_has_cards: deck must not be NULL");
        }
        if (out_has == nullptr) {
            return invalid_argument("xiapl_deck_has_cards: out_has must not be NULL");
        }
        *out_has = deck->impl.has_cards(static_cast<int>(n)) ? 1 : 0;
        return XIAPL_OK;
    });
}

std::int32_t xiapl_deck_repr(const xiapl_deck_t* deck, char* out_buf,
                             std::int32_t buf_capacity, std::int32_t* out_length) {
    return wrap([&]() -> std::int32_t {
        if (deck == nullptr) {
            return invalid_argument("xiapl_deck_repr: deck must not be NULL");
        }
        if (out_length == nullptr) {
            return invalid_argument("xiapl_deck_repr: out_length must not be NULL");
        }
        return emit_string(deck->impl.repr(), out_buf, buf_capacity, out_length);
    });
}

// ===========================================================================
// Canonical situation enumeration
// ===========================================================================

std::int32_t xiapl_generate_canonical_situations(
    std::int32_t board_size, std::int32_t version,
    xiapl_canonical_situations_t** out_situations) {
    return wrap([&]() -> std::int32_t {
        if (out_situations == nullptr) {
            return invalid_argument(
                "xiapl_generate_canonical_situations: out_situations must not be NULL");
        }
        *out_situations = nullptr;
        // board_size and version are both the C++ routine's to reject, in that
        // order; casting the version through keeps the order and the messages.
        // That pass-through is also how XIAPL_CANON_LEGACY (1) becomes
        // XIAPL_ERR_INVALID_ARGUMENT -- see the design note at the head of the
        // canonicalization section in waist_scalar.cpp.
        std::vector<HeroBoardMask> situations = generate_canonical_situations(
            static_cast<int>(board_size),
            static_cast<CanonVersion>(static_cast<std::uint32_t>(version)));
        *out_situations = new xiapl_canonical_situations{std::move(situations)};
        return XIAPL_OK;
    });
}

void xiapl_canonical_situations_destroy(xiapl_canonical_situations_t* situations) {
    delete situations;
}

std::int32_t xiapl_canonical_situations_count(
    const xiapl_canonical_situations_t* situations, std::int64_t* out_total) {
    return wrap([&]() -> std::int32_t {
        if (situations == nullptr) {
            return invalid_argument(
                "xiapl_canonical_situations_count: situations must not be NULL");
        }
        if (out_total == nullptr) {
            return invalid_argument(
                "xiapl_canonical_situations_count: out_total must not be NULL");
        }
        *out_total = static_cast<std::int64_t>(situations->impl.size());
        return XIAPL_OK;
    });
}

std::int32_t xiapl_canonical_situations_fill(
    const xiapl_canonical_situations_t* situations, std::int64_t offset,
    std::int64_t count, std::uint64_t* out_hero_masks,
    std::uint64_t* out_board_masks, std::int64_t* out_written) {
    return wrap([&]() -> std::int32_t {
        if (situations == nullptr) {
            return invalid_argument(
                "xiapl_canonical_situations_fill: situations must not be NULL");
        }
        if (out_written == nullptr) {
            return invalid_argument(
                "xiapl_canonical_situations_fill: out_written must not be NULL");
        }
        if (offset < 0) {
            return invalid_argument(
                "xiapl_canonical_situations_fill: offset must not be negative");
        }
        if (count < 0) {
            return invalid_argument(
                "xiapl_canonical_situations_fill: count must not be negative");
        }
        const std::int64_t total = static_cast<std::int64_t>(situations->impl.size());
        // An offset past the end is how a streaming loop terminates, not an
        // error: it writes nothing and reports 0.
        const std::int64_t available = offset >= total ? 0 : total - offset;
        const std::int64_t n = std::min(count, available);
        *out_written = n;
        for (std::int64_t i = 0; i < n; ++i) {
            const HeroBoardMask& pair =
                situations->impl[static_cast<std::size_t>(offset + i)];
            if (out_hero_masks != nullptr) {
                out_hero_masks[i] = pair.first;
            }
            if (out_board_masks != nullptr) {
                out_board_masks[i] = pair.second;
            }
        }
        return XIAPL_OK;
    });
}

}  // extern "C"
