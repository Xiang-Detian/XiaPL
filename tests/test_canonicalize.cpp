#include "doctest.h"
#include <xiapl/canonicalize.h>
#include <xiapl/card.h>
#include <xiapl/utils.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace xiapl;

// Helper: convert a vector of Cards to a sorted, concatenated string for comparison.
static std::string cards_to_sorted_string(const std::vector<Card>& cards) {
    std::vector<std::string> strs;
    strs.reserve(cards.size());
    for (const auto& c : cards) {
        strs.push_back(c.to_string());
    }
    std::sort(strs.begin(), strs.end());
    std::string result;
    for (const auto& s : strs) {
        result += s;
    }
    return result;
}

TEST_CASE("test_suit_invariance") {
    // Hero1(Ah, Kh) + Board1(Qh, Jh, 2c)
    std::vector<Card> hero1 = {Card(14, 2), Card(13, 2)};
    std::vector<Card> board1 = {Card(12, 2), Card(11, 2), Card(2, 0)};

    // Hero2(As, Ks) + Board2(Qs, Js, 2d)
    std::vector<Card> hero2 = {Card(14, 3), Card(13, 3)};
    std::vector<Card> board2 = {Card(12, 3), Card(11, 3), Card(2, 1)};

    auto [ch1, cb1] = canonicalize_hero_and_board_cards(hero1, board1);
    auto [ch2, cb2] = canonicalize_hero_and_board_cards(hero2, board2);

    CHECK(cards_to_sorted_string(ch1) == cards_to_sorted_string(ch2));
    CHECK(cards_to_sorted_string(cb1) == cards_to_sorted_string(cb2));
}

TEST_CASE("test_board_priority") {
    // Hero(As, Kd) + Board(7c, 8c, 2d)
    std::vector<Card> hero = {Card(14, 3), Card(13, 1)};
    std::vector<Card> board = {Card(7, 0), Card(8, 0), Card(2, 1)};

    auto [ch, cb] = canonicalize_hero_and_board_cards(hero, board);

    // The canonical board's first two cards should share the same suit
    CHECK(cb[0].suit() == cb[1].suit());
}

TEST_CASE("test_masks_consistency") {
    // Hero(Ah, Kh) + Board(Qh, Jh, 2c)
    std::vector<Card> hero = {Card(14, 2), Card(13, 2)};
    std::vector<Card> board = {Card(12, 2), Card(11, 2), Card(2, 0)};

    // Card-based canonicalization
    auto [ch, cb] = canonicalize_hero_and_board_cards(hero, board);

    // Mask-based canonicalization
    std::uint64_t hero_mask = cards_to_mask(hero);
    std::uint64_t board_mask = cards_to_mask(board);
    auto [cm_hero, cm_board] = canonicalize_hero_and_board(hero_mask, board_mask);

    // Convert card-based results to masks for comparison
    std::uint64_t ch_mask = cards_to_mask(ch);
    std::uint64_t cb_mask = cards_to_mask(cb);

    CHECK(ch_mask == cm_hero);
    CHECK(cb_mask == cm_board);
}

TEST_CASE("test_canonicalization_stability") {
    // Same cards, different board order
    // H1/B1: 2c,3c + Ah,Kd,Qs
    std::vector<Card> hero1 = {Card(2, 0), Card(3, 0)};
    std::vector<Card> board1 = {Card(14, 2), Card(13, 1), Card(12, 3)};

    // H2/B2: 2c,3c + Kd,Qs,Ah
    std::vector<Card> hero2 = {Card(2, 0), Card(3, 0)};
    std::vector<Card> board2 = {Card(13, 1), Card(12, 3), Card(14, 2)};

    auto [ch1, cb1] = canonicalize_hero_and_board_cards(hero1, board1);
    auto [ch2, cb2] = canonicalize_hero_and_board_cards(hero2, board2);

    CHECK(cards_to_sorted_string(ch1) == cards_to_sorted_string(ch2));
    CHECK(cards_to_sorted_string(cb1) == cards_to_sorted_string(cb2));
}

