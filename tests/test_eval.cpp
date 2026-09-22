#include "doctest.h"
#include <xiapl/card.h>
#include <xiapl/eval.h>
#include <xiapl/game_type.h>
#include <xiapl/hand_value.h>
#include <xiapl/utils.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xiapl;

namespace {

std::uint64_t mk(const std::string& s) {
    return card_to_mask(Card::from_string(s));
}

std::uint64_t cards(std::initializer_list<const char*> labels) {
    std::uint64_t mask = 0;
    for (auto s : labels) mask |= mk(s);
    return mask;
}

std::vector<Card> to_cards(std::initializer_list<const char*> labels) {
    std::vector<Card> result;
    for (auto s : labels) result.push_back(Card::from_string(s));
    return result;
}

} // namespace

TEST_CASE("evaluate_cards detects full house") {
    auto hv = evaluate_cards(to_cards({"As", "Ah", "Ac", "Kd", "Ks", "3d", "2c"}));
    CHECK(hv.category == HandCategory::FullHouse);
    CHECK(hv.kickers[0] == 14);
    CHECK(hv.kickers[1] == 13);
    CHECK(describe_hand(hv).find("Full House") != std::string::npos);
}

TEST_CASE("evaluate_cards detects straight flush") {
    auto hv = evaluate_cards(to_cards({"9s", "Ts", "Js", "Qs", "Ks", "2d", "3c"}));
    CHECK(hv.category == HandCategory::StraightFlush);
    CHECK(hv.kickers[0] == 13);
}

TEST_CASE("evaluate_cards detects four of a kind") {
    auto hv = evaluate_cards(to_cards({"9s", "9h", "9d", "9c", "2d", "3c", "5h"}));
    CHECK(hv.category == HandCategory::FourOfAKind);
    CHECK(hv.kickers[0] == 9);
    CHECK(hv.kickers[1] == 5);
}

TEST_CASE("evaluate_cards detects three of a kind") {
    auto hv = evaluate_cards(to_cards({"8s", "8h", "8d", "Kd", "7s", "3c", "2d"}));
    CHECK(hv.category == HandCategory::ThreeOfAKind);
    CHECK(hv.kickers[0] == 8);
}

TEST_CASE("evaluate_cards detects two pair") {
    auto hv = evaluate_cards(to_cards({"Js", "Jd", "9h", "9c", "2s", "4d", "3h"}));
    CHECK(hv.category == HandCategory::TwoPair);
    CHECK(hv.kickers[0] == 11);
    CHECK(hv.kickers[1] == 9);
}

TEST_CASE("evaluate_cards detects one pair") {
    auto hv = evaluate_cards(to_cards({"Qs", "Qh", "3s", "5d", "7h", "9c", "2d"}));
    CHECK(hv.category == HandCategory::OnePair);
    CHECK(hv.kickers[0] == 12);
}

TEST_CASE("evaluate_cards detects high card") {
    auto hv = evaluate_cards(to_cards({"As", "Kc", "Td", "9s", "7c", "4h", "2d"}));
    CHECK(hv.category == HandCategory::HighCard);
    CHECK(hv.kickers[0] == 14);
}

TEST_CASE("evaluate_cards detects wheel straight (A-2-3-4-5)") {
    auto hv = evaluate_cards(to_cards({"Ah", "2s", "3d", "4c", "5h", "9d", "Kd"}));
    CHECK(hv.category == HandCategory::Straight);
    CHECK(hv.kickers[0] == 5);
}

TEST_CASE("evaluate_cards detects flush") {
    auto hv = evaluate_cards(to_cards({"2s", "5s", "7s", "9s", "Ks", "3d", "4h"}));
    CHECK(hv.category == HandCategory::Flush);
    CHECK(hv.kickers[0] == 13);
}

