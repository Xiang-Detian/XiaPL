#pragma once

#include <vector>
#include <xiapl/card.h>
#include <string>
#include <cstdint>

namespace xiapl {

class Deck {
public:
    // Default-constructed Deck is sorted (id=0..51), NOT shuffled, so that
    // tests and library callers get deterministic state by default. Call
    // `shuffle()` or `shuffle(seed)` explicitly to randomize.
    Deck();
    Deck(const std::vector<Card>& cards);
    // Convenience: shuffles using std::random_device (non-reproducible).
    // For reproducibility use the shuffle(seed) overload below.
    void shuffle();
    std::vector<Card> deal(int n = 1);
    // shuffle: if true, shuffles with std::random_device after reset.
    //         if false (default), leaves the deck in sorted order.
    void reset(bool shuffle = false);
    void remove_cards(const std::vector<Card>& cards);
    int size() const;
    std::vector<Card> get_cards() const;
    void set_cards(const std::vector<Card>& new_cards);

    bool empty() const;
    bool has_cards(int n = 1) const;
    Card deal_one();
    void burn(int n = 1);
    // Reproducible shuffle: the same seed always produces the same
    // permutation. Seed 0 is a valid, ordinary seed (not "random").
    void shuffle(std::uint64_t seed);
    std::string repr() const;

    // Return list of remaining card IDs (0-51) for direct C++ use.
    std::vector<int> get_card_ids() const;
    // Deal card IDs directly (0..51) without constructing Card objects.
    std::vector<int> deal_ids(int n);

    // Deal a single card ID (0..51). Returns -1 if empty.
    int deal_one_id();

    // Deal `n` cards and return as a bitmask (each dealt card sets one bit).
    // Returns 0 if the deck is empty.
    std::uint64_t deal_mask(int n);

private:
    // Internally stores cards as integer IDs (0-51) instead of Card objects
    // to facilitate bitmask/ID-based operations.
    std::vector<int> card_ids;
};

} // namespace xiapl
