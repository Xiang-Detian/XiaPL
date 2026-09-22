// card.cpp
#include <xiapl/card.h>
#include <xiapl/utils.h>

#include <stdexcept>

namespace xiapl {

// Constant arrays for display
static const char SUIT_CHARS[] = {'c', 'd', 'h', 's'};
static const char RANK_CHARS[] = "23456789TJQKA"; // index 0 corresponds to '2'

// Throwing wrappers around the shared non-throwing parsers in utils.
static int get_suit_int(char s) {
    if (auto v = try_suit_from_char(s)) return *v;
    // Message text is duplicated in binding/core_card_utils.cpp (PyCard rank/suit constructor); keep both in sync (grep binding/ before editing).
    throw std::invalid_argument("Invalid suit char");
}

static int get_rank_int(char r) {
    if (auto v = try_rank_from_char(r)) return *v;
    throw std::invalid_argument("Invalid rank char");
}

// Validating id constructor.
Card::Card(IdType raw_id) : id(raw_id) {
    if (raw_id != INVALID_ID && raw_id >= 52) {
        throw std::invalid_argument("Card id must be 0..51 or INVALID_ID(255)");
    }
}

// Constructor: compute ID from rank(2-14), suit(0-3)
Card::Card(int rank, int suit) {
    if (rank < 2 || rank > 14 || suit < 0 || suit > 3) {
        throw std::invalid_argument("Invalid rank or suit");
    }
    id = static_cast<IdType>(suit * 13 + (rank - 2));
}

// Typed constructor: enum values are constrained to valid ranges by definition.
Card::Card(Rank rank, Suit suit) {
    int r = static_cast<int>(rank);
    int s = static_cast<int>(suit);
    if (r < 2 || r > 14 || s < 0 || s > 3) {
        throw std::invalid_argument("Invalid Rank or Suit enum value");
    }
    id = static_cast<IdType>(s * 13 + (r - 2));
}

// Static method: convert to string
std::string Card::to_string() const {
    if (id == INVALID_ID) {
        return "Invalid";
    }

    int r = rank();        // 2-14
    int s = suit();        // 0-3
    int r_idx = r - 2;     // 0-12
    int s_idx = s;         // 0-3

    std::string s_out;
    s_out += RANK_CHARS[r_idx];
    s_out += SUIT_CHARS[s_idx];
    return s_out;
}

std::string Card::repr() const {
    if (id == INVALID_ID) {
        return "<Card Invalid>";
    }
    return "<Card " + to_string() + ">";
}

// Static method: construct from string (e.g. "As", "Td")
Card Card::from_string(const std::string& s) {
    if (s.size() != 2) {
        throw std::invalid_argument("Card string must be 2 characters");
    }

    int rank = get_rank_int(s[0]); // '2'..'A'
    int suit = get_suit_int(s[1]); // 'c','d','h','s'

    return Card(rank, suit);
}

// Static method: construct from ID.
// IdType is unsigned, so `id < 52` alone is the full validity test; an
// `id >= 0` lower bound would be vacuously true.
Card Card::from_id(IdType id) {
    if (id < 52) {
        return Card(id);
    } else {
        return Card();
    }
}

// Non-throwing parser.
std::optional<Card> try_parse_card(std::string_view text) {
    if (text.size() != 2) return std::nullopt;
    auto rank = try_rank_from_char(text[0]);
    auto suit = try_suit_from_char(text[1]);
    if (!rank || !suit) return std::nullopt;
    return Card(*rank, *suit);
}

} // namespace xiapl
