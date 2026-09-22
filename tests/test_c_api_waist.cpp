// Behavioural tests for the waist v1 C ABI (scalar half).
//
// The waist is a projection of the C++ API, so almost every check here is a
// COMPARISON against include/xiapl/*: the C entry point must answer what the
// C++ entry point answers, including its exception class and its exact
// message. Structure and layout are pinned separately, in pure C, by
// tests/test_c_api_abi.c.

#include "doctest.h"

#include <xiapl/c_api.h>

// The waist's private error plumbing. Included on purpose: std::out_of_range
// and std::bad_alloc have no public entry point that raises them, so the only
// honest way to pin all four status classes is to exercise wrap() directly and
// observe the same thread-local slot xiapl_last_error_message() reads.
#include <api/waist_error.h>

#include <xiapl/canonicalize.h>
#include <xiapl/card.h>
#include <xiapl/deck.h>
#include <xiapl/eval.h>
#include <xiapl/game_type.h>
#include <xiapl/hand_value.h>
#include <xiapl/range.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>
#include <xiapl/version.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Card ids: id = suit * 13 + (rank - 2), suit 0=c,1=d,2=h,3=s.
constexpr std::uint8_t kAs = 51, kKs = 50, kQs = 49, kJs = 48, kTs = 47;
constexpr std::uint8_t kAh = 38, kQh = 36, kAd = 25, kKd = 24, kKc = 11;
constexpr std::uint8_t k2c = 0, k3d = 14, k7h = 31, k9h = 33, k9s = 46;

// what() of a call that is expected to throw; "" if it did not.
template <class F>
std::string cpp_message(F&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        return std::string(e.what());
    }
    return std::string();
}

std::uint64_t mask_of(const std::vector<std::uint8_t>& ids) {
    std::uint64_t mask = 0;
    REQUIRE(xiapl_cards_to_mask(ids.data(), static_cast<std::int32_t>(ids.size()),
                                &mask) == XIAPL_OK);
    return mask;
}

// Fetch a string through the buffer protocol, exercising query-then-fill.
template <class F>
std::string fetch_string(F&& fn) {
    std::int32_t length = -1;
    REQUIRE(fn(nullptr, 0, &length) == XIAPL_OK);
    REQUIRE(length >= 0);
    std::vector<char> buf(static_cast<std::size_t>(length) + 1, '\xEE');
    std::int32_t again = -1;
    REQUIRE(fn(buf.data(), length + 1, &again) == XIAPL_OK);
    CHECK(again == length);
    CHECK(buf[static_cast<std::size_t>(length)] == '\0');
    return std::string(buf.data());
}

xiapl_hand_value_t waist_evaluate_cards(const std::vector<std::uint8_t>& ids) {
    xiapl_hand_value_t value{};
    REQUIRE(xiapl_evaluate_cards(ids.data(), static_cast<std::int32_t>(ids.size()),
                                 &value) == XIAPL_OK);
    return value;
}

void check_same_hand_value(const xiapl_hand_value_t& waist,
                           const xiapl::HandValue& cpp) {
    CHECK(waist.category == static_cast<std::int32_t>(cpp.category));
    CHECK(waist.kicker_count == static_cast<std::int32_t>(cpp.kicker_count));
    for (std::size_t i = 0; i < XIAPL_HAND_VALUE_MAX_KICKERS; ++i) {
        CHECK(waist.kickers[i] == static_cast<std::int32_t>(cpp.kickers[i]));
    }
}

}  // namespace

TEST_CASE("waist reports its ABI and library version") {
    CHECK(xiapl_c_abi_version() == XIAPL_C_ABI_VERSION);

    std::int32_t major = -1, minor = -1, patch = -1;
    CHECK(xiapl_version(&major, &minor, &patch) == XIAPL_OK);
    CHECK(major == XIAPL_VERSION_MAJOR);
    CHECK(minor == XIAPL_VERSION_MINOR);
    CHECK(patch == XIAPL_VERSION_PATCH);
    CHECK(std::strcmp(xiapl_version_string(), XIAPL_VERSION_STRING) == 0);
}

TEST_CASE("waist card accessors agree with xiapl::Card on every id") {
    for (int raw = 0; raw < 52; ++raw) {
        const std::uint8_t id = static_cast<std::uint8_t>(raw);
        const xiapl::Card card = xiapl::Card::from_id(id);

        std::int32_t rank = -1, suit = -1;
        CHECK(xiapl_card_rank(id, &rank) == XIAPL_OK);
        CHECK(xiapl_card_suit(id, &suit) == XIAPL_OK);
        CHECK(rank == card.rank());
        CHECK(suit == card.suit());

        CHECK(fetch_string([&](char* b, std::int32_t c, std::int32_t* l) {
                  return xiapl_card_to_string(id, b, c, l);
              }) == card.to_string());
        CHECK(fetch_string([&](char* b, std::int32_t c, std::int32_t* l) {
                  return xiapl_card_repr(id, b, c, l);
              }) == card.repr());

        std::uint64_t mask = 0;
        CHECK(xiapl_card_to_mask(id, &mask) == XIAPL_OK);
        CHECK(mask == xiapl::card_to_mask(card));

        std::uint8_t round_trip = 0;
        CHECK(xiapl_card_from_string(card.to_string().c_str(), &round_trip) == XIAPL_OK);
        CHECK(round_trip == id);

        std::uint8_t from_parts = 0;
        CHECK(xiapl_card_from_rank_suit(card.rank(), card.suit(), &from_parts) == XIAPL_OK);
        CHECK(from_parts == id);
    }
}

TEST_CASE("waist card_from_id keeps the C++ sentinel contract") {
    // An id outside [0, 51] is an ANSWER (XIAPL_CARD_INVALID_ID), not an
    // error -- xiapl::Card::from_id verbatim.
    for (int raw : {52, 100, 200, 255}) {
        std::uint8_t id = 7;
        CHECK(xiapl_card_from_id(static_cast<std::uint8_t>(raw), &id) == XIAPL_OK);
        CHECK(id == XIAPL_CARD_INVALID_ID);
        CHECK(id == xiapl::Card::from_id(static_cast<std::uint8_t>(raw)).id);
    }
    // The accessors, by contrast, reject the same ids (header contract).
    std::int32_t rank = 0;
    CHECK(xiapl_card_rank(XIAPL_CARD_INVALID_ID, &rank) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_card_suit(60, &rank) == XIAPL_ERR_INVALID_ARGUMENT);
    // ... while to_string / repr format it as "Invalid", like a default Card.
    CHECK(fetch_string([](char* b, std::int32_t c, std::int32_t* l) {
              return xiapl_card_to_string(XIAPL_CARD_INVALID_ID, b, c, l);
          }) == "Invalid");
    CHECK(fetch_string([](char* b, std::int32_t c, std::int32_t* l) {
              return xiapl_card_repr(200, b, c, l);
          }) == "<Card Invalid>");
}

TEST_CASE("waist char parsers agree with the C++ non-throwing parsers") {
    for (int ch = 0; ch < 128; ++ch) {
        const char c = static_cast<char>(ch);
        std::int32_t value = -1, found = -1;

        CHECK(xiapl_try_rank_from_char(c, &value, &found) == XIAPL_OK);
        const auto rank = xiapl::try_rank_from_char(c);
        CHECK(found == (rank ? 1 : 0));
        CHECK(value == (rank ? *rank : 0));

        CHECK(xiapl_try_suit_from_char(c, &value, &found) == XIAPL_OK);
        const auto suit = xiapl::try_suit_from_char(c);
        CHECK(found == (suit ? 1 : 0));
        CHECK(value == (suit ? *suit : 0));
    }

    for (int rank = 2; rank <= 14; ++rank) {
        char c = 0;
        CHECK(xiapl_rank_to_char(rank, &c) == XIAPL_OK);
        CHECK(c == xiapl::rank_to_char(rank));
    }
    char c = 0;
    // xiapl::rank_to_char answers '?' here; the waist contract is an error.
    CHECK(xiapl_rank_to_char(1, &c) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_rank_to_char(15, &c) == XIAPL_ERR_INVALID_ARGUMENT);
}

TEST_CASE("waist mask helpers agree with xiapl::utils") {
    const std::vector<std::uint8_t> ids = {kAs, kAh, k2c, k7h};
    const std::uint64_t mask = mask_of(ids);
    CHECK(mask == xiapl::cards_to_mask({xiapl::Card::from_id(kAs),
                                        xiapl::Card::from_id(kAh),
                                        xiapl::Card::from_id(k2c),
                                        xiapl::Card::from_id(k7h)}));

    // Duplicates collapse silently, exactly as in C++.
    const std::vector<std::uint8_t> with_dupes = {kAs, kAs, kAh, k2c, k7h, k7h};
    CHECK(mask_of(with_dupes) == mask);

    for (std::uint64_t probe : {std::uint64_t{0}, mask, XIAPL_FULL_DECK_MASK,
                                mask | (std::uint64_t{1} << 60)}) {
        const std::vector<int> expected = xiapl::mask_to_ids(probe);
        std::int32_t total = -1;
        CHECK(xiapl_mask_to_ids(probe, nullptr, 0, &total) == XIAPL_OK);
        CHECK(total == static_cast<std::int32_t>(expected.size()));

        std::vector<std::uint8_t> got(expected.size() + 1, 0xEE);
        std::int32_t again = -1;
        CHECK(xiapl_mask_to_ids(probe, got.data(), total, &again) == XIAPL_OK);
        CHECK(again == total);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(got[i] == static_cast<std::uint8_t>(expected[i]));
        }
        CHECK(got[expected.size()] == 0xEE);  // nothing written past capacity
    }

    // A short buffer is not an error (convention 4).
    std::uint8_t small[2] = {0xEE, 0xEE};
    std::int32_t total = -1;
    CHECK(xiapl_mask_to_ids(mask, small, 2, &total) == XIAPL_OK);
    CHECK(total == 4);
    CHECK(small[0] == k2c);

    std::int32_t per_hand = -1;
    CHECK(xiapl_cards_per_hand(XIAPL_GAME_HOLDEM, &per_hand) == XIAPL_OK);
    CHECK(per_hand == xiapl::cards_per_hand(xiapl::GameType::Holdem));
    CHECK(xiapl_cards_per_hand(XIAPL_GAME_PLO, &per_hand) == XIAPL_OK);
    CHECK(per_hand == xiapl::cards_per_hand(xiapl::GameType::Plo));
    CHECK(xiapl_cards_per_hand(2, &per_hand) == XIAPL_ERR_INVALID_ARGUMENT);
}

