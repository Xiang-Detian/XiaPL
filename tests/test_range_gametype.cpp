// Tests for the GameType tag on Range (PLO range-support foundation, task 1
// of the plo-range-ws1 plan). Hold'em behavior must stay bit-for-bit
// unchanged; these tests cover the new tag/validation surface only.
#include "doctest.h"
#include <xiapl/card.h>
#include <xiapl/game_type.h>
#include <xiapl/range.h>
#include <xiapl/utils.h>

#include <stdexcept>
#include <vector>

using namespace xiapl;

TEST_CASE("range_default_ctor_tags_holdem") {
    Range range;
    CHECK(range.game() == GameType::Holdem);
}

TEST_CASE("range_all_tags_holdem") {
    Range range = Range::all();
    CHECK(range.game() == GameType::Holdem);
}

TEST_CASE("range_from_string_one_arg_tags_holdem") {
    Range range = Range::from_string("AA");
    CHECK(range.game() == GameType::Holdem);
}

TEST_CASE("range_from_string_two_arg_holdem_matches_one_arg") {
    Range one_arg = Range::from_string("AA");
    Range two_arg = Range::from_string("AA", GameType::Holdem);

    CHECK(two_arg.game() == GameType::Holdem);
    REQUIRE(two_arg.size() == one_arg.size());
    for (std::size_t i = 0; i < one_arg.size(); ++i) {
        CHECK(two_arg.combos()[i].mask == one_arg.combos()[i].mask);
        CHECK(two_arg.combos()[i].weight == one_arg.combos()[i].weight);
    }
}

// Task 3 replaced the "PLO parsing is not yet supported" placeholder with
// the real parser (see tests/test_plo_parser.cpp for the grammar coverage).
// What this file still pins is the tag-level contract: the PLO overload
// parses PLO notation, tags the result Plo, and does NOT silently accept
// Hold'em notation ("AA" is a 2-symbol pattern, and a PLO pattern needs 4).
TEST_CASE("range_from_string_plo_parses_plo_notation") {
    Range range = Range::from_string("AAKKds", GameType::Plo);
    CHECK(range.game() == GameType::Plo);
    CHECK(range.size() == 6);
    for (const Combo& combo : range.combos()) {
        CHECK(popcount64(combo.mask) == 4);
    }
}

