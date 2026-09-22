// PLO range-vs-range equity (task 4 of the plo-range-ws1 plan).
//
// Everything runs through the public surface calculate_range_equity(hero,
// villain, board, options, mode) with PLO-tagged ranges, except the scorer
// equivalence gate at the top, which pins the allocation-free PLO scorer
// (src/core/eval_internal.h) against an independently written reference.
//
// Where a ground truth is claimed as "hand computable", the derivation is
// written out in a comment above the assertion so this file can be audited
// without running anything.
#include "doctest.h"

#include "../src/core/eval_internal.h"

#include <xiapl/card.h>
#include <xiapl/eval.h>
#include <xiapl/game_type.h>
#include <xiapl/hand_value.h>
#include <xiapl/range.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xiapl;

namespace {

std::uint64_t mk(const std::string& s) {
    return card_to_mask(Card::from_string(s));
}

// A 4-card PLO hole mask from four card spellings.
std::uint64_t hand4(const std::string& a, const std::string& b,
                    const std::string& c, const std::string& d) {
    return mk(a) | mk(b) | mk(c) | mk(d);
}

Range plo(const std::string& text) {
    return Range::from_string(text, GameType::Plo);
}

// A PLO range holding exactly the given 4-card hand.
Range plo_hand(std::uint64_t mask) {
    return Range(std::vector<Combo>{Combo{mask, 1.0}}, GameType::Plo);
}

// Independent reference for the PLO "best 5 of exactly 2 hole + 3 board"
// rule. Enumerates every 5-card subset of the available cards (4 hole + a
// 3/4/5-card board) and keeps the best one that uses EXACTLY two hole cards
// (which forces exactly three board cards). The enumeration shape is
// deliberately different from the (2-of-4) x (3-of-board) nested loops the
// library uses, so agreement is evidence rather than a restatement, and it
// scores through the PUBLIC evaluate_mask() rather than the internal
// packed-score path. Generalized (2026-08-07) from a hardcoded 9-card (5-card
// board) pool to any board size >= 3 so the same reference covers the
// flop/turn scorer-gate arms below, not just river.
HandValue reference_plo_value(std::uint64_t hole_mask,
                              std::uint64_t board_mask) {
    int ids[9];
    int n = 0;
    for (std::uint64_t m = (hole_mask | board_mask); m; m &= (m - 1)) {
        ids[n++] = ctz64(m);
    }
    REQUIRE(n == static_cast<int>(popcount64(hole_mask)) +
                     static_cast<int>(popcount64(board_mask)));
    HandValue best;
    bool have_best = false;
    for (int a = 0; a < n; ++a) {
        for (int b = a + 1; b < n; ++b) {
            for (int c = b + 1; c < n; ++c) {
                for (int d = c + 1; d < n; ++d) {
                    for (int e = d + 1; e < n; ++e) {
                        const std::uint64_t five =
                            (1ULL << ids[a]) | (1ULL << ids[b]) |
                            (1ULL << ids[c]) | (1ULL << ids[d]) |
                            (1ULL << ids[e]);
                        if (popcount64(five & hole_mask) != 2) continue;
                        const HandValue hv = evaluate_mask(five);
                        if (!have_best || best < hv) {
                            best = hv;
                            have_best = true;
                        }
                    }
                }
            }
        }
    }
    REQUIRE(have_best);
    return best;
}

// Deals `count` distinct cards into `target`, skipping anything in `used`.
void deal_into(std::mt19937_64& rng, int count, std::uint64_t& used,
               std::uint64_t& target) {
    for (int i = 0; i < count; ++i) {
        int id = 0;
        do {
            id = static_cast<int>(rng() % 52);
        } while (used & (1ULL << id));
        used |= (1ULL << id);
        target |= (1ULL << id);
    }
}

// ---- Hand-derived showdown fixtures (river) -------------------------------
//
// Board: Kc Jh 9s 7d 2c  (no pair, no flush -- only two clubs -- and the
// board ranks K J 9 7 2 contain no straight).
//
// ThTs8h8s: the only 2-hole-card options are
//   Th Ts + K J 9 -> pair of tens
//   8h 8s + K J 9 -> pair of eights
//   T. 8. + J 9 7 -> 7-8-9-T-J STRAIGHT, J high      <- best
//   (Th 8h + Jh is only three hearts, so no flush.)
// AsAdKsKh: the options are
//   Ks Kh + Kc J 9 -> trip kings, J 9 kickers        <- best
//   As Ad + Kc J 9 -> pair of aces
//   (As Ks + 9s is only three spades, so no flush.)
// Straight (category 5) beats three of a kind (category 4), so the straight
// hand wins outright. TdTc8d8c is the same straight in the other two suits:
// it makes 7-8-9-T-J as well, with an identical packed score, so those two
// chop. All three claims are re-derived by reference_plo_value() in the
// scorer gate below.
const char* const kRiverBoard[5] = {"Kc", "Jh", "9s", "7d", "2c"};

std::uint64_t river_board() {
    return mk(kRiverBoard[0]) | mk(kRiverBoard[1]) | mk(kRiverBoard[2]) |
           mk(kRiverBoard[3]) | mk(kRiverBoard[4]);
}

// One arm of the scorer-equivalence gate below: 10000 random deals against a
// `board_cards`-card board (3, 4, or 5 -- flop/turn/river). Also pins
// plo_board_triples()'s n_triples == C(board_cards, 3) directly, since that
// is the dimension a 5-card-only board never exercises past n_triples == 10.
struct ScorerGateCounts {
    int score_mismatches = 0;
    int winner_mismatches = 0;
    int zero_scores = 0;
    int triple_count_mismatches = 0;
};

ScorerGateCounts run_scorer_gate(std::mt19937_64& rng, int board_cards) {
    ScorerGateCounts c;
    int expected_n_triples = 1;
    if (board_cards == 4) expected_n_triples = 4;
    if (board_cards == 5) expected_n_triples = 10;
    for (int trial = 0; trial < 10000; ++trial) {
        std::uint64_t used = 0, hero = 0, vill = 0, board = 0;
        deal_into(rng, 4, used, hero);
        deal_into(rng, 4, used, vill);
        deal_into(rng, board_cards, used, board);

        std::uint64_t triples[internal::kPloMaxBoardTriples];
        const int n_triples = internal::plo_board_triples(board, triples);
        if (n_triples != expected_n_triples) ++c.triple_count_mismatches;

        const std::uint32_t hs = internal::plo_score(hero, board);
        const std::uint32_t vs = internal::plo_score(vill, board);
        if (hs == 0 || vs == 0) ++c.zero_scores;

        // 1. Packed score decodes to the independent reference HandValue.
        if (!(internal::decode_score(hs) == reference_plo_value(hero, board))) {
            ++c.score_mismatches;
        }
        if (!(internal::decode_score(vs) == reference_plo_value(vill, board))) {
            ++c.score_mismatches;
        }

        // 2. The score ordering agrees with the existing public winner API.
        const std::vector<int> winners =
            judge({hero, vill}, board, GameType::Plo);
        const bool hero_wins = (winners.size() == 1 && winners[0] == 0);
        const bool vill_wins = (winners.size() == 1 && winners[0] == 1);
        const bool chop = (winners.size() == 2);
        if (hero_wins != (hs > vs)) ++c.winner_mismatches;
        if (vill_wins != (vs > hs)) ++c.winner_mismatches;
        if (chop != (hs == vs)) ++c.winner_mismatches;
    }
    return c;
}

}  // namespace

