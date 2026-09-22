#include "doctest.h"

#include "test_ulp_util.h"

#include "../src/core/eval_internal.h"

#include <xiapl/card.h>
#include <xiapl/detail/fast_rng.h>
#include <xiapl/eval.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Thread-contract tests for calculate_equity's Monte Carlo paths.
//
// Both MC paths (hand-vs-hand and multiway/PLO) partition their trials into
// fixed 65536-trial chunks, run chunk k off an RNG substream derived from the
// master seed alone, and reduce the per-chunk accumulators in chunk-index
// order. Consequences pinned below:
//   1. a given seed yields bit-identical output at every `threads` value,
//   2. a run that fits in one chunk reproduces the pre-chunking serial
//      stream bit-for-bit (derive_chunk_seed(s, 0) == s),
//   3. chunks past the first are genuinely distinct substreams,
//   4. exact enumeration (requested or auto-fallback) is untouched by any of
//      this and stays serial.

using namespace xiapl;

namespace {

std::uint64_t mk(const std::string& s) {
    return card_to_mask(Card::from_string(s));
}

// AhAs vs KdKc — preflop, so the MC board space is C(48, 5) = 1712304 and
// every iteration count used below stays genuinely Monte Carlo (a fixed flop
// would have only C(45, 2) = 990 completions and auto-fall back to exact).
std::vector<std::uint64_t> hu_holdem() {
    return {mk("Ah") | mk("As"), mk("Kd") | mk("Kc")};
}

std::vector<std::uint64_t> three_holdem() {
    return {mk("As") | mk("Ah"), mk("Kd") | mk("Kc"), mk("Qs") | mk("Qc")};
}

std::vector<std::uint64_t> two_plo() {
    return {
        mk("As") | mk("Ks") | mk("Ah") | mk("Kh"),
        mk("Qd") | mk("Jd") | mk("Qc") | mk("Jc"),
    };
}

// C(40, 5) = 658008 board completions, comfortably above every iteration
// count used here.
std::vector<std::uint64_t> three_plo() {
    return {
        mk("As") | mk("Ks") | mk("Ah") | mk("Kh"),
        mk("Qd") | mk("Jd") | mk("Qc") | mk("Jc"),
        mk("Td") | mk("9d") | mk("Tc") | mk("9c"),
    };
}

// Deals `num_players` hands of `cards_per_hand` cards plus a 5-card board out
// of a full deck, by partial Fisher-Yates. Returns the hole masks; the board
// mask goes to `out_board`.
std::vector<std::uint64_t> deal_random(FastRng& rng, int num_players,
                                       int cards_per_hand,
                                       std::uint64_t& out_board) {
    std::vector<int> deck(52);
    for (int i = 0; i < 52; ++i) deck[i] = i;
    const int needed = num_players * cards_per_hand + 5;
    REQUIRE(needed <= 52);
    for (int k = 0; k < needed; ++k) {
        const std::uint32_t j =
            static_cast<std::uint32_t>(k) +
            rng.bounded(52u - static_cast<std::uint32_t>(k));
        std::swap(deck[static_cast<std::size_t>(k)], deck[j]);
    }
    std::vector<std::uint64_t> holes(static_cast<std::size_t>(num_players), 0);
    int at = 0;
    for (int p = 0; p < num_players; ++p) {
        for (int c = 0; c < cards_per_hand; ++c) {
            holes[static_cast<std::size_t>(p)] |= (1ULL << deck[at++]);
        }
    }
    out_board = 0;
    for (int c = 0; c < 5; ++c) out_board |= (1ULL << deck[at++]);
    return holes;
}

// Independently derives the expected showdown winner mask for `holes` on
// `board`: evaluates every player's hand via the public evaluate_hand API,
// takes the best HandValue by operator<, and sets bit i for every player
// whose HandValue == best (so a chop sets more than one bit). This never
// touches judge_holdem_mask / judge_plo_mask or their bitmask scoring path
// (eval_score7 -> winner_mask_from_scores), so it can catch a bug there
// instead of reproducing it.
std::uint32_t expected_winner_mask(const std::vector<std::uint64_t>& holes,
                                   std::uint64_t board, GameType game) {
    std::vector<HandValue> values;
    values.reserve(holes.size());
    for (std::uint64_t hole : holes) {
        values.push_back(evaluate_hand(board, hole, game));
    }
    HandValue best = values[0];
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (best < values[i]) best = values[i];
    }
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] == best) mask |= (1u << i);
    }
    return mask;
}

