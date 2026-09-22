#pragma once

// Internal-only range parsing helpers. Not installed; consumers should use
// xiapl::Range (include/xiapl/range.h) instead.

#include <xiapl/card.h>
#include <xiapl/game_type.h>
#include <xiapl/utils.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xiapl::internal {

// Expand a single range token (e.g. "JJ+", "AKs", "TT-88", "76s-54s",
// "A9o-A2o", "98s+", "AK", "AK+") into a list of 2-card hand masks.
// Throws std::invalid_argument on malformed tokens.
std::vector<std::uint64_t> expand_range_token_to_masks(const std::string& token);

// Shared ASCII whitespace trim, used by range_holdem.cpp (its own
// definition), range.cpp, and range_plo.cpp.
std::string trim(std::string_view text);

// Rank/suit character predicates shared by the Hold'em parser (range.cpp's
// exact-combo token detection) and the PLO parser (range_plo.cpp's pattern
// lexer, where a rank SYMBOL additionally accepts the '*' wildcard on top of
// this).
inline bool is_rank_char(char c) {
    return try_rank_from_char(c).has_value();
}

inline bool is_suit_char(char c) {
    return try_suit_from_char(c).has_value();
}

// Outcome of parse_weight_literal below.
enum class WeightLiteral {
    Ok,                 // fully consumed; the value is written to `out`
    NotANumber,         // no weight literal at all ("abc", "+0.5", "nan")
    TrailingCharacters, // a literal followed by junk ("0.5x", "0x1", "1e")
    OutOfRange,         // a well-formed literal `double` cannot represent
};

// Locale-INDEPENDENT parse of a range weight literal, shared by the Hold'em
// parser (range.cpp) and the PLO parser (range_plo.cpp) so both agree on the
// grammar and on the decimal separator.
//
// `text` must already be trimmed and non-empty. The accepted grammar is
// deliberately narrower than strtod's:
//
//   digits [ "." digits ] [ ("e"|"E") ["+"|"-"] digits ]  |  "." digits [ exp ]
//
// i.e. an unsigned decimal literal only: no sign, no hex form, no "inf" /
// "nan" spellings. Weights live in (0.0, 1.0], so none of those can name a
// legal weight, and accepting them silently would let typos through --
// strtod reads "0x1" as 1.0, which would have been a valid weight.
WeightLiteral parse_weight_literal(const std::string& text, double& out);

// Failure categories reported by parse_range_weight below. OutOfBounds
// covers the caller-side (0.0, 1.0] range check, not just parse_weight_literal's
// own OutOfRange (an unrepresentable literal).
enum class WeightRangeError {
    NotANumber,
    TrailingCharacters,
    OutOfRange,
    OutOfBounds,
};

// Shared switch-and-range-check skeleton behind both range-weight parsers:
// range.cpp's Hold'em `parse_weight` and range_plo.cpp's PLO `parse_weight`.
// The two differ in signature and in error wording -- the PLO one wraps
// every message in the offending item's text (via item_error) and appends a
// shared teaching hint, the Hold'em one throws a flat message, and PLO's
// wording is pinned by tests/test_plo_parser.cpp -- so the wording itself
// stays with each caller; only the parse-and-range-check logic is shared.
//
// `value` must already be trimmed and non-empty (both callers reject an
// empty value themselves, with their own message, before calling this).
// `report(kind, value)` must throw std::invalid_argument with the caller's
// own wording for `kind` and must not return.
template <typename Report>
double parse_range_weight(const std::string& value, Report&& report) {
    double weight = 0.0;
    switch (parse_weight_literal(value, weight)) {
        case WeightLiteral::Ok:
            break;
        case WeightLiteral::NotANumber:
            report(WeightRangeError::NotANumber, value);
            break;
        case WeightLiteral::TrailingCharacters:
            report(WeightRangeError::TrailingCharacters, value);
            break;
        case WeightLiteral::OutOfRange:
            report(WeightRangeError::OutOfRange, value);
            break;
    }
    // isfinite is belt-and-braces: the grammar behind parse_weight_literal has
    // no "inf" / "nan" spelling, so weight can never be non-finite here, but
    // NaN compares false against both bounds and would otherwise slip through
    // the (0.0, 1.0] test undetected.
    if (!std::isfinite(weight) || weight <= 0.0 || weight > 1.0) {
        report(WeightRangeError::OutOfBounds, value);
    }
    return weight;
}

// Human-readable game name for diagnostics; shared by Range's set-algebra
// error messages (range_setops.cpp) and calculate_range_equity's
// hero/villain game-mismatch message (equity_range.cpp).
inline const char* game_name(GameType game) {
    // Message text is duplicated in binding/core_common.h:game_name; keep both in sync (grep binding/ before editing).
    return game == GameType::Plo ? "PLO" : "Hold'em";
}

// Spells a card mask as concatenated card names ("AsKsQhJd"). Bits outside
// the 52-card deck cannot be spelled; fall back to a "#<decimal>" rendering
// so error paths never throw while building their own message.
inline std::string describe_mask(std::uint64_t mask) {
    if ((mask & ~FULL_DECK_MASK) != 0) {
        return "#" + std::to_string(mask);
    }
    std::string out;
    std::uint64_t rest = mask;
    while (rest != 0) {
        const int id = ctz64(rest);
        rest &= rest - 1;
        out += Card(static_cast<Card::IdType>(id)).to_string();
    }
    return out;
}

} // namespace xiapl::internal