TEST_CASE("test_hand_swap") {
    // As,Ks vs Ks,As with the same board
    std::vector<Card> hero1 = {Card(14, 3), Card(13, 3)};
    std::vector<Card> hero2 = {Card(13, 3), Card(14, 3)};
    std::vector<Card> board = {Card(12, 2), Card(11, 2), Card(2, 0)};

    auto [ch1, cb1] = canonicalize_hero_and_board_cards(hero1, board);
    auto [ch2, cb2] = canonicalize_hero_and_board_cards(hero2, board);

    CHECK(cards_to_sorted_string(ch1) == cards_to_sorted_string(ch2));
    CHECK(cards_to_sorted_string(cb1) == cards_to_sorted_string(cb2));
}

TEST_CASE("test_isomorphism") {
    // Ah,Kh + 2d,3d,4d  and  As,Ks + 2c,3c,4c  should be isomorphic
    std::vector<Card> hero1 = {Card(14, 2), Card(13, 2)};
    std::vector<Card> board1 = {Card(2, 1), Card(3, 1), Card(4, 1)};

    std::vector<Card> hero2 = {Card(14, 3), Card(13, 3)};
    std::vector<Card> board2 = {Card(2, 0), Card(3, 0), Card(4, 0)};

    auto [ch1, cb1] = canonicalize_hero_and_board_cards(hero1, board1);
    auto [ch2, cb2] = canonicalize_hero_and_board_cards(hero2, board2);

    CHECK(cards_to_sorted_string(ch1) == cards_to_sorted_string(ch2));
    CHECK(cards_to_sorted_string(cb1) == cards_to_sorted_string(cb2));
}

TEST_CASE("test_generate_unique_situations_flop") {
    auto situations = generate_canonical_situations(3);
    CHECK(!situations.empty());

    // Check the first 200 entries
    std::size_t limit = std::min<std::size_t>(200, situations.size());
    for (std::size_t i = 0; i < limit; ++i) {
        auto hero_mask = situations[i].first;
        auto board_mask = situations[i].second;

        // Hero and board must not overlap
        CHECK((hero_mask & board_mask) == 0);

        // Hero must have exactly 2 cards
        CHECK(popcount64(hero_mask) == 2);

        // Board must have exactly 3 cards (flop)
        CHECK(popcount64(board_mask) == 3);
    }
}

TEST_CASE("test_canonicalize_board_order") {
    // Ah,Kd,Qs and Qs,Ah,Kd should give the same canonical board
    std::vector<Card> board1 = {Card(14, 2), Card(13, 1), Card(12, 3)};
    std::vector<Card> board2 = {Card(12, 3), Card(14, 2), Card(13, 1)};

    auto cb1 = canonicalize_board_cards(board1);
    auto cb2 = canonicalize_board_cards(board2);

    CHECK(cards_to_sorted_string(cb1) == cards_to_sorted_string(cb2));
}

TEST_CASE("test_canonicalize_board_suit_iso") {
    // Ah,Kd,Qc and As,Kh,Qd should be suit-isomorphic
    std::vector<Card> board1 = {Card(14, 2), Card(13, 1), Card(12, 0)};
    std::vector<Card> board2 = {Card(14, 3), Card(13, 2), Card(12, 1)};

    auto cb1 = canonicalize_board_cards(board1);
    auto cb2 = canonicalize_board_cards(board2);

    CHECK(cards_to_sorted_string(cb1) == cards_to_sorted_string(cb2));
}

// ---------------------------------------------------------------------------
// Board-only canonicalization: the properties that make it a canonical form.
//
// These are exhaustive on the flop and strided on the turn / river, so they
// pin the map itself rather than a handful of examples.
// ---------------------------------------------------------------------------

// Relabel the suits of a board mask: suit s of the input becomes suit perm[s].
static std::uint64_t permute_suits(std::uint64_t mask, const int perm[4]) {
    std::uint64_t out = 0;
    for (int s = 0; s < 4; ++s) {
        const std::uint64_t pat = (mask >> (s * 13)) & 0x1FFFULL;
        out |= pat << (perm[s] * 13);
    }
    return out;
}

// All boards of `size` cards, ascending by mask.
static std::vector<std::uint64_t> all_boards(int size) {
    std::vector<std::uint64_t> out;
    std::vector<int> idx(static_cast<std::size_t>(size));
    for (int i = 0; i < size; ++i) idx[static_cast<std::size_t>(i)] = i;
    for (;;) {
        std::uint64_t m = 0;
        for (int i = 0; i < size; ++i)
            m |= 1ULL << idx[static_cast<std::size_t>(i)];
        out.push_back(m);
        int i = size - 1;
        while (i >= 0 && idx[static_cast<std::size_t>(i)] == 52 - size + i) --i;
        if (i < 0) break;
        ++idx[static_cast<std::size_t>(i)];
        for (int j = i + 1; j < size; ++j)
            idx[static_cast<std::size_t>(j)] =
                idx[static_cast<std::size_t>(j - 1)] + 1;
    }
    return out;
}