// Every field of EquityResult, compared bit-exactly.
void check_identical(const EquityResult& r, const EquityResult& ref) {
    REQUIRE(r.players.size() == ref.players.size());
    CHECK(r.trials == ref.trials);
    CHECK(r.exact == ref.exact);
    CHECK(r.chop_rate == ref.chop_rate);
    for (std::size_t i = 0; i < r.players.size(); ++i) {
        CHECK(r.players[i].winrate == ref.players[i].winrate);
        CHECK(r.players[i].equity == ref.players[i].equity);
        CHECK(r.players[i].std_error == ref.players[i].std_error);
    }
}

} // namespace

TEST_CASE("judge_*_mask: hand-checked expectations") {
    // Non-derived anchor for the comparison logic itself. The equivalence
    // test below only proves the two representations agree; if the shared
    // scoring/tie core were wrong, both would be wrong together. These
    // expectations were worked out by hand from the hand rankings.

    // Board 2c 5d 9h Jc Qs (no flush, no straight).
    const std::uint64_t board =
        mk("2c") | mk("5d") | mk("9h") | mk("Jc") | mk("Qs");
    // p0 AhKh and p1 AdKd both play A-K-Q-J-9 high card: a two-way chop.
    // p2 3c4c plays Q-J-9-5-4 and loses.
    const std::vector<std::uint64_t> holdem = {
        mk("Ah") | mk("Kh"), mk("Ad") | mk("Kd"), mk("3c") | mk("4c")};
    CHECK(internal::judge_holdem_mask(holdem, board) == 0b011u);

    // p1 alone makes two pair (queens and jacks beats p0's ace high).
    const std::vector<std::uint64_t> holdem2 = {
        mk("Ah") | mk("Kh"), mk("Qd") | mk("Jd"), mk("3c") | mk("4c")};
    CHECK(internal::judge_holdem_mask(holdem2, board) == 0b010u);

    // Royal flush on the board: nothing in a Hold'em hand can beat or miss
    // it, so every player chops no matter what they hold.
    const std::uint64_t royal =
        mk("As") | mk("Ks") | mk("Qs") | mk("Js") | mk("Ts");
    const std::vector<std::uint64_t> holdem3 = {mk("2c") | mk("3c"),
                                                mk("7d") | mk("8d"),
                                                mk("Ah") | mk("Ad")};
    CHECK(internal::judge_holdem_mask(holdem3, royal) == 0b111u);

    // PLO plays exactly 2 hole + 3 board, so a hand is only as good as its
    // best such pairing. On 2c 5d 9h Jc Qs:
    //   p0 AcAd7c8d -> Ac Ad + Q J 9 = pair of aces (Q, J kickers).
    //   p1 7h8h3c4d -> no pair reachable (board is unpaired and neither hole
    //     pair matches it); best is 7h 8h + Q J 9 = Q-high.
    // Pair beats high card, so p0 wins outright. Neither side can make a
    // flush: 2 hole cards + at most 2 same-suit board cards is only 4.
    const std::vector<std::uint64_t> plo = {
        mk("Ac") | mk("Ad") | mk("7c") | mk("8d"),
        mk("7h") | mk("8h") | mk("3c") | mk("4d")};
    CHECK(internal::judge_plo_mask(plo, board) == 0b01u);

    // Both hold a pocket pair of aces, so both play A-A-Q-J-9 and chop; the
    // side cards (7/8) never enter the best five.
    const std::vector<std::uint64_t> plo_chop = {
        mk("Ac") | mk("Ad") | mk("7c") | mk("8d"),
        mk("Ah") | mk("As") | mk("7h") | mk("8s")};
    CHECK(internal::judge_plo_mask(plo_chop, board) == 0b11u);
}

