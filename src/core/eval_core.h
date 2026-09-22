#pragma once

// Single evaluator: 5-7 card 52-bit mask -> 24-bit packed score.
//
// score layout (high -> low):
//   [category:4][k0:4][k1:4][k2:4][k3:4][k4:4]   (bit 23..0)
//   category is the HandCategory enum value itself (HighCard=1 .. StraightFlush=9).
//   ki is the raw rank value (2..14, A=14). Unused kickers are 0.
//   Since rank is always >= 2, "0 = unused" is never ambiguous, so the decode
//   side can recover kicker_count just by counting trailing zero nibbles.
//   Both category and ki are ordered with higher-priority values first, so
//   plain uint32 integer comparison matches HandValue::operator< exactly
//   (category, then kickers[] in lexicographic order).
//
// Implementation: per-suit lane extraction + rank multiplicity masks + an 8KB
// straight LUT. (The lane-extraction approach has been cross-checked against
// the reference over the full C(52,5/6/7) enumeration.)
//
// About the check ordering (this relies on the input being 5-7 cards):
//   - The flush check can go first: with at most 7 cards, at most one suit
//     can reach 5+ cards, and once a flush is present only 2 cards remain,
//     so a full house or four-of-a-kind can never also be present.
//   - The straight check can go before four-of-a-kind/full house: a straight
//     needs 5 distinct ranks, so it can never coexist with four-of-a-kind
//     (4 cards + 4 more distinct ranks = 8 cards) or a full house (at most
//     4 distinct ranks) within 7 cards or fewer.
//   Both orderings are covered by the exhaustive gates in
//   tests/test_eval_exhaustive.cpp (independent reference + golden hashes).

#include <xiapl/hand_value.h>
#include <xiapl/utils.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace xiapl::internal {

// 13-bit rank mask -> highest straight rank (2..14, 0 if none).
// bit i = rank i+2. The wheel (A-2-3-4-5) returns high=5.
constexpr std::array<std::uint8_t, 8192> make_straight_lut() {
    std::array<std::uint8_t, 8192> lut{};
    for (int m = 0; m < 8192; ++m) {
        int hi = 0;
        for (int h = 12; h >= 4; --h) {   // h = bit position of the highest rank
            const int p = 0x1F << (h - 4);
            if ((m & p) == p) { hi = h + 2; break; }
        }
        if (hi == 0) {
            constexpr int wheel = (1 << 12) | 0x0F;  // A + 2345
            if ((m & wheel) == wheel) hi = 5;
        }
        lut[static_cast<std::size_t>(m)] = static_cast<std::uint8_t>(hi);
    }
    return lut;
}

inline constexpr std::array<std::uint8_t, 8192> kStraightHigh = make_straight_lut();

inline int straight_high(std::uint32_t rank_mask) {
    return kStraightHigh[static_cast<std::size_t>(rank_mask)];
}

// Highest rank (2..14) in a 13-bit rank mask. Assumes m != 0 (clz32(0) is UB).
inline int score_top_rank(std::uint32_t m) {
    return 31 - clz32(m) + 2;
}

// Packs the top n ranks of m into score nibbles nib, nib-1, ... in descending
// order. nib is "that nibble's low bit position / 4" (k0=4, k1=3, ..., k4=0).
// Stops as soon as m runs out of bits (remaining nibbles stay 0 = unused).
inline std::uint32_t score_take_top(std::uint32_t m, int n, std::uint32_t s, int nib) {
    while (n-- > 0 && m) {
        const int b = 31 - clz32(m);
        s |= static_cast<std::uint32_t>(b + 2) << (nib * 4);
        --nib;
        m &= ~(1u << b);
    }
    return s;
}

