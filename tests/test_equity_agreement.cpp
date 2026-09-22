#include "doctest.h"

#include "../src/core/equity_internal.h"
#include "../src/core/mc_chunking.h"

#include <xiapl/card.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// HU-vs-multiway equity agreement test (whitebox).
//
// equity_hands.cpp's dispatcher (compute_player_equities, guarded by
// `!use_plo && num_players == 2`) routes every 2-player Hold'em call through
// the public calculate_equity API to the HU-specialized simulator
// (internal::simulate_heads_up). The generic multiway simulator
// (internal::simulate_multiway) is therefore UNREACHABLE for that
// shape through any public entry point -- 2-player PLO reaches it, N > 2
// Hold'em reaches it, but 2-player Hold'em never does. Consequently nothing
// pinned the two implementations against each other before this test: they
// could silently drift and no existing test would notice, because
// tests/test_equity_threading.cpp pins each path separately, never on the
// same spot.
//
// The maintainer decided (2026-08-13) to KEEP the HU specialization rather
// than unify it onto the generic path: measured 1.13x-4.99x faster across
// workload shapes (largest on small exact boards, where the generic path's
// per-call heap allocations dominate), and a prototype "thin kernel
// specialization" could not close that gap. Both
// simulators were promoted from file-static to xiapl::internal:: linkage
// (src/core/equity_internal.h) purely so this test can drive them directly
// on the same inputs and guard against future drift.
//
// A 4000-random-spot sweep
// found the two paths agree bit-exactly on trials and chop_rate, and within
// a handful of ULPs on equity/winrate -- the two accumulate the same
// integer win/chop/trial counts from the same board stream, but combine them
// into a final double via different arithmetic (wins/n + chops/(2n) vs a
// directly accumulated equity sum; 1 - hero vs a direct villain sum). This
// test freezes that measured tolerance as a regression gate.

using namespace xiapl;

