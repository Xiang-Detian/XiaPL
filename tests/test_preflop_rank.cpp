// Tests for xiapl::rank_starting_hands / xiapl::generate_top_percent_range
// (Task 5 of the phh-range-utils plan): the exact-table promotion of
// top-percent starting-hand selection from the C ABI's old MC-noise
// implementation into the C++ core.
#include "doctest.h"

#include <xiapl/game_type.h>
#include <xiapl/range.h>

#include <cstddef>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xiapl;

TEST_CASE("rank_starting_hands: full ranking is all 169 canonical labels, AA first") {
    std::vector<std::string> all = rank_starting_hands(1.0);
    REQUIRE(all.size() == 169);
    CHECK(all.front() == "AA");

    std::set<std::string> unique(all.begin(), all.end());
    CHECK(unique.size() == 169);
}

TEST_CASE("rank_starting_hands: default argument matches the explicit 1.0 call") {
    std::vector<std::string> defaulted = rank_starting_hands();
    std::vector<std::string> explicit_full = rank_starting_hands(1.0);
    CHECK(defaulted == explicit_full);
}

TEST_CASE("rank_starting_hands: floor semantics keep floor(169 * top_percent) labels") {
    // floor(169 * 0.2) == 33.
    std::vector<std::string> top20 = rank_starting_hands(0.2);
    CHECK(top20.size() == 33);

    // Below 1/169 (~0.005917): floor(169 * top_percent) == 0.
    std::vector<std::string> tiny = rank_starting_hands(0.005);
    CHECK(tiny.empty());
}

TEST_CASE("rank_starting_hands: top_percent out of (0.0, 1.0] throws") {
    CHECK_THROWS_AS(rank_starting_hands(0.0), std::invalid_argument);
    CHECK_THROWS_AS(rank_starting_hands(1.01), std::invalid_argument);
    CHECK_THROWS_AS(rank_starting_hands(-1.0), std::invalid_argument);
}

TEST_CASE("rank_starting_hands: PLO throws naming the 0.2 roadmap item") {
    bool threw = false;
    try {
        rank_starting_hands(1.0, GameType::Plo);
    } catch (const std::invalid_argument& e) {
        threw = true;
        const std::string what = e.what();
        CHECK(what.find("rank_starting_hands") == 0);
        CHECK(what.find("deferred to 0.2") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("generate_top_percent_range: PLO throws naming the 0.2 roadmap item "
          "with its own function name") {
    bool threw = false;
    try {
        generate_top_percent_range(1.0, GameType::Plo);
    } catch (const std::invalid_argument& e) {
        threw = true;
        const std::string what = e.what();
        CHECK(what.find("generate_top_percent_range") == 0);
        CHECK(what.find("deferred to 0.2") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("generate_top_percent_range: top_percent out of (0.0, 1.0] throws") {
    CHECK_THROWS_AS(generate_top_percent_range(0.0), std::invalid_argument);
    CHECK_THROWS_AS(generate_top_percent_range(1.01), std::invalid_argument);
    CHECK_THROWS_AS(generate_top_percent_range(-1.0), std::invalid_argument);
}

TEST_CASE("generate_top_percent_range: matches the label expansion at 0.2, "
          "all weights 1.0, sorted ascending by mask") {
    std::vector<std::string> labels = rank_starting_hands(0.2);
    REQUIRE(labels.size() == 33);

    std::size_t expected_combos = 0;
    for (const std::string& label : labels) {
        expected_combos += Range::from_string(label).size();
    }

    Range range = generate_top_percent_range(0.2);
    CHECK(range.size() == expected_combos);
    CHECK(range.game() == GameType::Holdem);

    for (std::size_t i = 0; i < range.combos().size(); ++i) {
        CHECK(range.combos()[i].weight == doctest::Approx(1.0));
        if (i > 0) {
            CHECK(range.combos()[i - 1].mask < range.combos()[i].mask);
        }
    }
}

TEST_CASE("generate_top_percent_range: full ranking (1.0) has all 1326 combos") {
    Range range = generate_top_percent_range(1.0);
    CHECK(range.size() == 1326);
}

TEST_CASE("generate_top_percent_range: default game argument is Hold'em") {
    Range defaulted = generate_top_percent_range(0.2);
    Range explicit_holdem = generate_top_percent_range(0.2, GameType::Holdem);
    REQUIRE(defaulted.size() == explicit_holdem.size());
    for (std::size_t i = 0; i < defaulted.size(); ++i) {
        CHECK(defaulted.combos()[i].mask == explicit_holdem.combos()[i].mask);
    }
}
