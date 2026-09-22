#include "doctest.h"
#include <xiapl/card.h>
#include <xiapl/range.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace xiapl;

namespace {

std::uint64_t mk(const std::string& s) {
    return card_to_mask(Card::from_string(s));
}

} // namespace

TEST_CASE("calculate_equity heads-up MC equity sum ≈ 1") {
    std::vector<std::uint64_t> hands = {
        mk("As") | mk("Ah"),
        mk("Kd") | mk("Kc"),
    };

    SimulationOptions opt;
    opt.iterations = 1000;
    opt.deterministic = true;
    opt.seed = 7;

    auto result = calculate_equity(hands, 0, opt, GameType::Holdem);
    REQUIRE(result.players.size() == 2);
    CHECK(result.chop_rate >= 0.0);
    CHECK(result.chop_rate <= 1.0);

    double equity_sum = 0.0;
    for (const auto& pe : result.players) {
        CHECK(pe.winrate >= 0.0);
        CHECK(pe.winrate <= 1.0);
        CHECK(pe.equity >= 0.0);
        CHECK(pe.equity <= 1.0);
        equity_sum += pe.equity;
    }
    CHECK(equity_sum == doctest::Approx(1.0).epsilon(0.05));
}

TEST_CASE("calculate_equity 3way equity sum ≈ 1") {
    std::vector<std::uint64_t> hands = {
        mk("As") | mk("Ah"),
        mk("Kd") | mk("Kc"),
        mk("Qs") | mk("Qc"),
    };

    SimulationOptions opt;
    opt.iterations = 500;
    opt.deterministic = true;
    opt.seed = 11;

    auto result = calculate_equity(hands, 0, opt, GameType::Holdem);
    REQUIRE(result.players.size() == 3);

    double equity_sum = 0.0;
    for (const auto& pe : result.players) equity_sum += pe.equity;
    CHECK(equity_sum == doctest::Approx(1.0).epsilon(0.05));
}

TEST_CASE("calculate_range_equity AA dominates KK heads-up") {
    Range hero = Range::from_string("AA");
    Range villain = Range::from_string("KK");

    SimulationOptions opt;
    opt.iterations = 0; // exact: AA vs KK preflop
    auto result = calculate_range_equity(hero, villain, 0ULL, opt);

    CHECK(result.exact == true);
    // AA vs KK heads-up preflop equity is ~81%
    CHECK(result.hero_aggregate_equity > 0.78);
    CHECK(result.hero_aggregate_equity < 0.83);
    CHECK(result.villain_aggregate_equity ==
          doctest::Approx(1.0 - result.hero_aggregate_equity).epsilon(1e-9));
}

TEST_CASE("calculate_range_equity ranks AA above KK against same villain range") {
    // Hero contains AA and KK; expect AA per-combo > KK per-combo against AKs.
    Range hero = Range::from_string("AA, KK");
    Range villain = Range::from_string("AKs");

    SimulationOptions opt;
    opt.iterations = 1000;
    opt.deterministic = true;
    opt.seed = 31;

    auto result = calculate_range_equity(hero, villain, 0ULL, opt);
    REQUIRE(!result.hero.empty());

    double aa_sum = 0.0;
    int aa_n = 0;
    double kk_sum = 0.0;
    int kk_n = 0;
    for (const auto& e : result.hero) {
        // AA combos have both bits at rank 14; KK at rank 13.
        // Rank id within suit = id % 13. AA -> rank = 12; KK -> rank = 11.
        // Cheap test: bits 12, 25, 38, 51 are aces; bits 11, 24, 37, 50 kings.
        bool is_aa = (e.combo_mask & ((1ULL << 12) | (1ULL << 25) |
                                      (1ULL << 38) | (1ULL << 51))) == e.combo_mask;
        bool is_kk = (e.combo_mask & ((1ULL << 11) | (1ULL << 24) |
                                      (1ULL << 37) | (1ULL << 50))) == e.combo_mask;
        if (is_aa) { aa_sum += e.equity; ++aa_n; }
        else if (is_kk) { kk_sum += e.equity; ++kk_n; }
    }
    REQUIRE(aa_n > 0);
    REQUIRE(kk_n > 0);
    CHECK(aa_sum / aa_n > kk_sum / kk_n);
}
