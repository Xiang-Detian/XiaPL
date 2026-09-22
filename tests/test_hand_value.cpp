#include "doctest.h"
#include <xiapl/card.h>
#include <xiapl/eval.h>
#include <xiapl/game_type.h>
#include <xiapl/hand_value.h>
#include <xiapl/utils.h>

#include <stdexcept>
#include <vector>

using namespace xiapl;

TEST_CASE("hand_value_comparison_uses_category_then_kickers") {
    HandValue pair_aces = make_hand_value(
        HandCategory::OnePair,
        {14, 13, 9, 7}
    );
    HandValue two_pair = make_hand_value(
        HandCategory::TwoPair,
        {2, 2, 14}
    );
    HandValue pair_kings = make_hand_value(
        HandCategory::OnePair,
        {13, 14, 9, 7}
    );

    CHECK(two_pair > pair_aces);
    CHECK(pair_aces > pair_kings);
    CHECK(pair_aces == pair_aces);
    CHECK(pair_aces != pair_kings);
}

TEST_CASE("evaluate_cards_returns_hand_value") {
    std::vector<Card> cards = {
        Card::from_string("As"), Card::from_string("Ah"), Card::from_string("Ac"),
        Card::from_string("Kd"), Card::from_string("Ks"),
        Card::from_string("3d"), Card::from_string("2c")
    };

    HandValue value = evaluate_cards(cards);
    CHECK(value.category == HandCategory::FullHouse);
    CHECK(value.kicker_count == 2);
    CHECK(static_cast<int>(value.kickers[0]) == 14);
    CHECK(static_cast<int>(value.kickers[1]) == 13);
    CHECK(to_string(value.category) == "Full House");
}

TEST_CASE("evaluate_hand_returns_hand_value") {
    std::uint64_t board = cards_to_mask({
        Card::from_string("Ah"),
        Card::from_string("Kd"),
        Card::from_string("7s"),
        Card::from_string("2c"),
        Card::from_string("9h")
    });
    std::uint64_t hole = cards_to_mask({
        Card::from_string("As"),
        Card::from_string("Ac")
    });

    HandValue value = evaluate_hand(board, hole);
    CHECK(value.category == HandCategory::ThreeOfAKind);
    CHECK(static_cast<int>(value.kickers[0]) == 14);
}

TEST_CASE("evaluate_hand_plo_uses_exactly_two_hole_cards") {
    std::uint64_t hole = cards_to_mask({
        Card::from_string("As"),
        Card::from_string("Ah"),
        Card::from_string("Kd"),
        Card::from_string("Ks")
    });
    std::uint64_t board = cards_to_mask({
        Card::from_string("Ac"),
        Card::from_string("Kc"),
        Card::from_string("2d"),
        Card::from_string("3h"),
        Card::from_string("3s")
    });

    HandValue value = evaluate_hand(board, hole, GameType::Plo);
    CHECK(value.category == HandCategory::FullHouse);
    CHECK(static_cast<int>(value.kickers[0]) == 14);
    CHECK(static_cast<int>(value.kickers[1]) == 3);
}

TEST_CASE("judge_returns_best_player") {
    std::uint64_t board = cards_to_mask({
        Card::from_string("6s"),
        Card::from_string("7s"),
        Card::from_string("8s"),
        Card::from_string("9c"),
        Card::from_string("Td")
    });
    std::vector<std::uint64_t> holes = {
        cards_to_mask({
            Card::from_string("2s"),
            Card::from_string("3s")
        }),
        cards_to_mask({
            Card::from_string("4h"),
            Card::from_string("5h")
        })
    };

    CHECK(judge(holes, board) == std::vector<int>{0});
}

TEST_CASE("judge_returns_chop") {
    std::uint64_t board = cards_to_mask({
        Card::from_string("Ks"),
        Card::from_string("Kh"),
        Card::from_string("2d"),
        Card::from_string("3c"),
        Card::from_string("7s")
    });
    std::vector<std::uint64_t> holes = {
        cards_to_mask({
            Card::from_string("As"),
            Card::from_string("Ah")
        }),
        cards_to_mask({
            Card::from_string("Ad"),
            Card::from_string("Ac")
        })
    };

    CHECK(judge(holes, board) == std::vector<int>{0, 1});
}

TEST_CASE("judge_plo_returns_best_player") {
    std::uint64_t board = cards_to_mask({
        Card::from_string("Ac"),
        Card::from_string("Kc"),
        Card::from_string("2d"),
        Card::from_string("3h"),
        Card::from_string("5s")
    });
    std::vector<std::uint64_t> holes = {
        cards_to_mask({
            Card::from_string("As"),
            Card::from_string("Ah"),
            Card::from_string("9d"),
            Card::from_string("8c")
        }),
        cards_to_mask({
            Card::from_string("Ks"),
            Card::from_string("Kh"),
            Card::from_string("9h"),
            Card::from_string("8d")
        })
    };

    CHECK(judge(holes, board, GameType::Plo) == std::vector<int>{0});
}

TEST_CASE("judge_rejects_overlapping_cards") {
    std::uint64_t board = cards_to_mask({
        Card::from_string("Ah"),
        Card::from_string("Kd"),
        Card::from_string("7s"),
        Card::from_string("2c"),
        Card::from_string("9h")
    });
    std::vector<std::uint64_t> holes = {
        cards_to_mask({
            Card::from_string("As"),
            Card::from_string("Ac")
        }),
        cards_to_mask({
            Card::from_string("As"),
            Card::from_string("Qd")
        })
    };

    CHECK_THROWS_AS(judge(holes, board), std::runtime_error);
}
