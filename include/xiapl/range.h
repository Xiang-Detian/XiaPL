#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <xiapl/game_type.h>

namespace xiapl {

struct Combo {
    std::uint64_t mask = 0;
    double weight = 1.0;
};

class Range {
public:
    Range() = default;
    // Legacy raw-combo path: keeps Holdem semantics, so every combo mask
    // must have cards_per_hand(GameType::Holdem) == 2 bits set (validated
    // the same way as the GameType-tagged constructor below).
    explicit Range(std::vector<Combo> combos);
    // Every combo mask must have popcount == cards_per_hand(game); throws
    // std::invalid_argument (naming the offending mask) on the first
    // mismatch found.
    Range(std::vector<Combo> combos, GameType game);

    // Every combo of the game at weight 1.0: 1,326 two-card combos for
    // Hold'em, all C(52,4) = 270,725 four-card combos for PLO. The PLO
    // enumeration is colexicographic, i.e. ascending mask order, matching what
    // from_string(..., GameType::Plo) emits for the full range.
    static Range all(GameType game = GameType::Holdem);
    // Delegates to from_string(text, GameType::Holdem); bit-for-bit
    // identical to calling the 2-arg overload explicitly.
    static Range from_string(std::string_view text);
    // GameType::Holdem parses Hold'em notation; GameType::Plo parses the
    // frozen v0.1 PLO notation: comma-separated items of 4 rank symbols
    // ('2'-'9', 'T', 'J', 'Q', 'K', 'A' or '*' wildcard, any written order)
    // with an optional "ds" / "ss" / "r" suit suffix, exact 4-card hands
    // ("AsKsQhJd"), progressions ("JJ**+", "JT98-8765") and ":w" weights in
    // (0.0, 1.0]. A trailing "-" is the downward progression, the mirror of
    // "+": "8765-" is {8765, 7654, 6543, 5432}, i.e. shift every rank down
    // until one would leave [2,14]. Matching is multiset containment, so
    // "AA**" means "at least two aces". Items are unioned; a combo reached
    // twice with equal parsed weight merges silently, conflicting weights
    // throw. Result combos are sorted ascending by mask. Throws
    // std::invalid_argument (message always pure ASCII) on malformed input.
    // A well-formed but unsatisfiable pattern is NOT malformed input: it
    // expands to zero combos ("AAAKds" -- three aces need three suits, so the
    // double-suited {2,2} histogram is unreachable), so callers that care
    // about an empty result must check size() / empty().
    static Range from_string(std::string_view text, GameType game);

    const std::vector<Combo>& combos() const { return combos_; }
    std::vector<Combo> valid_combos(std::uint64_t dead_mask) const;
    double total_weight(std::uint64_t dead_mask = 0) const;

    bool empty() const { return combos_.empty(); }
    std::size_t size() const { return combos_.size(); }
    GameType game() const noexcept { return game_; }

private:
    std::vector<Combo> combos_;
    GameType game_ = GameType::Holdem;
};

// Weighted set algebra over GameType-tagged ranges.
//
// A weight is read as the frequency with which the combo is in the range,
// so the operations are the max/min lattice:
//
//   union         w = max(wa, wb)
//   intersection  w = min(wa, wb)   (combo must be present in both)
//   difference    w = max(0, wa - wb)   (combo dropped when it reaches 0)
//
// Every candidate ruleset (max/min, product, bounded sum) agrees whenever
// one operand is unweighted -- the overwhelmingly common case. max/min is
// chosen for the weighted-vs-weighted case because it is the only one that
// keeps `a | a == a` and `a & a == a`. Note the price of the difference
// rule: `(a - b) | (a & b) == a` does NOT hold for weighted operands.
//
// Both operands must carry the same GameType tag; a mismatch throws
// std::invalid_argument naming both games. The result carries that tag and
// is sorted ascending by mask, so downstream seeded Monte Carlo never
// depends on how the operands were spelled (same guarantee as Range::all
// and the PLO parser). A duplicated mask inside either operand throws
// std::invalid_argument: only the raw-combo constructor can build one, and
// there is no honest answer for what a set operation should do with it.
Range range_union(const Range& a, const Range& b);
Range range_intersection(const Range& a, const Range& b);
Range range_difference(const Range& a, const Range& b);

inline Range operator|(const Range& a, const Range& b) { return range_union(a, b); }
inline Range operator&(const Range& a, const Range& b) { return range_intersection(a, b); }
inline Range operator-(const Range& a, const Range& b) { return range_difference(a, b); }

// Canonical Hold'em starting-hand labels ("AA", "AKs", "AQo", ...) ranked by
// EXACT equity against a uniformly random single opponent and a uniformly
// random five-card board, strongest first, truncated to the top
// `top_percent` of the 169 LABELS (not of the 1,326 combos). The count kept
// is floor(169 * top_percent), so top_percent below 1/169 returns an empty
// vector; rank_starting_hands(1.0) is the full ranking.
//
// Deterministic: backed by a compile-time table generated once by
// apps/gen_preflop_rank.cpp (exact enumeration). No RNG, no seed, no
// platform variance.
//
// Honest reading of the metric: vs-random equity is a WEAK notion of
// preflop strength. It over-ranks small pairs and offsuit aces and
// under-ranks suited connectors -- this is "hand strength in a vacuum",
// not an opening range; do not present it as one.
//
// top_percent must be in (0.0, 1.0]; anything else throws
// std::invalid_argument. Only GameType::Holdem is ranked in 0.1;
// GameType::Plo throws std::invalid_argument naming the roadmap item.
std::vector<std::string> rank_starting_hands(double top_percent = 1.0,
                                             GameType game = GameType::Holdem);

// The same selection as a Range at weight 1.0 (every combo of every kept
// label), ready for equity calculation. Equivalent to
// Range::from_string(join(rank_starting_hands(p), ","), game).
Range generate_top_percent_range(double top_percent,
                                 GameType game = GameType::Holdem);

std::optional<Range> try_parse_range(std::string_view text);

} // namespace xiapl