static std::size_t class_count(const std::vector<std::uint64_t>& boards) {
    std::vector<std::uint64_t> canon;
    canon.reserve(boards.size());
    for (std::uint64_t m : boards) canon.push_back(canonicalize_board(m));
    std::sort(canon.begin(), canon.end());
    canon.erase(std::unique(canon.begin(), canon.end()), canon.end());
    return canon.size();
}

TEST_CASE("test_canonicalize_board_class_counts") {
    // The suit-isomorphism class counts of Hold'em boards. These are the
    // published values; hitting them exactly is what distinguishes a canonical
    // form from a merely self-consistent relabelling.
    CHECK(class_count(all_boards(3)) == 1755);
    CHECK(class_count(all_boards(4)) == 16432);
    CHECK(class_count(all_boards(5)) == 134459);
}

TEST_CASE("test_canonicalize_board_suit_permutation_invariance") {
    // Every flop against all 24 suit permutations, then a strided sample of
    // turns and rivers.
    int perm[4] = {0, 1, 2, 3};
    std::vector<std::array<int, 4>> perms;
    std::sort(perm, perm + 4);
    do {
        perms.push_back({perm[0], perm[1], perm[2], perm[3]});
    } while (std::next_permutation(perm, perm + 4));
    REQUIRE(perms.size() == 24);

    struct Lane { int size; int stride; };
    for (Lane lane : {Lane{3, 1}, Lane{4, 7}, Lane{5, 61}}) {
        const std::vector<std::uint64_t> boards = all_boards(lane.size);
        std::size_t bad = 0;
        for (std::size_t i = 0; i < boards.size();
             i += static_cast<std::size_t>(lane.stride)) {
            const std::uint64_t base = canonicalize_board(boards[i]);
            for (const auto& p : perms)
                if (canonicalize_board(permute_suits(boards[i], p.data())) !=
                    base)
                    ++bad;
        }
        CHECK(bad == 0);
    }
}

TEST_CASE("test_canonicalize_board_idempotent") {
    for (int size : {3, 4, 5}) {
        const std::vector<std::uint64_t> boards = all_boards(size);
        const std::size_t stride = (size == 3) ? 1u : (size == 4) ? 7u : 61u;
        std::size_t bad = 0;
        for (std::size_t i = 0; i < boards.size(); i += stride) {
            const std::uint64_t c = canonicalize_board(boards[i]);
            if (canonicalize_board(c) != c) ++bad;
        }
        CHECK(bad == 0);
    }
}

TEST_CASE("test_canonicalize_board_suit_map_reproduces_mask") {
    // The reported permutation is the one that was applied: relabelling the
    // input with it has to land exactly on the canonical mask. Callers that
    // carry hole cards across the canonicalization depend on this.
    const std::vector<std::uint64_t> boards = all_boards(3);
    std::size_t bad = 0;
    for (std::uint64_t m : boards) {
        int suit_map[4];
        const std::uint64_t c = canonicalize_board_with_suit_map(m, suit_map);
        if (permute_suits(m, suit_map) != c) ++bad;
        // A permutation, not just any map.
        int seen[4] = {0, 0, 0, 0};
        for (int s = 0; s < 4; ++s) ++seen[suit_map[s]];
        for (int s = 0; s < 4; ++s)
            if (seen[s] != 1) ++bad;
    }
    CHECK(bad == 0);
}

// Independent oracle for the case below: the smallest mask reachable by any of
// the 24 suit relabellings. Deliberately written as a brute-force minimum
// rather than reusing anything canonicalize_board depends on -- it shares no
// code with the pattern sort, so agreement is evidence, not tautology.
static std::uint64_t orbit_minimum_board(std::uint64_t mask) {
    std::uint64_t best = ~0ULL;
    int perm[4] = {0, 1, 2, 3};
    do {
        best = std::min(best, permute_suits(mask, perm));
    } while (std::next_permutation(perm, perm + 4));
    return best;
}

