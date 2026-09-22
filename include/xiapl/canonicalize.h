#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <utility>
#include <xiapl/card.h>

namespace xiapl {

// Which hero+board canonicalization a table, a cached feature file or a lookup
// was built with.  The numeric values are ON-DISK values (they are written into
// the headers of derived artefacts) -- never renumber them.
//
// An abstraction is only self-consistent when the artefact and the lookup agree
// on this, so it is carried explicitly rather than inferred: every generator
// stamps what it used and every consumer reads the stamp back.
// Version 1 ("legacy") was REMOVED.  It ordered suits by their TOP board rank
// and was not a canonical form: the tie-break split suit-isomorphic situations,
// so the flop's 25,989,600 (hero, board) pairs landed on 1,420,796
// representatives instead of the 1,286,792 orbits that exist.  Artefacts keyed
// by it remain correct under the build that made them, but this build cannot
// reproduce their keys, so it REFUSES a container stamped 1 rather than
// guessing.
enum class CanonVersion : std::uint32_t {
    // A true canonical form.  Per suit, board and hero rank patterns are packed
    // into one 26-bit key (board_pattern << 13) | hero_pattern; the four keys
    // are sorted DESCENDING and the i-th largest is re-emitted at suit i.  A
    // suit permutation permutes the keys, so the sorted sequence -- and hence
    // the representative -- is invariant, and the key determines the suit's
    // contribution to both masks, so distinct classes cannot collide.  Flop:
    // 1,286,792 classes; turn: 13,960,050; river: 123,156,254 (Burnside on S4).
    // Because board patterns are the primary sort key, the board half of the
    // result is exactly canonicalize_board(board_mask).
    Strict = 2,
};

// Highest version this build understands. Bump with the enum.
inline constexpr CanonVersion kCanonVersionMax = CanonVersion::Strict;

// "strict". A stable string: it is what derived artefacts record as the
// abstraction they were built under, so it is a name, not a display label.
const char* canon_version_name(CanonVersion v);

// Parse an on-disk / command-line version. Throws std::invalid_argument on
// anything this build does not implement -- guessing would silently run a
// different card abstraction than the artefact was built for. That includes
// the removed version 1 / "legacy".
CanonVersion canon_version_from_u32(std::uint32_t v);
CanonVersion canon_version_from_string(const std::string& s);

std::pair<std::vector<Card>, std::vector<Card>> canonicalize_hero_and_board_cards(
    std::vector<Card> hero_hand,
    std::vector<Card> board
);

// Accept (hero_mask, board_mask) bitmasks and return the canonicalized mask pair
// under the pre-v2 suit ordering (suits ranked by their top board rank, hero
// rank then suit index as tie-breaks).
//
// This is NO LONGER an abstraction generation -- version 1 was removed and
// cannot be selected through CanonVersion any more.  The routine itself is
// frozen and stays because callers depend on this exact relabelling and on
// nothing else: the public card-level API (canonicalize_hero_and_board_cards)
// and detail::legacy_canonicalize_board, which artefacts built before the
// removal are keyed by.  For an abstraction-keyed lookup use
// canonicalize_hero_and_board_strict / _v.
std::pair<std::uint64_t, std::uint64_t> canonicalize_hero_and_board(
    std::uint64_t hero_mask,
    std::uint64_t board_mask
);

// CanonVersion::Strict -- see the enum for the representative and the class
// counts.  Any board size is accepted (flop/turn/river share this code); the
// routine only ever relabels suits.
std::pair<std::uint64_t, std::uint64_t> canonicalize_hero_and_board_strict(
    std::uint64_t hero_mask,
    std::uint64_t board_mask
);

// Dispatch on an explicit version. Throws std::invalid_argument on an unknown
// one rather than falling back to a default.
std::pair<std::uint64_t, std::uint64_t> canonicalize_hero_and_board_v(
    std::uint64_t hero_mask,
    std::uint64_t board_mask,
    CanonVersion version
);

using HeroBoardMask = std::pair<std::uint64_t, std::uint64_t>;

// board_size: 3(Flop), 4(Turn), 5(River)
//
// `version` selects the canonicalization the enumeration dedups by, and hence
// the population size.  Only Strict remains (flop 1,286,792); the removed
// legacy version's population of 1,420,796 is not reproducible from this build.
std::vector<HeroBoardMask> generate_canonical_situations(
    int board_size,
    CanonVersion version = CanonVersion::Strict);

// Canonicalize a board mask alone (no hero cards).
//
// Representative: the four 13-bit per-suit rank patterns are sorted in
// DESCENDING numeric order and the i-th largest is re-emitted at suit i.  A
// suit permutation permutes the patterns, so the sorted sequence -- and hence
// the representative -- is invariant, and two boards share a representative if
// and only if they are suit-isomorphic.  That makes this a true canonical form:
// the 22,100 flops collapse to 1,755 classes, the 270,725 turns to 16,432 and
// the 2,598,960 rivers to 134,459.  Any card count is accepted; the routine
// only ever relabels suits, never ranks.
std::uint64_t canonicalize_board(std::uint64_t board_mask);

// canonicalize_board, additionally reporting the relabelling it applied:
// suit_map_out[original_suit] = canonical_suit.  Suits carrying identical
// patterns are interchangeable, so which of them lands first is arbitrary; the
// original relative order is kept, making the map deterministic.  Callers that
// have to carry hole cards or per-suit statistics across the canonicalization
// need this permutation, which the mask alone cannot supply.
std::uint64_t canonicalize_board_with_suit_map(std::uint64_t board_mask,
                                               int suit_map_out[4]);

// Canonicalize a board given as a Card array
std::vector<Card> canonicalize_board_cards(const std::vector<Card>& board);

namespace detail {

// Board canonicalization as it behaved before 2026-08-18: suits are ordered by
// their TOP board rank with the suit index as tie-break, i.e. the hero-less
// case of canonicalize_hero_and_board.  It is NOT a canonical form -- Ah Kh Ad
// and Ad Kd Ah are the same board up to relabelling, yet both have two suits
// topped by an ace and the tie-break sends them to different representatives,
// splitting the flops into 1,833 classes instead of 1,755.  Under-merging is
// self-consistent (a lookup canonicalizes the same way it stored), so frozen
// artefacts built on it are correct, merely redundant.
//
// Consumers of those artefacts pin this entry point so that fixing the public
// routine cannot move them.  New code should use canonicalize_board.
std::uint64_t legacy_canonicalize_board(std::uint64_t board_mask);

} // namespace detail

// Canonicalize a 2-card hand to "AKs" / "AKo" / "TT" notation
std::string canonicalize_hand(const std::vector<Card>& hand);

// Canonicalize a 2-card hand mask to "AKs" notation
std::string canonicalize_hand_mask(std::uint64_t hero_mask);

} // namespace xiapl

