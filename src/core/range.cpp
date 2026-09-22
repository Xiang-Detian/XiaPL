#include <xiapl/range.h>

#include <xiapl/card.h>
#include <xiapl/utils.h>
#include "range_parse.h"
#include "range_plo.h"

#include <cctype>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace xiapl {

namespace {

using internal::trim;
using internal::is_rank_char;
using internal::is_suit_char;

std::vector<std::string> split_tokens(std::string_view text) {
    std::vector<std::string> tokens;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t comma = text.find(',', pos);
        std::string token;
        if (comma == std::string_view::npos) {
            token = trim(text.substr(pos));
            pos = text.size();
        } else {
            token = trim(text.substr(pos, comma - pos));
            pos = comma + 1;
        }
        if (!token.empty()) {
            tokens.push_back(std::move(token));
        }
    }
    return tokens;
}

bool is_exact_combo_token(const std::string& token) {
    return token.size() == 4 &&
           is_rank_char(token[0]) &&
           is_suit_char(token[1]) &&
           is_rank_char(token[2]) &&
           is_suit_char(token[3]);
}

std::uint64_t parse_exact_combo_mask(const std::string& token) {
    Card first = Card::from_string(token.substr(0, 2));
    Card second = Card::from_string(token.substr(2, 2));
    if (first == second) {
        throw std::invalid_argument("range token contains duplicate cards: " + token);
    }
    return cards_to_mask({first, second});
}

double parse_weight(const std::string& text) {
    std::string value = trim(text);
    if (value.empty()) {
        throw std::invalid_argument("range weight is empty");
    }

    // Locale-independent (see parse_weight_literal): std::stod would reject
    // "0.5" outright under a comma-decimal global locale. The grammar there
    // also has no "nan" spelling, which the (0, 1] test below cannot catch on
    // its own -- NaN compares false against both bounds.
    //
    // Reject 0.0 here (via the (0.0, 1.0] check inside parse_range_weight) so
    // the contract matches filter_combos, which drops weights <= 0.0 silently
    // in simulation.
    return internal::parse_range_weight(
        value, [](internal::WeightRangeError kind, const std::string& v) {
            switch (kind) {
                case internal::WeightRangeError::NotANumber:
                    throw std::invalid_argument("range weight is not a number: " + v);
                case internal::WeightRangeError::TrailingCharacters:
                    throw std::invalid_argument("range weight has trailing characters: " + v);
                case internal::WeightRangeError::OutOfRange:
                    throw std::invalid_argument(
                        "range weight is out of the representable range: " + v);
                case internal::WeightRangeError::OutOfBounds:
                    throw std::invalid_argument("range weight must be in (0.0, 1.0]");
            }
        });
}

struct ParsedToken {
    std::string notation;
    double weight;
};

ParsedToken parse_weighted_token(const std::string& token) {
    std::size_t colon = token.find(':');
    if (colon == std::string::npos) {
        return {trim(token), 1.0};
    }
    if (token.find(':', colon + 1) != std::string::npos) {
        throw std::invalid_argument("range token contains multiple ':' separators: " + token);
    }

    std::string notation = trim(std::string_view(token).substr(0, colon));
    std::string weight_text = trim(std::string_view(token).substr(colon + 1));
    if (notation.empty()) {
        throw std::invalid_argument("range notation is empty");
    }
    return {notation, parse_weight(weight_text)};
}

std::vector<std::uint64_t> expand_holdem_token(const std::string& notation) {
    if (is_exact_combo_token(notation)) {
        return {parse_exact_combo_mask(notation)};
    }
    return internal::expand_range_token_to_masks(notation);
}

// Validates every combo mask against cards_per_hand(game); throws
// std::invalid_argument naming the offending mask on the first mismatch.
void validate_combo_masks(const std::vector<Combo>& combos, GameType game) {
    const int expected_bits = cards_per_hand(game);
    for (const auto& combo : combos) {
        const int bits = popcount64(combo.mask);
        if (bits != expected_bits) {
            throw std::invalid_argument(
                "Range: combo " + internal::describe_mask(combo.mask) +
                " has " + std::to_string(bits) + " cards, expected " +
                std::to_string(expected_bits) + " for this GameType");
        }
    }
}