TEST_CASE("test_canonicalize_board_matches_brute_force_orbit_minimum") {
    // The orbit minimum is a canonical form by construction, and it lands on
    // the same representative as canonicalize_board for every flop -- so the
    // pattern sort is not merely self-consistent, it agrees with the minimum.
    std::size_t bad = 0;
    for (std::uint64_t m : all_boards(3)) {
        if (orbit_minimum_board(m) != canonicalize_board(m)) ++bad;
    }
    CHECK(bad == 0);
}

TEST_CASE("test_legacy_canonicalize_board_frozen") {
    // detail::legacy_canonicalize_board is what frozen artefacts were built
    // against, and its definition is "canonicalize_hero_and_board with an empty
    // hero". Both halves are checked here: the identity on every flop and turn,
    // and the 1,833-class signature that says it is still the old, under-merging
    // map rather than having quietly picked up the new one.
    for (int size : {3, 4}) {
        const std::vector<std::uint64_t> boards = all_boards(size);
        const std::size_t stride = (size == 3) ? 1u : 7u;
        std::size_t bad = 0;
        for (std::size_t i = 0; i < boards.size(); i += stride) {
            const std::uint64_t viapair =
                canonicalize_hero_and_board(0ULL, boards[i]).second;
            if (detail::legacy_canonicalize_board(boards[i]) != viapair) ++bad;
        }
        CHECK(bad == 0);
    }

    const std::vector<std::uint64_t> flops = all_boards(3);
    std::vector<std::uint64_t> canon;
    canon.reserve(flops.size());
    for (std::uint64_t m : flops)
        canon.push_back(detail::legacy_canonicalize_board(m));
    std::sort(canon.begin(), canon.end());
    canon.erase(std::unique(canon.begin(), canon.end()), canon.end());
    CHECK(canon.size() == 1833);
}

// ---------------------------------------------------------------------------
// CanonVersion::Strict -- the hero+board canonical form (Phase 1).
//
// The legacy hero+board map orders suits by their top board rank, so two suits
// sharing a top rank are separated by a tie-break rather than merged: the flop's
// 25,989,600 (hero, board) pairs land on 1,420,796 representatives where only
// 1,286,792 orbits exist. Strict sorts the full per-suit key
// (board_pattern << 13) | hero_pattern, which is a complete invariant.
//
// What is pinned below is the map itself, not samples of it: exhaustive suit
// permutation invariance over every flop, the exact orbit count, and the
// identity that ties the board half back to canonicalize_board.
// ---------------------------------------------------------------------------

// Relabel BOTH masks with the same permutation. A canonicalization of the pair
// may only be invariant under a joint relabelling -- permuting one alone is a
// different situation.
static void permute_hero_board(std::uint64_t hero, std::uint64_t board,
                               const int perm[4],
                               std::uint64_t& hero_out,
                               std::uint64_t& board_out) {
    hero_out = permute_suits(hero, perm);
    board_out = permute_suits(board, perm);
}

static std::vector<std::array<int, 4>> all_suit_perms() {
    int perm[4] = {0, 1, 2, 3};
    std::vector<std::array<int, 4>> perms;
    do {
        perms.push_back({perm[0], perm[1], perm[2], perm[3]});
    } while (std::next_permutation(perm, perm + 4));
    return perms;
}

TEST_CASE("strict canonicalization is invariant under a joint suit permutation") {
    const auto perms = all_suit_perms();
    REQUIRE(perms.size() == 24);

    // Exhaustive on the flop for a strided set of hero pairs, then strided on
    // turn and river. The flop lane alone is ~1.2M (hero, board, perm) triples.
    struct Lane { int size; int hero_stride; int board_stride; };
    for (Lane lane : {Lane{3, 1, 97}, Lane{4, 7, 1013}, Lane{5, 13, 10007}}) {
        const std::vector<std::uint64_t> boards = all_boards(lane.size);
        std::size_t bad = 0;
        std::size_t checked = 0;
        for (int a = 0; a < 52; ++a) {
            for (int b = a + 1; b < 52; ++b) {
                if (((a * 52 + b) % lane.hero_stride) != 0) continue;
                const std::uint64_t hero = (1ULL << a) | (1ULL << b);
                for (std::size_t i = 0; i < boards.size();
                     i += static_cast<std::size_t>(lane.board_stride)) {
                    if (boards[i] & hero) continue;
                    const auto base =
                        canonicalize_hero_and_board_strict(hero, boards[i]);
                    ++checked;
                    for (const auto& p : perms) {
                        std::uint64_t ph = 0, pb = 0;
                        permute_hero_board(hero, boards[i], p.data(), ph, pb);
                        if (canonicalize_hero_and_board_strict(ph, pb) != base)
                            ++bad;
                    }
                }
            }
        }
        CHECK(bad == 0);
        CHECK(checked > 0);
    }
}