// ---------------------------------------------------------------------------
// Scorer gate: the allocation-free PLO scorer vs an independent reference.
// ---------------------------------------------------------------------------

TEST_CASE("plo_score matches an independent reference on 10000 random deals") {
    std::mt19937_64 rng(20260807ULL);
    const ScorerGateCounts river = run_scorer_gate(rng, 5);
    CHECK(river.score_mismatches == 0);
    CHECK(river.winner_mismatches == 0);
    CHECK(river.triple_count_mismatches == 0);
    // A legal PLO hand always makes at least a high card (category 1), so a
    // packed score of 0 is unreachable -- which is what lets the PerCombo
    // evaluation cache keep 0 as its "board conflicts with these holes"
    // sentinel for PLO exactly as it does for Hold'em.
    CHECK(river.zero_scores == 0);
}

// Extra arms (task 5 review leftover, 2026-08-07): the river arm above always
// deals a 5-card board, so plo_board_triples() only ever exercises
// n_triples == C(5,3) == 10 there. calculate_range_equity's AggregateOnly MC
// path calls plo_board_triples() on flop/turn boards too (n_triples 1 and 4
// respectively), which had no direct scorer-equivalence coverage before this.
// Same seeded methodology as the river arm, distinct seeds so all three arms
// sample independent deals.
TEST_CASE("plo_score matches an independent reference on 10000 random deals with a turn board") {
    std::mt19937_64 rng(20260807ULL ^ 0x7475726eULL);  // seed mixed with "turn"
    const ScorerGateCounts turn = run_scorer_gate(rng, 4);
    CHECK(turn.score_mismatches == 0);
    CHECK(turn.winner_mismatches == 0);
    CHECK(turn.triple_count_mismatches == 0);
    CHECK(turn.zero_scores == 0);
}

