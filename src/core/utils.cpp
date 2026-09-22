#include <xiapl/utils.h>

#include <stdexcept>

namespace xiapl {

// Single card -> bitmask. Throws on invalid Card (id >= 52).
std::uint64_t card_to_mask(const Card& c) {
    if (!is_valid_card_id(c.id)) {
        throw std::invalid_argument("card_to_mask: invalid Card id");
    }
    return (1ULL << c.id);
}

// Multiple cards -> mask. Throws on any invalid Card in the input.
std::uint64_t cards_to_mask(const std::vector<Card>& cards) {
    std::uint64_t mask = 0;
    for (const auto& c : cards) {
        if (!is_valid_card_id(c.id)) {
            throw std::invalid_argument("cards_to_mask: invalid Card id");
        }
        mask |= (1ULL << c.id);
    }
    return mask;
}

// Mask -> Card vector
std::vector<Card> mask_to_cards(std::uint64_t mask) {
    std::vector<Card> out;
    out.reserve(popcount64(mask));
    for (int id = 0; id < 52; ++id) {
        if (mask & (1ULL << id)) {
            out.push_back(Card::from_id(id));
        }
    }
    return out;
}

// Mask -> ID vector
std::vector<int> mask_to_ids(std::uint64_t mask) {
    std::vector<int> ids;
    ids.reserve(popcount64(mask));
    for (int id = 0; id < 52; ++id) {
        if (mask & (1ULL << id)) {
            ids.push_back(id);
        }
    }
    return ids;
}

// Card ID vector -> 52-bit mask. Throws std::invalid_argument on any id
// outside [0, 52) -- no sentinel value is skipped.
std::uint64_t ids_to_mask(const std::vector<int>& ids) {
    std::uint64_t mask = 0ULL;
    for (const int id : ids) {
        if (id < 0 || id >= 52) {
            throw std::invalid_argument("ids_to_mask: id out of range ([0, 52))");
        }
        mask |= (1ULL << id);
    }
    return mask;
}

// Check if two masks have overlapping cards
bool masks_overlap(std::uint64_t a, std::uint64_t b) {
    return (a & b) != 0ULL;
}

std::optional<int> try_rank_from_char(char c) noexcept {
    switch (c) {
        case '2': return 2;
        case '3': return 3;
        case '4': return 4;
        case '5': return 5;
        case '6': return 6;
        case '7': return 7;
        case '8': return 8;
        case '9': return 9;
        case 'T': case 't': return 10;
        case 'J': case 'j': return 11;
        case 'Q': case 'q': return 12;
        case 'K': case 'k': return 13;
        case 'A': case 'a': return 14;
        default: return std::nullopt;
    }
}

std::optional<int> try_suit_from_char(char c) noexcept {
    switch (c) {
        case 'c': case 'C': return 0;
        case 'd': case 'D': return 1;
        case 'h': case 'H': return 2;
        case 's': case 'S': return 3;
        default: return std::nullopt;
    }
}

// Convert rank (2-14) to char '2'..'9','T','J','Q','K','A'
char rank_to_char(int rank) {
    if (rank >= 2 && rank <= 9) {
        return static_cast<char>('0' + rank);
    }
    switch (rank) {
        case 10: return 'T';
        case 11: return 'J';
        case 12: return 'Q';
        case 13: return 'K';
        case 14: return 'A';
        default: return '?';
    }
}

// Convert suit (0=c,1=d,2=h,3=s) to char 'c','d','h','s'
char suit_to_char(int suit) {
    switch (suit) {
        case 0: return 'c';
        case 1: return 'd';
        case 2: return 'h';
        case 3: return 's';
        default: return '?';
    }
}

bool next_combination(std::vector<int>& indices, int n_total) {
    const int k = static_cast<int>(indices.size());
    for (int i = k - 1; i >= 0; --i) {
        if (indices[i] < n_total - k + i) {
            ++indices[i];
            for (int j = i + 1; j < k; ++j) {
                indices[j] = indices[j - 1] + 1;
            }
            return true;
        }
    }
    return false;
}
} // namespace xiapl
