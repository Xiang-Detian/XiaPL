// Tests for weighted set operations on Range (union/intersection/difference
// and the |/&/- operators). See include/xiapl/range.h for the frozen weight
// semantics (max/min lattice) and CMakeLists.txt for registration.
#include "doctest.h"
#include <xiapl/card.h>
#include <xiapl/game_type.h>
#include <xiapl/range.h>
#include <xiapl/utils.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xiapl;

namespace {

// Sorts a copy of combos by mask, ascending -- used to build an expected
// vector from an arbitrarily-ordered from_string() result.
std::vector<Combo> sorted_by_mask(std::vector<Combo> combos) {
    std::sort(combos.begin(), combos.end(),
              [](const Combo& a, const Combo& b) { return a.mask < b.mask; });
    return combos;
}

bool masks_ascending(const std::vector<Combo>& combos) {
    for (std::size_t i = 1; i < combos.size(); ++i) {
        if (!(combos[i - 1].mask < combos[i].mask)) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("range_union_AA_KK_matches_combined_notation") {
    Range a = Range::from_string("AA");
    Range b = Range::from_string("KK");
    Range expected = Range::from_string("AA,KK");

    Range result = range_union(a, b);

    CHECK(masks_ascending(result.combos()));
    std::vector<Combo> expected_sorted = sorted_by_mask(expected.combos());
    REQUIRE(result.size() == expected_sorted.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        CHECK(result.combos()[i].mask == expected_sorted[i].mask);
        CHECK(result.combos()[i].weight == doctest::Approx(expected_sorted[i].weight));
    }
}

TEST_CASE("range_union_weight_is_max") {
    Range a = Range::from_string("AA:0.3");
    Range b = Range::from_string("AA:0.7");

    Range result = range_union(a, b);

    REQUIRE(result.size() == 6);
    for (const Combo& combo : result.combos()) {
        CHECK(combo.weight == doctest::Approx(0.7));
    }
}

TEST_CASE("range_intersection_weight_is_min") {
    Range a = Range::from_string("AA:0.3");
    Range b = Range::from_string("AA:0.7");

    Range result = range_intersection(a, b);

    REQUIRE(result.size() == 6);
    for (const Combo& combo : result.combos()) {
        CHECK(combo.weight == doctest::Approx(0.3));
    }
}

TEST_CASE("range_difference_weight_is_bounded_subtraction") {
    Range a = Range::from_string("AA:0.7");
    Range b = Range::from_string("AA:0.3");

    Range result = range_difference(a, b);

    REQUIRE(result.size() == 6);
    for (const Combo& combo : result.combos()) {
        CHECK(combo.weight == doctest::Approx(0.4));
    }
}

TEST_CASE("range_difference_self_is_empty") {
    Range a = Range::from_string("AA:0.7");

    Range result = range_difference(a, a);

    CHECK(result.empty());
}

// max(w, w) / min(w, w) are pure selections -- no arithmetic -- so r|r and
// r&r must reproduce r's weights bit-exactly, not just approximately.
TEST_CASE("range_union_is_idempotent") {
    Range r = Range::from_string("AA,KQs:0.5");

    Range result = range_union(r, r);

    std::vector<Combo> expected = sorted_by_mask(r.combos());
    REQUIRE(result.size() == expected.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        CHECK(result.combos()[i].mask == expected[i].mask);
        CHECK(result.combos()[i].weight == expected[i].weight);
    }
}

TEST_CASE("range_intersection_is_idempotent") {
    Range r = Range::from_string("AA,KQs:0.5");

    Range result = range_intersection(r, r);

    std::vector<Combo> expected = sorted_by_mask(r.combos());
    REQUIRE(result.size() == expected.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        CHECK(result.combos()[i].mask == expected[i].mask);
        CHECK(result.combos()[i].weight == expected[i].weight);
    }
}

TEST_CASE("range_intersection_disjoint_is_empty") {
    Range a = Range::from_string("AA");
    Range b = Range::from_string("KK");

    Range result = range_intersection(a, b);

    CHECK(result.empty());
}

TEST_CASE("range_difference_disjoint_leaves_a_unchanged") {
    Range a = Range::from_string("AA");
    Range b = Range::from_string("KK");

    Range result = range_difference(a, b);

    std::vector<Combo> expected = sorted_by_mask(a.combos());
    REQUIRE(result.size() == expected.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        CHECK(result.combos()[i].mask == expected[i].mask);
        CHECK(result.combos()[i].weight == doctest::Approx(expected[i].weight));
    }
}

TEST_CASE("range_intersection_unweighted_common_case_matches_narrower_range") {
    Range a = Range::from_string("TT+");
    Range b = Range::from_string("QQ+");
    Range expected = Range::from_string("QQ+");

    Range result = range_intersection(a, b);

    std::vector<Combo> expected_sorted = sorted_by_mask(expected.combos());
    REQUIRE(result.size() == expected_sorted.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        CHECK(result.combos()[i].mask == expected_sorted[i].mask);
        CHECK(result.combos()[i].weight == doctest::Approx(expected_sorted[i].weight));
    }
}

// Hold'em from_string emits token order, not sorted -- "KK,AA" is deliberately
// the reverse of ascending mask order (AA masks are higher than KK masks
// since Ace is the top rank). The set op must still output ascending order.
TEST_CASE("range_union_holdem_token_order_input_yields_ascending_output") {
    Range a = Range::from_string("KK,AA");
    Range b = Range::from_string("QQ");

    Range result = range_union(a, b);

    CHECK(masks_ascending(result.combos()));
    CHECK(result.size() == 18);
}

TEST_CASE("range_setops_gametype_mismatch_throws_with_both_game_names") {
    Range holdem = Range::from_string("AA");
    Range plo = Range::from_string("AAKKds", GameType::Plo);

    bool threw = false;
    try {
        range_intersection(holdem, plo);
    } catch (const std::invalid_argument& e) {
        threw = true;
        CHECK(std::string(e.what()) ==
              "range_intersection: left range is Hold'em but right range is "
              "PLO; both ranges must be the same game");
    }
    CHECK(threw);
}

TEST_CASE("range_setops_union_gametype_mismatch_message_names_union") {
    Range holdem = Range::from_string("AA");
    Range plo = Range::from_string("AAKKds", GameType::Plo);

    bool threw = false;
    try {
        range_union(holdem, plo);
    } catch (const std::invalid_argument& e) {
        threw = true;
        std::string what = e.what();
        CHECK(what.find("range_union") == 0);
    }
    CHECK(threw);
}

TEST_CASE("range_setops_difference_gametype_mismatch_message_names_difference") {
    Range holdem = Range::from_string("AA");
    Range plo = Range::from_string("AAKKds", GameType::Plo);

    bool threw = false;
    try {
        range_difference(holdem, plo);
    } catch (const std::invalid_argument& e) {
        threw = true;
        std::string what = e.what();
        CHECK(what.find("range_difference") == 0);
    }
    CHECK(threw);
}

TEST_CASE("range_setops_duplicate_mask_in_operand_throws_naming_combo") {
    std::uint64_t mask = cards_to_mask({
        Card::from_string("As"),
        Card::from_string("Kd"),
    });
    Range with_dup(std::vector<Combo>{Combo{mask, 0.5}, Combo{mask, 0.5}});
    Range other = Range::from_string("AA");

    bool threw = false;
    try {
        range_union(with_dup, other);
    } catch (const std::invalid_argument& e) {
        threw = true;
        std::string what = e.what();
        CHECK(what.find("As") != std::string::npos);
        CHECK(what.find("Kd") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("range_setops_duplicate_mask_in_right_operand_throws") {
    std::uint64_t mask = cards_to_mask({
        Card::from_string("As"),
        Card::from_string("Kd"),
    });
    Range with_dup(std::vector<Combo>{Combo{mask, 0.5}, Combo{mask, 0.5}});
    Range other = Range::from_string("AA");

    CHECK_THROWS_AS(range_intersection(other, with_dup), std::invalid_argument);
    CHECK_THROWS_AS(range_difference(other, with_dup), std::invalid_argument);
}

TEST_CASE("range_setops_plo_union_preserves_tag_and_combo_size") {
    Range a = Range::from_string("AAKKds", GameType::Plo);
    Range b = Range::from_string("AAQQds", GameType::Plo);

    Range result = range_union(a, b);

    CHECK(result.game() == GameType::Plo);
    CHECK(masks_ascending(result.combos()));
    for (const Combo& combo : result.combos()) {
        CHECK(popcount64(combo.mask) == 4);
    }
    // AAKKds and AAQQds are disjoint patterns (different second pair rank),
    // so the union is the plain sum of sizes.
    CHECK(result.size() == a.size() + b.size());
}

TEST_CASE("range_setops_operators_match_named_functions") {
    Range a = Range::from_string("AA:0.6,KK");
    Range b = Range::from_string("AA:0.4,QQ");

    Range union_fn = range_union(a, b);
    Range union_op = a | b;
    Range intersection_fn = range_intersection(a, b);
    Range intersection_op = a & b;
    Range difference_fn = range_difference(a, b);
    Range difference_op = a - b;

    REQUIRE(union_fn.size() == union_op.size());
    for (std::size_t i = 0; i < union_fn.size(); ++i) {
        CHECK(union_fn.combos()[i].mask == union_op.combos()[i].mask);
        CHECK(union_fn.combos()[i].weight == doctest::Approx(union_op.combos()[i].weight));
    }

    REQUIRE(intersection_fn.size() == intersection_op.size());
    for (std::size_t i = 0; i < intersection_fn.size(); ++i) {
        CHECK(intersection_fn.combos()[i].mask == intersection_op.combos()[i].mask);
        CHECK(intersection_fn.combos()[i].weight ==
              doctest::Approx(intersection_op.combos()[i].weight));
    }

    REQUIRE(difference_fn.size() == difference_op.size());
    for (std::size_t i = 0; i < difference_fn.size(); ++i) {
        CHECK(difference_fn.combos()[i].mask == difference_op.combos()[i].mask);
        CHECK(difference_fn.combos()[i].weight ==
              doctest::Approx(difference_op.combos()[i].weight));
    }
}

// The raw-combo Range constructor validates only mask popcount, never
// weight, so a Combo{mask, 0.0} is a legal (if unusual) operand. When that
// mask is absent from the other operand, merge_union's pass-through branch
// (not the matched-in-both branch) is the one that must still apply the
// weight <= 0.0 drop.
TEST_CASE("range_union_drops_zero_weight_combo_present_only_in_one_operand") {
    std::uint64_t zero_weight_mask = cards_to_mask({
        Card::from_string("As"),
        Card::from_string("Kd"),
    });
    std::uint64_t positive_mask = cards_to_mask({
        Card::from_string("Ah"),
        Card::from_string("Kh"),
    });
    Range a(std::vector<Combo>{
        Combo{zero_weight_mask, 0.0},
        Combo{positive_mask, 0.3},
    });
    Range b = Range::from_string("QQ");

    Range result = range_union(a, b);

    for (const Combo& combo : result.combos()) {
        CHECK(combo.mask != zero_weight_mask);
    }
    bool found_positive = false;
    for (const Combo& combo : result.combos()) {
        if (combo.mask == positive_mask) {
            found_positive = true;
            CHECK(combo.weight == doctest::Approx(0.3));
        }
    }
    CHECK(found_positive);
    // Only the zero-weight combo is dropped: 1 surviving combo from `a` +
    // all 6 QQ combos from `b`.
    CHECK(result.size() == 1 + b.size());
}

// Same pass-through gap, but for merge_difference's "present only in the
// minuend" branch: a zero-weight combo in `a` whose mask never appears in
// `b` must still be dropped, not pass through unfiltered.
TEST_CASE("range_difference_drops_zero_weight_combo_absent_from_subtrahend") {
    std::uint64_t zero_weight_mask = cards_to_mask({
        Card::from_string("As"),
        Card::from_string("Kd"),
    });
    std::uint64_t positive_mask = cards_to_mask({
        Card::from_string("Ah"),
        Card::from_string("Kh"),
    });
    Range minuend(std::vector<Combo>{
        Combo{zero_weight_mask, 0.0},
        Combo{positive_mask, 0.3},
    });
    Range subtrahend = Range::from_string("QQ");

    Range result = range_difference(minuend, subtrahend);

    for (const Combo& combo : result.combos()) {
        CHECK(combo.mask != zero_weight_mask);
    }
    REQUIRE(result.size() == 1);
    CHECK(result.combos()[0].mask == positive_mask);
    CHECK(result.combos()[0].weight == doctest::Approx(0.3));
}
