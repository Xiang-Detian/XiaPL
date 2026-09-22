#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xiapl {

// Rank in [2, 14]. Two=2, Three=3, ..., Ten=10, Jack=11, Queen=12, King=13, Ace=14.
enum class Rank : std::uint8_t {
    Two = 2,
    Three = 3,
    Four = 4,
    Five = 5,
    Six = 6,
    Seven = 7,
    Eight = 8,
    Nine = 9,
    Ten = 10,
    Jack = 11,
    Queen = 12,
    King = 13,
    Ace = 14,
};

// Suit ordering matches the integer convention used everywhere else in the
// codebase: 0=c, 1=d, 2=h, 3=s.
enum class Suit : std::uint8_t {
    Clubs = 0,
    Diamonds = 1,
    Hearts = 2,
    Spades = 3,
};

// Card represented as an integer ID in [0, 51].
// id = suit * 13 + (rank - 2)
// rank: 2-14 (2,3,4,5,6,7,8,9,T,J,Q,K,A)
// suit: 0-3  (0=c, 1=d, 2=h, 3=s)
struct Card {
    using IdType = std::uint8_t;
    IdType id; // 0-51, 255 = Invalid

    // Invalid card sentinel ID
    static constexpr IdType INVALID_ID = 255;

    // Constructors
    Card() : id(INVALID_ID) {}
    // Throws std::invalid_argument if id >= 52 and id != INVALID_ID.
    // For a non-throwing factory use Card::from_id.
    explicit Card(IdType id);
    Card(int rank, int suit); // rank: 2-14, suit: 0-3
    Card(Rank rank, Suit suit); // typed constructor

    // Accessors (id is interpreted as suit * 13 + (rank - 2))
    int rank() const { return static_cast<int>(id % 13) + 2; } // 0-12 -> 2-14
    int suit() const { return static_cast<int>(id / 13); }     // 0-3

    Rank rank_enum() const { return static_cast<Rank>(rank()); }
    Suit suit_enum() const { return static_cast<Suit>(suit()); }

    // String representation
    std::string to_string() const;
    std::string repr() const;

    // Comparison operators
    bool operator==(const Card& other) const { return id == other.id; }
    bool operator!=(const Card& other) const { return id != other.id; }
    bool operator<(const Card& other) const { return id < other.id; }

    // Construct from string (e.g. "As", "Td"). Throws on invalid input.
    static Card from_string(const std::string& s);
    static Card from_id(IdType id);
};

// Non-throwing parser. Returns std::nullopt on invalid input.
std::optional<Card> try_parse_card(std::string_view text);

} // namespace xiapl

// Hash support (for use with unordered_map, etc.)
namespace std {
    template <>
    struct hash<xiapl::Card> {
        std::size_t operator()(const xiapl::Card& c) const noexcept {
            return static_cast<std::size_t>(c.id);
        }
    };
}
