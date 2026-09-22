#include "doctest.h"

#include <xiapl/card.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include <stdexcept>
#include <string>
#include <vector>

using namespace xiapl;

namespace {

std::uint64_t mk(const std::string& s) {
    return card_to_mask(Card::from_string(s));
}

// N distinct, pairwise-disjoint 2-card Hold'em hands built from consecutive
// card ids (0,1), (2,3), ... -- valid for N up to 26 (52 cards / 2 per hand).
std::vector<std::uint64_t> distinct_holdem_hands(int n) {
    std::vector<std::uint64_t> hands;
    hands.reserve(n);
    for (int i = 0; i < n; ++i) {
        hands.push_back((1ULL << (2 * i)) | (1ULL << (2 * i + 1)));
    }
    return hands;
}

} // namespace

// ---------------------------------------------------------------------------
// calculate_equity player-count cap.
//
// Before this fix, calculate_equity with 11 Hold'em players threw from
// *inside* the per-trial Monte Carlo lambda -- on a worker thread whenever
// iterations > 0 -- with the internal symbol name
// "judge_holdem_mask: too many players" in the message. That contradicted
// the "validate up front so the per-trial lambda never throws" contract and
// leaked an internal name to a public caller. The fix validates the
// player-count cap in internal::validate_equity_input (board_sample.h),
// called once from compute_player_equities before either simulator
// dispatches a single trial -- so the throw is now synchronous, on the
// caller's own thread, with a calculate_equity-named message, regardless of
// SimulationOptions::threads.
// ---------------------------------------------------------------------------

TEST_CASE("calculate_equity rejects 11 Hold'em players with a public up-front message") {
    SimulationOptions opt = SimulationOptions::exact();
    auto hands = distinct_holdem_hands(11);

    // Plain try/catch: confirms the exception type, that it is thrown
    // synchronously from this (the caller's) thread -- no worker thread is
    // ever spawned for SimulationOptions::exact() -- and that the message
    // names the public entry point rather than an internal symbol.
    bool threw = false;
    try {
        calculate_equity(hands, 0, opt, GameType::Holdem);
    } catch (const std::invalid_argument& e) {
        threw = true;
        const std::string msg = e.what();
        CHECK(msg == "calculate_equity: at most 10 players supported for Hold'em (got 11)");
        CHECK(msg.find("judge_holdem_mask") == std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("calculate_equity 11-player cap throws std::invalid_argument with the public message") {
    // Death-free plain assertion form (no threads involved, no worker
    // dispatch to race against): the cap check is a simple size comparison,
    // so this alone already demonstrates it cannot be coming from inside a
    // showdown lambda.
    auto hands = distinct_holdem_hands(11);
    REQUIRE_THROWS_WITH_AS(
        calculate_equity(hands, 0, SimulationOptions::exact(), GameType::Holdem),
        "calculate_equity: at most 10 players supported for Hold'em (got 11)",
        std::invalid_argument);
}

TEST_CASE("calculate_equity 11-player cap throw stays synchronous with threads=8 Monte Carlo") {
    // Regression for the original defect shape: SimulationOptions selecting
    // the threaded Monte Carlo path must not change which thread the
    // rejection comes from, or its message. If the cap check regressed back
    // into the per-trial lambda, this would either hang/crash inside
    // run_mc_chunks's worker pool or surface the internal
    // "judge_holdem_mask: too many players" message instead.
    SimulationOptions opt = SimulationOptions::mc_seeded(5000, 424242);
    opt.threads = 8;
    auto hands = distinct_holdem_hands(11);

    REQUIRE_THROWS_WITH_AS(
        calculate_equity(hands, 0, opt, GameType::Holdem),
        "calculate_equity: at most 10 players supported for Hold'em (got 11)",
        std::invalid_argument);
}

TEST_CASE("calculate_equity accepts exactly 10 Hold'em players (cap boundary)") {
    SimulationOptions opt = SimulationOptions::mc_seeded(100, 1);
    auto hands = distinct_holdem_hands(10);

    auto result = calculate_equity(hands, 0, opt, GameType::Holdem);
    CHECK(result.players.size() == 10);
    CHECK(result.trials == 100ULL);
}

TEST_CASE("calculate_equity rejects more than 32 PLO players with a public up-front message") {
    // The player-count cap check runs before the hole-mask overlap check
    // (internal::validate_equity_input, board_sample.h), so a single reused
    // 4-card mask is enough to exercise it: 33 genuinely disjoint PLO hands
    // would need 132 distinct cards, which does not fit in a 52-card deck.
    std::uint64_t one_hand = mk("As") | mk("Ks") | mk("Ah") | mk("Kh");
    std::vector<std::uint64_t> hands(33, one_hand);

    REQUIRE_THROWS_WITH_AS(
        calculate_equity(hands, 0, SimulationOptions::exact(), GameType::Plo),
        "calculate_equity: at most 32 players supported for PLO (got 33)",
        std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Regression guard: unifying the validators must not disturb the
// pre-existing checks that used to be duplicated across simulate_heads_up,
// compute_player_equities and calculate_range_equity.
// ---------------------------------------------------------------------------

TEST_CASE("calculate_equity still rejects a physically-impossible board count") {
    SimulationOptions opt = SimulationOptions::exact();
    std::uint64_t bad_board = mk("2c"); // 1 board card: neither 0 nor 3-5
    CHECK_THROWS_AS(
        calculate_equity(distinct_holdem_hands(2), bad_board, opt, GameType::Holdem),
        std::runtime_error);
}

TEST_CASE("calculate_equity still rejects negative iterations") {
    SimulationOptions opt;
    opt.iterations = -1;
    CHECK_THROWS_AS(
        calculate_equity(distinct_holdem_hands(2), 0, opt, GameType::Holdem),
        std::runtime_error);
}

TEST_CASE("calculate_equity with zero players is still a silent empty-result no-op") {
    // Pre-existing contract, unchanged by the validator unification: zero
    // players returns an empty EquityResult without validating board_mask at
    // all -- must NOT start throwing now that validation moved up front.
    SimulationOptions opt = SimulationOptions::exact();
    std::uint64_t bad_board = mk("2c"); // would be rejected for any non-empty input
    auto result = calculate_equity({}, bad_board, opt, GameType::Holdem);
    CHECK(result.players.empty());
    CHECK(result.trials == 0ULL);
}