TEST_CASE("strict canonicalization is idempotent") {
    std::size_t bad = 0;
    for (int size : {3, 4, 5}) {
        const std::vector<std::uint64_t> boards = all_boards(size);
        const std::size_t stride = (size == 3) ? 11u : (size == 4) ? 331u : 2003u;
        for (int a = 0; a < 52; ++a) {
            for (int b = a + 1; b < 52; ++b) {
                const std::uint64_t hero = (1ULL << a) | (1ULL << b);
                for (std::size_t i = 0; i < boards.size(); i += stride) {
                    if (boards[i] & hero) continue;
                    const auto c = canonicalize_hero_and_board_strict(hero, boards[i]);
                    if (canonicalize_hero_and_board_strict(c.first, c.second) != c)
                        ++bad;
                }
            }
        }
    }
    CHECK(bad == 0);
}

TEST_CASE("strict board half equals canonicalize_board, hero and all") {
    // Board patterns are the HIGH half of the per-suit sort key, so they come
    // out descending whatever the hero is. That identity is what keeps the
    // board-only marginal of a strict enumeration at exactly the 1,755 / 16,432
    // / 134,459 board classes instead of splitting them by hero.
    std::size_t bad = 0;
    for (int size : {3, 4, 5}) {
        const std::vector<std::uint64_t> boards = all_boards(size);
        const std::size_t stride = (size == 3) ? 7u : (size == 4) ? 211u : 1511u;
        for (int a = 0; a < 52; ++a) {
            for (int b = a + 1; b < 52; ++b) {
                const std::uint64_t hero = (1ULL << a) | (1ULL << b);
                for (std::size_t i = 0; i < boards.size(); i += stride) {
                    if (boards[i] & hero) continue;
                    if (canonicalize_hero_and_board_strict(hero, boards[i]).second !=
                        canonicalize_board(boards[i]))
                        ++bad;
                }
            }
        }
    }
    CHECK(bad == 0);

    // With no hero at all the two must coincide outright.
    for (std::uint64_t m : all_boards(3))
        REQUIRE(canonicalize_hero_and_board_strict(0ULL, m).second ==
                canonicalize_board(m));
}

TEST_CASE("strict canonicalization preserves the cards it is given") {
    // A relabelling, never a re-rank: hero and board keep their sizes, stay
    // disjoint, and every suit's rank multiset is preserved as a multiset.
    const auto perms = all_suit_perms();
    std::size_t bad = 0;
    const std::vector<std::uint64_t> boards = all_boards(3);
    for (int a = 0; a < 52; ++a) {
        for (int b = a + 1; b < 52; ++b) {
            const std::uint64_t hero = (1ULL << a) | (1ULL << b);
            for (std::size_t i = 0; i < boards.size(); i += 53) {
                if (boards[i] & hero) continue;
                const auto c = canonicalize_hero_and_board_strict(hero, boards[i]);
                if (popcount64(c.first) != 2) ++bad;
                if (popcount64(c.second) != 3) ++bad;
                if (c.first & c.second) ++bad;

                // The result must be reachable from the input by one of the 24
                // relabellings -- that is what "only suits move" means.
                bool reachable = false;
                for (const auto& p : perms) {
                    std::uint64_t ph = 0, pb = 0;
                    permute_hero_board(hero, boards[i], p.data(), ph, pb);
                    if (ph == c.first && pb == c.second) { reachable = true; break; }
                }
                if (!reachable) ++bad;
            }
        }
    }
    CHECK(bad == 0);
}