TEST_CASE("judge_*_mask winners agree with independently evaluated hands") {
    // judge_holdem_mask / judge_plo_mask score showdowns through
    // eval_score7 + winner_mask_from_scores. This test re-derives the
    // winner set through a disjoint path -- the public evaluate_hand API,
    // compared with HandValue::operator< / operator== -- and requires the
    // two to agree bit-for-bit on every deal, including chops (more than
    // one bit set). (An earlier version of this test only OR'd the mask's
    // own bits back together, which holds for any uint32_t regardless of
    // whether the winners it names are correct; it never exercised judge_*
    // at all.)
    FastRng rng;
    rng.seed(20260807);
    int chops_seen = 0;
    for (int trial = 0; trial < 10000; ++trial) {
        const int num_players = 2 + (trial % 5);  // 2..6
        std::uint64_t board = 0;
        const std::vector<std::uint64_t> holes =
            deal_random(rng, num_players, 2, board);
        const std::uint32_t got =
            internal::judge_holdem_mask(holes, board);
        const std::uint32_t expected =
            expected_winner_mask(holes, board, GameType::Holdem);
        REQUIRE(got == expected);
        if (popcount64(got) > 1) ++chops_seen;
    }
    CHECK(chops_seen > 0);

    int plo_chops_seen = 0;
    for (int trial = 0; trial < 10000; ++trial) {
        const int num_players = 2 + (trial % 5);  // 2..6 (6 * 4 + 5 = 29 cards)
        std::uint64_t board = 0;
        const std::vector<std::uint64_t> holes =
            deal_random(rng, num_players, 4, board);
        const std::uint32_t got =
            internal::judge_plo_mask(holes, board);
        const std::uint32_t expected =
            expected_winner_mask(holes, board, GameType::Plo);
        REQUIRE(got == expected);
        if (popcount64(got) > 1) ++plo_chops_seen;
    }
    CHECK(plo_chops_seen > 0);

    // Degenerate input: no players evaluated -> no winners.
    const std::vector<std::uint64_t> none;
    CHECK(internal::judge_holdem_mask(none, 0) == 0u);
    CHECK(internal::judge_plo_mask(none, 0) == 0u);
}

TEST_CASE("calculate_equity HU MC: bit-identical across thread counts") {
    // Iteration counts straddle the 65536-trial chunk boundary: a partial
    // single chunk, an exactly-full chunk, a full chunk plus a 1-trial
    // remainder, and several chunks plus a remainder. threads = 16 against
    // iterations = 1 also checks the worker count is clamped to the chunk
    // count.
    for (int iters : {1, 65535, 65536, 65537, 3 * 65536 + 7}) {
        EquityResult ref;
        bool have_ref = false;
        for (int threads : {1, 2, 3, 8, 16}) {
            SimulationOptions o = SimulationOptions::mc_seeded(iters, 424242);
            o.threads = threads;
            const EquityResult r =
                calculate_equity(hu_holdem(), 0, o, GameType::Holdem);
            REQUIRE(r.exact == false);
            REQUIRE(r.trials == static_cast<std::uint64_t>(iters));
            if (!have_ref) {
                ref = r;
                have_ref = true;
                continue;
            }
            INFO("iters = ", iters, " threads = ", threads);
            check_identical(r, ref);
        }
    }
}