TEST_CASE("judge: flush beats straight") {
    std::vector<std::uint64_t> players = {
        cards({"2s", "3s"}),
        cards({"4h", "5h"}),
    };
    std::uint64_t board = cards({"6s", "7s", "8s", "9c", "Td"});
    auto winners = judge(players, board);
    CHECK(winners == std::vector<int>{0});
}

TEST_CASE("judge: higher flush wins") {
    std::vector<std::uint64_t> players = {
        cards({"2s", "3s"}),
        cards({"4s", "5s"}),
    };
    std::uint64_t board = cards({"6s", "7s", "8c", "9c", "Ts"});
    auto winners = judge(players, board);
    CHECK(winners == std::vector<int>{1});
}

TEST_CASE("judge: split pot on identical board straight") {
    std::vector<std::uint64_t> players = {
        cards({"As", "Ah"}),
        cards({"Ad", "Ac"}),
    };
    std::uint64_t board = cards({"Ks", "Kh", "2d", "3c", "7s"});
    auto winners = judge(players, board);
    REQUIRE(winners.size() == 2);
    CHECK(winners[0] == 0);
    CHECK(winners[1] == 1);
}

TEST_CASE("judge: kicker decides pair-vs-pair") {
    std::vector<std::uint64_t> players = {
        cards({"As", "7h"}),
        cards({"Ac", "2d"}),
    };
    std::uint64_t board = cards({"Ah", "Ks", "3d", "6c", "9h"});
    auto winners = judge(players, board);
    CHECK(winners == std::vector<int>{0});
}

TEST_CASE("evaluate_hand detects full house using 2 hole + 3 board") {
    std::uint64_t hole = cards({"As", "Ah", "Kd", "Ks"});
    std::uint64_t board = cards({"Ac", "Kc", "2d", "3h", "3s"});
    auto hv = evaluate_hand(board, hole, GameType::Plo);
    CHECK(hv.category == HandCategory::FullHouse);
    CHECK(hv.kickers[0] == 14);
    CHECK(hv.kickers[1] == 3);
}

TEST_CASE("judge: higher two pair (aces) wins over kings") {
    std::vector<std::uint64_t> players = {
        cards({"As", "Ah", "9d", "8c"}),
        cards({"Ks", "Kh", "9h", "8d"}),
    };
    std::uint64_t board = cards({"Ac", "Kc", "2d", "3h", "5s"});
    auto winners = judge(players, board, GameType::Plo);
    CHECK(winners == std::vector<int>{0});
}

// Unified game-dispatch entry points. The fixtures are the per-game cases
// above, replayed through the single entry point: the default must be the
// Hold'em arm, and the explicit Plo tag must reach the PLO arm.
TEST_CASE("judge dispatches by game, Holdem default") {
    // Same inputs as "judge: flush beats straight" above.
    std::vector<std::uint64_t> holdem_players = {
        cards({"2s", "3s"}),
        cards({"4h", "5h"}),
    };
    std::uint64_t holdem_board = cards({"6s", "7s", "8s", "9c", "Td"});
    CHECK(judge(holdem_players, holdem_board) == std::vector<int>{0});
    CHECK(judge(holdem_players, holdem_board) ==
          judge(holdem_players, holdem_board, GameType::Holdem));

    // Same inputs as "judge: higher two pair (aces) wins over kings" above.
    std::vector<std::uint64_t> plo_players = {
        cards({"As", "Ah", "9d", "8c"}),
        cards({"Ks", "Kh", "9h", "8d"}),
    };
    std::uint64_t plo_board = cards({"Ac", "Kc", "2d", "3h", "5s"});
    CHECK(judge(plo_players, plo_board, GameType::Plo) == std::vector<int>{0});
}

// The public entry point validates before it dispatches, so an empty player
// list throws rather than returning {}. The empty-in / empty-out behavior
// belongs to the internal winner-bitmask helper, not to `judge` -- this pins
// the header contract on the public side for both game arms.
TEST_CASE("judge rejects an empty player list") {
    const std::vector<std::uint64_t> no_players;
    std::uint64_t board = cards({"6s", "7s", "8s", "9c", "Td"});
    CHECK_THROWS_AS(judge(no_players, board), std::runtime_error);
    CHECK_THROWS_AS(judge(no_players, board, GameType::Plo), std::runtime_error);
}

