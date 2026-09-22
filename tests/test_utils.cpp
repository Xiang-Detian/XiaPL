#include "doctest.h"
#include <xiapl/utils.h>
#include <xiapl/card.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

using namespace xiapl;

TEST_CASE("test_card_to_mask_single_bit") {
    Card c = Card::from_string("As");
    std::uint64_t mask = card_to_mask(c);

    CHECK(popcount64(mask) == 1);
    CHECK(mask == (1ULL << c.id));
}

TEST_CASE("test_cards_to_mask_multiple") {
    Card as = Card::from_string("As");
    Card kh = Card::from_string("Kh");
    Card td = Card::from_string("Td");
    std::vector<Card> cards = {as, kh, td};

    std::uint64_t mask = cards_to_mask(cards);

    CHECK(popcount64(mask) == 3);
    CHECK((mask & (1ULL << as.id)) != 0);
    CHECK((mask & (1ULL << kh.id)) != 0);
    CHECK((mask & (1ULL << td.id)) != 0);
}

TEST_CASE("test_mask_to_cards_roundtrip") {
    std::vector<Card> original = {
        Card::from_string("As"),
        Card::from_string("Kh"),
        Card::from_string("Td"),
        Card::from_string("7c"),
        Card::from_string("2h")
    };

    std::uint64_t mask = cards_to_mask(original);
    std::vector<Card> recovered = mask_to_cards(mask);

    std::vector<int> original_ids;
    for (const auto& c : original) {
        original_ids.push_back(static_cast<int>(c.id));
    }
    std::sort(original_ids.begin(), original_ids.end());

    std::vector<int> recovered_ids;
    for (const auto& c : recovered) {
        recovered_ids.push_back(static_cast<int>(c.id));
    }
    std::sort(recovered_ids.begin(), recovered_ids.end());

    CHECK(original_ids == recovered_ids);
}

TEST_CASE("test_mask_to_ids_roundtrip") {
    std::vector<Card> original = {
        Card::from_string("As"),
        Card::from_string("Kh"),
        Card::from_string("Td"),
        Card::from_string("7c"),
        Card::from_string("2h")
    };

    std::vector<int> original_ids;
    for (const auto& c : original) {
        original_ids.push_back(static_cast<int>(c.id));
    }
    std::sort(original_ids.begin(), original_ids.end());

    std::uint64_t mask = cards_to_mask(original);
    std::vector<int> recovered_ids = mask_to_ids(mask);
    std::sort(recovered_ids.begin(), recovered_ids.end());

    CHECK(original_ids == recovered_ids);
}

TEST_CASE("test_cards_to_mask_is_OR") {
    std::vector<Card> cards = {
        Card::from_string("As"),
        Card::from_string("Kh"),
        Card::from_string("Td"),
        Card::from_string("7c"),
        Card::from_string("2h")
    };

    std::uint64_t combined = cards_to_mask(cards);

    std::uint64_t manual_or = 0;
    for (const auto& c : cards) {
        manual_or |= card_to_mask(c);
    }

    CHECK(combined == manual_or);
}

TEST_CASE("test_popcount64") {
    CHECK(popcount64(0) == 0);
    CHECK(popcount64(1) == 1);
    CHECK(popcount64(FULL_DECK_MASK) == 52);
}

TEST_CASE("test_ids_to_mask_vector") {
    std::vector<int> ids = {0, 1, 51};
    std::uint64_t mask = ids_to_mask(ids);

    CHECK((mask & (1ULL << 0)) != 0);
    CHECK((mask & (1ULL << 1)) != 0);
    CHECK((mask & (1ULL << 51)) != 0);
    CHECK(popcount64(mask) == 3);
}

TEST_CASE("test_ids_to_mask_vector_rejects_negative") {
    std::vector<int> ids = {0, -1, 5};
    CHECK_THROWS_AS(ids_to_mask(ids), std::invalid_argument);
}

TEST_CASE("test_ids_to_mask_vector_rejects_out_of_range") {
    std::vector<int> ids = {0, 52};
    CHECK_THROWS_AS(ids_to_mask(ids), std::invalid_argument);
}
