#pragma once

#include <bit>
#include <optional>
#include <vector>
#include <cstdint>
#include <xiapl/card.h>

namespace xiapl {

// 52-card deck bitmask
inline constexpr std::uint64_t FULL_DECK_MASK = (1ULL << 52) - 1;

// Returns true iff `id` is a valid card id (0..51). Cards constructed via
// the default Card{} (id=255) or any out-of-range integer must NOT be used
// as a shift count or array index — every public helper that does so should
// guard with this predicate.
inline constexpr bool is_valid_card_id(int id) noexcept {
    return id >= 0 && id < 52;
}
inline constexpr bool is_valid_card_id(Card::IdType id) noexcept {
    return id < 52;
}

// Index of the least-significant set bit (0..63).
//
// 0-input semantics: backed by std::countr_zero, which is total and returns
// 64 for x == 0 (defined, unlike the pre-C++20 __builtin_ctzll(0) UB this
// wrapper used to forward). Call sites are numerous (~80 across
// engine/cfr/solver/apps/tests, 2026-09-09 count) and not individually
// audited for x != 0, so this wrapper deliberately documents and keeps the
// std:: defined-for-0 behavior rather than asserting a precondition.
// Shared between simulation / canonicalize hot paths.
inline int ctz64(std::uint64_t x) noexcept {
    return std::countr_zero(x);
}

// Index from the high end of the most-significant set bit.
// Defined as `63 - position_of_highest_set_bit` for clz64 (so a single set
// bit at position 0 returns 63), matching __builtin_clzll's semantics for
// x != 0.
//
// Precondition: x != 0 (call site audited 2026-09-09 — the sole caller,
// canonicalize.cpp's pack_hero_board(), passes a 2-card hero mask that is
// never empty). std::countl_zero making 0 a defined input (returns 64) does
// not relax this precondition; it only means a future misuse would return a
// deterministic-but-meaningless 64 instead of being UB.
inline int clz64(std::uint64_t x) noexcept {
    return std::countl_zero(x);
}

// 32-bit clz. Matches __builtin_clz's semantics for x != 0.
//
// Precondition: x != 0 (call sites audited 2026-09-09 — all four callers,
// in src/core/eval_core.h::score_top_rank/score_take_top and
// src/core/canonicalize.cpp, are guarded by an explicit m != 0 check or loop
// condition before the call). std::countl_zero making 0 a defined input
// (returns 32) does not relax this precondition.
inline int clz32(std::uint32_t x) noexcept {
    return std::countl_zero(x);
}

// --- Bitmask Utility -----------------------------------------------------

// Single card -> 52-bit mask
std::uint64_t card_to_mask(const Card& c);

// Multiple cards -> 52-bit mask
std::uint64_t cards_to_mask(const std::vector<Card>& cards);

// Card ID vector -> 52-bit mask. Throws std::invalid_argument on any id
// outside [0, 52).
std::uint64_t ids_to_mask(const std::vector<int>& ids);

// 52-bit mask -> list of Cards
std::vector<Card> mask_to_cards(std::uint64_t mask);

// 52-bit mask -> list of card IDs (no Card construction)
std::vector<int> mask_to_ids(std::uint64_t mask);

// Check if two masks share any set bits
bool masks_overlap(std::uint64_t a, std::uint64_t b);

inline int popcount64(std::uint64_t x) {
    return std::popcount(x);
}

// Convert rank (2-14) to char '2'..'9','T','J','Q','K','A'
char rank_to_char(int rank);

// Convert suit (0=c,1=d,2=h,3=s) to char 'c','d','h','s'
char suit_to_char(int suit);

// Non-throwing char->rank / char->suit parsers.
// Accept upper and lower case for rank chars T,J,Q,K,A and all suit chars.
// Returns std::nullopt on invalid input.
std::optional<int> try_rank_from_char(char c) noexcept;
std::optional<int> try_suit_from_char(char c) noexcept;

// Helper: generate next k-combination (lexicographic) for indices[0..k-1] in [0, n_total)
bool next_combination(std::vector<int>& indices, int n_total);

} // namespace xiapl