TEST_CASE("waist evaluation agrees with the C++ evaluator") {
    const std::vector<std::vector<std::uint8_t>> hands = {
        {kTs, kJs, kQs, kKs, kAs},              // straight flush
        {kAs, kAh, kKd, kKc, k2c},              // two pair
        {kAs, kAh, kKd, kKc, k2c, k3d},         // six loose cards
        {kAs, kAh, kKd, kKc, k2c, k3d, k7h},    // seven loose cards
        {k2c, k3d, k7h, k9s, kQh},              // high card
    };
    for (const auto& ids : hands) {
        std::vector<xiapl::Card> cards;
        for (std::uint8_t id : ids) cards.push_back(xiapl::Card::from_id(id));
        const xiapl::HandValue expected = xiapl::evaluate_cards(cards);

        check_same_hand_value(waist_evaluate_cards(ids), expected);

        xiapl_hand_value_t from_mask{};
        CHECK(xiapl_evaluate_mask(mask_of(ids), &from_mask) == XIAPL_OK);
        check_same_hand_value(from_mask, xiapl::evaluate_mask(mask_of(ids)));

        CHECK(fetch_string([&](char* b, std::int32_t c, std::int32_t* l) {
                  const xiapl_hand_value_t value = waist_evaluate_cards(ids);
                  return xiapl_describe_hand(&value, b, c, l);
              }) == xiapl::describe_hand(expected));
    }

    const std::uint64_t board = mask_of({kQs, kJs, kTs, k2c, k3d});
    const std::uint64_t hero = mask_of({kAs, kAh});
    const std::uint64_t villain = mask_of({kKd, kKc});

    xiapl_hand_value_t value{};
    CHECK(xiapl_evaluate_hand(board, hero, XIAPL_GAME_HOLDEM, &value) == XIAPL_OK);
    check_same_hand_value(value, xiapl::evaluate_hand(board, hero, xiapl::GameType::Holdem));

    const std::uint64_t plo_hero = mask_of({kAs, kAh, kKd, kKc});
    CHECK(xiapl_evaluate_hand(board, plo_hero, XIAPL_GAME_PLO, &value) == XIAPL_OK);
    check_same_hand_value(value, xiapl::evaluate_hand(board, plo_hero, xiapl::GameType::Plo));

    // Showdown: heads-up, three-handed, and a chop (both players hold A9 and
    // play the same A-Q-J-T-9).
    const std::vector<std::vector<std::uint64_t>> tables = {
        {hero, villain},
        {hero, villain, mask_of({k7h, k9s})},
        {mask_of({kAh, k9s}), mask_of({kAd, k9h})},
    };
    for (const auto& holes : tables) {
        const std::vector<int> expected = xiapl::judge(holes, board, xiapl::GameType::Holdem);
        std::vector<std::int32_t> winners(holes.size(), -1);
        std::int32_t count = -1;
        CHECK(xiapl_judge(holes.data(), static_cast<std::int32_t>(holes.size()), board,
                          XIAPL_GAME_HOLDEM, winners.data(),
                          static_cast<std::int32_t>(winners.size()), &count) == XIAPL_OK);
        CHECK(count == static_cast<std::int32_t>(expected.size()));
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(winners[i] == expected[i]);
        }
    }
}

TEST_CASE("waist hand category names agree with xiapl::to_string") {
    for (int category = XIAPL_HAND_HIGH_CARD; category <= XIAPL_HAND_STRAIGHT_FLUSH;
         ++category) {
        CHECK(fetch_string([&](char* b, std::int32_t c, std::int32_t* l) {
                  return xiapl_hand_category_name(category, b, c, l);
              }) == xiapl::to_string(static_cast<xiapl::HandCategory>(category)));
    }
    // An unknown category is an error here (C++ answers "Unknown").
    char buf[32];
    std::int32_t length = -1;
    CHECK(xiapl_hand_category_name(0, buf, sizeof buf, &length) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_hand_category_name(10, buf, sizeof buf, &length) == XIAPL_ERR_INVALID_ARGUMENT);
}