TEST_CASE("range_from_string_plo_rejects_holdem_notation") {
    bool threw = false;
    try {
        Range::from_string("AA", GameType::Plo);
    } catch (const std::invalid_argument& e) {
        threw = true;
        std::string what = e.what();
        // Teaching error: a PLO pattern is 4 symbols, pad with '*'.
        CHECK(what.find("AA**") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("range_raw_combo_plo_construction_succeeds") {
    std::uint64_t mask = cards_to_mask({
        Card::from_string("As"),
        Card::from_string("Kd"),
        Card::from_string("Qh"),
        Card::from_string("Jc"),
    });
    Range range(std::vector<Combo>{Combo{mask, 1.0}}, GameType::Plo);

    CHECK(range.game() == GameType::Plo);
    REQUIRE(range.size() == 1);
    CHECK(range.combos()[0].mask == mask);
    CHECK(popcount64(range.combos()[0].mask) == 4);
}

TEST_CASE("range_raw_combo_two_bit_mask_under_plo_throws") {
    std::uint64_t mask = cards_to_mask({
        Card::from_string("As"),
        Card::from_string("Kd"),
    });
    CHECK_THROWS_AS(
        Range(std::vector<Combo>{Combo{mask, 1.0}}, GameType::Plo),
        std::invalid_argument);
}

TEST_CASE("range_raw_combo_four_bit_mask_under_holdem_throws") {
    std::uint64_t mask = cards_to_mask({
        Card::from_string("As"),
        Card::from_string("Kd"),
        Card::from_string("Qh"),
        Card::from_string("Jc"),
    });
    CHECK_THROWS_AS(
        Range(std::vector<Combo>{Combo{mask, 1.0}}, GameType::Holdem),
        std::invalid_argument);
    // Legacy 1-arg raw-combo ctor keeps Holdem semantics and the same
    // validation.
    CHECK_THROWS_AS(
        Range(std::vector<Combo>{Combo{mask, 1.0}}),
        std::invalid_argument);
}

// The error spells the offending combo as concatenated card names (matching
// the PLO parser's own diagnostic style; see internal::describe_mask in
// src/core/range_parse.h), not a raw decimal mask value.
TEST_CASE("range_raw_combo_error_names_offending_mask") {
    std::uint64_t mask = cards_to_mask({
        Card::from_string("As"),
        Card::from_string("Kd"),
    });
    bool threw = false;
    try {
        Range(std::vector<Combo>{Combo{mask, 1.0}}, GameType::Plo);
    } catch (const std::invalid_argument& e) {
        threw = true;
        std::string what = e.what();
        CHECK(what.find("As") != std::string::npos);
        CHECK(what.find("Kd") != std::string::npos);
        CHECK(what.find(std::to_string(mask)) == std::string::npos);
    }
    CHECK(threw);
}

// mask=0b11 sets bits 0 and 1, which (id = suit*13 + (rank-2), suit 0=C) are
// "2c" (id 0) and "3c" (id 1); describe_mask renders low-bit-first, so the
// message contains "2c3c".
TEST_CASE("range_raw_combo_two_bit_mask_error_spells_2c3c") {
    bool threw = false;
    try {
        Range(std::vector<Combo>{Combo{0b11ULL, 1.0}}, GameType::Plo);
    } catch (const std::invalid_argument& e) {
        threw = true;
        std::string what = e.what();
        CHECK(what.find("2c3c") != std::string::npos);
    }
    CHECK(threw);
}

// A bit outside the 52-card deck cannot be spelled as a card; describe_mask
// falls back to a "#<decimal>" rendering instead of throwing while building
// its own error message.
TEST_CASE("range_raw_combo_out_of_deck_bit_error_falls_back_to_decimal") {
    const std::uint64_t mask = (1ULL << 55) | 0b111ULL;
    bool threw = false;
    try {
        Range(std::vector<Combo>{Combo{mask, 1.0}}, GameType::Holdem);
    } catch (const std::invalid_argument& e) {
        threw = true;
        std::string what = e.what();
        CHECK(what.find("#" + std::to_string(mask)) != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("range_empty_range_keeps_explicit_tag") {
    Range holdem(std::vector<Combo>{}, GameType::Holdem);
    Range plo(std::vector<Combo>{}, GameType::Plo);

    CHECK(holdem.empty());
    CHECK(holdem.game() == GameType::Holdem);
    CHECK(plo.empty());
    CHECK(plo.game() == GameType::Plo);
}

TEST_CASE("range_all_plo_arm_enumerates_all_combos") {
    Range range = Range::all(GameType::Plo);

    CHECK(range.game() == GameType::Plo);
    REQUIRE(range.size() == 270725);

    for (const Combo& combo : range.combos()) {
        CHECK(popcount64(combo.mask) == 4);
        CHECK(combo.weight == doctest::Approx(1.0));
    }
}

TEST_CASE("range_all_dispatches_by_game_holdem_default") {
    const Range holdem = Range::all();
    CHECK(holdem.game() == GameType::Holdem);
    CHECK(holdem.combos().size() == 1326);

    const Range plo = Range::all(GameType::Plo);
    CHECK(plo.game() == GameType::Plo);
    REQUIRE(plo.combos().size() == 270725);
    // Task 2 contract preserved through the rename: ascending-mask (colex)
    // order, so a seeded MC stream never depends on the enumeration route.
    for (std::size_t i = 1; i < plo.combos().size(); ++i) {
        REQUIRE(plo.combos()[i - 1].mask < plo.combos()[i].mask);
    }
}
