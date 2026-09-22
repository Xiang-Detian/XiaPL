// Contract tests for the internal PLO rank-pattern engine
// (src/core/plo_pattern.h), task 2 of the plo-range-ws1 plan. Task 3 builds
// the PLO range-string parser directly on top of this header, so the two
// properties pinned hardest here are:
//
//   1. MULTISET CONTAINMENT matching (PPT semantics), not positional matching
//      after sorting. "AK**" must accept AAKT: sorted ranks A,A,K,T against
//      slots A,K,*,* would reject it positionally (slot 1 = A vs literal K),
//      but containment only requires count(A) >= 1 and count(K) >= 1. Every
//      downstream equity number depends on this; the |AK**| = 17,316 fixture
//      is the discriminator (positional matching yields a smaller set).
//   2. EXACT suit histograms for the qualifiers: ds = {2,2}, ss = {2,1,1},
//      r = {1,1,1,1}. Monotone {4} and three-one {3,1} match no qualifier
//      other than Any, so ds+ss+r < Any whenever those shapes are reachable.
//
// Every count below is derived by hand in the comment above its assertion so
// the fixture can be audited without running the code.
#include "doctest.h"
#include "../src/core/plo_pattern.h"

#include <xiapl/card.h>
#include <xiapl/utils.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <set>
#include <vector>

using namespace xiapl;
using xiapl::internal::PloPattern;
using xiapl::internal::SuitQualifier;
using xiapl::internal::expand_plo_pattern;
using xiapl::internal::make_plo_pattern;
using xiapl::internal::plo_pattern_matches;

namespace {

constexpr std::uint8_t kA = 14;
constexpr std::uint8_t kK = 13;
constexpr std::uint8_t kQ = 12;
constexpr std::uint8_t kJ = 11;
constexpr std::uint8_t kT = 10;
constexpr std::uint8_t kNine = 9;
constexpr std::uint8_t kEight = 8;
constexpr std::uint8_t kSeven = 7;
constexpr std::uint8_t kStar = 0;  // wildcard slot

PloPattern pattern(std::uint8_t r0, std::uint8_t r1, std::uint8_t r2,
                   std::uint8_t r3,
                   SuitQualifier suit = SuitQualifier::Any) {
    return make_plo_pattern({{r0, r1, r2, r3}}, suit);
}

std::uint64_t hand4(const char* a, const char* b, const char* c,
                    const char* d) {
    return cards_to_mask({Card::from_string(a), Card::from_string(b),
                          Card::from_string(c), Card::from_string(d)});
}

// Per-suit multiplicities of a 4-card mask, sorted descending (zeros kept):
// {4,0,0,0} monotone / {3,1,0,0} three-one / {2,2,0,0} ds / {2,1,1,0} ss /
// {1,1,1,1} rainbow. Computed independently of the header under test.
std::array<int, 4> suit_shape(std::uint64_t mask) {
    std::array<int, 4> counts{0, 0, 0, 0};
    std::uint64_t rest = mask;
    while (rest != 0) {
        const int id = ctz64(rest);
        rest &= rest - 1;
        counts[static_cast<std::size_t>(id / 13)] += 1;
    }
    std::sort(counts.begin(), counts.end(), std::greater<int>());
    return counts;
}

std::size_t count_shape(const std::vector<std::uint64_t>& masks,
                        const std::array<int, 4>& shape) {
    std::size_t n = 0;
    for (std::uint64_t m : masks) {
        if (suit_shape(m) == shape) ++n;
    }
    return n;
}

// popcount == 4, strictly ascending (hence unique). Violations are counted
// and asserted once so a broken expansion does not emit 270k CHECK lines.
void check_wellformed(const std::vector<std::uint64_t>& masks) {
    std::size_t bad_popcount = 0;
    std::size_t not_ascending = 0;
    for (std::size_t i = 0; i < masks.size(); ++i) {
        if (popcount64(masks[i]) != 4) ++bad_popcount;
        if (i > 0 && !(masks[i - 1] < masks[i])) ++not_ascending;
    }
    CHECK(bad_popcount == 0);
    CHECK(not_ascending == 0);
}

}  // namespace

// C(52,4) = 52*51*50*49 / 24 = 6497400 / 24 = 270,725.
TEST_CASE("plo_pattern_all_wildcards_is_c52_4") {
    const std::vector<std::uint64_t> all =
        expand_plo_pattern(pattern(kStar, kStar, kStar, kStar));
    CHECK(all.size() == 270725u);
    check_wellformed(all);
}