TEST_CASE("calculate_equity multiway PLO MC: bit-identical across thread counts") {
    // The multiway path carries per-player floating-point accumulators, so it
    // is the one that would expose a reduction order that depends on the
    // worker count.
    const int iters = 3 * 65536 + 7;
    EquityResult ref;
    bool have_ref = false;
    for (int threads : {1, 2, 4, 8}) {
        SimulationOptions o = SimulationOptions::mc_seeded(iters, 424242);
        o.threads = threads;
        const EquityResult r =
            calculate_equity(three_plo(), 0, o, GameType::Plo);
        REQUIRE(r.exact == false);
        REQUIRE(r.players.size() == 3);
        if (!have_ref) {
            ref = r;
            have_ref = true;
            continue;
        }
        INFO("threads = ", threads);
        check_identical(r, ref);
    }
}

TEST_CASE("calculate_equity multiway Hold'em MC: auto threads never move the result") {
    // 250000 iterations is above the 200000 auto-threading threshold, so
    // threads = 0 (the default) actually fans out here and must still match
    // the serial result bit for bit.
    SimulationOptions serial = SimulationOptions::mc_seeded(250000, 31337);
    serial.threads = 1;
    const EquityResult ref =
        calculate_equity(three_holdem(), 0, serial, GameType::Holdem);
    REQUIRE(ref.exact == false);

    SimulationOptions autos = serial;
    autos.threads = 0;
    check_identical(calculate_equity(three_holdem(), 0, autos, GameType::Holdem),
                    ref);

    // Negative thread counts are documented as "serial on every path".
    SimulationOptions neg = serial;
    neg.threads = -3;
    check_identical(calculate_equity(three_holdem(), 0, neg, GameType::Holdem),
                    ref);
}

