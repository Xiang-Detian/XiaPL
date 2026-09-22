#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <xiapl/utils.h>

namespace xiapl {
namespace internal {

// Internal PLO rank-pattern engine: the matching predicate and the reference
// expansion that the PLO range-string parser is built on. Not installed and
// not part of the public API -- the parser (xiapl::Range) is the public surface.

// Suit-shape constraint attached to a rank pattern. Each qualifier is an
// EXACT constraint on the hand's suit histogram, i.e. the four cards' per-suit
// multiplicities sorted descending:
//
//   DoubleSuited  {2,2}      "ds"
//   SingleSuited  {2,1,1}    "ss"
//   Rainbow       {1,1,1,1}  "r"
//   Any           no constraint
//
// The remaining two shapes -- monotone {4} and three-one {3,1} -- are matched
// by Any only; PPT-style notation has no qualifier for them. The qualifiers
// therefore do NOT partition the space in general: ds + ss + r == Any holds
// only for rank patterns that make {4} and {3,1} unreachable (e.g. two-pair
// patterns such as AAKK), and falls short of Any otherwise (AKQJ: 36 + 144 +
// 24 = 204 of 256, the missing 52 being 4 monotone + 48 three-one).
enum class SuitQualifier : std::uint8_t {
    Any,
    DoubleSuited,
    SingleSuited,
    Rainbow,
};

// One 4-slot rank pattern. ranks[] holds rank values 2..14 for literals and 0
// for a '*' wildcard, stored as a canonicalized multiset: descending, so the
// wildcard 0s trail. Canonical form exists for equality/dedup by the parser;
// plo_pattern_matches() itself is order-agnostic (it counts per rank, not per
// slot), so a hand-built non-canonical aggregate still matches identically.
struct PloPattern {
    std::array<std::uint8_t, 4> ranks{{0, 0, 0, 0}};  // canonical: desc, 0s last
    SuitQualifier suit = SuitQualifier::Any;
};

// Canonicalizing factory: sorts the four slots descending, which places the
// wildcard 0s last because every literal rank is >= 2.
//
// Preconditions (asserted): every slot is either 0 (wildcard) or a rank value
// in [2,14]. Out-of-domain values are silently destructive rather than merely
// wrong -- rank 1 sorts below every literal and degrades into a wildcard (so
// "1***" would expand to all 270,725 hands), and rank > 14 indexes past the
// required[] histogram in plo_pattern_matches. User-facing validation is the
// parser's job (task 3); this assert is the engine's internal contract.
inline PloPattern make_plo_pattern(std::array<std::uint8_t, 4> ranks,
                                   SuitQualifier suit = SuitQualifier::Any) {
    for (std::uint8_t r : ranks) {
        assert(r == 0 || (r >= 2 && r <= 14));
        (void)r;
    }
    std::sort(ranks.begin(), ranks.end(), std::greater<std::uint8_t>());
    PloPattern p;
    p.ranks = ranks;
    p.suit = suit;
    return p;
}

// True iff the four cards' suit multiplicities, sorted descending, satisfy
// `q`. `suit_counts` is indexed by suit (0=c,1=d,2=h,3=s).
//
// Preconditions (asserted): the multiplicities sum to 4 (they describe a
// 4-card hand). Any is the single unconstrained exit, taken before the sort;
// every path below it tests one exact histogram shape.
inline bool plo_suit_qualifier_matches(const std::array<int, 4>& suit_counts,
                                       SuitQualifier q) {
    if (q == SuitQualifier::Any) return true;
    std::array<int, 4> h = suit_counts;
    std::sort(h.begin(), h.end(), std::greater<int>());
    assert(h[0] + h[1] + h[2] + h[3] == 4);
    if (q == SuitQualifier::DoubleSuited) {
        return h[0] == 2 && h[1] == 2;  // {2,2,0,0}
    }
    if (q == SuitQualifier::SingleSuited) {
        return h[0] == 2 && h[1] == 1 && h[2] == 1;  // {2,1,1,0}
    }
    assert(q == SuitQualifier::Rainbow);
    return h[0] == 1 && h[1] == 1 && h[2] == 1 && h[3] == 1;
}

// Multiset-containment match (PPT semantics). The hand `mask` matches iff
// there is an injective assignment of every non-wildcard slot to a distinct
// card of that rank, with the wildcards absorbing the remaining cards, AND the
// hand's suit histogram satisfies p.suit.
//
// Preconditions (asserted): `mask` holds exactly four cards AND every set bit
// is a valid card id (< 52, i.e. mask & ~FULL_DECK_MASK == 0). Both halves are
// load-bearing: a popcount-4 mask carrying a bit >= 52 passes the popcount
// check and then indexes suit_count out of bounds via id/13 >= 4. Callers
// enumerate real deck combos, so this is a contract, not input validation.
//
// Containment reduction: the assignment exists iff, for every rank r,
//   required(r) = #{slots holding literal r}  <=  count_in_hand(r).
// Necessity is immediate (distinct cards per slot). Sufficiency: the slots
// requiring r can only be filled by cards of rank r, and those groups are
// disjoint across ranks, so per-rank feasibility is Hall's condition here;
// the leftover cards (exactly 4 - #literal slots of them) go to the
// wildcards. NOTE this is deliberately NOT "sort the hand's ranks and compare
// positionally": "AK**" must accept AAKT (count(A)=2 >= 1, count(K)=1 >= 1),
// which positional comparison rejects. |AK**| = 17,316 with containment.
inline bool plo_pattern_matches(const PloPattern& p, std::uint64_t mask) {
    assert(popcount64(mask) == 4 && (mask & ~FULL_DECK_MASK) == 0);

    // Hand histograms. rank_count is indexed by rank value (2..14).
    std::array<int, 15> rank_count{};
    std::array<int, 4> suit_count{{0, 0, 0, 0}};
    std::uint64_t rest = mask;
    while (rest != 0) {
        const int id = ctz64(rest);
        rest &= rest - 1;  // clear lowest set bit
        // id = suit * 13 + (rank - 2)
        suit_count[static_cast<std::size_t>(id / 13)] += 1;
        rank_count[static_cast<std::size_t>(id % 13 + 2)] += 1;
    }

    // Required rank multiplicities from the pattern's literal slots.
    std::array<int, 15> required{};
    for (std::uint8_t r : p.ranks) {
        if (r == 0) continue;  // wildcard
        assert(r >= 2 && r <= 14);
        required[static_cast<std::size_t>(r)] += 1;
    }

    for (std::size_t r = 2; r <= 14; ++r) {
        if (required[r] > rank_count[r]) return false;
    }
    return plo_suit_qualifier_matches(suit_count, p.suit);
}

// Reference expansion: test the predicate against every 4-card combination of
// the 52-card deck and collect the matches. Deliberately exhaustive rather
// than clever -- auditability is the point, and this is the oracle that any
// future group-major/indexed expansion must reproduce exactly.
//
// Loop order note: the outermost loop runs the HIGHEST card id, so combos come
// out in colexicographic order, which for 4-bit masks is exactly ascending
// numeric order (the highest set bit dominates the mask value). The intuitive
// lowest-card-outermost nesting is lexicographic and is NOT numerically
// ascending -- e.g. {0,1,2,51} would precede {0,1,3,4}.
inline std::vector<std::uint64_t> expand_plo_pattern(const PloPattern& p) {
    std::vector<std::uint64_t> out;
    for (int c3 = 3; c3 < 52; ++c3) {
        const std::uint64_t m3 = 1ULL << c3;
        for (int c2 = 2; c2 < c3; ++c2) {
            const std::uint64_t m2 = m3 | (1ULL << c2);
            for (int c1 = 1; c1 < c2; ++c1) {
                const std::uint64_t m1 = m2 | (1ULL << c1);
                for (int c0 = 0; c0 < c1; ++c0) {
                    const std::uint64_t mask = m1 | (1ULL << c0);
                    if (plo_pattern_matches(p, mask)) out.push_back(mask);
                }
            }
        }
    }
    return out;
}

}  // namespace internal
}  // namespace xiapl