// "AA**" = hands holding at least two aces (containment: count(A) >= 2).
// Split by ace count: 2 aces C(4,2)*C(48,2) = 6*1128 = 6768;
// 3 aces C(4,3)*C(48,1) = 4*48 = 192; 4 aces C(4,4) = 1.
// 6768 + 192 + 1 = 6,961.
TEST_CASE("plo_pattern_pair_pattern_counts_at_least_two_aces") {
    const std::vector<std::uint64_t> aa =
        expand_plo_pattern(pattern(kA, kA, kStar, kStar));
    CHECK(aa.size() == 6961u);
    check_wellformed(aa);

    // Three and four aces are in, one ace is out: containment, not equality.
    CHECK(plo_pattern_matches(pattern(kA, kA, kStar, kStar),
                              hand4("Ah", "Ad", "Ac", "Ts")));
    CHECK(plo_pattern_matches(pattern(kA, kA, kStar, kStar),
                              hand4("Ah", "Ad", "Ac", "As")));
    CHECK_FALSE(plo_pattern_matches(pattern(kA, kA, kStar, kStar),
                                    hand4("Ah", "Kd", "Qc", "Ts")));
}

// THE CONTAINMENT DISCRIMINATOR. "AK**" = at least one ace AND at least one
// king. Inclusion-exclusion over C(52,4):
//   |no ace| = |no king| = C(48,4) = 194,580; |neither| = C(44,4) = 135,751.
//   270,725 - 194,580 - 194,580 + 135,751 = 17,316.
// Positional-after-sort matching would exclude AAKx / AKKx / AAKK style hands
// and land well below this number.
TEST_CASE("plo_pattern_ak_uses_multiset_containment") {
    const PloPattern ak = pattern(kA, kK, kStar, kStar);
    const std::vector<std::uint64_t> masks = expand_plo_pattern(ak);
    CHECK(masks.size() == 17316u);
    check_wellformed(masks);

    // AAKT: two aces absorb the A slot and one wildcard; the K slot is still
    // satisfiable. Explicit membership assert on the expansion, not just on
    // the predicate, so expansion and matcher are checked together.
    const std::uint64_t aakt = hand4("Ah", "Ad", "Kc", "Ts");
    CHECK(plo_pattern_matches(ak, aakt));
    CHECK(std::binary_search(masks.begin(), masks.end(), aakt));

    // Neighbours of the discriminator.
    CHECK(plo_pattern_matches(ak, hand4("Ah", "Kd", "Qc", "Ts")));   // plain
    CHECK(plo_pattern_matches(ak, hand4("Ah", "Ad", "Kc", "Ks")));   // AAKK
    CHECK(plo_pattern_matches(ak, hand4("Ah", "Kd", "Kc", "Ks")));   // AKKK
    CHECK_FALSE(plo_pattern_matches(ak, hand4("Ah", "Ad", "Qc", "Ts")));  // no K
    CHECK_FALSE(plo_pattern_matches(ak, hand4("Kh", "Kd", "Qc", "Ts")));  // no A
}

// "A***" = at least one ace = C(52,4) - C(48,4) = 270,725 - 194,580 = 76,145.
TEST_CASE("plo_pattern_single_literal_counts_at_least_one_ace") {
    const std::vector<std::uint64_t> a =
        expand_plo_pattern(pattern(kA, kStar, kStar, kStar));
    CHECK(a.size() == 76145u);
    check_wellformed(a);
}

// "AAAA": all four aces required, exactly one such 4-card hand.
TEST_CASE("plo_pattern_quads_expand_to_one_hand") {
    const std::vector<std::uint64_t> quads =
        expand_plo_pattern(pattern(kA, kA, kA, kA));
    REQUIRE(quads.size() == 1u);
    CHECK(quads[0] == hand4("Ac", "Ad", "Ah", "As"));
}

// "T987" = four distinct ranks, each suit chosen freely: 4^4 = 256.
TEST_CASE("plo_pattern_rundown_expands_to_256") {
    const std::vector<std::uint64_t> t987 =
        expand_plo_pattern(pattern(kT, kNine, kEight, kSeven));
    CHECK(t987.size() == 256u);
    check_wellformed(t987);
}