TEST_CASE("calculate_equity: small serial MC keeps the pre-chunking stream") {
    // Regression pin. iterations <= 65536 is a single chunk whose derived seed
    // is the master seed itself (derive_chunk_seed(s, 0) == s), so chunking
    // must reproduce the pre-chunking serial stream bit-for-bit — for the
    // hand-vs-hand path, the multiway path and PLO alike.
    //
    // Literals measured at commit 79fafb9 (Task 2 HEAD, before this task's
    // chunked hand-vs-hand/multiway MC) with a standalone program linked
    // against libxiapl_core.a, printed with "%a". Hex float literals, not
    // decimals: the comparison is bit-exact. They must never move.
    //
    // Every std_error pin below (across all SUBCASEs in this TEST_CASE) is
    // compared via ulp_distance(..) <= 2 rather than ==: equity_hands.cpp
    // derives std_error through `var = mean_sq - mean * mean`, and GCC's
    // default -ffp-contract=fast fuses that multiply-subtract into a single
    // FMA, shifting the final rounding by up to a couple of ULPs relative to
    // the two-step double rounding these literals were captured under
    // (measured on WSL Ubuntu / GCC 13 -O3 -- only two of the six pins
    // happened to move under that specific run, but all six sit on the same
    // FMA-sensitive code path, so all are loosened together rather than
    // leaving a compiler-version-dependent landmine). The hex-float literals
    // are kept as the recorded baseline; only the comparison is loosened.
    // Stream-invariance for every case is still pinned bit-exact by the
    // equity/winrate/trials/chop_rate CHECKs alongside each std_error --
    // cross-compiler bit-identity of a derived (sqrt-based) statistic is not
    // part of this library's contract.

    SUBCASE("HU Hold'em preflop, mc_seeded(50000, 424242)") {
        const EquityResult r = calculate_equity(
            hu_holdem(), 0, SimulationOptions::mc_seeded(50000, 424242),
            GameType::Holdem);
        REQUIRE(r.players.size() == 2);
        CHECK(r.trials == 50000ULL);
        CHECK(r.exact == false);
        CHECK(r.players[0].winrate == 0x1.9ff2e48e8a71ep-1);   // 0.8124
        CHECK(r.players[0].equity == 0x1.a0ded288ce704p-1);    // 0.8142
        CHECK(ulp_distance(r.players[0].std_error, 0x1.c69e77701696ap-10) <=
              2);
        CHECK(r.players[1].winrate == 0x1.78d4fdf3b645ap-3);   // 0.184
        CHECK(r.players[1].equity == 0x1.7c84b5dcc63fp-3);     // 0.1858
        CHECK(r.chop_rate == 0x1.d7dbf487fcb92p-9);            // 0.0036
    }

    SUBCASE("HU Hold'em flop 2h7d9c, mc_seeded(500, 424242)") {
        // 500 < C(45, 2) = 990, so this stays Monte Carlo and exercises the
        // 2-cards-to-come dealing loop rather than the 5-card one.
        const std::uint64_t flop = mk("2h") | mk("7d") | mk("9c");
        const EquityResult r = calculate_equity(
            hu_holdem(), flop, SimulationOptions::mc_seeded(500, 424242),
            GameType::Holdem);
        REQUIRE(r.players.size() == 2);
        CHECK(r.trials == 500ULL);
        CHECK(r.exact == false);
        CHECK(r.players[0].winrate == 0x1.d1eb851eb851fp-1);  // 0.91
        CHECK(r.players[0].equity == 0x1.d1eb851eb851fp-1);
        CHECK(ulp_distance(r.players[0].std_error, 0x1.a361130bdf3b4p-7) <=
              2);
        CHECK(r.chop_rate == 0x0p+0);
    }

    SUBCASE("3-way Hold'em preflop, mc_seeded(30000, 7)") {
        const EquityResult r = calculate_equity(
            three_holdem(), 0, SimulationOptions::mc_seeded(30000, 7),
            GameType::Holdem);
        REQUIRE(r.players.size() == 3);
        CHECK(r.trials == 30000ULL);
        CHECK(r.players[0].winrate == 0x1.544f3078263abp-1);
        CHECK(r.players[0].equity == 0x1.5516b5c57903bp-1);
        CHECK(ulp_distance(r.players[0].std_error, 0x1.640b950cbc5e2p-9) <=
              2);
        CHECK(r.players[1].winrate == 0x1.7a43fe5c91d15p-3);
        CHECK(r.players[1].equity == 0x1.7d621391dcf4cp-3);
        CHECK(ulp_distance(r.players[1].std_error, 0x1.259a19ac45829p-9) <=
              2);
        CHECK(r.players[2].winrate == 0x1.2b250022f3d94p-3);
        CHECK(r.players[2].equity == 0x1.2e4315583efd4p-3);
        CHECK(ulp_distance(r.players[2].std_error, 0x1.0b539396bc235p-9) <=
              2);
        CHECK(r.chop_rate == 0x1.2b47f3fc2d544p-8);
    }

    SUBCASE("2-way PLO preflop, mc_seeded(20000, 999)") {
        const EquityResult r = calculate_equity(
            two_plo(), 0, SimulationOptions::mc_seeded(20000, 999),
            GameType::Plo);
        REQUIRE(r.players.size() == 2);
        CHECK(r.trials == 20000ULL);
        CHECK(r.players[0].winrate == 0x1.43404ea4a8c15p-1);
        CHECK(r.players[0].equity == 0x1.43404ea4a8c15p-1);
        CHECK(ulp_distance(r.players[0].std_error, 0x1.bf22286338fadp-9) <=
              2);
        CHECK(r.players[1].equity == 0x1.797f62b6ae7d5p-2);
        CHECK(ulp_distance(r.players[1].std_error, 0x1.bf22286338facp-9) <=
              2);
        CHECK(r.chop_rate == 0x0p+0);
    }

    SUBCASE("3-way PLO preflop, mc_seeded(65536, 12345) — exactly one chunk") {
        const EquityResult r = calculate_equity(
            three_plo(), 0, SimulationOptions::mc_seeded(65536, 12345),
            GameType::Plo);
        REQUIRE(r.players.size() == 3);
        CHECK(r.trials == 65536ULL);
        CHECK(r.players[0].winrate == 0x1.13p-1);        // 0.537109375
        CHECK(r.players[0].equity == 0x1.1302p-1);
        CHECK(ulp_distance(r.players[0].std_error, 0x1.fe9432bc0f2f5p-10) <=
              2);
        CHECK(r.players[1].equity == 0x1.2648p-2);
        CHECK(r.players[2].equity == 0x1.6768p-3);
        CHECK(r.chop_rate == 0x1p-15);
    }
}