namespace {

std::uint64_t mk(const std::string& s) {
    return card_to_mask(Card::from_string(s));
}

// Card ids of `remain_mask` in ascending order: the remaining-deck index
// list internal::simulate_heads_up's `deck_indices` parameter expects.
// Mirrors board_sample.h's internal::deck_indices_of (not itself promoted by
// this task -- only the two simulators are).
std::vector<int> deck_indices_of(std::uint64_t remain_mask) {
    std::vector<int> out;
    out.reserve(52);
    for (std::uint64_t m = remain_mask; m; m &= (m - 1)) {
        out.push_back(ctz64(m));
    }
    return out;
}

// ULP distance between two doubles that are always >= 0 here (equities,
// winrates and chop_rate all live in [0, 1]): for non-negative IEEE-754
// doubles the raw bit pattern, read as an unsigned integer, is monotonic in
// the value, so integer subtraction of the bit patterns is exactly the ULP
// distance. Not valid across a sign change -- unneeded for this fixture.
//
// REQUIRE(a >= 0.0) enforces "non-negative-signed", not "non-negative bit
// pattern": IEEE-754 comparison treats -0.0 == 0.0, so -0.0 would pass this
// REQUIRE while its bit pattern (0x8000...) would break the monotonic-bits
// assumption above (~2^63 of spurious distance against a same-magnitude
// positive double). -0.0 cannot arise from any value compared here: every
// producer is either a non-negative sum divided by a strictly positive
// trial count (win_counts[i]/trials, equity_sum[i]/trials, hero_wins/trials,
// chops/trials -- none of these are subtractions that could round to -0.0),
// or -- for vill_winrate below -- clamped through std::max(0.0, x), which
// returns the +0.0 literal for a non-positive x, never -0.0.
std::uint64_t ulp_diff(double a, double b) {
    REQUIRE(a >= 0.0);
    REQUIRE(b >= 0.0);
    const std::uint64_t ua = std::bit_cast<std::uint64_t>(a);
    const std::uint64_t ub = std::bit_cast<std::uint64_t>(b);
    return ua > ub ? ua - ub : ub - ua;
}

// Hero (index 0 in every fixture below) is counted DIRECTLY by both paths:
// HU divides hero_wins/trials, the generic path divides win_counts[0]/
// trials -- the same single IEEE division of identical integers (identical
// because hu_trials == gen_trials is asserted first, and both count wins
// from the same board stream). That makes hero_winrate provably bit-
// identical, same guarantee class as chop_rate -- both are checked with
// hard `==` below, not ulp_diff.
//
// hero_equity additionally folds in chop_rate/2.0 (HU: hero_winrate +
// chop_rate/2.0) vs. a directly accumulated equity_sum[0]/trials (generic)
// -- two different roundings of the chop contribution, so it is not
// provably exact the way hero_winrate is. Empirically 0 ULP across every
// spot in this grid; 1 ULP leaves a hair of headroom without reusing the
// much looser villain bound below.
constexpr std::uint64_t kMaxUlpHeroEquity = 1;

// Villain (index 1) is never counted directly on the HU path -- only
// hero_wins and chops are, so villain's winrate is always the subtraction
// `1.0 - hero_winrate - chop_rate` (equity_hands.cpp's HU branch), while the
// generic path counts villain's wins directly and divides once. That is a
// different rounding path to the same rational number, and it widens (in
// ULP terms, not in absolute terms) as the exact trial count shrinks --
// turn/river boards enumerate as few as 44 / 1 completions, where 1/N has
// coarser ULP spacing than at a broad MC sample size. This grid's own
// turn-street fixture (AA vs KK, villain=KK) hits 6 ULP on both equity and
// winrate deterministically (every options case auto-falls back to the same
// 44-board exact enumeration): a real, reproduced instance of the mechanism
// described above, not test flakiness. Measured separately: max ULP diff
// over 4000 random spots was 180 for equity / 109 for winrate.
// Why 8 ULP: it gives headroom over the observed 6 while staying
// an order of magnitude tighter than that 109-180 ULP ceiling -- a genuine
// regression would need to clear that gap unnoticed. This bound applies
// only to villain; hero gets the much tighter bounds above because it is
// not subject to this subtraction asymmetry.
constexpr std::uint64_t kMaxUlpVillain = 8;

// One player's stats, reshaped from the HU simulator's raw (hero_winrate,
// chop_rate) pair exactly the way compute_player_equities does in
// equity_hands.cpp (see its HU branch). Deliberately duplicated here rather
// than calling compute_player_equities: the point of this test is to
// drive both low-level simulators directly on identical inputs, not to
// re-exercise the dispatcher (which would just call the HU path again).
struct DerivedPlayer {
    double winrate;
    double equity;
};

std::pair<DerivedPlayer, DerivedPlayer> hu_derive(double hero_winrate,
                                                   double chop_rate) {
    const double hero_equity = hero_winrate + chop_rate / 2.0;
    const double vill_equity = 1.0 - hero_equity;
    double vill_winrate = 1.0 - hero_winrate - chop_rate;
    if (vill_winrate < 0.0) vill_winrate = std::max(0.0, vill_winrate);
    return {{hero_winrate, hero_equity}, {vill_winrate, vill_equity}};
}

// One fixed 2-player Hold'em spot: hero/villain hole masks plus the board at
// each of the four streets probed below.
struct Matchup {
    const char* name;
    std::uint64_t hero;
    std::uint64_t vill;
    std::uint64_t preflop;  // 0 cards
    std::uint64_t flop;     // 3 cards
    std::uint64_t turn;     // 4 cards
    std::uint64_t river;    // 5 cards
};

std::vector<Matchup> matchups() {
    std::vector<Matchup> out;

    // AA vs KK: overpair vs underpair, mostly no-chop -- a chop only occurs
    // on the rare board that makes both hands play the same 5 cards.
    {
        const std::uint64_t hero = mk("Ah") | mk("As");
        const std::uint64_t vill = mk("Kd") | mk("Kc");
        const std::uint64_t flop = mk("Jh") | mk("4c") | mk("8s");
        const std::uint64_t turn = flop | mk("2d");
        const std::uint64_t river = turn | mk("6h");
        out.push_back({"AA vs KK", hero, vill, 0, flop, turn, river});
    }

    // AhKh vs AdKd: identical rank multiset (A, K), different suit pair.
    // Every non-flush hand category (pair/two pair/trips/straight/full
    // house/quads/high card) depends only on ranks, so hero and villain play
    // identical hands unless one side completes a flush. The fixed
    // postflop boards below hold at most 2 hearts and 2 diamonds at every
    // street, so neither side can ever complete a flush there -- guaranteed
    // chop at flop/turn/river, and chop-heavy at preflop (only boards with
    // >= 3 hearts or >= 3 diamonds break the tie). This exercises the
    // win/chop split accumulation path on both simulators, which the AA vs
    // KK spot above rarely reaches.
    {
        const std::uint64_t hero = mk("Ah") | mk("Kh");
        const std::uint64_t vill = mk("Ad") | mk("Kd");
        const std::uint64_t flop = mk("2c") | mk("5s") | mk("9c");
        const std::uint64_t turn = flop | mk("Tc");
        const std::uint64_t river = turn | mk("3s");
        out.push_back(
            {"AhKh vs AdKd (chop-heavy)", hero, vill, 0, flop, turn, river});
    }

    return out;
}

struct BoardCase {
    const char* label;
    std::uint64_t mask;
};

struct OptionCase {
    const char* label;
    SimulationOptions opts;
};

std::vector<OptionCase> option_cases() {
    return {
        {"exact", SimulationOptions::exact()},
        {"mc_seeded(20000, 42)", SimulationOptions::mc_seeded(20000, 42)},
        {"mc_seeded(20000, 424242)",
         SimulationOptions::mc_seeded(20000, 424242)},
        {"mc_seeded(20000, 7)", SimulationOptions::mc_seeded(20000, 7)},
    };
}

} // namespace