// Evaluates a 5-7 card 52-bit mask and returns a 24-bit packed score.
// The return value is unspecified if mask's card count is outside 5-7
// (caller must guarantee this).
inline std::uint32_t eval_score7(std::uint64_t mask) {
    // Per-suit 13-bit lanes (id = suit * 13 + (rank - 2))
    const auto s0 = static_cast<std::uint32_t>( mask        & 0x1FFF);
    const auto s1 = static_cast<std::uint32_t>((mask >> 13) & 0x1FFF);
    const auto s2 = static_cast<std::uint32_t>((mask >> 26) & 0x1FFF);
    const auto s3 = static_cast<std::uint32_t>((mask >> 39) & 0x1FFF);

    const auto make = [](HandCategory c) {
        return static_cast<std::uint32_t>(c) << 20;
    };

    // Flush: with 7 cards or fewer, at most one suit can form a flush
    std::uint32_t fm = 0;
    if      (popcount64(s0) >= 5) fm = s0;
    else if (popcount64(s1) >= 5) fm = s1;
    else if (popcount64(s2) >= 5) fm = s2;
    else if (popcount64(s3) >= 5) fm = s3;
    if (fm) {
        if (const int h = straight_high(fm)) {
            return make(HandCategory::StraightFlush) | (static_cast<std::uint32_t>(h) << 16);
        }
        return score_take_top(fm, 5, make(HandCategory::Flush), 4);
    }

    // Rank multiplicity masks: c1 = 1+ cards, c2 = 2+ cards, c3 = 3+ cards, c4 = 4 cards
    std::uint32_t c1 = s0, c2 = 0, c3 = 0, c4 = 0;
    c2 = c1 & s1;                 c1 |= s1;
    c3 = c2 & s2; c2 |= c1 & s2;  c1 |= s2;
    c4 = c3 & s3; c3 |= c2 & s3;  c2 |= c1 & s3; c1 |= s3;

    if (const int h = straight_high(c1)) {
        return make(HandCategory::Straight) | (static_cast<std::uint32_t>(h) << 16);
    }

    if (c4) {
        const int q = score_top_rank(c4);
        const std::uint32_t s = make(HandCategory::FourOfAKind)
                              | (static_cast<std::uint32_t>(q) << 16);
        return score_take_top(c1 & ~c4, 1, s, 3);
    }
    if (c3) {
        const int t = score_top_rank(c3);
        // c2 is "2+ cards", so it includes both the trips itself and any
        // second trips. The highest rank in it excluding t is the pair side
        // of the full house: within 7 cards a second trips and an extra pair
        // can never coexist, so taking the highest is always correct.
        const std::uint32_t rest_pairs = c2 & ~(1u << (t - 2));
        if (rest_pairs) {
            return make(HandCategory::FullHouse)
                 | (static_cast<std::uint32_t>(t) << 16)
                 | (static_cast<std::uint32_t>(score_top_rank(rest_pairs)) << 12);
        }
        const std::uint32_t s = make(HandCategory::ThreeOfAKind)
                              | (static_cast<std::uint32_t>(t) << 16);
        return score_take_top(c1 & ~(1u << (t - 2)), 2, s, 3);
    }
    if (c2) {
        const int p1 = score_top_rank(c2);
        const std::uint32_t c2b = c2 & ~(1u << (p1 - 2));
        if (c2b) {
            const int p2 = score_top_rank(c2b);
            const std::uint32_t s = make(HandCategory::TwoPair)
                                  | (static_cast<std::uint32_t>(p1) << 16)
                                  | (static_cast<std::uint32_t>(p2) << 12);
            return score_take_top(c1 & ~(1u << (p1 - 2)) & ~(1u << (p2 - 2)), 1, s, 2);
        }
        const std::uint32_t s = make(HandCategory::OnePair)
                              | (static_cast<std::uint32_t>(p1) << 16);
        return score_take_top(c1 & ~(1u << (p1 - 2)), 3, s, 3);
    }
    return score_take_top(c1, 5, make(HandCategory::HighCard), 4);
}

// packed score -> HandValue. kicker_count is the count excluding trailing
// zero nibbles (used nibbles are never 0 since ranks are always 2..14).
inline HandValue decode_score(std::uint32_t score) {
    HandValue hv;
    hv.category = static_cast<HandCategory>(score >> 20);
    int count = 0;
    for (int i = 0; i < 5; ++i) {
        const auto r = static_cast<std::uint8_t>((score >> ((4 - i) * 4)) & 0xF);
        hv.kickers[static_cast<std::size_t>(i)] = r;
        if (r != 0) count = i + 1;
    }
    hv.kicker_count = static_cast<std::uint8_t>(count);
    return hv;
}

}  // namespace xiapl::internal
