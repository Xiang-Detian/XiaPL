#include <xiapl/deck.h>
#include <xiapl/detail/fast_rng.h>
#include <xiapl/utils.h>

#include <algorithm>
#include <random>
#include <chrono>
#include <stdexcept>
#include <string>

namespace xiapl {

Deck::Deck() {
    reset();
}

Deck::Deck(const std::vector<Card>& cards) {
    set_cards(cards);
}

void Deck::shuffle() {
    std::mt19937_64 rng(std::random_device{}());
    std::shuffle(card_ids.begin(), card_ids.end(), rng);
}

void Deck::shuffle(std::uint64_t seed) {
    // Default-construct then seed explicitly: the seeding ctor remaps
    // seed == 0 to std::random_device (see FastRng's documented footgun),
    // which would make shuffle(0) non-reproducible. seed() has no such
    // zero-guard. Same convention as src/core/equity_range.cpp's seeding sites.
    FastRng rng;
    rng.seed(seed);
    std::shuffle(card_ids.begin(), card_ids.end(), rng);
}

std::vector<Card> Deck::deal(int n) {
    std::vector<Card> dealt;
    for (int i = 0; i < n && !card_ids.empty(); ++i) {
        int id = card_ids.back();
        card_ids.pop_back();
        dealt.push_back(Card::from_id(id));
    }
    return dealt;
}

void Deck::reset(bool shuffle) {
    card_ids.clear();
    card_ids.reserve(52);
    // Build 52-card deck (managed by IDs 0..51). Jokers are not used.
    for (int id = 0; id < 52; ++id) {
        card_ids.push_back(id);
    }
    if (shuffle) {
        this->shuffle();
    }
}

void Deck::remove_cards(const std::vector<Card>& to_remove) {
    for (const auto& card : to_remove) {
        int id = static_cast<int>(card.id);
        auto it = std::find(card_ids.begin(), card_ids.end(), id);
        if (it != card_ids.end()) {
            card_ids.erase(it);
        }
    }
}

int Deck::size() const {
    return static_cast<int>(card_ids.size());
}

std::vector<Card> Deck::get_cards() const {
    std::vector<Card> out;
    out.reserve(card_ids.size());
    for (int id : card_ids) {
        out.push_back(Card::from_id(id));
    }
    return out;
}

void Deck::set_cards(const std::vector<Card>& new_cards) {
    // Validate inputs before mutating internal state: every card must be a
    // real id (0..51) and the multiset must be unique. This keeps invalid /
    // duplicate cards from silently propagating through deal_mask, deal_one,
    // and downstream evaluation.
    std::uint64_t seen = 0ULL;
    for (const auto& c : new_cards) {
        if (!is_valid_card_id(c.id)) {
            throw std::invalid_argument("Deck::set_cards: invalid Card id");
        }
        const std::uint64_t bit = (1ULL << c.id);
        if (seen & bit) {
            throw std::invalid_argument("Deck::set_cards: duplicate card");
        }
        seen |= bit;
    }
    card_ids.clear();
    card_ids.reserve(new_cards.size());
    for (const auto& c : new_cards) {
        card_ids.push_back(static_cast<int>(c.id));
    }
}

bool Deck::empty() const {
    return card_ids.empty();
}

bool Deck::has_cards(int n) const {
    return static_cast<int>(card_ids.size()) >= n;
}

Card Deck::deal_one() {
    if (card_ids.empty()) {
        return Card();
    }
    int id = card_ids.back();
    card_ids.pop_back();
    return Card::from_id(id);
}

void Deck::burn(int n) {
    deal(n);
}

std::string Deck::repr() const {
    std::string result;
    for (int id : card_ids) {
        result += Card::from_id(id).to_string();
    }
    return result;
}

std::vector<int> Deck::get_card_ids() const {
    return card_ids;
}

std::vector<int> Deck::deal_ids(int n) {
    std::vector<int> dealt;
    dealt.reserve(static_cast<std::size_t>(std::max(0, n)));
    for (int i = 0; i < n && !card_ids.empty(); ++i) {
        int id = card_ids.back();
        card_ids.pop_back();
        dealt.push_back(id);
    }
    return dealt;
}

int Deck::deal_one_id() {
    if (card_ids.empty()) {
        return -1;
    }
    int id = card_ids.back();
    card_ids.pop_back();
    return id;
}

std::uint64_t Deck::deal_mask(int n) {
    std::uint64_t mask = 0ULL;
    for (int i = 0; i < n && !card_ids.empty(); ++i) {
        int id = card_ids.back();
        card_ids.pop_back();
        mask |= (1ULL << static_cast<std::uint64_t>(id));
    }
    return mask;
}
} // namespace xiapl