TEST_CASE("the pre-v2 suit ordering is unchanged") {
    // canonicalize_hero_and_board is no longer an abstraction generation
    // (version 1 was removed), but the routine survives: the public card-level
    // API and detail::legacy_canonicalize_board both pin this exact
    // relabelling. The digest below is what proves it did not move.
    std::uint64_t digest = 1469598103934665603ULL; // FNV-1a 64 offset basis
    auto mix = [&digest](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            digest ^= (v >> (i * 8)) & 0xFFULL;
            digest *= 1099511628211ULL;
        }
    };

    const std::vector<std::uint64_t> boards = all_boards(3);
    for (int a = 0; a < 52; ++a) {
        for (int b = a + 1; b < 52; ++b) {
            const std::uint64_t hero = (1ULL << a) | (1ULL << b);
            for (std::size_t i = 0; i < boards.size(); i += 37) {
                if (boards[i] & hero) continue;
                const auto c = canonicalize_hero_and_board(hero, boards[i]);
                mix(c.first);
                mix(c.second);
            }
        }
    }
    // Measured from the PRE-change src/core/canonicalize.cpp compiled
    // standalone, so it certifies "the routine did not move", not merely "it is
    // self-consistent". The dispatcher half of this case
    // (canonicalize_hero_and_board_v with the legacy version agreeing with the
    // direct call) died with that version.
    CHECK(digest == 0x11ba45f74cd5790aULL);
}

TEST_CASE("canon version parsing refuses what this build cannot reproduce") {
    CHECK(canon_version_from_u32(2) == CanonVersion::Strict);
    CHECK_THROWS_AS(canon_version_from_u32(0), std::invalid_argument);
    CHECK_THROWS_AS(canon_version_from_u32(3), std::invalid_argument);
    CHECK_THROWS_AS(canon_version_from_u32(0xFFFFFFFFu), std::invalid_argument);

    CHECK(canon_version_from_string("strict") == CanonVersion::Strict);
    CHECK(canon_version_from_string("2") == CanonVersion::Strict);
    CHECK_THROWS_AS(canon_version_from_string(""), std::invalid_argument);
    CHECK_THROWS_AS(canon_version_from_string("Strict"), std::invalid_argument);

    CHECK(std::string(canon_version_name(CanonVersion::Strict)) == "strict");

    // Version 1 / "legacy" was REMOVED. It is refused like any other version
    // this build cannot reproduce, but its message has to say WHY -- a caller
    // holding a legacy-keyed artefact needs to learn that the number was
    // retired, not merely that it is wrong.
    CHECK_THROWS_AS(canon_version_from_u32(1), std::invalid_argument);
    CHECK_THROWS_AS(canon_version_from_string("legacy"), std::invalid_argument);
    CHECK_THROWS_AS(canon_version_from_string("1"), std::invalid_argument);
    for (int which = 0; which < 2; ++which) {
        try {
            if (which == 0) canon_version_from_u32(1);
            else canon_version_from_string("legacy");
            FAIL("the removed legacy version was accepted");
        } catch (const std::invalid_argument& e) {
            const std::string msg = e.what();
            CHECK(msg.find("legacy") != std::string::npos);
            CHECK(msg.find("removed") != std::string::npos);
        }
    }

    // The dispatcher must not silently pick a default for a value that is not
    // an enumerator.
    CHECK_THROWS_AS(canonicalize_hero_and_board_v(3ULL, 0x1C000ULL,
                                                  static_cast<CanonVersion>(9)),
                    std::invalid_argument);
    CHECK_THROWS_AS(generate_canonical_situations(3, static_cast<CanonVersion>(9)),
                    std::invalid_argument);
}