TEST_CASE("evaluate_hand dispatches by game, Holdem default") {
    // Hold'em fixture from tests/test_hand_value.cpp.
    std::uint64_t holdem_board = cards({"Ah", "Kd", "7s", "2c", "9h"});
    std::uint64_t holdem_hole = cards({"As", "Ac"});
    auto holdem = evaluate_hand(holdem_board, holdem_hole);
    CHECK(holdem.category == HandCategory::ThreeOfAKind);
    CHECK(holdem.kickers[0] == 14);
    auto holdem_explicit =
        evaluate_hand(holdem_board, holdem_hole, GameType::Holdem);
    CHECK(holdem.category == holdem_explicit.category);
    CHECK(holdem.kickers == holdem_explicit.kickers);

    // PLO fixture from "evaluate_hand detects full house using 2 hole + 3
    // board" above.
    std::uint64_t plo_hole = cards({"As", "Ah", "Kd", "Ks"});
    std::uint64_t plo_board = cards({"Ac", "Kc", "2d", "3h", "3s"});
    auto plo = evaluate_hand(plo_board, plo_hole, GameType::Plo);
    CHECK(plo.category == HandCategory::FullHouse);
    CHECK(plo.kickers[0] == 14);
    CHECK(plo.kickers[1] == 3);
}

TEST_CASE("edge: steel wheel (A2345 straight flush)") {
    // Ah 2h 3h 4h 5h + Kc Qd
    auto hv = evaluate_mask(cards({"Ah", "2h", "3h", "4h", "5h", "Kc", "Qd"}));
    CHECK(hv.category == HandCategory::StraightFlush);
    CHECK(hv.kickers[0] == 5);  // 5-high
}
TEST_CASE("edge: wheel straight (A2345 offsuit)") {
    auto hv = evaluate_mask(cards({"Ah", "2c", "3h", "4d", "5s", "Kc", "Qd"}));
    CHECK(hv.category == HandCategory::Straight);
    CHECK(hv.kickers[0] == 5);
}
TEST_CASE("edge: quads on board + higher kicker in hand") {
    // board 8888 + A kicker
    auto hv = evaluate_mask(cards({"8c", "8d", "8h", "8s", "2c", "Ah", "3d"}));
    CHECK(hv.category == HandCategory::FourOfAKind);
    CHECK(hv.kickers[0] == 8);
    CHECK(hv.kickers[1] == 14);
}
TEST_CASE("edge: double trips = full house, higher trips wins") {
    auto hv = evaluate_mask(cards({"9c", "9d", "9h", "5c", "5d", "5h", "Ac"}));
    CHECK(hv.category == HandCategory::FullHouse);
    CHECK(hv.kickers[0] == 9);
    CHECK(hv.kickers[1] == 5);
}
TEST_CASE("edge: trips + two pairs = full house with best pair") {
    auto hv = evaluate_mask(cards({"9c", "9d", "9h", "5c", "5d", "7h", "7c"}));
    CHECK(hv.category == HandCategory::FullHouse);
    CHECK(hv.kickers[0] == 9);
    CHECK(hv.kickers[1] == 7);
}
TEST_CASE("edge: 7-card flush picks top 5") {
    auto hv = evaluate_mask(cards({"2h", "4h", "6h", "8h", "Th", "Qh", "Ah"}));
    CHECK(hv.category == HandCategory::Flush);
    CHECK(hv.kickers[0] == 14); CHECK(hv.kickers[1] == 12); CHECK(hv.kickers[2] == 10);
    CHECK(hv.kickers[3] == 8);  CHECK(hv.kickers[4] == 6);
}
