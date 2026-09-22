#include "doctest.h"
#include <xiapl/card.h>
#include <xiapl/range.h>
#include <xiapl/utils.h>

#include <cmath>
#include <stdexcept>
#include <vector>

using namespace xiapl;

TEST_CASE("range_from_string_parses_existing_notation") {
    Range range = Range::from_string("JJ+, AQs+, KQo");

    CHECK(range.size() == 44);
    CHECK(range.total_weight() == doctest::Approx(44.0));
}

TEST_CASE("range_from_string_parses_weighted_notation") {
    Range range = Range::from_string("AA:1.0, AKs:0.5");

    CHECK(range.size() == 10);
    CHECK(range.total_weight() == doctest::Approx(8.0));

    int half_weight_count = 0;
    for (const Combo& combo : range.combos()) {
        if (std::abs(combo.weight - 0.5) < 1e-12) {
            ++half_weight_count;
        }
    }
    CHECK(half_weight_count == 4);
}

TEST_CASE("range_from_string_parses_exact_combo") {
    Range range = Range::from_string("AsKs:0.25");

    CHECK(range.size() == 1);
    CHECK(range.combos()[0].weight == doctest::Approx(0.25));
    CHECK(range.combos()[0].mask == cards_to_mask({
        Card::from_string("As"),
        Card::from_string("Ks")
    }));
}

TEST_CASE("range_valid_combos_filters_dead_cards") {
    Range range = Range::from_string("AA");
    std::uint64_t dead = cards_to_mask({
        Card::from_string("As")
    });

    std::vector<Combo> valid = range.valid_combos(dead);
    CHECK(valid.size() == 3);
    CHECK(range.total_weight(dead) == doctest::Approx(3.0));
}

TEST_CASE("range_from_string_rejects_duplicate_combos") {
    CHECK_THROWS_AS(Range::from_string("AA, AsAh"), std::invalid_argument);
}

TEST_CASE("range_from_string_rejects_invalid_weight") {
    CHECK_THROWS_AS(Range::from_string("AKs:1.5"), std::invalid_argument);
    CHECK_FALSE(try_parse_range("AKs:1.5").has_value());
}

TEST_CASE("range_from_string_rejects_strtod_only_weight_spellings") {
    // The weight literal is an unsigned decimal, narrower than strtod's
    // grammar on purpose: strtod reads "0x1" as the legal weight 1.0, accepts
    // a leading '+', and produces a NaN for "nan" that slips through a
    // (0.0, 1.0] test because NaN compares false against both bounds.
    CHECK_THROWS_AS(Range::from_string("AKs:0x1"), std::invalid_argument);
    CHECK_THROWS_AS(Range::from_string("AKs:+0.5"), std::invalid_argument);
    CHECK_THROWS_AS(Range::from_string("AKs:nan"), std::invalid_argument);
    CHECK_FALSE(try_parse_range("AKs:0x1").has_value());
    // Exponent notation is a plain decimal literal and stays legal.
    CHECK(Range::from_string("AKs:2.5e-1").combos()[0].weight ==
          doctest::Approx(0.25));
}

TEST_CASE("range_all_contains_all_two_card_combos") {
    Range range = Range::all();

    CHECK(range.size() == 1326);
    CHECK(range.total_weight() == doctest::Approx(1326.0));
}

TEST_CASE("range_from_string_simple_pair_AA") {
    CHECK(Range::from_string("AA").size() == 6);
}

TEST_CASE("range_from_string_JJ_plus") {
    CHECK(Range::from_string("JJ+").size() == 24);  // JJ + QQ + KK + AA = 24
}

TEST_CASE("range_from_string_AQs_plus") {
    CHECK(Range::from_string("AQs+").size() == 8);  // AQs + AKs = 8
}

TEST_CASE("range_from_string_unsuffixed_AK") {
    CHECK(Range::from_string("AK").size() == 16);  // AKs(4) + AKo(12)
}

TEST_CASE("range_from_string_pair_dash") {
    CHECK(Range::from_string("TT-88").size() == 18);  // TT + 99 + 88 = 18
}

TEST_CASE("range_from_string_connector_dash") {
    CHECK(Range::from_string("76s-54s").size() == 12);  // 76s + 65s + 54s = 12
}

TEST_CASE("range_from_string_highcard_dash") {
    // A9o..A2o = 8 ranks * 12 combos
    CHECK(Range::from_string("A9o-A2o").size() == 96);
}

TEST_CASE("range_from_string_invalid_mixed_suitedness_throws") {
    CHECK_THROWS_AS(Range::from_string("A9s-A2o"), std::invalid_argument);
}

TEST_CASE("range_from_string_AK_plus_unsuffixed") {
    // AK+ unsuffixed: AK is the highest non-pair Ace-X, so AK+ == AK == 16
    CHECK(Range::from_string("AK+").size() == 16);
}

TEST_CASE("range_from_string_connector_plus_98s") {
    // 98s+: 98s, T9s, JTs, QJs, KQs, AKs = 6 * 4 = 24
    CHECK(Range::from_string("98s+").size() == 24);
}
