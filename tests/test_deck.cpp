#include "doctest.h"
#include <xiapl/deck.h>

#include <algorithm>
#include <numeric>
#include <vector>

using namespace xiapl;

TEST_CASE("test_deck_initialization") {
    Deck deck;
    CHECK(deck.size() == 52);
}

TEST_CASE("test_deck_shuffle") {
    Deck deck;
    deck.shuffle();
    // Shuffle must preserve the number of cards
    CHECK(deck.size() == 52);
}

TEST_CASE("Deck::shuffle(seed) is reproducible, including seed 0") {
    for (std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{42}}) {
        Deck a, b;
        a.shuffle(seed);
        b.shuffle(seed);
        REQUIRE(a.size() == b.size());
        while (a.size() > 0) {
            CHECK(a.deal_one().id == b.deal_one().id);
        }
    }
    Deck c, d;
    c.shuffle(1);
    d.shuffle(2);
    bool any_diff = false;
    while (c.size() > 0) {
        if (c.deal_one().id != d.deal_one().id) { any_diff = true; break; }
    }
    CHECK(any_diff);
}

TEST_CASE("test_deal_cards") {
    Deck deck;
    std::vector<Card> dealt = deck.deal(5);
    CHECK(dealt.size() == 5);
    CHECK(deck.size() == 47);
}

TEST_CASE("test_deal_single_card") {
    Deck deck;
    std::vector<Card> single = deck.deal(1);
    CHECK(single.size() == 1);
    CHECK(deck.size() == 51);
}

TEST_CASE("test_remove_cards") {
    Deck deck;
    int initial_size = deck.size();

    // Get the first 3 cards to remove
    std::vector<Card> cards = deck.get_cards();
    std::vector<Card> to_remove = {cards[0], cards[1], cards[2]};

    deck.remove_cards(to_remove);
    CHECK(deck.size() == initial_size - 3);

    // Verify removed cards are no longer present
    std::vector<Card> remaining = deck.get_cards();
    for (const auto& removed : to_remove) {
        bool found = false;
        for (const auto& c : remaining) {
            if (c == removed) {
                found = true;
                break;
            }
        }
        CHECK_FALSE(found);
    }
}

TEST_CASE("test_set_cards") {
    Deck deck;
    std::vector<Card> new_cards = {
        Card(14, 3), // Ace of spades
        Card(13, 2), // King of hearts
        Card(12, 1), // Queen of diamonds
    };
    deck.set_cards(new_cards);

    CHECK(deck.size() == 3);

    std::vector<Card> cards = deck.get_cards();
    CHECK(cards[0].rank() == 14);
    CHECK(cards[0].suit() == 3);
    CHECK(cards[1].rank() == 13);
    CHECK(cards[1].suit() == 2);
    CHECK(cards[2].rank() == 12);
    CHECK(cards[2].suit() == 1);
}

TEST_CASE("test_deck_size_and_empty") {
    Deck deck;

    // Initial state: 52 cards, not empty
    CHECK(deck.size() == 52);
    CHECK_FALSE(deck.empty());

    // Deal all 52 cards
    deck.deal(52);
    CHECK(deck.size() == 0);
    CHECK(deck.empty());
}

TEST_CASE("test_deck_has_cards") {
    Deck deck;

    CHECK(deck.has_cards(1));
    CHECK(deck.has_cards(52));
    CHECK_FALSE(deck.has_cards(53));

    // Deal 51, leaving 1
    deck.deal(51);
    CHECK(deck.has_cards(1));
    CHECK_FALSE(deck.has_cards(2));
}

TEST_CASE("test_deal_one") {
    Deck deck;
    int before_size = deck.size();

    Card c = deck.deal_one();

    // Returns a single Card and deck shrinks by 1
    CHECK(c.id != Card::INVALID_ID);
    CHECK(deck.size() == before_size - 1);
}

TEST_CASE("test_burn_cards") {
    Deck deck;
    int before_size = deck.size();

    // Burn 1 card
    deck.burn(1);
    CHECK(deck.size() == before_size - 1);

    // Burn 2 more cards
    deck.burn(2);
    CHECK(deck.size() == before_size - 3);
}

TEST_CASE("test_card_ids_property") {
    Deck deck;
    std::vector<int> ids = deck.get_card_ids();

    // Initial state: 52 IDs covering 0..51
    CHECK(ids.size() == 52);

    std::vector<int> sorted_ids = ids;
    std::sort(sorted_ids.begin(), sorted_ids.end());

    std::vector<int> expected(52);
    std::iota(expected.begin(), expected.end(), 0); // 0, 1, ..., 51

    CHECK(sorted_ids == expected);

    // After dealing some cards, size must still be consistent
    deck.deal(5);
    CHECK(deck.get_card_ids().size() == static_cast<size_t>(deck.size()));
}