// "AAKK" (two aces + two kings; the hand is exactly those four cards):
//   Any = C(4,2) * C(4,2) = 6 * 6 = 36.
// Let S_A / S_K be the chosen suit pairs and k = |S_A n S_K|:
//   k=2 -> shape {2,2}      = ds: S_K must equal S_A -> 6.
//   k=0 -> shape {1,1,1,1}  = r : S_K is the complement of S_A -> 6.
//   k=1 -> shape {2,1,1}    = ss: 36 - 6 - 6 = 24.
// No monotone {4} or three-one {3,1} shape exists (a suit can appear at most
// twice: once as an ace, once as a king), so ds + ss + r == Any exactly.
TEST_CASE("plo_pattern_two_pair_suit_qualifiers_partition_exactly") {
    const std::size_t any =
        expand_plo_pattern(pattern(kA, kA, kK, kK)).size();
    const std::size_t ds =
        expand_plo_pattern(pattern(kA, kA, kK, kK, SuitQualifier::DoubleSuited))
            .size();
    const std::size_t ss =
        expand_plo_pattern(pattern(kA, kA, kK, kK, SuitQualifier::SingleSuited))
            .size();
    const std::size_t rb =
        expand_plo_pattern(pattern(kA, kA, kK, kK, SuitQualifier::Rainbow))
            .size();

    CHECK(any == 36u);
    CHECK(ds == 6u);
    CHECK(ss == 24u);
    CHECK(rb == 6u);
    CHECK(ds + ss + rb == any);
}

// "AKQJ" (four distinct ranks, 4^4 = 256 suit assignments):
//   {4}       monotone : 4 (one suit for all four)                     = 4
//   {3,1}     three-one: 4 triple-suits * 4 singleton ranks * 3 suits  = 48
//   {2,2}     ds       : C(4,2)=6 suit pairs * C(4,2)=6 rank splits    = 36
//   {2,1,1}   ss       : 4 * C(4,2)=6 * (3*2)=6                        = 144
//   {1,1,1,1} r        : 4! = 24
// 4 + 48 + 36 + 144 + 24 = 256, and Any - ds - ss - r = 52 = 4 + 48.
TEST_CASE("plo_pattern_four_distinct_ranks_suit_qualifier_counts") {
    const std::vector<std::uint64_t> any =
        expand_plo_pattern(pattern(kA, kK, kQ, kJ));
    const std::size_t ds =
        expand_plo_pattern(pattern(kA, kK, kQ, kJ, SuitQualifier::DoubleSuited))
            .size();
    const std::size_t ss =
        expand_plo_pattern(pattern(kA, kK, kQ, kJ, SuitQualifier::SingleSuited))
            .size();
    const std::size_t rb =
        expand_plo_pattern(pattern(kA, kK, kQ, kJ, SuitQualifier::Rainbow))
            .size();

    CHECK(any.size() == 256u);
    CHECK(ds == 36u);
    CHECK(ss == 144u);
    CHECK(rb == 24u);

    const std::size_t monotone = count_shape(any, {{4, 0, 0, 0}});
    const std::size_t three_one = count_shape(any, {{3, 1, 0, 0}});
    CHECK(monotone == 4u);
    CHECK(three_one == 48u);
    // The qualifiers do NOT partition Any here: the 52 unqualified hands are
    // exactly the monotone and three-one shapes.
    CHECK(any.size() - ds - ss - rb == 52u);
    CHECK(monotone + three_one == 52u);
}

// Suit shapes over all C(52,4) hands (rank-free, so a pure suit count):
//   {4}       4 * C(13,4) = 4 * 715                        =   2,860
//   {3,1}     4 * C(13,3) * 3 * 13 = 4*286*39              =  44,616
//   {2,2}     C(4,2) * C(13,2)^2 = 6 * 78^2                =  36,504
//   {2,1,1}   4 * C(13,2) * C(3,2) * 13^2 = 4*78*3*169     = 158,184
//   {1,1,1,1} 13^4                                         =  28,561
// Sum = 270,725 = C(52,4).
TEST_CASE("plo_pattern_wildcard_suit_shape_totals_sum_to_c52_4") {
    const std::vector<std::uint64_t> any =
        expand_plo_pattern(pattern(kStar, kStar, kStar, kStar));
    const std::size_t ds =
        expand_plo_pattern(
            pattern(kStar, kStar, kStar, kStar, SuitQualifier::DoubleSuited))
            .size();
    const std::size_t ss =
        expand_plo_pattern(
            pattern(kStar, kStar, kStar, kStar, SuitQualifier::SingleSuited))
            .size();
    const std::size_t rb =
        expand_plo_pattern(
            pattern(kStar, kStar, kStar, kStar, SuitQualifier::Rainbow))
            .size();
    const std::size_t monotone = count_shape(any, {{4, 0, 0, 0}});
    const std::size_t three_one = count_shape(any, {{3, 1, 0, 0}});

    CHECK(ds == 36504u);
    CHECK(ss == 158184u);
    CHECK(rb == 28561u);
    CHECK(monotone == 2860u);
    CHECK(three_one == 44616u);
    CHECK(ds + ss + rb + monotone + three_one == 270725u);
    CHECK(any.size() == 270725u);
}