TEST_CASE("plo_score matches an independent reference on 10000 random deals with a flop board") {
    std::mt19937_64 rng(20260807ULL ^ 0x666c6f70ULL);  // seed mixed with "flop"
    const ScorerGateCounts flop = run_scorer_gate(rng, 3);
    CHECK(flop.score_mismatches == 0);
    CHECK(flop.winner_mismatches == 0);
    CHECK(flop.triple_count_mismatches == 0);
    CHECK(flop.zero_scores == 0);
}

TEST_CASE("plo_score reports 0 for a board that cannot make five cards") {
    // Two board cards: no 3-card board subset exists, so nothing is
    // evaluated and the sentinel value comes back.
    const std::uint64_t hole = hand4("As", "Ad", "Ks", "Kh");
    CHECK(internal::plo_score(hole, mk("2c") | mk("7d")) == 0u);
}

// ---------------------------------------------------------------------------
// Dispatch: the two ranges must agree on the game.
// ---------------------------------------------------------------------------

TEST_CASE("calculate_range_equity rejects mismatched game tags") {
    const Range holdem = Range::from_string("AA");
    const Range plo_range = plo("AAKKds");
    SimulationOptions opt = SimulationOptions::exact();

    CHECK_THROWS_AS(calculate_range_equity(holdem, plo_range, 0, opt),
                    std::invalid_argument);
    CHECK_THROWS_AS(calculate_range_equity(plo_range, holdem, 0, opt),
                    std::invalid_argument);

    // The message must name both sides so the caller can tell which one to fix.
    std::string message;
    try {
        calculate_range_equity(holdem, plo_range, 0, opt);
    } catch (const std::invalid_argument& e) {
        message = e.what();
    }
    CHECK(message.find("Hold'em") != std::string::npos);
    CHECK(message.find("PLO") != std::string::npos);

    // The check runs before the empty-range shortcut, so an empty (Hold'em
    // tagged) range paired with a PLO range still throws rather than
    // silently returning zeros.
    const Range empty;
    CHECK_THROWS_AS(calculate_range_equity(empty, plo_range, 0, opt),
                    std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Exact ground truth: hand-computable river showdowns (0 / 0.5 / 1).
// ---------------------------------------------------------------------------

TEST_CASE("PLO exact river: hand-derived 0 / 0.5 / 1 equities") {
    const std::uint64_t board = river_board();
    const std::uint64_t straight_hs = hand4("Th", "Ts", "8h", "8s");
    const std::uint64_t straight_dc = hand4("Td", "Tc", "8d", "8c");
    const std::uint64_t trip_kings = hand4("As", "Ad", "Ks", "Kh");

    SimulationOptions opt = SimulationOptions::exact();

    SUBCASE("straight beats trips -> hero equity 1") {
        const auto r = calculate_range_equity(plo_hand(straight_hs),
                                              plo_hand(trip_kings), board, opt);
        CHECK(r.exact == true);
        CHECK(r.trials == 1ULL);  // river: a single board completion
        REQUIRE(r.hero.size() == 1);
        REQUIRE(r.villain.size() == 1);
        CHECK(r.hero_aggregate_equity == 1.0);
        CHECK(r.villain_aggregate_equity == 0.0);
        CHECK(r.hero[0].equity == 1.0);
        CHECK(r.villain[0].equity == 0.0);
        CHECK(r.hero[0].combo_mask == straight_hs);
        CHECK(r.villain[0].combo_mask == trip_kings);
    }

    SUBCASE("trips loses to straight -> hero equity 0") {
        const auto r = calculate_range_equity(plo_hand(trip_kings),
                                              plo_hand(straight_hs), board, opt);
        CHECK(r.hero_aggregate_equity == 0.0);
        CHECK(r.villain_aggregate_equity == 1.0);
    }

    SUBCASE("identical straights chop -> hero equity 0.5") {
        const auto r = calculate_range_equity(
            plo_hand(straight_hs), plo_hand(straight_dc), board, opt);
        CHECK(r.hero_aggregate_equity == 0.5);
        CHECK(r.villain_aggregate_equity == 0.5);
    }

    SUBCASE("the derivation itself, via the reference evaluator") {
        // Restates the comment above kRiverBoard as assertions.
        const HandValue hs = reference_plo_value(straight_hs, board);
        const HandValue dc = reference_plo_value(straight_dc, board);
        const HandValue kk = reference_plo_value(trip_kings, board);
        CHECK(hs.category == HandCategory::Straight);
        CHECK(hs.kickers[0] == 11);  // J-high straight
        CHECK(dc.category == HandCategory::Straight);
        CHECK(dc.kickers[0] == 11);
        CHECK(kk.category == HandCategory::ThreeOfAKind);
        CHECK(kk.kickers[0] == 13);
        CHECK(kk < hs);
        CHECK(!(hs < dc));
        CHECK(!(dc < hs));
    }
}

// ---------------------------------------------------------------------------
// Exact turn spot cross-checked bit-exactly against calculate_equity.
// ---------------------------------------------------------------------------

TEST_CASE("PLO exact turn agrees bit-exactly with calculate_equity") {
    // Turn board Jc 9d 6h 3s, hero ThTs8h8s, villain AsAdKsKh. Both APIs
    // enumerate the same 40 river cards: calculate_range_equity draws from
    // the 48 cards outside the board and discards the 8 that collide with a
    // hole card (leaving 40 valid boards), while calculate_equity removes the
    // hole cards from the deck up front (40 boards). The accumulated
    // numerator is a sum of 1.0 and 0.5 terms bounded by 40, so it is exact
    // in binary in both paths, and both divide it by 40 -- the results must
    // therefore be equal to the last bit, not merely close.
    const std::uint64_t board = mk("Jc") | mk("9d") | mk("6h") | mk("3s");
    const std::uint64_t hero = hand4("Th", "Ts", "8h", "8s");
    const std::uint64_t vill = hand4("As", "Ad", "Ks", "Kh");

    SimulationOptions opt = SimulationOptions::exact();
    const auto range_result =
        calculate_range_equity(plo_hand(hero), plo_hand(vill), board, opt);
    const EquityResult hand_result =
        calculate_equity({hero, vill}, board, opt, GameType::Plo);

    REQUIRE(range_result.hero.size() == 1);
    REQUIRE(hand_result.players.size() == 2);
    CHECK(range_result.exact == true);
    CHECK(hand_result.exact == true);
    // trials means different things in the two APIs: the global board sample
    // set (48 = C(48,1)) vs the number of board completions actually
    // evaluated after the hole cards are removed (40).
    CHECK(range_result.trials == 48ULL);
    CHECK(hand_result.trials == 40ULL);

    CHECK(range_result.hero_aggregate_equity == hand_result.players[0].equity);
    CHECK(range_result.villain_aggregate_equity ==
          hand_result.players[1].equity);
    CHECK(range_result.hero[0].equity == hand_result.players[0].equity);
    // Measured value, recorded so a silent change of either path is visible:
    // 12 of the 40 rivers, i.e. 0.3 (not representable in binary, which is
    // what makes the equality above a real bit-exactness check).
    CHECK(range_result.hero_aggregate_equity == 0x1.3333333333333p-2);
}

TEST_CASE("PLO exact and AggregateOnly agree bit-exactly (shared engine)") {
    const std::uint64_t board = river_board();
    const Range hero = plo_hand(hand4("Th", "Ts", "8h", "8s"));
    const Range vill = plo("QQJJds");
    SimulationOptions opt = SimulationOptions::exact();
    const auto full =
        calculate_range_equity(hero, vill, board, opt, RangeEquityMode::PerCombo);
    const auto agg = calculate_range_equity(hero, vill, board, opt,
                                            RangeEquityMode::AggregateOnly);
    CHECK(agg.exact);
    CHECK(agg.hero.empty());
    CHECK(agg.villain.empty());
    CHECK(agg.hero_aggregate_equity == full.hero_aggregate_equity);
    CHECK(agg.aggregate_std_error == 0.0);
}

// ---------------------------------------------------------------------------
// AggregateOnly Monte Carlo.
// ---------------------------------------------------------------------------

TEST_CASE("PLO AggregateOnly MC never auto-falls back to exact") {
    // River board: the board space is a single completion, so a PerCombo run
    // with iterations >= 1 would promote itself to exact. AggregateOnly still
    // samples the pair dimension, so it stays Monte Carlo.
    const std::uint64_t board = river_board();
    SimulationOptions opt = SimulationOptions::mc_seeded(10, 5);
    const auto r = calculate_range_equity(plo("AAKKds"), plo("QQJJds"), board,
                                          opt, RangeEquityMode::AggregateOnly);
    CHECK(r.exact == false);
    CHECK(r.trials == 10ULL);
}

TEST_CASE("PLO AggregateOnly MC is reproducible with the same seed") {
    const Range hero = plo("AAKKds");
    const Range vill = plo("QQJJds, JT98ds");

    for (std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{7}}) {
        SimulationOptions opt = SimulationOptions::mc_seeded(20000, seed);
        const auto a = calculate_range_equity(hero, vill, 0, opt,
                                              RangeEquityMode::AggregateOnly);
        const auto b = calculate_range_equity(hero, vill, 0, opt,
                                              RangeEquityMode::AggregateOnly);
        CHECK(a.hero_aggregate_equity == b.hero_aggregate_equity);
        CHECK(a.villain_aggregate_equity == b.villain_aggregate_equity);
        CHECK(a.aggregate_std_error == b.aggregate_std_error);
        CHECK(a.trials == b.trials);
    }

    // Different seeds must actually move the estimate.
    const auto r1 = calculate_range_equity(
        hero, vill, 0, SimulationOptions::mc_seeded(20000, 1),
        RangeEquityMode::AggregateOnly);
    const auto r2 = calculate_range_equity(
        hero, vill, 0, SimulationOptions::mc_seeded(20000, 2),
        RangeEquityMode::AggregateOnly);
    CHECK(r1.hero_aggregate_equity != r2.hero_aggregate_equity);
}

TEST_CASE("PLO AggregateOnly: bit-identical across thread counts") {
    // Same contract as the Hold'em path: fixed 65536-trial chunks with
    // seed-derived substreams, reduced in chunk-index order. The iteration
    // counts straddle the chunk boundary (a partial chunk, one trial short of
    // a full chunk, and a full chunk plus a 1-trial remainder).
    const Range hero = plo("AAKKds");
    const Range vill = plo("QQJJds, JT98ds");
    for (int iters : {1, 65535, 65537}) {
        RangeEquityResult ref;
        bool have_ref = false;
        for (int threads : {1, 2, 8}) {
            SimulationOptions o = SimulationOptions::mc_seeded(iters, 4242);
            o.threads = threads;
            const RangeEquityResult r = calculate_range_equity(
                hero, vill, 0, o, RangeEquityMode::AggregateOnly);
            CHECK(r.trials == static_cast<std::uint64_t>(iters));
            CHECK(r.exact == false);
            if (!have_ref) {
                ref = r;
                have_ref = true;
                continue;
            }
            INFO("iters = ", iters, " threads = ", threads);
            CHECK(r.hero_aggregate_equity == ref.hero_aggregate_equity);
            CHECK(r.villain_aggregate_equity == ref.villain_aggregate_equity);
            CHECK(r.aggregate_std_error == ref.aggregate_std_error);
            CHECK(r.trials == ref.trials);
        }
    }
}

TEST_CASE("PLO AggregateOnly: blocking ranges throw from the threaded path") {
    // Two single-combo PLO ranges sharing the ace of spades: every pair draw
    // collides, so every chunk exhausts the rejection cap and throws. The
    // per-chunk exception_ptr capture must surface that after the join
    // instead of terminating the process.
    const Range hero = plo_hand(hand4("As", "Ad", "Ks", "Kh"));
    const Range vill = plo_hand(hand4("As", "Qd", "Jc", "9h"));
    SimulationOptions o = SimulationOptions::mc_seeded(200000, 42);
    o.threads = 4;
    CHECK_THROWS_AS(calculate_range_equity(hero, vill, 0, o,
                                           RangeEquityMode::AggregateOnly),
                    std::runtime_error);
}

TEST_CASE("PLO AggregateOnly: heavy pair rejection stays unbiased") {
    // AAKKds vs AAKKds. A combo is Ax Kx Ay Ky for a suit pair {x, y}, so two
    // combos are card-compatible exactly when their suit pairs are disjoint:
    // 6 of the 36 ordered pairs, i.e. ~83% of draws are rejected. The
    // matchup is symmetric, so the true aggregate is exactly 0.5.
    const Range hero = plo("AAKKds");
    const Range vill = plo("AAKKds");
    SimulationOptions opt = SimulationOptions::mc_seeded(50000, 3);
    const auto r = calculate_range_equity(hero, vill, 0, opt,
                                          RangeEquityMode::AggregateOnly);
    CHECK(r.trials == 50000ULL);  // rejections are not trials
    CHECK(r.exact == false);
    REQUIRE(r.aggregate_std_error > 0.0);
    CHECK(std::abs(r.hero_aggregate_equity - 0.5) <=
          5.0 * r.aggregate_std_error);
    CHECK(r.villain_aggregate_equity ==
          doctest::Approx(1.0 - r.hero_aggregate_equity).epsilon(1e-12));
}

namespace {

struct BiasBatteryResult {
    double mean_z = 0.0;
    int outliers = 0;
};

// Same pooled-z design as the Hold'em battery in tests/test_range_equity.cpp:
// per-seed z = (mc_mean - exact) / mc_se, checked in aggregate (mean |z| and
// an outlier count) rather than per run, which is both far more sensitive to
// a small systematic bias and effectively flake-free at a fixed seed sweep.
BiasBatteryResult run_bias_battery(const Range& hero, const Range& vill,
                                   std::uint64_t board, int mc_iterations) {
    SimulationOptions ex = SimulationOptions::exact();
    const auto truth = calculate_range_equity(hero, vill, board, ex,
                                              RangeEquityMode::PerCombo);
    const double exact_equity = truth.hero_aggregate_equity;

    const int kNumSeeds = 100;
    BiasBatteryResult result;
    double sum_z = 0.0;
    for (int s = 0; s < kNumSeeds; ++s) {
        SimulationOptions mc = SimulationOptions::mc_seeded(
            mc_iterations, static_cast<std::uint64_t>(s + 1));
        const auto agg = calculate_range_equity(hero, vill, board, mc,
                                                RangeEquityMode::AggregateOnly);
        REQUIRE(agg.aggregate_std_error > 0.0);
        const double z =
            (agg.hero_aggregate_equity - exact_equity) / agg.aggregate_std_error;
        sum_z += z;
        if (std::abs(z) > 3.0) ++result.outliers;
    }
    result.mean_z = sum_z / kNumSeeds;
    return result;
}

}  // namespace

TEST_SUITE("slow") {
TEST_CASE("PLO AggregateOnly MC vs exact: 100-seed aggregate-z battery") {
    // Flop 2c 3d 4h, so the two remaining board cards are sampled as well as
    // the combo pair -- the estimator's per-trial equity has to vary for the
    // z-scores to be defined, and a river spot with these ranges is a
    // deterministic hero win. Ranks 2/3/4 collide with nothing either side
    // holds, so no combo is filtered out as a board blocker and the exact
    // ground truth covers the same pair set the sampler draws from.
    const std::uint64_t board = mk("2c") | mk("3d") | mk("4h");

    SUBCASE("unweighted villain range (uniform picker fast path)") {
        // Hero is a single combo and every villain combo has weight 1, so both
        // AliasPickers take the uniform bounded(n) fast path.
        const Range hero = plo_hand(hand4("Ah", "Ad", "Kh", "Kd"));
        const Range vill = plo("QQJJds, JT98ds");
        const BiasBatteryResult r = run_bias_battery(hero, vill, board, 20000);
        INFO("mean_z = ", r.mean_z, " outliers(|z|>3) = ", r.outliers);
        CHECK(std::abs(r.mean_z) < 0.3);
        CHECK(r.outliers <= 2);
    }

    SUBCASE("weighted villain range (exercises the alias table)") {
        // Distinct explicit weights force the villain picker through the full
        // alias-table construction. Villain ranks are Q/J/T/9/8/7 and hero
        // holds A/K, so the two sides never block each other either.
        const Range hero = plo_hand(hand4("Ah", "Ad", "Kh", "Kd"));
        const Range vill = plo("QQJJds:0.25, JT98ds, 8877ds:0.6");
        const BiasBatteryResult r = run_bias_battery(hero, vill, board, 20000);
        INFO("mean_z = ", r.mean_z, " outliers(|z|>3) = ", r.outliers);
        CHECK(std::abs(r.mean_z) < 0.3);
        CHECK(r.outliers <= 2);
    }
}
}  // TEST_SUITE("slow")

// ---------------------------------------------------------------------------
// PerCombo mode for PLO.
// ---------------------------------------------------------------------------

TEST_CASE("PLO PerCombo exact emits 4-card combos in input order") {
    const std::uint64_t board = river_board();
    const Range hero = plo("QQJJds");
    const Range vill = plo_hand(hand4("Th", "Ts", "8h", "8s"));
    SimulationOptions opt = SimulationOptions::exact();
    const auto r = calculate_range_equity(hero, vill, board, opt);

    // The board holds Jh, so the three QQJJds combos that use it are dropped
    // as board blockers; the survivors keep their input order.
    std::vector<std::uint64_t> expected;
    for (const Combo& c : hero.combos()) {
        if ((c.mask & board) == 0) expected.push_back(c.mask);
    }
    REQUIRE(expected.size() == 3);
    REQUIRE(r.hero.size() == expected.size());
    for (std::size_t i = 0; i < r.hero.size(); ++i) {
        CHECK(r.hero[i].combo_mask == expected[i]);
        CHECK(popcount64(r.hero[i].combo_mask) == 4);
    }
    REQUIRE(r.villain.size() == 1);
    CHECK(popcount64(r.villain[0].combo_mask) == 4);
    CHECK(r.hero_aggregate_equity + r.villain_aggregate_equity ==
          doctest::Approx(1.0).epsilon(1e-12));
}

TEST_CASE("PLO PerCombo carries combo weights through") {
    const std::uint64_t board = river_board();
    const Range hero = plo_hand(hand4("Th", "Ts", "8h", "8s"));
    const Range vill = plo("QQJJds:0.25, AAKKds");
    SimulationOptions opt = SimulationOptions::exact();
    const auto r = calculate_range_equity(hero, vill, board, opt);
    bool found_quarter = false, found_one = false;
    for (const auto& e : r.villain) {
        if (std::abs(e.weight - 0.25) < 1e-12) found_quarter = true;
        if (std::abs(e.weight - 1.0) < 1e-12) found_one = true;
    }
    CHECK(found_quarter);
    CHECK(found_one);
}

TEST_CASE("PLO PerCombo exact: explicit thread counts agree") {
    // The PerCombo engine parallelizes its evaluation-cache fill and its pair
    // loop on options.threads. The pair loop's hero-row partition (a fixed
    // NUMBER of chunks, capped at kPerComboMaxRowChunks, and a function of
    // hero range width alone -- see equity_range.cpp) and its reduction order
    // are both independent of the worker count, so the result is
    // bit-identical at every thread count -- the old ~1e-9 per-thread-split
    // caveat this test used to compare under tolerance for is gone (task:
    // PerCombo fixed-chunk reduction). It is also still the case the TSan
    // run exercises for the PLO fill path.
    const std::uint64_t flop = mk("2c") | mk("3d") | mk("4h");
    const Range hero = plo("QQJJds");
    const Range vill = plo("JT98ds");

    SimulationOptions serial = SimulationOptions::exact();
    serial.threads = 1;
    SimulationOptions threaded = SimulationOptions::exact();
    threaded.threads = 4;

    const auto r1 = calculate_range_equity(hero, vill, flop, serial);
    const auto r4 = calculate_range_equity(hero, vill, flop, threaded);

    REQUIRE(r1.hero.size() == r4.hero.size());
    REQUIRE(r1.villain.size() == r4.villain.size());
    CHECK(r4.hero_aggregate_equity == r1.hero_aggregate_equity);
    for (std::size_t i = 0; i < r1.hero.size(); ++i) {
        CHECK(r4.hero[i].combo_mask == r1.hero[i].combo_mask);
        CHECK(r4.hero[i].equity == r1.hero[i].equity);
    }
}

TEST_CASE("PLO PerCombo exact: narrow spot agrees across thread counts") {
    // Two exact hands = TWO evaluation-cache rows. That is the shape the PLO
    // exact docs steer callers toward ("a handful of combos, or a turn/river
    // board"), and the fill used to be split by hand ROW, so it could never
    // put more than 2 workers on this spot however high options.threads went.
    // The fill now switches to BOARD slices when the rows would starve the
    // workers, which is the path threads=8 takes here.
    //
    // The assertion is BIT equality -- now the general PerCombo contract at
    // every threads value (see "PLO PerCombo exact: explicit thread counts
    // agree" above), but this particular spot is bit-exact for a second,
    // narrower reason worth pinning on its own:
    //   * the fill writes one independent value per (hand, board) cell, so any
    //     partition of the rectangle produces the same cache bytes; and
    //   * hero_combos.size() == 1 means num_row_chunks == 1 by construction
    //     (num_row_chunks = min(hero.size(), kPerComboMaxRowChunks)), so the
    //     pair loop never has more than one chunk to reduce, at any threads
    //     value.
    // This test exists to pin the fill's board-slice path specifically, which
    // the multi-combo case above does not exercise (its fill stays on the row
    // split).
    const Range hero = plo_hand(hand4("As", "Ks", "Qh", "Jh"));
    const Range vill = plo_hand(hand4("Ad", "Kd", "Qc", "Jc"));

    SimulationOptions serial = SimulationOptions::exact();
    serial.threads = 1;
    SimulationOptions threaded = SimulationOptions::exact();
    threaded.threads = 8;

    // Flop board: 2 rows x C(49,2) = 1,176 boards, so the 8 workers get ~147
    // board columns each.
    const std::uint64_t flop = mk("2c") | mk("3d") | mk("4h");
    const auto f1 = calculate_range_equity(hero, vill, flop, serial);
    const auto f8 = calculate_range_equity(hero, vill, flop, threaded);
    REQUIRE(f1.hero.size() == 1);
    REQUIRE(f8.hero.size() == 1);
    REQUIRE(f1.villain.size() == 1);
    REQUIRE(f8.villain.size() == 1);
    CHECK(f8.trials == f1.trials);
    CHECK(f8.hero[0].combo_mask == f1.hero[0].combo_mask);
    CHECK(f8.hero[0].equity == f1.hero[0].equity);
    CHECK(f8.villain[0].equity == f1.villain[0].equity);
    CHECK(f8.hero_aggregate_equity == f1.hero_aggregate_equity);
    CHECK(f8.villain_aggregate_equity == f1.villain_aggregate_equity);

    // Fewer boards than requested workers (a complete board enumerates to a
    // single board): the board split has to clamp to the board count instead
    // of spawning empty workers.
    const std::uint64_t river = flop | mk("9s") | mk("2h");
    const auto r1 = calculate_range_equity(hero, vill, river, serial);
    const auto r8 = calculate_range_equity(hero, vill, river, threaded);
    REQUIRE(r1.trials == 1ULL);
    CHECK(r8.trials == 1ULL);
    REQUIRE(r1.hero.size() == 1);
    REQUIRE(r8.hero.size() == 1);
    CHECK(r8.hero[0].equity == r1.hero[0].equity);
    CHECK(r8.hero_aggregate_equity == r1.hero_aggregate_equity);
}

TEST_SUITE("slow") {
TEST_CASE("PLO wide exact hits the cache cap and says so") {
    // AKQJds and T987ds are 36 combos each and share no rank, so the cache is
    // keyed on 72 distinct hands (the cache dedups hero against villain, which
    // is why two copies of the SAME range would only be 36 rows). Preflop
    // exact enumerates C(52,5) = 2,598,960 boards, so the cache would be
    // 72 x 2,598,960 x 4 B ~= 714 MB, over the 512 MB limit. Throwing here is
    // the designed behavior, and the message has to point at the mode that CAN
    // answer the question.
    const Range hero = plo("AKQJds");
    const Range vill = plo("T987ds");
    SimulationOptions opt = SimulationOptions::exact();

    CHECK_THROWS_AS(calculate_range_equity(hero, vill, 0, opt),
                    std::runtime_error);
    std::string message;
    try {
        calculate_range_equity(hero, vill, 0, opt);
    } catch (const std::runtime_error& e) {
        message = e.what();
    }
    CHECK(message.find("PLO") != std::string::npos);
    CHECK(message.find("AggregateOnly") != std::string::npos);
    CHECK(message.find("mc_seeded") != std::string::npos);

    // The recommended escape hatch actually works.
    SimulationOptions mc = SimulationOptions::mc_seeded(20000, 1);
    const auto r = calculate_range_equity(hero, vill, 0, mc,
                                          RangeEquityMode::AggregateOnly);
    CHECK(r.trials == 20000ULL);
    CHECK(r.exact == false);
    REQUIRE(r.aggregate_std_error > 0.0);
    // Two double-suited broadway/middling rundowns: a believable window, not
    // a pinned value (the point of the assertion is that the escape hatch
    // produces an answer at all).
    CHECK(r.hero_aggregate_equity > 0.35);
    CHECK(r.hero_aggregate_equity < 0.75);
}
}  // TEST_SUITE("slow")

TEST_CASE("PLO empty range returns an empty result") {
    const Range hero(std::vector<Combo>{}, GameType::Plo);
    const Range vill = plo("AAKKds");
    SimulationOptions opt = SimulationOptions::mc_seeded(100, 1);
    const auto r = calculate_range_equity(hero, vill, 0, opt,
                                          RangeEquityMode::AggregateOnly);
    CHECK(r.hero.empty());
    CHECK(r.villain.empty());
    CHECK(r.hero_aggregate_equity == 0.0);
}