// Hold'em range-notation parser (shared by both from_string overloads).
std::vector<Combo> parse_holdem_range(std::string_view text) {
    std::vector<Combo> combos;
    std::set<std::uint64_t> seen;

    for (const auto& token : split_tokens(text)) {
        ParsedToken parsed = parse_weighted_token(token);
        std::vector<std::uint64_t> masks = expand_holdem_token(parsed.notation);
        if (masks.empty()) {
            throw std::invalid_argument("range token produced no combos: " + parsed.notation);
        }

        for (std::uint64_t mask : masks) {
            if (!seen.insert(mask).second) {
                throw std::invalid_argument("range contains duplicate combo");
            }
            combos.push_back(Combo{mask, parsed.weight});
        }
    }

    return combos;
}

} // namespace

Range::Range(std::vector<Combo> combos)
    : Range(std::move(combos), GameType::Holdem) {}

Range::Range(std::vector<Combo> combos, GameType game)
    : combos_(std::move(combos)), game_(game) {
    validate_combo_masks(combos_, game_);
}

namespace {

// Per-game arms of Range::all. Formerly the public all_holdem() / all_plo();
// the surface is now one entry point with a trailing `game`.
Range all_holdem_combos() {
    std::vector<Combo> combos;
    combos.reserve(1326);
    for (int first = 0; first < 52; ++first) {
        for (int second = first + 1; second < 52; ++second) {
            std::uint64_t mask = (std::uint64_t{1} << first) |
                                 (std::uint64_t{1} << second);
            combos.push_back(Combo{mask, 1.0});
        }
    }
    return Range(std::move(combos), GameType::Holdem);
}

Range all_plo_combos() {
    // Enumerate all C(52,4) = 270,725 4-card masks in colexicographic order
    // (highest card id outermost), which for 4-bit masks is exactly ascending
    // numeric mask order -- the same order the PLO range-string parser emits
    // after its sort. Keeping the two routes order-identical means a seeded
    // MC stream never depends on how the caller spelled the full range.
    std::vector<Combo> combos;
    combos.reserve(270725);
    for (int c3 = 3; c3 < 52; ++c3) {
        for (int c2 = 2; c2 < c3; ++c2) {
            for (int c1 = 1; c1 < c2; ++c1) {
                for (int c0 = 0; c0 < c1; ++c0) {
                    const std::uint64_t mask = (std::uint64_t{1} << c0) |
                                               (std::uint64_t{1} << c1) |
                                               (std::uint64_t{1} << c2) |
                                               (std::uint64_t{1} << c3);
                    combos.push_back(Combo{mask, 1.0});
                }
            }
        }
    }
    return Range(std::move(combos), GameType::Plo);
}

} // namespace

Range Range::all(GameType game) {
    return game == GameType::Plo ? all_plo_combos() : all_holdem_combos();
}

Range Range::from_string(std::string_view text) {
    return from_string(text, GameType::Holdem);
}

Range Range::from_string(std::string_view text, GameType game) {
    if (game == GameType::Plo) {
        // Frozen v0.1 PLO notation; see src/core/range_plo.cpp for the
        // grammar. Hold'em notation is not accepted under the PLO tag (a PLO
        // pattern is 4 rank symbols), and vice versa.
        return Range(internal::parse_plo_range(text), GameType::Plo);
    }
    return Range(parse_holdem_range(text), GameType::Holdem);
}

std::vector<Combo> Range::valid_combos(std::uint64_t dead_mask) const {
    std::vector<Combo> valid;
    valid.reserve(combos_.size());
    for (const auto& combo : combos_) {
        if ((combo.mask & dead_mask) == 0) {
            valid.push_back(combo);
        }
    }
    return valid;
}

double Range::total_weight(std::uint64_t dead_mask) const {
    double total = 0.0;
    for (const auto& combo : combos_) {
        if ((combo.mask & dead_mask) == 0) {
            total += combo.weight;
        }
    }
    return total;
}

std::optional<Range> try_parse_range(std::string_view text) {
    // Catch parser-level errors only; std::bad_alloc and other system errors
    // must propagate so callers can distinguish "malformed input" from
    // "resource exhaustion".
    try {
        return Range::from_string(text);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    } catch (const std::out_of_range&) {
        // std::stod inside parse_weight can throw std::out_of_range on
        // overflow; that is also a malformed weight.
        return std::nullopt;
    }
}

} // namespace xiapl