TEST_CASE("waist string outputs follow the buffer protocol") {
    // "<Card As>" is 9 bytes.
    const std::string full = "<Card As>";
    std::int32_t length = -1;

    // Query pass: no buffer, full length reported.
    CHECK(xiapl_card_repr(kAs, nullptr, 0, &length) == XIAPL_OK);
    CHECK(length == static_cast<std::int32_t>(full.size()));
    // A non-positive capacity is also a query, not an error.
    char scratch[16] = {'\xEE'};
    CHECK(xiapl_card_repr(kAs, scratch, 0, &length) == XIAPL_OK);
    CHECK(length == static_cast<std::int32_t>(full.size()));
    CHECK(scratch[0] == '\xEE');

    // Truncation: at most `buf_capacity` bytes, always NUL-terminated, and
    // *out_length still reports the untruncated length.
    for (std::int32_t capacity = 1; capacity <= 12; ++capacity) {
        std::vector<char> buf(16, '\xEE');
        CHECK(xiapl_card_repr(kAs, buf.data(), capacity, &length) == XIAPL_OK);
        CHECK(length == static_cast<std::int32_t>(full.size()));
        const std::size_t written = static_cast<std::size_t>(
            std::min<std::int32_t>(capacity - 1, static_cast<std::int32_t>(full.size())));
        CHECK(std::string(buf.data()) == full.substr(0, written));
        CHECK(buf[written] == '\0');
        CHECK(buf[static_cast<std::size_t>(capacity)] == '\xEE');  // no overrun
    }

    // out_length is required.
    CHECK(xiapl_card_repr(kAs, scratch, 16, nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
}

TEST_CASE("waist simulation options mirror the C++ factories") {
    const xiapl::SimulationOptions cpp_exact = xiapl::SimulationOptions::exact();
    xiapl_sim_options_t exact{};
    std::memset(&exact, 0xAB, sizeof exact);
    CHECK(xiapl_sim_options_exact(&exact) == XIAPL_OK);
    CHECK(exact.iterations == cpp_exact.iterations);
    CHECK(exact.seed == cpp_exact.seed);
    CHECK(exact.deterministic == (cpp_exact.deterministic ? 1 : 0));
    CHECK(exact.threads == cpp_exact.threads);
    CHECK(exact.reserved == 0);

    // Zero-initialized IS the library default (permanent invariant).
    const xiapl_sim_options_t zero{};
    CHECK(std::memcmp(&exact, &zero, sizeof zero) == 0);

    const xiapl::SimulationOptions cpp_random = xiapl::SimulationOptions::mc_random(5000);
    xiapl_sim_options_t random_options{};
    CHECK(xiapl_sim_options_mc_random(5000, &random_options) == XIAPL_OK);
    CHECK(random_options.iterations == cpp_random.iterations);
    CHECK(random_options.deterministic == (cpp_random.deterministic ? 1 : 0));
    CHECK(random_options.seed == cpp_random.seed);

    const xiapl::SimulationOptions cpp_seeded = xiapl::SimulationOptions::mc_seeded(5000, 42);
    xiapl_sim_options_t seeded{};
    CHECK(xiapl_sim_options_mc_seeded(5000, 42, &seeded) == XIAPL_OK);
    CHECK(seeded.iterations == cpp_seeded.iterations);
    CHECK(seeded.deterministic == (cpp_seeded.deterministic ? 1 : 0));
    CHECK(seeded.seed == cpp_seeded.seed);

    // effective_mode is derived, never stored: check the whole matrix against
    // the C++ derivation, seed 0 included (it is an ordinary seed).
    for (std::int32_t iterations : {0, 1, 5000}) {
        for (std::int32_t deterministic : {0, 1}) {
            for (std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{42}}) {
                xiapl_sim_options_t options{};
                options.iterations = iterations;
                options.deterministic = deterministic;
                options.seed = seed;
                std::int32_t mode = -1;
                CHECK(xiapl_sim_options_effective_mode(&options, &mode) == XIAPL_OK);

                xiapl::SimulationOptions cpp;
                cpp.iterations = iterations;
                cpp.deterministic = deterministic != 0;
                cpp.seed = seed;
                CHECK(mode == static_cast<std::int32_t>(cpp.effective_mode()));
            }
        }
    }
}

TEST_CASE("waist maps every C++ exception class onto its status code") {
    // wrap() is the single classifier for the whole ABI; std::out_of_range and
    // std::bad_alloc are unreachable from the T1 entry points, so they are
    // pinned here directly.
    xiapl_clear_last_error();

    CHECK(xiapl::waist::wrap([]() -> std::int32_t {
              throw std::invalid_argument("bad argument");
          }) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(std::strcmp(xiapl_last_error_message(), "bad argument") == 0);

    CHECK(xiapl::waist::wrap([]() -> std::int32_t {
              throw std::out_of_range("past the end");
          }) == XIAPL_ERR_OUT_OF_RANGE);
    CHECK(std::strcmp(xiapl_last_error_message(), "past the end") == 0);

    CHECK(xiapl::waist::wrap([]() -> std::int32_t {
              throw std::bad_alloc();
          }) == XIAPL_ERR_BAD_ALLOC);
    CHECK(std::strcmp(xiapl_last_error_message(), std::bad_alloc().what()) == 0);

    CHECK(xiapl::waist::wrap([]() -> std::int32_t {
              throw std::runtime_error("something else");
          }) == XIAPL_ERR_RUNTIME);
    CHECK(std::strcmp(xiapl_last_error_message(), "something else") == 0);

    // A non-std throw has no what() to carry, but it is still "anything else".
    CHECK(xiapl::waist::wrap([]() -> std::int32_t { throw 42; }) == XIAPL_ERR_RUNTIME);
    CHECK(std::strlen(xiapl_last_error_message()) > 0);

    // A successful call records nothing.
    xiapl_clear_last_error();
    CHECK(xiapl::waist::wrap([]() -> std::int32_t { return XIAPL_OK; }) == XIAPL_OK);
    CHECK(std::strcmp(xiapl_last_error_message(), "") == 0);
}

TEST_CASE("waist carries the C++ exception message through verbatim") {
    // This is what the Python conformance gate pins byte for byte, so every
    // expectation below is captured from the C++ call itself, never spelled
    // out as a literal.
    std::uint8_t id = 0;
    xiapl_clear_last_error();
    CHECK(xiapl_card_from_string("Zz", &id) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_last_error_message() ==
          cpp_message([] { xiapl::Card::from_string("Zz"); }));

    CHECK(xiapl_card_from_string("A", &id) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_last_error_message() ==
          cpp_message([] { xiapl::Card::from_string("A"); }));

    CHECK(xiapl_card_from_rank_suit(1, 0, &id) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_last_error_message() == cpp_message([] { xiapl::Card(1, 0); }));

    const std::vector<std::uint8_t> dupes = {kAs, kAs, kQs, kJs, kTs};
    const std::vector<xiapl::Card> cpp_dupes = {
        xiapl::Card::from_id(kAs), xiapl::Card::from_id(kAs),
        xiapl::Card::from_id(kQs), xiapl::Card::from_id(kJs),
        xiapl::Card::from_id(kTs)};
    xiapl_hand_value_t value{};
    CHECK(xiapl_evaluate_cards(dupes.data(), 5, &value) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_last_error_message() ==
          cpp_message([&] { xiapl::evaluate_cards(cpp_dupes); }));

    const std::vector<std::uint8_t> three = {kAs, kQs, kJs};
    const std::vector<xiapl::Card> cpp_three = {xiapl::Card::from_id(kAs),
                                                xiapl::Card::from_id(kQs),
                                                xiapl::Card::from_id(kJs)};
    CHECK(xiapl_evaluate_cards(three.data(), 3, &value) == XIAPL_ERR_RUNTIME);
    CHECK(xiapl_last_error_message() ==
          cpp_message([&] { xiapl::evaluate_cards(cpp_three); }));

    // An invalid card id inside an otherwise fine list: the message comes from
    // cards_to_mask, deep inside the C++ call, and still arrives verbatim.
    const std::vector<std::uint8_t> invalid = {XIAPL_CARD_INVALID_ID, kQs, kJs, kTs, k2c};
    const std::vector<xiapl::Card> cpp_invalid = {
        xiapl::Card(), xiapl::Card::from_id(kQs), xiapl::Card::from_id(kJs),
        xiapl::Card::from_id(kTs), xiapl::Card::from_id(k2c)};
    CHECK(xiapl_evaluate_cards(invalid.data(), 5, &value) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_last_error_message() ==
          cpp_message([&] { xiapl::evaluate_cards(cpp_invalid); }));

    // Showdown without players: runtime_error, message verbatim.
    const std::uint64_t board = mask_of({kQs, kJs, kTs, k2c, k3d});
    std::int32_t count = -1;
    CHECK(xiapl_judge(nullptr, 0, board, XIAPL_GAME_HOLDEM, nullptr, 0, &count)
          == XIAPL_ERR_RUNTIME);
    CHECK(xiapl_last_error_message() ==
          cpp_message([&] { xiapl::judge({}, board, xiapl::GameType::Holdem); }));
}

TEST_CASE("waist rejects NULL out pointers and negative counts") {
    std::uint8_t id = 0;
    std::uint64_t mask = 0;
    xiapl_hand_value_t value{};

    CHECK(xiapl_card_from_string(nullptr, &id) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_card_from_string("As", nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_card_from_id(0, nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_card_rank(0, nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_cards_to_mask(nullptr, 3, &mask) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_cards_to_mask(nullptr, -1, &mask) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_mask_to_ids(0, nullptr, 0, nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_evaluate_cards(nullptr, 5, &value) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_evaluate_mask(0, nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_describe_hand(nullptr, nullptr, 0, nullptr) == XIAPL_ERR_INVALID_ARGUMENT);

    // An empty list is legal input; only NULL-with-count is not.
    CHECK(xiapl_cards_to_mask(nullptr, 0, &mask) == XIAPL_OK);
    CHECK(mask == 0);

    // Bounded-output rule for judge: the capacity is checked before any work.
    const std::uint64_t board = mask_of({kQs, kJs, kTs, k2c, k3d});
    const std::vector<std::uint64_t> holes = {mask_of({kAs, kAh}), mask_of({kKd, kKc})};
    std::int32_t winners[2] = {-1, -1};
    std::int32_t count = -1;
    CHECK(xiapl_judge(holes.data(), 2, board, XIAPL_GAME_HOLDEM, winners, 1, &count)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(count == -1);  // untouched
    CHECK(xiapl_judge(holes.data(), -1, board, XIAPL_GAME_HOLDEM, winners, 2, &count)
          == XIAPL_ERR_INVALID_ARGUMENT);

    // describe_hand indexes a five-slot array on both sides of the boundary.
    xiapl_hand_value_t bogus{};
    bogus.category = XIAPL_HAND_ONE_PAIR;
    bogus.kicker_count = 6;
    char buf[64];
    std::int32_t length = -1;
    CHECK(xiapl_describe_hand(&bogus, buf, sizeof buf, &length) == XIAPL_ERR_INVALID_ARGUMENT);
}

TEST_CASE("waist error slot is thread local") {
    xiapl_clear_last_error();

    std::atomic<int> arrived{0};
    std::string first_message, second_message;
    std::int32_t first_status = XIAPL_OK, second_status = XIAPL_OK;

    // Both threads fail BEFORE either reads, so a shared slot would show up as
    // one message overwriting the other. Assertions run after the join:
    // doctest's macros are not for concurrent use.
    auto worker = [&arrived](bool parse_failure, std::int32_t* status,
                             std::string* message) {
        xiapl_clear_last_error();
        if (parse_failure) {
            std::uint8_t id = 0;
            *status = xiapl_card_from_string("Zz", &id);
        } else {
            const std::uint8_t ids[3] = {kAs, kQs, kJs};
            xiapl_hand_value_t value{};
            *status = xiapl_evaluate_cards(ids, 3, &value);
        }
        arrived.fetch_add(1, std::memory_order_acq_rel);
        while (arrived.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
        *message = xiapl_last_error_message();
    };

    std::thread a(worker, true, &first_status, &first_message);
    std::thread b(worker, false, &second_status, &second_message);
    a.join();
    b.join();

    CHECK(first_status == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(second_status == XIAPL_ERR_RUNTIME);
    CHECK(first_message == cpp_message([] { xiapl::Card::from_string("Zz"); }));
    CHECK(second_message == cpp_message([] {
              xiapl::evaluate_cards({xiapl::Card::from_id(kAs),
                                     xiapl::Card::from_id(kQs),
                                     xiapl::Card::from_id(kJs)});
          }));
    CHECK(first_message != second_message);
    // The calling thread's own slot was never touched by either worker.
    CHECK(std::strcmp(xiapl_last_error_message(), "") == 0);
}

// ===========================================================================
// Handle half: range, deck, equity, result handles, canonicalization.
//
// Same rule as above -- the C entry point must answer what the C++ entry point
// answers, bit for bit. Nothing below spells out a numeric expectation that
// C++ can produce itself; the only hard-coded numbers are the two frozen
// canonical-situation populations, which are the point of that test.
// ===========================================================================

namespace {

// Owning wrapper so a failing CHECK cannot leak a handle. The destroy function
// is a runtime member rather than a template parameter: the xiapl_*_destroy
// symbols have C language linkage, and language linkage is part of a function
// type.
template <class T>
struct Owned {
    T* handle = nullptr;
    void (*destroy)(T*) = nullptr;

    explicit Owned(void (*destroy_fn)(T*)) : destroy(destroy_fn) {}
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    ~Owned() { destroy(handle); }

    T** out() { return &handle; }
    T* get() const { return handle; }
};

using Combos = std::vector<std::pair<std::uint64_t, double>>;

Combos read_combos(const xiapl_range_t* range) {
    std::int32_t total = -1;
    REQUIRE(xiapl_range_combos(range, nullptr, nullptr, 0, &total) == XIAPL_OK);
    REQUIRE(total >= 0);
    std::vector<std::uint64_t> masks(static_cast<std::size_t>(total));
    std::vector<double> weights(static_cast<std::size_t>(total));
    std::int32_t again = -1;
    REQUIRE(xiapl_range_combos(range, masks.data(), weights.data(), total, &again) == XIAPL_OK);
    REQUIRE(again == total);

    Combos out;
    out.reserve(masks.size());
    for (std::size_t i = 0; i < masks.size(); ++i) {
        out.emplace_back(masks[i], weights[i]);
    }
    return out;
}

Combos cpp_combos(const std::vector<xiapl::Combo>& combos) {
    Combos out;
    out.reserve(combos.size());
    for (const xiapl::Combo& combo : combos) {
        out.emplace_back(combo.mask, combo.weight);
    }
    return out;
}

// Full comparison of a handle against the C++ Range it should be projecting:
// size, emptiness, game tag, and the combo columns bit for bit.
void check_same_range(const xiapl_range_t* handle, const xiapl::Range& expected) {
    std::int32_t size = -1, is_empty = -1, game = -1;
    REQUIRE(xiapl_range_size(handle, &size) == XIAPL_OK);
    CHECK(size == static_cast<std::int32_t>(expected.size()));
    REQUIRE(xiapl_range_empty(handle, &is_empty) == XIAPL_OK);
    CHECK(is_empty == (expected.empty() ? 1 : 0));
    REQUIRE(xiapl_range_game(handle, &game) == XIAPL_OK);
    CHECK(game == static_cast<std::int32_t>(expected.game()));
    CHECK(read_combos(handle) == cpp_combos(expected.combos()));
}

std::vector<std::uint8_t> read_deck_ids(const xiapl_deck_t* deck) {
    std::int32_t total = -1;
    REQUIRE(xiapl_deck_get_cards(deck, nullptr, 0, &total) == XIAPL_OK);
    REQUIRE(total >= 0);
    std::vector<std::uint8_t> ids(static_cast<std::size_t>(total));
    std::int32_t again = -1;
    REQUIRE(xiapl_deck_get_cards(deck, ids.data(), total, &again) == XIAPL_OK);
    REQUIRE(again == total);
    return ids;
}

std::vector<std::uint8_t> cpp_deck_ids(const xiapl::Deck& deck) {
    const std::vector<int> ids = deck.get_card_ids();
    std::vector<std::uint8_t> out;
    out.reserve(ids.size());
    for (int id : ids) out.push_back(static_cast<std::uint8_t>(id));
    return out;
}

// Everything xiapl_calculate_equity can write, in one comparable value.
struct EquitySoA {
    std::vector<double> winrate, equity, std_error;
    double chop_rate = 0.0;
    std::uint64_t trials = 0;
    std::int32_t exact = -1;

    bool operator==(const EquitySoA& other) const {
        return winrate == other.winrate && equity == other.equity &&
               std_error == other.std_error && chop_rate == other.chop_rate &&
               trials == other.trials && exact == other.exact;
    }
};

EquitySoA waist_equity(const std::vector<std::uint64_t>& holes,
                       std::uint64_t board, const xiapl_sim_options_t& options,
                       std::int32_t game) {
    const std::size_t n = holes.size();
    EquitySoA soa;
    soa.winrate.assign(n, -1.0);
    soa.equity.assign(n, -1.0);
    soa.std_error.assign(n, -1.0);
    xiapl_equity_summary_t summary{};
    REQUIRE(xiapl_calculate_equity(holes.data(), static_cast<std::int32_t>(n), board,
                                   &options, game, soa.winrate.data(),
                                   soa.equity.data(), soa.std_error.data(),
                                   static_cast<std::int32_t>(n), &summary) == XIAPL_OK);
    soa.chop_rate = summary.chop_rate;
    soa.trials = summary.trials;
    soa.exact = summary.exact;
    return soa;
}

EquitySoA cpp_equity(const std::vector<std::uint64_t>& holes, std::uint64_t board,
                     const xiapl::SimulationOptions& options, xiapl::GameType game) {
    const xiapl::EquityResult result =
        xiapl::calculate_equity(holes, board, options, game);
    EquitySoA soa;
    for (const xiapl::PlayerEquity& player : result.players) {
        soa.winrate.push_back(player.winrate);
        soa.equity.push_back(player.equity);
        soa.std_error.push_back(player.std_error);
    }
    soa.chop_rate = result.chop_rate;
    soa.trials = result.trials;
    soa.exact = result.exact ? 1 : 0;
    return soa;
}

using Entries = std::vector<std::pair<std::uint64_t, std::pair<double, double>>>;

Entries read_entries(const xiapl_range_equity_t* result, bool hero) {
    auto fill = [&](std::uint64_t* m, double* e, double* w, std::int32_t cap,
                    std::int32_t* total) {
        return hero ? xiapl_range_equity_hero(result, m, e, w, cap, total)
                    : xiapl_range_equity_villain(result, m, e, w, cap, total);
    };
    std::int32_t total = -1;
    REQUIRE(fill(nullptr, nullptr, nullptr, 0, &total) == XIAPL_OK);
    REQUIRE(total >= 0);
    std::vector<std::uint64_t> masks(static_cast<std::size_t>(total));
    std::vector<double> equities(static_cast<std::size_t>(total));
    std::vector<double> weights(static_cast<std::size_t>(total));
    std::int32_t again = -1;
    REQUIRE(fill(masks.data(), equities.data(), weights.data(), total, &again) == XIAPL_OK);
    REQUIRE(again == total);

    Entries out;
    out.reserve(masks.size());
    for (std::size_t i = 0; i < masks.size(); ++i) {
        out.emplace_back(masks[i], std::make_pair(equities[i], weights[i]));
    }
    return out;
}

Entries cpp_entries(const std::vector<xiapl::RangeEquityEntry>& entries) {
    Entries out;
    out.reserve(entries.size());
    for (const xiapl::RangeEquityEntry& entry : entries) {
        out.emplace_back(entry.combo_mask,
                         std::make_pair(entry.equity, entry.weight));
    }
    return out;
}

void check_same_range_equity(const xiapl_range_equity_t* handle,
                             const xiapl::RangeEquityResult& expected) {
    xiapl_range_equity_summary_t summary{};
    REQUIRE(xiapl_range_equity_summary(handle, &summary) == XIAPL_OK);
    CHECK(summary.hero_aggregate_equity == expected.hero_aggregate_equity);
    CHECK(summary.villain_aggregate_equity == expected.villain_aggregate_equity);
    CHECK(summary.aggregate_std_error == expected.aggregate_std_error);
    CHECK(summary.trials == expected.trials);
    CHECK(summary.exact == (expected.exact ? 1 : 0));
    CHECK(summary.reserved == 0);
    CHECK(read_entries(handle, true) == cpp_entries(expected.hero));
    CHECK(read_entries(handle, false) == cpp_entries(expected.villain));
}

// A range handle built from notation, for tests that need one in one line.
void make_range(Owned<xiapl_range_t>& owner, const char* text, std::int32_t game) {
    REQUIRE(xiapl_range_create_from_string(text, game, owner.out()) == XIAPL_OK);
    REQUIRE(owner.get() != nullptr);
}

}  // namespace

TEST_CASE("waist range constructors agree with xiapl::Range") {
    Owned<xiapl_range_t> empty(xiapl_range_destroy);
    REQUIRE(xiapl_range_create(empty.out()) == XIAPL_OK);
    REQUIRE(empty.get() != nullptr);
    check_same_range(empty.get(), xiapl::Range());

    Owned<xiapl_range_t> holdem(xiapl_range_destroy);
    make_range(holdem, "AA,KK,AKs", XIAPL_GAME_HOLDEM);
    check_same_range(holdem.get(),
                     xiapl::Range::from_string("AA,KK,AKs", xiapl::GameType::Holdem));

    Owned<xiapl_range_t> plo(xiapl_range_destroy);
    make_range(plo, "AAKK", XIAPL_GAME_PLO);
    check_same_range(plo.get(), xiapl::Range::from_string("AAKK", xiapl::GameType::Plo));

    // A well-formed but unsatisfiable pattern is an EMPTY range, not an error.
    Owned<xiapl_range_t> unsatisfiable(xiapl_range_destroy);
    make_range(unsatisfiable, "AAAKds", XIAPL_GAME_PLO);
    check_same_range(unsatisfiable.get(),
                     xiapl::Range::from_string("AAAKds", xiapl::GameType::Plo));
    std::int32_t is_empty = -1;
    CHECK(xiapl_range_empty(unsatisfiable.get(), &is_empty) == XIAPL_OK);
    CHECK(is_empty == 1);

    Owned<xiapl_range_t> all_holdem(xiapl_range_destroy);
    REQUIRE(xiapl_range_create_all(XIAPL_GAME_HOLDEM, all_holdem.out()) == XIAPL_OK);
    check_same_range(all_holdem.get(), xiapl::Range::all(xiapl::GameType::Holdem));

    Owned<xiapl_range_t> all_plo(xiapl_range_destroy);
    REQUIRE(xiapl_range_create_all(XIAPL_GAME_PLO, all_plo.out()) == XIAPL_OK);
    check_same_range(all_plo.get(), xiapl::Range::all(xiapl::GameType::Plo));

    // Raw combos, with and without a weight column.
    const std::vector<std::uint64_t> masks = {mask_of({kAs, kAh}), mask_of({kKd, kKc})};
    const std::vector<double> weights = {0.25, 0.75};
    Owned<xiapl_range_t> weighted(xiapl_range_destroy);
    REQUIRE(xiapl_range_create_from_combos(masks.data(), weights.data(), 2,
                                           XIAPL_GAME_HOLDEM, weighted.out()) == XIAPL_OK);
    check_same_range(weighted.get(),
                     xiapl::Range({{masks[0], weights[0]}, {masks[1], weights[1]}},
                                  xiapl::GameType::Holdem));

    Owned<xiapl_range_t> unweighted(xiapl_range_destroy);
    REQUIRE(xiapl_range_create_from_combos(masks.data(), nullptr, 2,
                                           XIAPL_GAME_HOLDEM, unweighted.out()) == XIAPL_OK);
    check_same_range(unweighted.get(),
                     xiapl::Range({{masks[0], 1.0}, {masks[1], 1.0}},
                                  xiapl::GameType::Holdem));

    // An empty combo list is legal; a NULL list with a count is not.
    Owned<xiapl_range_t> no_combos(xiapl_range_destroy);
    REQUIRE(xiapl_range_create_from_combos(nullptr, nullptr, 0, XIAPL_GAME_HOLDEM,
                                           no_combos.out()) == XIAPL_OK);
    check_same_range(no_combos.get(), xiapl::Range({}, xiapl::GameType::Holdem));
}

TEST_CASE("waist range constructors reject what xiapl::Range rejects") {
    xiapl_range_t* range = reinterpret_cast<xiapl_range_t*>(1);

    // A combo whose popcount is wrong for the game: the C++ message names the
    // offending mask and arrives verbatim.
    const std::uint64_t two_card = mask_of({kAs, kAh});
    xiapl_clear_last_error();
    CHECK(xiapl_range_create_from_combos(&two_card, nullptr, 1, XIAPL_GAME_PLO, &range)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(range == nullptr);  // NULL on every non-OK status
    CHECK(xiapl_last_error_message() == cpp_message([&] {
              xiapl::Range({{two_card, 1.0}}, xiapl::GameType::Plo);
          }));

    range = reinterpret_cast<xiapl_range_t*>(1);
    CHECK(xiapl_range_create_from_string("not a range", XIAPL_GAME_HOLDEM, &range)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(range == nullptr);
    CHECK(xiapl_last_error_message() == cpp_message([] {
              xiapl::Range::from_string("not a range", xiapl::GameType::Holdem);
          }));

    CHECK(xiapl_range_create_all(7, &range) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_range_create_from_string(nullptr, XIAPL_GAME_HOLDEM, &range)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_range_create_from_combos(nullptr, nullptr, 3, XIAPL_GAME_HOLDEM, &range)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_range_create_from_combos(&two_card, nullptr, -1, XIAPL_GAME_HOLDEM, &range)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_range_create(nullptr) == XIAPL_ERR_INVALID_ARGUMENT);

    // Destroying NULL is a no-op.
    xiapl_range_destroy(nullptr);
}

TEST_CASE("waist try_parse_range answers instead of failing") {
    Owned<xiapl_range_t> parsed(xiapl_range_destroy);
    REQUIRE(xiapl_try_parse_range("AA,KK", parsed.out()) == XIAPL_OK);
    REQUIRE(parsed.get() != nullptr);
    check_same_range(parsed.get(), *xiapl::try_parse_range("AA,KK"));

    // A non-parse is XIAPL_OK + a NULL handle, and records NO message: nothing
    // failed, so the caller's error slot must be left alone.
    xiapl_clear_last_error();
    xiapl_range_t* not_a_range = reinterpret_cast<xiapl_range_t*>(1);
    CHECK(xiapl_try_parse_range("not a range", &not_a_range) == XIAPL_OK);
    CHECK(not_a_range == nullptr);
    CHECK(std::strcmp(xiapl_last_error_message(), "") == 0);
    CHECK(!xiapl::try_parse_range("not a range").has_value());

    // A NULL text pointer is still a NULL pointer where one is required.
    CHECK(xiapl_try_parse_range(nullptr, &not_a_range) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(not_a_range == nullptr);
    CHECK(xiapl_try_parse_range("AA", nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
}

TEST_CASE("waist range accessors agree with xiapl::Range") {
    Owned<xiapl_range_t> range(xiapl_range_destroy);
    make_range(range, "AA,KK,AKs", XIAPL_GAME_HOLDEM);
    const xiapl::Range expected =
        xiapl::Range::from_string("AA,KK,AKs", xiapl::GameType::Holdem);

    const std::uint64_t dead = mask_of({kAs, kKd});
    for (std::uint64_t probe : {std::uint64_t{0}, dead, XIAPL_FULL_DECK_MASK}) {
        const std::vector<xiapl::Combo> valid = expected.valid_combos(probe);
        std::int32_t total = -1;
        REQUIRE(xiapl_range_valid_combos(range.get(), probe, nullptr, nullptr, 0, &total)
                == XIAPL_OK);
        CHECK(total == static_cast<std::int32_t>(valid.size()));

        std::vector<std::uint64_t> masks(static_cast<std::size_t>(total) + 1, 0xEE);
        std::vector<double> weights(static_cast<std::size_t>(total) + 1, -1.0);
        std::int32_t again = -1;
        REQUIRE(xiapl_range_valid_combos(range.get(), probe, masks.data(), weights.data(),
                                         total, &again) == XIAPL_OK);
        CHECK(again == total);
        Combos got;
        for (std::size_t i = 0; i < valid.size(); ++i) got.emplace_back(masks[i], weights[i]);
        CHECK(got == cpp_combos(valid));
        CHECK(masks[static_cast<std::size_t>(total)] == 0xEE);  // nothing past capacity

        double weight = -1.0;
        REQUIRE(xiapl_range_total_weight(range.get(), probe, &weight) == XIAPL_OK);
        CHECK(weight == expected.total_weight(probe));
    }

    // Either column may be NULL independently, and a short buffer is not an
    // error (convention 4).
    std::vector<std::uint64_t> masks_only(4, 0);
    std::int32_t total = -1;
    CHECK(xiapl_range_combos(range.get(), masks_only.data(), nullptr, 2, &total) == XIAPL_OK);
    CHECK(total == static_cast<std::int32_t>(expected.size()));
    CHECK(masks_only[0] == expected.combos()[0].mask);
    CHECK(masks_only[2] == 0);

    // Required pointers.
    CHECK(xiapl_range_size(nullptr, &total) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_range_size(range.get(), nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_range_empty(nullptr, &total) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_range_game(range.get(), nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_range_combos(range.get(), nullptr, nullptr, 0, nullptr)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_range_valid_combos(range.get(), 0, nullptr, nullptr, 0, nullptr)
          == XIAPL_ERR_INVALID_ARGUMENT);
    double weight = -1.0;
    CHECK(xiapl_range_total_weight(nullptr, 0, &weight) == XIAPL_ERR_INVALID_ARGUMENT);
}

TEST_CASE("waist range set operations are pure and mirror the C++ lattice") {
    // Weighted operands, so the max/min lattice is actually exercised.
    const std::vector<std::uint64_t> a_masks = {mask_of({kAs, kAh}), mask_of({kKd, kKc}),
                                                mask_of({kQs, kQh})};
    const std::vector<double> a_weights = {1.0, 0.5, 0.25};
    const std::vector<std::uint64_t> b_masks = {mask_of({kKd, kKc}), mask_of({kQs, kQh}),
                                                mask_of({k9h, k9s})};
    const std::vector<double> b_weights = {0.75, 0.25, 1.0};

    Owned<xiapl_range_t> a(xiapl_range_destroy), b(xiapl_range_destroy);
    REQUIRE(xiapl_range_create_from_combos(a_masks.data(), a_weights.data(), 3,
                                           XIAPL_GAME_HOLDEM, a.out()) == XIAPL_OK);
    REQUIRE(xiapl_range_create_from_combos(b_masks.data(), b_weights.data(), 3,
                                           XIAPL_GAME_HOLDEM, b.out()) == XIAPL_OK);

    const xiapl::Range cpp_a({{a_masks[0], a_weights[0]}, {a_masks[1], a_weights[1]},
                              {a_masks[2], a_weights[2]}}, xiapl::GameType::Holdem);
    const xiapl::Range cpp_b({{b_masks[0], b_weights[0]}, {b_masks[1], b_weights[1]},
                              {b_masks[2], b_weights[2]}}, xiapl::GameType::Holdem);

    const Combos before_a = read_combos(a.get());
    const Combos before_b = read_combos(b.get());

    {
        Owned<xiapl_range_t> result(xiapl_range_destroy);
        REQUIRE(xiapl_range_union(a.get(), b.get(), result.out()) == XIAPL_OK);
        check_same_range(result.get(), xiapl::range_union(cpp_a, cpp_b));
    }
    {
        Owned<xiapl_range_t> result(xiapl_range_destroy);
        REQUIRE(xiapl_range_intersection(a.get(), b.get(), result.out()) == XIAPL_OK);
        check_same_range(result.get(), xiapl::range_intersection(cpp_a, cpp_b));
    }
    {
        Owned<xiapl_range_t> result(xiapl_range_destroy);
        REQUIRE(xiapl_range_difference(a.get(), b.get(), result.out()) == XIAPL_OK);
        check_same_range(result.get(), xiapl::range_difference(cpp_a, cpp_b));
    }

    // PURITY: neither operand may have been touched by any of the three.
    CHECK(read_combos(a.get()) == before_a);
    CHECK(read_combos(b.get()) == before_b);

    // Game-tag mismatch: the C++ message names both games.
    Owned<xiapl_range_t> plo(xiapl_range_destroy);
    make_range(plo, "AAKK", XIAPL_GAME_PLO);
    xiapl_range_t* result = reinterpret_cast<xiapl_range_t*>(1);
    xiapl_clear_last_error();
    CHECK(xiapl_range_union(a.get(), plo.get(), &result) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(result == nullptr);
    CHECK(xiapl_last_error_message() == cpp_message([&] {
              xiapl::range_union(cpp_a, xiapl::Range::from_string("AAKK", xiapl::GameType::Plo));
          }));

    // A duplicated mask -- only the raw-combo constructor can build one.
    const std::vector<std::uint64_t> duped = {a_masks[0], a_masks[0]};
    Owned<xiapl_range_t> dup(xiapl_range_destroy);
    REQUIRE(xiapl_range_create_from_combos(duped.data(), nullptr, 2, XIAPL_GAME_HOLDEM,
                                           dup.out()) == XIAPL_OK);
    CHECK(xiapl_range_intersection(dup.get(), b.get(), &result) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(result == nullptr);

    CHECK(xiapl_range_union(nullptr, b.get(), &result) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_range_difference(a.get(), nullptr, &result) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_range_intersection(a.get(), b.get(), nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
}

TEST_CASE("waist top-percent selection agrees with xiapl::rank_starting_hands") {
    for (double percent : {1.0, 0.2, 0.05, 1.0 / 200.0}) {
        const std::vector<std::string> expected =
            xiapl::rank_starting_hands(percent, xiapl::GameType::Holdem);

        std::int32_t total = -1;
        REQUIRE(xiapl_rank_starting_hands(percent, XIAPL_GAME_HOLDEM, nullptr, 0, &total)
                == XIAPL_OK);
        CHECK(total == static_cast<std::int32_t>(expected.size()));

        std::vector<char> buf(
            static_cast<std::size_t>(total + 1) * XIAPL_STARTING_HAND_LABEL_SIZE, '\xEE');
        std::int32_t again = -1;
        REQUIRE(xiapl_rank_starting_hands(percent, XIAPL_GAME_HOLDEM, buf.data(), total,
                                          &again) == XIAPL_OK);
        CHECK(again == total);
        std::vector<std::string> got;
        for (std::int32_t i = 0; i < total; ++i) {
            got.emplace_back(&buf[static_cast<std::size_t>(i) * XIAPL_STARTING_HAND_LABEL_SIZE]);
        }
        CHECK(got == expected);
        // Nothing written into the slot past the capacity.
        CHECK(buf[static_cast<std::size_t>(total) * XIAPL_STARTING_HAND_LABEL_SIZE] == '\xEE');

        Owned<xiapl_range_t> range(xiapl_range_destroy);
        REQUIRE(xiapl_generate_top_percent_range(percent, XIAPL_GAME_HOLDEM, range.out())
                == XIAPL_OK);
        check_same_range(range.get(),
                         xiapl::generate_top_percent_range(percent, xiapl::GameType::Holdem));
    }

    // A short buffer truncates without failing.
    std::vector<char> small(2 * XIAPL_STARTING_HAND_LABEL_SIZE, '\xEE');
    std::int32_t total = -1;
    CHECK(xiapl_rank_starting_hands(1.0, XIAPL_GAME_HOLDEM, small.data(), 2, &total)
          == XIAPL_OK);
    CHECK(total == 169);
    CHECK(std::string(&small[0]) == xiapl::rank_starting_hands(1.0)[0]);

    // top_percent out of range, and PLO, are the C++ routine's verdicts.
    xiapl_clear_last_error();
    CHECK(xiapl_rank_starting_hands(0.0, XIAPL_GAME_HOLDEM, nullptr, 0, &total)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_last_error_message() ==
          cpp_message([] { xiapl::rank_starting_hands(0.0, xiapl::GameType::Holdem); }));
    CHECK(xiapl_rank_starting_hands(1.5, XIAPL_GAME_HOLDEM, nullptr, 0, &total)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_rank_starting_hands(0.5, XIAPL_GAME_PLO, nullptr, 0, &total)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_last_error_message() ==
          cpp_message([] { xiapl::rank_starting_hands(0.5, xiapl::GameType::Plo); }));

    xiapl_range_t* range = reinterpret_cast<xiapl_range_t*>(1);
    CHECK(xiapl_generate_top_percent_range(0.5, XIAPL_GAME_PLO, &range)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(range == nullptr);
    CHECK(xiapl_rank_starting_hands(0.5, XIAPL_GAME_HOLDEM, nullptr, 0, nullptr)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_rank_starting_hands(0.5, 9, nullptr, 0, &total) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_generate_top_percent_range(0.5, XIAPL_GAME_HOLDEM, nullptr)
          == XIAPL_ERR_INVALID_ARGUMENT);
}

TEST_CASE("waist deck mirrors xiapl::Deck") {
    Owned<xiapl_deck_t> deck(xiapl_deck_destroy);
    REQUIRE(xiapl_deck_create(deck.out()) == XIAPL_OK);
    REQUIRE(deck.get() != nullptr);

    xiapl::Deck cpp;  // sorted 0..51, NOT shuffled
    CHECK(read_deck_ids(deck.get()) == cpp_deck_ids(cpp));
    CHECK(fetch_string([&](char* b, std::int32_t c, std::int32_t* l) {
              return xiapl_deck_repr(deck.get(), b, c, l);
          }) == cpp.repr());

    std::int32_t size = -1, is_empty = -1, has = -1;
    CHECK(xiapl_deck_size(deck.get(), &size) == XIAPL_OK);
    CHECK(size == cpp.size());
    CHECK(xiapl_deck_empty(deck.get(), &is_empty) == XIAPL_OK);
    CHECK(is_empty == (cpp.empty() ? 1 : 0));
    CHECK(xiapl_deck_has_cards(deck.get(), 52, &has) == XIAPL_OK);
    CHECK(has == (cpp.has_cards(52) ? 1 : 0));
    CHECK(xiapl_deck_has_cards(deck.get(), 53, &has) == XIAPL_OK);
    CHECK(has == (cpp.has_cards(53) ? 1 : 0));

    // Dealing takes from the END, in the order dealt.
    std::uint8_t dealt[5] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
    std::int32_t count = -1;
    CHECK(xiapl_deck_deal(deck.get(), 5, dealt, 5, &count) == XIAPL_OK);
    const std::vector<xiapl::Card> cpp_dealt = cpp.deal(5);
    CHECK(count == static_cast<std::int32_t>(cpp_dealt.size()));
    for (std::size_t i = 0; i < cpp_dealt.size(); ++i) CHECK(dealt[i] == cpp_dealt[i].id);
    CHECK(read_deck_ids(deck.get()) == cpp_deck_ids(cpp));

    // deal_mask discards draw order.
    std::uint64_t mask = 0;
    CHECK(xiapl_deck_deal_mask(deck.get(), 3, &mask) == XIAPL_OK);
    CHECK(mask == cpp.deal_mask(3));
    CHECK(read_deck_ids(deck.get()) == cpp_deck_ids(cpp));

    CHECK(xiapl_deck_burn(deck.get(), 2) == XIAPL_OK);
    cpp.burn(2);
    CHECK(read_deck_ids(deck.get()) == cpp_deck_ids(cpp));

    // remove_cards skips ids that are not present -- including an id that
    // cannot be present at all.
    const std::uint8_t remove[3] = {k2c, k3d, XIAPL_CARD_INVALID_ID};
    CHECK(xiapl_deck_remove_cards(deck.get(), remove, 3) == XIAPL_OK);
    cpp.remove_cards({xiapl::Card::from_id(k2c), xiapl::Card::from_id(k3d),
                      xiapl::Card::from_id(XIAPL_CARD_INVALID_ID)});
    CHECK(read_deck_ids(deck.get()) == cpp_deck_ids(cpp));

    CHECK(xiapl_deck_reset(deck.get(), 0) == XIAPL_OK);
    cpp.reset(false);
    CHECK(read_deck_ids(deck.get()) == cpp_deck_ids(cpp));

    const std::uint8_t three[3] = {kAs, kKs, kQs};
    CHECK(xiapl_deck_set_cards(deck.get(), three, 3) == XIAPL_OK);
    cpp.set_cards({xiapl::Card::from_id(kAs), xiapl::Card::from_id(kKs),
                   xiapl::Card::from_id(kQs)});
    CHECK(read_deck_ids(deck.get()) == cpp_deck_ids(cpp));

    // Running out is not an error: fewer than n cards come back.
    CHECK(xiapl_deck_deal(deck.get(), 5, dealt, 5, &count) == XIAPL_OK);
    CHECK(count == 3);
    CHECK(xiapl_deck_deal_mask(deck.get(), 5, &mask) == XIAPL_OK);
    CHECK(mask == 0);  // empty deck
    CHECK(xiapl_deck_empty(deck.get(), &is_empty) == XIAPL_OK);
    CHECK(is_empty == 1);

    // Dealing from an already-empty deck with a valid buffer is not an error:
    // it just deals nothing.
    CHECK(xiapl_deck_deal(deck.get(), 5, dealt, 5, &count) == XIAPL_OK);
    CHECK(count == 0);

    Owned<xiapl_deck_t> from_cards(xiapl_deck_destroy);
    REQUIRE(xiapl_deck_create_from_cards(three, 3, from_cards.out()) == XIAPL_OK);
    CHECK(read_deck_ids(from_cards.get()) ==
          std::vector<std::uint8_t>({kAs, kKs, kQs}));

    Owned<xiapl_deck_t> from_none(xiapl_deck_destroy);
    REQUIRE(xiapl_deck_create_from_cards(nullptr, 0, from_none.out()) == XIAPL_OK);
    CHECK(read_deck_ids(from_none.get()).empty());

    xiapl_deck_destroy(nullptr);
}

TEST_CASE("waist deck validates before it mutates") {
    Owned<xiapl_deck_t> deck(xiapl_deck_destroy);
    REQUIRE(xiapl_deck_create(deck.out()) == XIAPL_OK);
    const std::vector<std::uint8_t> before = read_deck_ids(deck.get());

    // Dealing mutates, so both the buffer and its capacity are checked BEFORE
    // the deck is touched -- otherwise a failed deal would eat cards.
    std::uint8_t small[5] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
    std::int32_t count = -1;
    CHECK(xiapl_deck_deal(deck.get(), 5, small, 3, &count) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(count == -1);
    CHECK(read_deck_ids(deck.get()) == before);
    CHECK(xiapl_deck_deal(deck.get(), 5, nullptr, 5, &count) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(read_deck_ids(deck.get()) == before);
    CHECK(xiapl_deck_deal(deck.get(), 5, small, 5, nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(read_deck_ids(deck.get()) == before);

    // out_ids is unconditional per the header, even when n <= 0 asks for
    // nothing: the NULL check fires before n is ever examined.
    CHECK(xiapl_deck_deal(deck.get(), 0, nullptr, 0, &count) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(read_deck_ids(deck.get()) == before);

    // set_cards validates the whole list first, so a rejected list leaves the
    // deck exactly as it was.
    const std::uint8_t duplicate[3] = {kAs, kKs, kAs};
    xiapl_clear_last_error();
    CHECK(xiapl_deck_set_cards(deck.get(), duplicate, 3) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(read_deck_ids(deck.get()) == before);
    CHECK(xiapl_last_error_message() == cpp_message([&] {
              xiapl::Deck().set_cards({xiapl::Card::from_id(kAs), xiapl::Card::from_id(kKs),
                                       xiapl::Card::from_id(kAs)});
          }));

    const std::uint8_t bad_id[2] = {kAs, XIAPL_CARD_INVALID_ID};
    CHECK(xiapl_deck_set_cards(deck.get(), bad_id, 2) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(read_deck_ids(deck.get()) == before);

    xiapl_deck_t* raw = reinterpret_cast<xiapl_deck_t*>(1);
    CHECK(xiapl_deck_create_from_cards(duplicate, 3, &raw) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(raw == nullptr);
    CHECK(xiapl_deck_create_from_cards(nullptr, 2, &raw) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_create_from_cards(duplicate, -1, &raw) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_create(nullptr) == XIAPL_ERR_INVALID_ARGUMENT);

    // Every deck entry point rejects a NULL deck.
    std::int32_t scratch = -1;
    std::uint64_t mask = 0;
    CHECK(xiapl_deck_shuffle(nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_shuffle_seeded(nullptr, 1) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_deal(nullptr, 1, small, 3, &scratch) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_deal_mask(nullptr, 1, &mask) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_burn(nullptr, 1) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_reset(nullptr, 0) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_remove_cards(nullptr, small, 1) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_set_cards(nullptr, small, 1) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_get_cards(nullptr, nullptr, 0, &scratch) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_get_cards(deck.get(), nullptr, 0, nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_size(nullptr, &scratch) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_empty(nullptr, &scratch) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_has_cards(nullptr, 1, &scratch) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_repr(nullptr, nullptr, 0, &scratch) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_remove_cards(deck.get(), nullptr, 1) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_remove_cards(deck.get(), small, -1) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_set_cards(deck.get(), nullptr, 1) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_deck_set_cards(deck.get(), small, -1) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(read_deck_ids(deck.get()) == before);
}

TEST_CASE("waist seeded deck shuffle is reproducible") {
    // This is the whole reason Deck is on the waist: a binding that
    // reimplemented the shuffle would have to reproduce the RNG bit for bit.
    for (std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{1},
                               std::uint64_t{0xDEADBEEFCAFEULL}}) {
        Owned<xiapl_deck_t> first(xiapl_deck_destroy), second(xiapl_deck_destroy);
        REQUIRE(xiapl_deck_create(first.out()) == XIAPL_OK);
        REQUIRE(xiapl_deck_create(second.out()) == XIAPL_OK);
        REQUIRE(xiapl_deck_shuffle_seeded(first.get(), seed) == XIAPL_OK);
        REQUIRE(xiapl_deck_shuffle_seeded(second.get(), seed) == XIAPL_OK);

        xiapl::Deck cpp;
        cpp.shuffle(seed);
        CHECK(read_deck_ids(first.get()) == read_deck_ids(second.get()));
        CHECK(read_deck_ids(first.get()) == cpp_deck_ids(cpp));
        // Seed 0 is an ordinary seed, not a request for randomness.
        CHECK(read_deck_ids(first.get()).size() == 52);
    }

    Owned<xiapl_deck_t> a(xiapl_deck_destroy), b(xiapl_deck_destroy);
    REQUIRE(xiapl_deck_create(a.out()) == XIAPL_OK);
    REQUIRE(xiapl_deck_create(b.out()) == XIAPL_OK);
    REQUIRE(xiapl_deck_shuffle_seeded(a.get(), 1) == XIAPL_OK);
    REQUIRE(xiapl_deck_shuffle_seeded(b.get(), 2) == XIAPL_OK);
    CHECK(read_deck_ids(a.get()) != read_deck_ids(b.get()));

    // shuffle() is a permutation of the same 52 cards, just not a reproducible
    // one, so only the multiset is checkable.
    Owned<xiapl_deck_t> random(xiapl_deck_destroy);
    REQUIRE(xiapl_deck_create(random.out()) == XIAPL_OK);
    REQUIRE(xiapl_deck_shuffle(random.get()) == XIAPL_OK);
    std::vector<std::uint8_t> ids = read_deck_ids(random.get());
    std::sort(ids.begin(), ids.end());
    CHECK(ids == cpp_deck_ids(xiapl::Deck()));
}

TEST_CASE("waist calculate_equity agrees with the C++ simulator") {
    const std::uint64_t flop = mask_of({kQs, kJs, kTs});
    const std::vector<std::uint64_t> heads_up = {mask_of({kAs, kAh}), mask_of({kKd, kKc})};
    const std::vector<std::uint64_t> three_way = {mask_of({kAs, kAh}), mask_of({kKd, kKc}),
                                                  mask_of({k9h, k9s})};

    xiapl_sim_options_t exact{};
    REQUIRE(xiapl_sim_options_exact(&exact) == XIAPL_OK);
    CHECK(waist_equity(heads_up, flop, exact, XIAPL_GAME_HOLDEM) ==
          cpp_equity(heads_up, flop, xiapl::SimulationOptions::exact(), xiapl::GameType::Holdem));
    CHECK(waist_equity(three_way, flop, exact, XIAPL_GAME_HOLDEM) ==
          cpp_equity(three_way, flop, xiapl::SimulationOptions::exact(), xiapl::GameType::Holdem));

    xiapl_sim_options_t seeded{};
    REQUIRE(xiapl_sim_options_mc_seeded(20000, 4242, &seeded) == XIAPL_OK);
    CHECK(waist_equity(heads_up, 0, seeded, XIAPL_GAME_HOLDEM) ==
          cpp_equity(heads_up, 0, xiapl::SimulationOptions::mc_seeded(20000, 4242),
                     xiapl::GameType::Holdem));

    // PLO, so the game tag is doing real work on both sides.
    const std::vector<std::uint64_t> plo = {mask_of({kAs, kAh, kKd, kKc}),
                                            mask_of({kQh, k9h, k9s, k3d})};
    CHECK(waist_equity(plo, flop, exact, XIAPL_GAME_PLO) ==
          cpp_equity(plo, flop, xiapl::SimulationOptions::exact(), xiapl::GameType::Plo));
}

TEST_CASE("waist seeded equity is bit-identical at every thread count") {
    // Convention 12, verified through the C path: the golden cross-language
    // vectors are allowed to assume this.
    const std::vector<std::uint64_t> holes = {mask_of({kAs, kAh}), mask_of({kKd, kKc})};

    xiapl_sim_options_t options{};
    REQUIRE(xiapl_sim_options_mc_seeded(300000, 20260904, &options) == XIAPL_OK);

    options.threads = 1;
    const EquitySoA serial = waist_equity(holes, 0, options, XIAPL_GAME_HOLDEM);
    for (std::int32_t threads : {2, 6}) {
        options.threads = threads;
        CHECK(waist_equity(holes, 0, options, XIAPL_GAME_HOLDEM) == serial);
    }

    // ... and identical to what the C++ simulator produces with the same seed.
    CHECK(serial == cpp_equity(holes, 0, xiapl::SimulationOptions::mc_seeded(300000, 20260904),
                               xiapl::GameType::Holdem));
}

TEST_CASE("waist calculate_equity applies the bounded-output rule") {
    const std::uint64_t flop = mask_of({kQs, kJs, kTs});
    const std::vector<std::uint64_t> holes = {mask_of({kAs, kAh}), mask_of({kKd, kKc})};
    xiapl_sim_options_t options{};
    REQUIRE(xiapl_sim_options_exact(&options) == XIAPL_OK);

    // Every column is optional; the summary alone needs no capacity at all.
    xiapl_equity_summary_t summary{};
    CHECK(xiapl_calculate_equity(holes.data(), 2, flop, &options, XIAPL_GAME_HOLDEM,
                                 nullptr, nullptr, nullptr, 0, &summary) == XIAPL_OK);
    CHECK(summary.exact == 1);
    CHECK(summary.reserved == 0);

    // A column the caller DOES want must have room, checked before any work.
    double column[2] = {-1.0, -1.0};
    CHECK(xiapl_calculate_equity(holes.data(), 2, flop, &options, XIAPL_GAME_HOLDEM,
                                 column, nullptr, nullptr, 1, &summary)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(column[0] == -1.0);

    CHECK(xiapl_calculate_equity(holes.data(), 2, flop, nullptr, XIAPL_GAME_HOLDEM,
                                 nullptr, nullptr, nullptr, 0, &summary)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_calculate_equity(nullptr, 2, flop, &options, XIAPL_GAME_HOLDEM,
                                 nullptr, nullptr, nullptr, 0, &summary)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_calculate_equity(holes.data(), -1, flop, &options, XIAPL_GAME_HOLDEM,
                                 nullptr, nullptr, nullptr, 0, &summary)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_calculate_equity(holes.data(), 2, flop, &options, 5,
                                 nullptr, nullptr, nullptr, 0, &summary)
          == XIAPL_ERR_INVALID_ARGUMENT);

    // A two-bit board is the C++ routine's verdict, message verbatim.
    const std::uint64_t two_card_board = mask_of({kQs, kJs});
    xiapl_clear_last_error();
    CHECK(xiapl_calculate_equity(holes.data(), 2, two_card_board, &options,
                                 XIAPL_GAME_HOLDEM, nullptr, nullptr, nullptr, 0, &summary)
          == XIAPL_ERR_RUNTIME);
    CHECK(xiapl_last_error_message() == cpp_message([&] {
              xiapl::calculate_equity(holes, two_card_board,
                                      xiapl::SimulationOptions::exact(),
                                      xiapl::GameType::Holdem);
          }));
}

TEST_CASE("waist range equity result handle mirrors the C++ result") {
    const std::uint64_t flop = mask_of({k7h, k9s, k2c});
    Owned<xiapl_range_t> hero(xiapl_range_destroy), villain(xiapl_range_destroy);
    make_range(hero, "AA,KK,AKs", XIAPL_GAME_HOLDEM);
    make_range(villain, "QQ,JJ,AQs", XIAPL_GAME_HOLDEM);
    const xiapl::Range cpp_hero = xiapl::Range::from_string("AA,KK,AKs", xiapl::GameType::Holdem);
    const xiapl::Range cpp_villain = xiapl::Range::from_string("QQ,JJ,AQs", xiapl::GameType::Holdem);

    xiapl_sim_options_t exact{};
    REQUIRE(xiapl_sim_options_exact(&exact) == XIAPL_OK);

    {
        Owned<xiapl_range_equity_t> result(xiapl_range_equity_destroy);
        REQUIRE(xiapl_calculate_range_equity(hero.get(), villain.get(), flop, &exact,
                                             XIAPL_RANGE_EQUITY_PER_COMBO, result.out())
                == XIAPL_OK);
        REQUIRE(result.get() != nullptr);
        check_same_range_equity(
            result.get(),
            xiapl::calculate_range_equity(cpp_hero, cpp_villain, flop,
                                          xiapl::SimulationOptions::exact(),
                                          xiapl::RangeEquityMode::PerCombo));
    }
    {
        // AggregateOnly returns both breakdowns empty.
        Owned<xiapl_range_equity_t> result(xiapl_range_equity_destroy);
        REQUIRE(xiapl_calculate_range_equity(hero.get(), villain.get(), flop, &exact,
                                             XIAPL_RANGE_EQUITY_AGGREGATE_ONLY, result.out())
                == XIAPL_OK);
        check_same_range_equity(
            result.get(),
            xiapl::calculate_range_equity(cpp_hero, cpp_villain, flop,
                                          xiapl::SimulationOptions::exact(),
                                          xiapl::RangeEquityMode::AggregateOnly));
        std::int32_t total = -1;
        CHECK(xiapl_range_equity_hero(result.get(), nullptr, nullptr, nullptr, 0, &total)
              == XIAPL_OK);
        CHECK(total == 0);
    }
    {
        // AggregateOnly Monte Carlo: the one path that fills
        // aggregate_std_error and never falls back to exact.
        xiapl_sim_options_t seeded{};
        REQUIRE(xiapl_sim_options_mc_seeded(50000, 99, &seeded) == XIAPL_OK);
        Owned<xiapl_range_equity_t> result(xiapl_range_equity_destroy);
        REQUIRE(xiapl_calculate_range_equity(hero.get(), villain.get(), flop, &seeded,
                                             XIAPL_RANGE_EQUITY_AGGREGATE_ONLY, result.out())
                == XIAPL_OK);
        const xiapl::RangeEquityResult expected = xiapl::calculate_range_equity(
            cpp_hero, cpp_villain, flop, xiapl::SimulationOptions::mc_seeded(50000, 99),
            xiapl::RangeEquityMode::AggregateOnly);
        check_same_range_equity(result.get(), expected);
        CHECK(expected.aggregate_std_error > 0.0);  // the column is actually exercised
        CHECK(expected.exact == false);
    }

    // Game-tag mismatch is the C++ routine's verdict.
    Owned<xiapl_range_t> plo(xiapl_range_destroy);
    make_range(plo, "AAKK", XIAPL_GAME_PLO);
    xiapl_range_equity_t* raw = reinterpret_cast<xiapl_range_equity_t*>(1);
    xiapl_clear_last_error();
    CHECK(xiapl_calculate_range_equity(hero.get(), plo.get(), flop, &exact,
                                       XIAPL_RANGE_EQUITY_PER_COMBO, &raw)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(raw == nullptr);
    CHECK(xiapl_last_error_message() == cpp_message([&] {
              xiapl::calculate_range_equity(cpp_hero,
                                            xiapl::Range::from_string("AAKK", xiapl::GameType::Plo),
                                            flop, xiapl::SimulationOptions::exact(),
                                            xiapl::RangeEquityMode::PerCombo);
          }));

    CHECK(xiapl_calculate_range_equity(hero.get(), villain.get(), flop, &exact, 7, &raw)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_calculate_range_equity(nullptr, villain.get(), flop, &exact, 0, &raw)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_calculate_range_equity(hero.get(), villain.get(), flop, nullptr, 0, &raw)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_calculate_range_equity(hero.get(), villain.get(), flop, &exact, 0, nullptr)
          == XIAPL_ERR_INVALID_ARGUMENT);

    xiapl_range_equity_summary_t summary{};
    std::int32_t total = -1;
    CHECK(xiapl_range_equity_summary(nullptr, &summary) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_range_equity_hero(nullptr, nullptr, nullptr, nullptr, 0, &total)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_range_equity_villain(nullptr, nullptr, nullptr, nullptr, 0, &total)
          == XIAPL_ERR_INVALID_ARGUMENT);
    xiapl_range_equity_destroy(nullptr);
}

TEST_CASE("waist canonicalization mirrors <xiapl/canonicalize.h>") {
    const std::uint64_t hero = mask_of({kAs, kKs});
    const std::uint64_t board = mask_of({kQh, k9h, k3d});

    // The bare form is the frozen pre-v2 relabelling, which survives as a
    // routine even though version 1 was retired as a generation.
    std::uint64_t hero_out = 0, board_out = 0;
    REQUIRE(xiapl_canonicalize_hero_and_board(hero, board, &hero_out, &board_out) == XIAPL_OK);
    const auto frozen = xiapl::canonicalize_hero_and_board(hero, board);
    CHECK(hero_out == frozen.first);
    CHECK(board_out == frozen.second);

    // Strict is the only version the _v form accepts.
    REQUIRE(xiapl_canonicalize_hero_and_board_v(hero, board, XIAPL_CANON_STRICT,
                                                &hero_out, &board_out) == XIAPL_OK);
    const auto strict = xiapl::canonicalize_hero_and_board_v(
        hero, board, xiapl::CanonVersion::Strict);
    CHECK(hero_out == strict.first);
    CHECK(board_out == strict.second);

    // XIAPL_CANON_LEGACY (1) is retired as of ABI v4. The waist passes the code
    // straight through, so this pins that the C++ core -- not the boundary --
    // is what refuses it, and that the refusal is an error code rather than a
    // silent strict answer. The cast goes through the enum TYPE only; the
    // Legacy enumerator no longer exists.
    xiapl_clear_last_error();
    CHECK(xiapl_canonicalize_hero_and_board_v(hero, board, XIAPL_CANON_LEGACY,
                                              &hero_out, &board_out)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_last_error_message() == cpp_message([&] {
              xiapl::canonicalize_hero_and_board_v(
                  hero, board,
                  static_cast<xiapl::CanonVersion>(XIAPL_CANON_LEGACY));
          }));

    // An unknown version is refused rather than silently defaulted.
    xiapl_clear_last_error();
    CHECK(xiapl_canonicalize_hero_and_board_v(hero, board, 99, &hero_out, &board_out)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_last_error_message() == cpp_message([&] {
              xiapl::canonicalize_hero_and_board_v(hero, board,
                                                   static_cast<xiapl::CanonVersion>(99));
          }));

    REQUIRE(xiapl_canonicalize_board(board, &board_out) == XIAPL_OK);
    CHECK(board_out == xiapl::canonicalize_board(board));

    // Both hand forms, on the inputs where they agree.
    const std::vector<std::vector<std::uint8_t>> hands = {
        {kAs, kKs},  // suited
        {kAs, kKd},  // offsuit
        {kAs, kAh},  // pair
    };
    for (const auto& ids : hands) {
        std::vector<xiapl::Card> cards;
        for (std::uint8_t id : ids) cards.push_back(xiapl::Card::from_id(id));
        const std::string expected = xiapl::canonicalize_hand(cards);
        CHECK(fetch_string([&](char* b, std::int32_t c, std::int32_t* l) {
                  return xiapl_canonicalize_hand_ids(ids.data(), 2, b, c, l);
              }) == expected);
        CHECK(fetch_string([&](char* b, std::int32_t c, std::int32_t* l) {
                  return xiapl_canonicalize_hand_mask(mask_of(ids), b, c, l);
              }) == expected);
    }

    // ... and on the input where they DISAGREE, which is why both exist: a
    // mask cannot represent a duplicated card, so the id form answers "AA"
    // where the mask form sees a one-card mask and fails.
    const std::uint8_t duplicated[2] = {kAs, kAs};
    CHECK(fetch_string([&](char* b, std::int32_t c, std::int32_t* l) {
              return xiapl_canonicalize_hand_ids(duplicated, 2, b, c, l);
          }) == xiapl::canonicalize_hand({xiapl::Card::from_id(kAs),
                                          xiapl::Card::from_id(kAs)}));
    char buf[16];
    std::int32_t length = -1;
    xiapl_clear_last_error();
    CHECK(xiapl_canonicalize_hand_mask(mask_of({kAs, kAs}), buf, sizeof buf, &length)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_last_error_message() ==
          cpp_message([] { xiapl::canonicalize_hand_mask(xiapl::card_to_mask(xiapl::Card::from_id(kAs))); }));

    // Wrong arity on the id form is the C++ routine's verdict too.
    const std::uint8_t three[3] = {kAs, kKs, kQs};
    CHECK(xiapl_canonicalize_hand_ids(three, 3, buf, sizeof buf, &length)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_canonicalize_hand_ids(nullptr, 0, buf, sizeof buf, &length)
          == XIAPL_ERR_INVALID_ARGUMENT);

    CHECK(xiapl_canonicalize_hero_and_board(hero, board, nullptr, &board_out)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_canonicalize_hero_and_board(hero, board, &hero_out, nullptr)
          == XIAPL_ERR_INVALID_ARGUMENT);
    // A VALID version here, so that what is being pinned is the NULL check and
    // not the version verdict that now also rejects 1.
    CHECK(xiapl_canonicalize_hero_and_board_v(hero, board, XIAPL_CANON_STRICT,
                                              nullptr, &board_out)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_canonicalize_hero_and_board_v(hero, board, XIAPL_CANON_STRICT,
                                              &hero_out, nullptr)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_canonicalize_board(board, nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_canonicalize_hand_mask(hero, buf, sizeof buf, nullptr)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_canonicalize_hand_ids(three, 2, buf, sizeof buf, nullptr)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_canonicalize_hand_ids(nullptr, 2, buf, sizeof buf, &length)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_canonicalize_hand_ids(three, -1, buf, sizeof buf, &length)
          == XIAPL_ERR_INVALID_ARGUMENT);
}

TEST_CASE("waist canonical situation enumeration reproduces the frozen flop population") {
    // 1,286,792 is the exact Burnside orbit count for S4 acting on the suits.
    // XIAPL_CANON_LEGACY used to enumerate 1,420,796 representatives here (a
    // 10.41% under-merge) and the two populations were the point of the version
    // argument; that generation was removed, so strict is the only
    // case left -- the loop stays because `version` is still a parameter and a
    // future generation is enumerated by adding a Case, not by re-indenting the
    // body. Only the flop is enumerated: turn and river are minutes of work.
    struct Case { std::int32_t version; std::int64_t expected; };
    for (const Case& probe : {Case{XIAPL_CANON_STRICT, 1286792}}) {
        Owned<xiapl_canonical_situations_t> situations(xiapl_canonical_situations_destroy);
        REQUIRE(xiapl_generate_canonical_situations(3, probe.version, situations.out())
                == XIAPL_OK);
        REQUIRE(situations.get() != nullptr);

        std::int64_t total = -1;
        REQUIRE(xiapl_canonical_situations_count(situations.get(), &total) == XIAPL_OK);
        CHECK(total == probe.expected);

        // One full read ...
        std::vector<std::uint64_t> hero(static_cast<std::size_t>(total));
        std::vector<std::uint64_t> board(static_cast<std::size_t>(total));
        std::int64_t written = -1;
        REQUIRE(xiapl_canonical_situations_fill(situations.get(), 0, total, hero.data(),
                                                board.data(), &written) == XIAPL_OK);
        CHECK(written == total);

        // ... and the same content read back in bounded chunks, which is how a
        // caller streams the river's ~123 million pairs.
        std::vector<std::uint64_t> chunk_hero, chunk_board;
        chunk_hero.reserve(hero.size());
        chunk_board.reserve(board.size());
        const std::int64_t chunk = 100000;
        std::vector<std::uint64_t> hero_buf(static_cast<std::size_t>(chunk));
        std::vector<std::uint64_t> board_buf(static_cast<std::size_t>(chunk));
        for (std::int64_t offset = 0;; offset += chunk) {
            std::int64_t got = -1;
            REQUIRE(xiapl_canonical_situations_fill(situations.get(), offset, chunk,
                                                    hero_buf.data(), board_buf.data(),
                                                    &got) == XIAPL_OK);
            if (got == 0) break;  // an offset past the end terminates the loop
            chunk_hero.insert(chunk_hero.end(), hero_buf.begin(),
                              hero_buf.begin() + static_cast<std::ptrdiff_t>(got));
            chunk_board.insert(chunk_board.end(), board_buf.begin(),
                               board_buf.begin() + static_cast<std::ptrdiff_t>(got));
        }
        CHECK(chunk_hero == hero);
        CHECK(chunk_board == board);

        // Either column may be NULL; the count still comes back.
        REQUIRE(xiapl_canonical_situations_fill(situations.get(), 0, 4, nullptr, nullptr,
                                                &written) == XIAPL_OK);
        CHECK(written == 4);

        // An offset past the end is not an error; a negative one is.
        REQUIRE(xiapl_canonical_situations_fill(situations.get(), total + 1000, 10, nullptr,
                                                nullptr, &written) == XIAPL_OK);
        CHECK(written == 0);
        CHECK(xiapl_canonical_situations_fill(situations.get(), -1, 10, nullptr, nullptr,
                                              &written) == XIAPL_ERR_INVALID_ARGUMENT);
        CHECK(xiapl_canonical_situations_fill(situations.get(), 0, -1, nullptr, nullptr,
                                              &written) == XIAPL_ERR_INVALID_ARGUMENT);
        CHECK(xiapl_canonical_situations_fill(situations.get(), 0, 10, nullptr, nullptr,
                                              nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    }

    // The retired version on a VALID board size: the version is what fails, no
    // enumeration is attempted, and the message is the C++ core's. This is the
    // pass-through contract for XIAPL_CANON_LEGACY stated in c_api.h.
    xiapl_canonical_situations_t* raw = reinterpret_cast<xiapl_canonical_situations_t*>(1);
    xiapl_clear_last_error();
    CHECK(xiapl_generate_canonical_situations(3, XIAPL_CANON_LEGACY, &raw)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(raw == nullptr);
    CHECK(xiapl_last_error_message() == cpp_message([] {
              xiapl::generate_canonical_situations(
                  3, static_cast<xiapl::CanonVersion>(XIAPL_CANON_LEGACY));
          }));

    // board_size and version are both refused by the C++ routine, in that
    // order, with its own messages: a call that gets BOTH wrong reports the
    // board size, which is what the waist's pass-through preserves.
    raw = reinterpret_cast<xiapl_canonical_situations_t*>(1);
    xiapl_clear_last_error();
    CHECK(xiapl_generate_canonical_situations(2, XIAPL_CANON_LEGACY, &raw)
          == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(raw == nullptr);
    CHECK(xiapl_last_error_message() == cpp_message([] {
              xiapl::generate_canonical_situations(
                  2, static_cast<xiapl::CanonVersion>(XIAPL_CANON_LEGACY));
          }));
    CHECK(xiapl_generate_canonical_situations(3, 99, &raw) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(raw == nullptr);
    CHECK(xiapl_generate_canonical_situations(3, XIAPL_CANON_LEGACY, nullptr)
          == XIAPL_ERR_INVALID_ARGUMENT);

    std::int64_t total = -1;
    CHECK(xiapl_canonical_situations_count(nullptr, &total) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_canonical_situations_fill(nullptr, 0, 0, nullptr, nullptr, &total)
          == XIAPL_ERR_INVALID_ARGUMENT);
    xiapl_canonical_situations_destroy(nullptr);
}

TEST_CASE("waist try_* char parsers require both out pointers") {
    // Both outs are written on every call, so neither is optional -- a caller
    // that passed only one would read an uninitialized value.
    std::int32_t value = -1;
    CHECK(xiapl_try_rank_from_char('A', &value, nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_try_rank_from_char('A', nullptr, &value) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_try_rank_from_char('!', &value, nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_try_suit_from_char('s', &value, nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_try_suit_from_char('s', nullptr, &value) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(xiapl_try_suit_from_char('!', &value, nullptr) == XIAPL_ERR_INVALID_ARGUMENT);
    CHECK(value == -1);  // untouched
}

TEST_CASE("waist sim option factories carry a negative iteration count verbatim") {
    // The C++ factories do not validate `iterations`; a non-positive count is
    // simply Exact mode. The waist adds no check the C++ API does not have.
    for (std::int32_t iterations : {-1, -1000000}) {
        xiapl_sim_options_t options{};
        REQUIRE(xiapl_sim_options_mc_random(iterations, &options) == XIAPL_OK);
        const xiapl::SimulationOptions cpp = xiapl::SimulationOptions::mc_random(iterations);
        CHECK(options.iterations == cpp.iterations);
        CHECK(options.deterministic == (cpp.deterministic ? 1 : 0));
        std::int32_t mode = -1;
        CHECK(xiapl_sim_options_effective_mode(&options, &mode) == XIAPL_OK);
        CHECK(mode == static_cast<std::int32_t>(cpp.effective_mode()));

        REQUIRE(xiapl_sim_options_mc_seeded(iterations, 7, &options) == XIAPL_OK);
        const xiapl::SimulationOptions cpp_seeded =
            xiapl::SimulationOptions::mc_seeded(iterations, 7);
        CHECK(options.iterations == cpp_seeded.iterations);
        CHECK(options.seed == cpp_seeded.seed);
        CHECK(xiapl_sim_options_effective_mode(&options, &mode) == XIAPL_OK);
        CHECK(mode == static_cast<std::int32_t>(cpp_seeded.effective_mode()));
    }
}

TEST_CASE("waist describe_hand narrows a kicker the way the C++ type does") {
    // kicker_count IS checked (both sides index a five-slot array), but a
    // kicker VALUE is not: xiapl::HandValue::kickers is uint8_t, so the waist
    // hands over what that type would hold rather than inventing a range check
    // the header never documented.
    xiapl_hand_value_t value{};
    value.category = XIAPL_HAND_ONE_PAIR;
    value.kicker_count = 2;
    value.kickers[0] = 300;
    value.kickers[1] = -1;

    xiapl::HandValue cpp;
    cpp.category = xiapl::HandCategory::OnePair;
    cpp.kicker_count = 2;
    cpp.kickers[0] = static_cast<std::uint8_t>(300);
    cpp.kickers[1] = static_cast<std::uint8_t>(-1);

    CHECK(fetch_string([&](char* b, std::int32_t c, std::int32_t* l) {
              return xiapl_describe_hand(&value, b, c, l);
          }) == xiapl::describe_hand(cpp));

    // The memory-safety check is still there.
    value.kicker_count = XIAPL_HAND_VALUE_MAX_KICKERS + 1;
    char buf[64];
    std::int32_t length = -1;
    CHECK(xiapl_describe_hand(&value, buf, sizeof buf, &length) == XIAPL_ERR_INVALID_ARGUMENT);
    value.kicker_count = -1;
    CHECK(xiapl_describe_hand(&value, buf, sizeof buf, &length) == XIAPL_ERR_INVALID_ARGUMENT);
}
