#include "doctest.h"
#include <xiapl/card.h>

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

using namespace xiapl;

TEST_CASE("test_card_to_string") {
    Card card(14, 3); // Ace of spades
    CHECK(card.to_string() == "As");
}

TEST_CASE("test_card_repr") {
    Card card(13, 2); // King of hearts
    CHECK(card.repr() == "<Card Kh>");
}

TEST_CASE("test_card_from_string") {
    Card card = Card::from_string("Qs");
    CHECK(card.rank() == 12);
    CHECK(card.suit() == 3); // spades = 3
}

TEST_CASE("test_card_equality") {
    Card c1(10, 1); // Td
    Card c2(10, 1); // Td
    Card c3(10, 2); // Th

    CHECK(c1 == c2);
    CHECK(c1 != c3);
}

TEST_CASE("test_card_order") {
    Card c1(10, 1); // Td
    Card c2(11, 1); // Jd

    CHECK(c1 < c2);
}

TEST_CASE("test_card_from_id_consistency") {
    // Round-trip: from_string -> id -> from_id -> verify rank/suit match
    std::vector<std::string> samples = {"As", "Kh", "Td", "7c", "2h", "Qd"};

    for (const auto& s : samples) {
        Card c1 = Card::from_string(s);
        Card::IdType cid = c1.id;

        Card c2 = Card::from_id(cid);

        CHECK(c1.rank() == c2.rank());
        CHECK(c1.suit() == c2.suit());
        CHECK(c1.id == c2.id);
        CHECK(c1 == c2);
    }
}

TEST_CASE("test_card_rank_suit_enum_constructor") {
    Card c(Rank::Ace, Suit::Spades);
    CHECK(c.id == 14 - 2 + 3 * 13);  // suit*13 + (rank-2)
    CHECK(c.rank() == 14);
    CHECK(c.suit() == 3);
    CHECK(c.rank_enum() == Rank::Ace);
    CHECK(c.suit_enum() == Suit::Spades);
    CHECK(c.to_string() == "As");
}

TEST_CASE("test_card_rank_suit_enum_round_trip") {
    // Every (Rank, Suit) round-trips via accessors
    for (int r = 2; r <= 14; ++r) {
        for (int s = 0; s < 4; ++s) {
            Card c(static_cast<Rank>(r), static_cast<Suit>(s));
            CHECK(static_cast<int>(c.rank_enum()) == r);
            CHECK(static_cast<int>(c.suit_enum()) == s);
            CHECK(c == Card(r, s));
        }
    }
}

TEST_CASE("test_try_parse_card_success") {
    auto c = try_parse_card("As");
    REQUIRE(c.has_value());
    CHECK(c->rank_enum() == Rank::Ace);
    CHECK(c->suit_enum() == Suit::Spades);

    auto t = try_parse_card("Td");
    REQUIRE(t.has_value());
    CHECK(t->rank() == 10);
    CHECK(t->suit() == 1);

    // Lowercase rank/suit accepted
    auto lo = try_parse_card("ah");
    REQUIRE(lo.has_value());
    CHECK(lo->rank() == 14);
    CHECK(lo->suit() == 2);
}

TEST_CASE("test_try_parse_card_failure") {
    CHECK_FALSE(try_parse_card("").has_value());
    CHECK_FALSE(try_parse_card("A").has_value());      // too short
    CHECK_FALSE(try_parse_card("Ash").has_value());    // too long
    CHECK_FALSE(try_parse_card("Xs").has_value());     // bad rank
    CHECK_FALSE(try_parse_card("Ax").has_value());     // bad suit
    CHECK_FALSE(try_parse_card("1s").has_value());     // rank not in [2..A]
}

TEST_CASE("test_try_parse_card_does_not_throw") {
    // from_string throws, try_parse_card returns nullopt — verify the contract.
    CHECK_THROWS(Card::from_string("Xs"));
    CHECK_NOTHROW(try_parse_card("Xs"));
}

TEST_CASE("test_card_hash") {
    Card c1(10, 1); // Td
    Card c2(10, 1); // Td (same card)
    Card c3(14, 3); // As (different card)

    std::hash<Card> hasher;

    // Two equal cards must have the same hash
    CHECK(hasher(c1) == hasher(c2));

    // Hash can be used in unordered containers
    std::unordered_set<Card> card_set;
    card_set.insert(c1);
    card_set.insert(c2);
    card_set.insert(c3);

    // c1 and c2 are equal, so set should contain 2 elements
    CHECK(card_set.size() == 2);
}