TEST_SUITE("slow") {

TEST_CASE("strict flop enumeration hits the exact orbit count") {
    // 25,989,600 flop situations collapse to 1,286,792 suit-isomorphism classes
    // (Burnside over S4). The removed legacy version produced 1,420,796 --
    // 10.41% redundant -- and that arm of this case went with it.
    const auto strict = generate_canonical_situations(3, CanonVersion::Strict);
    CHECK(strict.size() == 1286792u);

    // Strict is also what the defaulted call now enumerates.
    CHECK(generate_canonical_situations(3).size() == strict.size());

    // Every representative is a fixed point of its own map, and the strict
    // population is a subset of the legacy one only in count, not necessarily
    // in membership -- so check the invariant that does hold: the board halves
    // of the strict population are exactly the 1,755 canonical flops.
    std::vector<std::uint64_t> board_marginal;
    board_marginal.reserve(strict.size());
    std::size_t bad = 0;
    for (const auto& s : strict) {
        if (canonicalize_hero_and_board_strict(s.first, s.second) != s) ++bad;
        if (popcount64(s.first) != 2 || popcount64(s.second) != 3) ++bad;
        if (s.first & s.second) ++bad;
        board_marginal.push_back(s.second);
    }
    CHECK(bad == 0);
    std::sort(board_marginal.begin(), board_marginal.end());
    board_marginal.erase(
        std::unique(board_marginal.begin(), board_marginal.end()),
        board_marginal.end());
    CHECK(board_marginal.size() == 1755u);
}

TEST_CASE("strict is a canonical form on every flop situation") {
    // Two properties, checked exhaustively over all 25,989,600 (hero, board)
    // flop pairs, which together are exactly the definition of a canonical form:
    //
    //   invariance   strict(g.s) == strict(s) for all 24 relabellings g
    //                => isomorphic situations share a representative;
    //   reachability strict(s) is g.s for SOME g
    //                => the representative is a member of s's own orbit, so
    //                   distinct orbits cannot be merged.
    //
    // With the orbit count above (1,286,792 classes from 1,286,792 orbits) this
    // closes the proof in both directions; neither property alone would.
    const auto perms = all_suit_perms();
    const std::vector<std::uint64_t> boards = all_boards(3);

    struct HeroPair { int a, b; };
    std::vector<HeroPair> hero_pairs;
    for (int a = 0; a < 52; ++a)
        for (int b = a + 1; b < 52; ++b) hero_pairs.push_back({a, b});
    REQUIRE(hero_pairs.size() == 1326u);

    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    const unsigned n_threads = std::min(hw, 6u);
    std::vector<std::size_t> bad(n_threads, 0);
    std::vector<std::size_t> checked(n_threads, 0);

    auto worker = [&](unsigned tid) {
        for (std::size_t p = tid; p < hero_pairs.size(); p += n_threads) {
            const std::uint64_t hero =
                (1ULL << hero_pairs[p].a) | (1ULL << hero_pairs[p].b);
            for (std::uint64_t board : boards) {
                if (board & hero) continue;
                const auto base = canonicalize_hero_and_board_strict(hero, board);
                bool reachable = false;
                for (const auto& g : perms) {
                    std::uint64_t ph = 0, pb = 0;
                    permute_hero_board(hero, board, g.data(), ph, pb);
                    if (canonicalize_hero_and_board_strict(ph, pb) != base)
                        ++bad[tid];
                    if (ph == base.first && pb == base.second) reachable = true;
                }
                if (!reachable) ++bad[tid];
                ++checked[tid];
            }
        }
    };

    std::vector<std::thread> threads;
    for (unsigned t = 0; t < n_threads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    std::size_t total_bad = 0, total_checked = 0;
    for (unsigned t = 0; t < n_threads; ++t) {
        total_bad += bad[t];
        total_checked += checked[t];
    }
    CHECK(total_checked == 25989600u);
    CHECK(total_bad == 0);
}

} // TEST_SUITE("slow")

TEST_CASE("test_canonicalize_hand_pair") {
    // Td, Tc -> "TT"
    std::vector<Card> hand = {Card(10, 1), Card(10, 0)};
    CHECK(canonicalize_hand(hand) == "TT");
}

TEST_CASE("test_canonicalize_hand_suited") {
    // Ah, Kh -> "AKs"
    std::vector<Card> hand1 = {Card(14, 2), Card(13, 2)};
    CHECK(canonicalize_hand(hand1) == "AKs");

    // Kh, Ah -> "AKs" (order-independent)
    std::vector<Card> hand2 = {Card(13, 2), Card(14, 2)};
    CHECK(canonicalize_hand(hand2) == "AKs");
}

TEST_CASE("test_canonicalize_hand_offsuit") {
    // Ah, Kd -> "AKo"
    std::vector<Card> hand1 = {Card(14, 2), Card(13, 1)};
    CHECK(canonicalize_hand(hand1) == "AKo");

    // Kd, Ah -> "AKo" (order-independent)
    std::vector<Card> hand2 = {Card(13, 1), Card(14, 2)};
    CHECK(canonicalize_hand(hand2) == "AKo");
}

TEST_CASE("test_canonicalize_hand_mask_equivalence") {
    // Ah, Kd
    std::vector<Card> hand = {Card(14, 2), Card(13, 1)};

    std::string from_cards = canonicalize_hand(hand);
    std::string from_mask = canonicalize_hand_mask(cards_to_mask(hand));

    CHECK(from_cards == from_mask);
}