// A hand's suit histogram is a single value, so it can satisfy at most one of
// ds / ss / r. Swept over the whole C(52,4) space.
TEST_CASE("plo_pattern_suit_qualifiers_are_mutually_disjoint") {
    const PloPattern ds =
        pattern(kStar, kStar, kStar, kStar, SuitQualifier::DoubleSuited);
    const PloPattern ss =
        pattern(kStar, kStar, kStar, kStar, SuitQualifier::SingleSuited);
    const PloPattern rb =
        pattern(kStar, kStar, kStar, kStar, SuitQualifier::Rainbow);
    const PloPattern any = pattern(kStar, kStar, kStar, kStar);

    std::size_t multi_match = 0;
    std::size_t missing_from_any = 0;
    for (std::uint64_t m : expand_plo_pattern(any)) {
        const int hits = (plo_pattern_matches(ds, m) ? 1 : 0) +
                         (plo_pattern_matches(ss, m) ? 1 : 0) +
                         (plo_pattern_matches(rb, m) ? 1 : 0);
        if (hits > 1) ++multi_match;
        if (!plo_pattern_matches(any, m)) ++missing_from_any;
    }
    CHECK(multi_match == 0u);
    CHECK(missing_from_any == 0u);
}

// Qualifier semantics on individual hands, including the two shapes that no
// qualifier other than Any accepts.
TEST_CASE("plo_pattern_suit_qualifier_semantics_on_single_hands") {
    const std::uint64_t double_suited = hand4("Ah", "Kh", "Qd", "Jd");
    const std::uint64_t single_suited = hand4("Ah", "Kh", "Qd", "Jc");
    const std::uint64_t rainbow = hand4("Ah", "Kd", "Qc", "Js");
    const std::uint64_t monotone = hand4("Ah", "Kh", "Qh", "Jh");
    const std::uint64_t three_one = hand4("Ah", "Kh", "Qh", "Jd");

    const PloPattern ds =
        pattern(kA, kK, kQ, kJ, SuitQualifier::DoubleSuited);
    const PloPattern ss =
        pattern(kA, kK, kQ, kJ, SuitQualifier::SingleSuited);
    const PloPattern rb = pattern(kA, kK, kQ, kJ, SuitQualifier::Rainbow);
    const PloPattern any = pattern(kA, kK, kQ, kJ);

    CHECK(plo_pattern_matches(ds, double_suited));
    CHECK_FALSE(plo_pattern_matches(ss, double_suited));
    CHECK_FALSE(plo_pattern_matches(rb, double_suited));

    CHECK(plo_pattern_matches(ss, single_suited));
    CHECK_FALSE(plo_pattern_matches(ds, single_suited));
    CHECK_FALSE(plo_pattern_matches(rb, single_suited));

    CHECK(plo_pattern_matches(rb, rainbow));
    CHECK_FALSE(plo_pattern_matches(ds, rainbow));
    CHECK_FALSE(plo_pattern_matches(ss, rainbow));

    // {4} and {3,1} are reachable only through Any.
    CHECK(plo_pattern_matches(any, monotone));
    CHECK_FALSE(plo_pattern_matches(ds, monotone));
    CHECK_FALSE(plo_pattern_matches(ss, monotone));
    CHECK_FALSE(plo_pattern_matches(rb, monotone));

    CHECK(plo_pattern_matches(any, three_one));
    CHECK_FALSE(plo_pattern_matches(ds, three_one));
    CHECK_FALSE(plo_pattern_matches(ss, three_one));
    CHECK_FALSE(plo_pattern_matches(rb, three_one));
}