TEST_CASE("calculate_equity: chunks past the first run distinct substreams") {
    // Sharp detector for a broken seed derivation. If chunk 1 replayed chunk
    // 0's stream, the 2-chunk run would be chunk 0's trials twice: both the
    // win count and the trial count double, and 2a / 2n is bit-exactly a / n
    // (both scalings are exact powers of two). So inequality of the two
    // winrates rules out a duplicated substream.
    SimulationOptions one = SimulationOptions::mc_seeded(65536, 777);
    one.threads = 1;
    SimulationOptions two = SimulationOptions::mc_seeded(2 * 65536, 777);
    two.threads = 1;

    const EquityResult r1 = calculate_equity(hu_holdem(), 0, one, GameType::Holdem);
    const EquityResult r2 = calculate_equity(hu_holdem(), 0, two, GameType::Holdem);
    CHECK(r2.trials == 2ULL * 65536ULL);
    CHECK(r1.players[0].equity != r2.players[0].equity);
    // Both estimate the same quantity, so they must still agree to a few
    // standard errors (guards against chunk 1 sampling something else).
    CHECK(std::abs(r1.players[0].equity - r2.players[0].equity) <
          5.0 * r1.players[0].std_error);

    // Same detector on the multiway path.
    const EquityResult m1 = calculate_equity(three_plo(), 0, one, GameType::Plo);
    const EquityResult m2 = calculate_equity(three_plo(), 0, two, GameType::Plo);
    CHECK(m1.players[0].equity != m2.players[0].equity);
    CHECK(std::abs(m1.players[0].equity - m2.players[0].equity) <
          5.0 * m1.players[0].std_error);
}

TEST_CASE("calculate_equity: auto-exact fallback still exact and serial") {
    // iterations (1000000) far exceeds the C(45, 2) = 990 board completions of
    // a fixed flop, so the run silently promotes to exact enumeration. That
    // path never consumes the RNG and never threads, so its result must equal
    // the iterations == 0 result bit for bit at every `threads` value.
    const std::uint64_t flop = mk("2h") | mk("7d") | mk("9c");
    const EquityResult exact = calculate_equity(
        hu_holdem(), flop, SimulationOptions::exact(), GameType::Holdem);
    REQUIRE(exact.exact == true);
    CHECK(exact.trials == 990ULL);
    CHECK(exact.players[0].std_error == 0.0);
    CHECK(exact.players[0].equity == 0x1.d51322a6877fcp-1);  // pinned at 79fafb9

    for (int threads : {1, 2, 8, 0, -1}) {
        SimulationOptions o = SimulationOptions::mc_seeded(1000000, 5);
        o.threads = threads;
        const EquityResult r =
            calculate_equity(hu_holdem(), flop, o, GameType::Holdem);
        INFO("threads = ", threads);
        CHECK(r.exact == true);
        CHECK(r.players[0].std_error == 0.0);
        CHECK(r.players[1].std_error == 0.0);
        check_identical(r, exact);
    }

    // Multiway takes the same decision, including the degenerate "board is
    // already complete" case (num_to_draw == 0 -> one trial, exact).
    const std::uint64_t river = flop | mk("Kh") | mk("2d");
    SimulationOptions o = SimulationOptions::mc_seeded(1000, 11);
    o.threads = 4;
    const EquityResult r =
        calculate_equity(three_holdem(), river, o, GameType::Holdem);
    CHECK(r.exact == true);
    CHECK(r.trials == 1ULL);
    CHECK(r.players[0].std_error == 0.0);
}