TEST_CASE("HU vs multiway simulator: agreement on identical inputs") {
    for (const Matchup& mu : matchups()) {
        const BoardCase boards[] = {
            {"preflop", mu.preflop},
            {"flop", mu.flop},
            {"turn", mu.turn},
            {"river", mu.river},
        };
        const std::vector<OptionCase> options = option_cases();

        for (const BoardCase& bc : boards) {
            for (const OptionCase& oc : options) {
                CAPTURE(std::string(mu.name));
                CAPTURE(std::string(bc.label));
                CAPTURE(std::string(oc.label));

                const std::uint64_t used =
                    (mu.hero | mu.vill | bc.mask) & FULL_DECK_MASK;
                const std::vector<int> deck =
                    deck_indices_of(FULL_DECK_MASK & ~used);
                const std::uint64_t master_seed =
                    internal::resolve_master_seed(oc.opts);

                // HU path.
                std::uint64_t hu_trials = 0;
                bool hu_exact = false;
                const auto [hero_winrate, hu_chop] =
                    internal::simulate_heads_up(
                        mu.hero, mu.vill, oc.opts.iterations, bc.mask, deck,
                        master_seed, oc.opts.threads, &hu_trials, &hu_exact);
                const auto [hu_hero, hu_vill] =
                    hu_derive(hero_winrate, hu_chop);

                // Generic multiway path, identical inputs (use_plo = false).
                const std::vector<std::uint64_t> holes = {mu.hero, mu.vill};
                std::uint64_t gen_trials = 0;
                bool gen_exact = false;
                const auto [winrates, equities, std_errors, gen_chop] =
                    internal::simulate_multiway(
                        holes, oc.opts.iterations, bc.mask,
                        /*use_plo=*/false, master_seed, oc.opts.threads,
                        &gen_trials, &gen_exact);

                REQUIRE(winrates.size() == 2);
                REQUIRE(equities.size() == 2);
                REQUIRE(std_errors.size() == 2);

                CHECK(hu_trials == gen_trials);
                CHECK(hu_exact == gen_exact);
                // Bitwise equal, no tolerance: both paths derive chop_rate
                // from the same integer chop/trial counts over the same
                // board stream (measured 0 ULP over 4000 random spots).
                CHECK(hu_chop == gen_chop);

                CAPTURE(hu_hero.equity);
                CAPTURE(equities[0]);
                CAPTURE(hu_hero.winrate);
                CAPTURE(winrates[0]);
                CAPTURE(hu_vill.equity);
                CAPTURE(equities[1]);
                CAPTURE(hu_vill.winrate);
                CAPTURE(winrates[1]);

                // Hero: provably exact winrate (hard ==, see kMaxUlpHeroEquity's
                // comment), tight-but-nonzero equity bound.
                CHECK(hu_hero.winrate == winrates[0]);
                CHECK(ulp_diff(hu_hero.equity, equities[0]) <=
                      kMaxUlpHeroEquity);

                // Villain: wider bound, see kMaxUlpVillain's comment.
                CHECK(ulp_diff(hu_vill.equity, equities[1]) <= kMaxUlpVillain);
                CHECK(ulp_diff(hu_vill.winrate, winrates[1]) <=
                      kMaxUlpVillain);

                // Sanity: equity is a pot share -- both players' shares stay
                // in [0, 1] and sum to the whole pot (within floating-point
                // summation noise, not bit-exactly: equity_sum[0] +
                // equity_sum[1] is exactly 1.0 per trial, but the two totals
                // are divided by trials independently).
                CHECK(equities[0] >= 0.0);
                CHECK(equities[0] <= 1.0);
                CHECK(equities[1] >= 0.0);
                CHECK(equities[1] <= 1.0);
                CHECK(std::abs((equities[0] + equities[1]) - 1.0) < 1e-9);
            }
        }
    }
}