// Rank patterns that no suit shape can satisfy expand to an EMPTY vector and
// do NOT throw. Four cards of one rank are one of each suit -> {1,1,1,1}, and
// three of a rank plus any fourth card is {2,1,1} or {1,1,1,1}; neither can be
// {2,2}. Task 3's parser relies on "unsatisfiable" being a defined, quiet
// empty result so it can union pattern expansions without special-casing.
TEST_CASE("plo_pattern_unsatisfiable_patterns_expand_to_empty") {
    const std::vector<std::uint64_t> quads_ds =
        expand_plo_pattern(pattern(kA, kA, kA, kA, SuitQualifier::DoubleSuited));
    CHECK(quads_ds.empty());

    const std::vector<std::uint64_t> trips_ds = expand_plo_pattern(
        pattern(kA, kA, kA, kStar, SuitQualifier::DoubleSuited));
    CHECK(trips_ds.empty());

    // Same trips pattern under a satisfiable qualifier is non-empty, so the
    // emptiness above is the qualifier's doing, not a broken rank pattern.
    CHECK_FALSE(
        expand_plo_pattern(pattern(kA, kA, kA, kStar, SuitQualifier::SingleSuited))
            .empty());
}

// Canonical form is pinned: ranks sorted DESCENDING with wildcard 0s trailing.
// Any input order produces the same PloPattern and the same expansion, and
// matching is order-agnostic even for a hand-built (non-canonical) aggregate,
// because containment counts per rank rather than per slot.
TEST_CASE("plo_pattern_canonicalization_is_order_invariant") {
    const PloPattern canonical = make_plo_pattern({{kA, kA, kStar, kStar}});
    CHECK(canonical.ranks[0] == kA);
    CHECK(canonical.ranks[1] == kA);
    CHECK(canonical.ranks[2] == kStar);
    CHECK(canonical.ranks[3] == kStar);
    CHECK(canonical.suit == SuitQualifier::Any);

    const std::array<std::array<std::uint8_t, 4>, 5> permutations = {{
        {{kStar, kA, kStar, kA}},
        {{kStar, kStar, kA, kA}},
        {{kA, kStar, kA, kStar}},
        {{kA, kStar, kStar, kA}},
        {{kStar, kA, kA, kStar}},
    }};
    for (const auto& perm : permutations) {
        const PloPattern p = make_plo_pattern(perm);
        CHECK(p.ranks == canonical.ranks);
        CHECK(expand_plo_pattern(p).size() == 6961u);
    }

    // Mixed ranks: descending order, wildcards last.
    const PloPattern mixed = make_plo_pattern({{kStar, kJ, kA, kQ}});
    CHECK(mixed.ranks[0] == kA);
    CHECK(mixed.ranks[1] == kQ);
    CHECK(mixed.ranks[2] == kJ);
    CHECK(mixed.ranks[3] == kStar);

    // Raw aggregate in non-canonical order still matches identically.
    const PloPattern raw{{{kStar, kA, kStar, kA}}, SuitQualifier::Any};
    const std::uint64_t aakt = hand4("Ah", "Ad", "Kc", "Ts");
    CHECK(plo_pattern_matches(raw, aakt) ==
          plo_pattern_matches(canonical, aakt));
    CHECK(expand_plo_pattern(raw) == expand_plo_pattern(canonical));
}

// Containment arithmetic cross-check without a parser:
//   A = "AA**" (>= 2 aces) = 6,961
//   B = "AK**" (>= 1 ace and >= 1 king) = 17,316
//   A n B = (>= 2 aces AND >= 1 king), enumerated by hand:
//     2 aces + 1 king + 1 other:  C(4,2) * C(4,1) * C(44,1) = 6*4*44 = 1,056
//     2 aces + 2 kings:           C(4,2) * C(4,2)           = 6*6    =    36
//     3 aces + 1 king:            C(4,3) * C(4,1)           = 4*4    =    16
//     total = 1,108
//   |A u B| = 6,961 + 17,316 - 1,108 = 23,169.
TEST_CASE("plo_pattern_inclusion_exclusion_cross_check") {
    const std::vector<std::uint64_t> aa =
        expand_plo_pattern(pattern(kA, kA, kStar, kStar));
    const std::vector<std::uint64_t> ak =
        expand_plo_pattern(pattern(kA, kK, kStar, kStar));

    const std::set<std::uint64_t> set_aa(aa.begin(), aa.end());
    const std::set<std::uint64_t> set_ak(ak.begin(), ak.end());

    std::vector<std::uint64_t> union_masks;
    std::set_union(set_aa.begin(), set_aa.end(), set_ak.begin(), set_ak.end(),
                   std::back_inserter(union_masks));
    std::vector<std::uint64_t> intersection_masks;
    std::set_intersection(set_aa.begin(), set_aa.end(), set_ak.begin(),
                          set_ak.end(), std::back_inserter(intersection_masks));

    CHECK(intersection_masks.size() == 1108u);
    CHECK(union_masks.size() == 23169u);
    CHECK(aa.size() + ak.size() - intersection_masks.size() ==
          union_masks.size());
}
