#include "doctest.h"
#include <xiapl/card.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace xiapl;

namespace {

std::uint64_t mk(const std::string& s) {
    return card_to_mask(Card::from_string(s));
}

std::vector<std::uint64_t> two_holdem_hands() {
    return {mk("As") | mk("Ah"), mk("Kd") | mk("Kc")};
}

std::vector<std::uint64_t> three_holdem_hands() {
    return {mk("As") | mk("Ah"), mk("Kd") | mk("Kc"), mk("Qs") | mk("Qc")};
}

std::vector<std::uint64_t> two_plo_hands() {
    return {
        mk("As") | mk("Ks") | mk("Ah") | mk("Kh"),
        mk("Qd") | mk("Jd") | mk("Qc") | mk("Jc"),
    };
}

} // namespace

TEST_CASE("calculate_equity exact heads-up returns matching trials") {
    SimulationOptions opt;
    opt.iterations = 0; // exact enumeration

    auto result = calculate_equity(two_holdem_hands(), 0, opt, GameType::Holdem);

    REQUIRE(result.players.size() == 2);
    CHECK(result.exact == true);
    // C(48, 5) = 1712304 exact board completions for HU preflop
    CHECK(result.trials == 1712304ULL);

    double sum = result.players[0].equity + result.players[1].equity;
    CHECK(sum == doctest::Approx(1.0).epsilon(1e-9));
    CHECK(result.players[0].std_error == doctest::Approx(0.0));
    CHECK(result.players[1].std_error == doctest::Approx(0.0));
}

TEST_CASE("calculate_equity Monte Carlo is reproducible with same seed") {
    SimulationOptions opt;
    opt.iterations = 5000;
    opt.deterministic = true;
    opt.seed = 12345;

    auto r1 = calculate_equity(two_holdem_hands(), 0, opt, GameType::Holdem);
    auto r2 = calculate_equity(two_holdem_hands(), 0, opt, GameType::Holdem);

    REQUIRE(r1.players.size() == 2);
    REQUIRE(r2.players.size() == 2);
    CHECK(r1.exact == false);
    CHECK(r1.trials == 5000ULL);
    CHECK(r2.trials == 5000ULL);

    // Bit-exact reproducibility under the same seed.
    CHECK(r1.players[0].winrate == r2.players[0].winrate);
    CHECK(r1.players[0].equity == r2.players[0].equity);
    CHECK(r1.players[0].std_error == r2.players[0].std_error);
    CHECK(r1.players[1].winrate == r2.players[1].winrate);
    CHECK(r1.players[1].equity == r2.players[1].equity);
    CHECK(r1.chop_rate == r2.chop_rate);
}

TEST_CASE("calculate_equity Monte Carlo is reproducible with seed 0") {
    // Regression guard: FastRng(seed) treats seed == 0 as "pick a random
    // seed" (see fast_rng.h), so a caller path that ever routes seed 0
    // through that ctor instead of FastRng::seed(0) would silently break
    // determinism for this legal, default-valued seed.
    SimulationOptions opt;
    opt.iterations = 20000;
    opt.deterministic = true;
    opt.seed = 0;

    auto r1 = calculate_equity(two_holdem_hands(), 0, opt, GameType::Holdem);
    auto r2 = calculate_equity(two_holdem_hands(), 0, opt, GameType::Holdem);

    REQUIRE(r1.players.size() == 2);
    REQUIRE(r2.players.size() == 2);
    CHECK(r1.trials == 20000ULL);
    CHECK(r2.trials == 20000ULL);
    CHECK(r1.players[0].winrate == r2.players[0].winrate);
    CHECK(r1.players[0].equity == r2.players[0].equity);
    CHECK(r1.players[1].winrate == r2.players[1].winrate);
    CHECK(r1.players[1].equity == r2.players[1].equity);
}

TEST_CASE("calculate_equity different seeds usually give different samples") {
    SimulationOptions a;
    a.iterations = 5000;
    a.deterministic = true;
    a.seed = 1;

    SimulationOptions b = a;
    b.seed = 2;

    auto ra = calculate_equity(two_holdem_hands(), 0, a, GameType::Holdem);
    auto rb = calculate_equity(two_holdem_hands(), 0, b, GameType::Holdem);

    // We don't assert strict inequality (a collision is theoretically possible)
    // but for 5000-iteration MC with two distinct seeds it should hold.
    bool any_diff = false;
    for (int i = 0; i < 2; ++i) {
        if (ra.players[i].equity != rb.players[i].equity) {
            any_diff = true;
            break;
        }
    }
    CHECK(any_diff);
}

TEST_CASE("calculate_equity multiway exact (3 players)") {
    SimulationOptions opt;
    opt.iterations = 0;

    auto result = calculate_equity(three_holdem_hands(), 0, opt, GameType::Holdem);

    REQUIRE(result.players.size() == 3);
    CHECK(result.exact == true);
    // C(46, 5) = 1370754
    CHECK(result.trials == 1370754ULL);

    double sum = 0.0;
    for (const auto& pe : result.players) sum += pe.equity;
    CHECK(sum == doctest::Approx(1.0).epsilon(1e-9));
}

TEST_CASE("calculate_equity PLO Monte Carlo is reproducible with same seed") {
    SimulationOptions opt;
    opt.iterations = 2000;
    opt.deterministic = true;
    opt.seed = 999;

    auto r1 = calculate_equity(two_plo_hands(), 0, opt, GameType::Plo);
    auto r2 = calculate_equity(two_plo_hands(), 0, opt, GameType::Plo);

    REQUIRE(r1.players.size() == 2);
    CHECK(r1.exact == false);
    CHECK(r1.trials == 2000ULL);
    CHECK(r1.players[0].equity == r2.players[0].equity);
    CHECK(r1.players[1].equity == r2.players[1].equity);
    CHECK(r1.chop_rate == r2.chop_rate);
}

TEST_CASE("calculate_equity exact with fixed flop") {
    SimulationOptions opt;
    opt.iterations = 0;

    std::uint64_t flop = mk("2c") | mk("7d") | mk("Jh");

    auto result = calculate_equity(two_holdem_hands(), flop, opt, GameType::Holdem);

    REQUIRE(result.players.size() == 2);
    CHECK(result.exact == true);
    // C(45, 2) turn+river completions = 990
    CHECK(result.trials == 990ULL);
    double sum = result.players[0].equity + result.players[1].equity;
    CHECK(sum == doctest::Approx(1.0).epsilon(1e-9));
}

TEST_CASE("calculate_equity rejects negative iterations") {
    SimulationOptions opt;
    opt.iterations = -1;
    CHECK_THROWS(calculate_equity(two_holdem_hands(), 0, opt, GameType::Holdem));
}
