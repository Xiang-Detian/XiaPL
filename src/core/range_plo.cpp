#include "range_plo.h"

#include <xiapl/card.h>
#include <xiapl/utils.h>
#include "range_parse.h"
#include "plo_pattern.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// PLO range-notation parser (frozen v0.1 grammar):
//
//   range       := item ("," item)*
//   item        := body [":" weight]                 weight in (0.0, 1.0]
//   body        := exact | progression | pattern
//   exact       := four card spellings, any order    "AsKsQhJd"
//   pattern     := four rank symbols ("2".."9TJQKA" or "*"), any order,
//                  optionally followed by "ds" | "ss" | "r"
//   progression := pair form "RR**" or rundown form (four consecutive
//                  descending ranks), with a trailing "+" / "-" (shift until
//                  a rank would leave [2,14]) or an infix "-" (closed span
//                  between two endpoints of the same form and suffix)
//
// Whitespace is allowed around ',' and ':' only. Items are expanded to 4-card
// masks and unioned: a mask reached twice with the same PARSED weight merges
// silently, a mask reached with two different weights is an error naming both
// items. The result is sorted ascending by mask.
//
// This TU owns all user-facing validation: src/core/plo_pattern.h only
// asserts its preconditions (ranks in {0} u [2,14], popcount-4 in-deck masks)
// and those asserts vanish under NDEBUG. Nothing here reaches the engine
// before the input has been checked. The Hold'em parser (range_holdem.cpp,
// range_parse.h) is a separate grammar; only trim, the rank/suit character
// predicates, and the weight-literal helpers are shared (range_parse.h).

namespace xiapl::internal {
namespace {

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// Teaching guidance fragments. Every reserved-token diagnostic ends with the
// fragment for its class so the message says what to write instead; the
// substrings are pinned by tests/test_plo_parser.cpp.
constexpr const char* kSyntaxSummary =
    "a PLO item is 4 rank symbols ('2'-'9', 'T', 'J', 'Q', 'K', 'A' or '*') "
    "with an optional \"ds\" / \"ss\" / \"r\" suit suffix, an exact 4-card "
    "hand (e.g. \"AsAdKhKc\"), or a progression (e.g. \"JJ**+\", "
    "\"JT98-8765\")";

constexpr const char* kPadHint =
    "a rank pattern is exactly 4 symbols; pad the unknown cards with '*' "
    "(e.g. \"AA\" -> \"AA**\")";

// Same rule, opposite advice: padding is nonsense when there are already too
// many symbols. The "AA**" example is kept so both halves teach the shape.
constexpr const char* kTooManyHint =
    "a PLO pattern has exactly 4 rank symbols, one per card (e.g. \"AA**\", "
    "\"AKQJ\"); drop the extra symbols";

constexpr const char* kSuitHint =
    "suits cannot be written inside a rank pattern; use a \"ds\" / \"ss\" / "
    "\"r\" suffix for the suit shape, or an exact 4-card hand (e.g. "
    "\"AsAdKhKc\") to name specific suits";

constexpr const char* kWildcardHint =
    "'x' is not part of the v0.1 notation; write '*' for an unknown card, or "
    "a \"ds\" / \"ss\" / \"r\" suffix to constrain suits";

constexpr const char* kPercentileHint =
    "'%' percentile ranges are not supported in v0.1 (there is no ranking "
    "table); spell the hands out as patterns instead";

// Kept verbatim in every ':' diagnostic, including the ones where the weight
// does parse as a number but falls outside (0.0, 1.0]: the frozen spec asks
// for this guidance whenever ':' is not followed by a valid weight.
constexpr const char* kWeightHint =
    "':' here is a weight separator; boolean AND is not supported in v0.1 "
    "(write e.g. \"AA**:0.5\", weight in (0.0, 1.0])";

constexpr const char* kProgressionHint =
    "progressions are defined for pair patterns (\"JJ**\") and 4-card "
    "rundowns (\"JT98\") only";

// Renders one input byte for a diagnostic: printable ASCII (0x20-0x7E)
// verbatim, every other byte as "\xNN".
//
// Diagnostics quote the user's input back, and the input is raw bytes. A
// non-ASCII byte copied through (an en-dash pasted from a document, a stray
// 0x80) makes what() invalid UTF-8, and pybind11's PyErr_SetString then
// raises UnicodeDecodeError instead of the ValueError carrying this message:
// the teaching error is lost exactly when a beginner needs it. Escaping keeps
// every message pure ASCII, hence always representable.
std::string escape_byte(char c) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte >= 0x20 && byte <= 0x7E) return std::string(1, c);
    static const char kHex[] = "0123456789ABCDEF";
    std::string out = "\\x";
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0F]);
    return out;
}

// Quoted single character, for "unexpected character 'z'" diagnostics.
std::string describe_char(char c) { return "'" + escape_byte(c) + "'"; }

std::string escape_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) out += escape_byte(c);
    return out;
}

// Both throw sites escape the WHOLE composed message rather than each
// interpolation: the fixed text is printable ASCII, so escaping is the
// identity on it, and every user-supplied fragment (the echoed item, a
// pattern core, a weight literal) is covered by construction.
[[noreturn]] void item_error(const std::string& item, const std::string& detail) {
    throw std::invalid_argument(
        escape_text("PLO range item \"" + item + "\": " + detail));
}

[[noreturn]] void range_error(const std::string& detail) {
    throw std::invalid_argument(escape_text("PLO range: " + detail));
}

// Round-trip rendering of a parsed weight for the conflict message. Weights
// are compared for EXACT equality, so the message must print enough digits to
// distinguish any two values that compare unequal; the default 6 significant
// digits would render 0.1234567 and 0.1234568 identically and turn the
// diagnostic into a puzzle. defaultfloat still drops trailing zeros, so
// 0.5 -> "0.5" and 1.0 -> "1" while 0.3 -> "0.29999999999999999".
std::string format_weight(double weight) {
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << weight;
    return out.str();
}

// ---------------------------------------------------------------------------
// Character classes
// ---------------------------------------------------------------------------

// A rank SYMBOL is the shared rank character vocabulary (range_parse.h) plus
// the '*' wildcard, which only this (PLO) parser accepts.
bool is_rank_symbol(char c) {
    return c == '*' || is_rank_char(c);
}

// Precondition: is_rank_symbol(c). Returns 0 for the wildcard and the rank
// value in [2,14] otherwise, which is exactly make_plo_pattern's domain.
std::uint8_t rank_symbol_value(char c) {
    if (c == '*') return 0;
    return static_cast<std::uint8_t>(*try_rank_from_char(c));
}

// ASCII-only case folding: std::tolower's answer depends on the process
// locale, and the notation is defined in ASCII.
char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string ascii_lower(std::string text) {
    for (char& c : text) c = ascii_lower(c);
    return text;
}

// ---------------------------------------------------------------------------
// Item lexing
// ---------------------------------------------------------------------------

struct Item {
    std::string text;    // the item exactly as written (trimmed): diagnostics
    std::string body;    // notation part, weight stripped
    double weight = 1.0; // an omitted weight is exactly 1.0
};

std::vector<std::string> split_items(std::string_view text) {
    std::vector<std::string> items;
    std::size_t pos = 0;
    while (true) {
        const std::size_t comma = text.find(',', pos);
        const std::string_view raw = comma == std::string_view::npos
                                         ? text.substr(pos)
                                         : text.substr(pos, comma - pos);
        std::string item = trim(raw);
        if (item.empty()) {
            range_error("item " + std::to_string(items.size() + 1) +
                        " is empty; items are separated by ',' and every item "
                        "must hold a hand, pattern or progression");
        }
        items.push_back(std::move(item));
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return items;
}

double parse_weight(const std::string& item, std::string_view weight_text) {
    const std::string value = trim(weight_text);
    if (value.empty()) {
        item_error(item, std::string("the weight after ':' is empty; ") + kWeightHint);
    }

    // Locale-independent, and with a grammar narrower than strtod's -- see
    // parse_weight_literal (shared with the Hold'em parser). std::stod, which
    // this used to call, rejects "0.5" outright under a comma-decimal global
    // locale and accepts "0x1" as 1.0.
    return parse_range_weight(value, [&item](WeightRangeError kind, const std::string& v) {
        switch (kind) {
            case WeightRangeError::NotANumber:
                item_error(item, "\"" + v + "\" is not a number; " + kWeightHint);
            case WeightRangeError::TrailingCharacters:
                item_error(item, "weight \"" + v + "\" has trailing characters; " + kWeightHint);
            case WeightRangeError::OutOfRange:
                // Reported as invalid_argument: an unrepresentable literal is
                // malformed input, not a lookup failure, and callers only expect
                // invalid_argument from this parser.
                item_error(item, "weight \"" + v + "\" is out of the representable "
                                                   "range; " +
                                     kWeightHint);
            case WeightRangeError::OutOfBounds:
                // isfinite is belt-and-braces now that the grammar has no
                // "inf" / "nan" spelling: NaN compares false against every
                // bound, so the range test alone would not stop one if it
                // ever got this far.
                item_error(item, "weight \"" + v + "\" is not in (0.0, 1.0]; " + kWeightHint);
        }
    });
}

void reject_reserved_characters(const std::string& item, const std::string& body) {
    for (char c : body) {
        if (c == '%') {
            item_error(item, kPercentileHint);
        }
        if (c == 'x' || c == 'X') {
            item_error(item, kWildcardHint);
        }
        if (c == '!' || c == '$' || c == '@' || c == '(' || c == ')') {
            item_error(item, std::string("'") + c +
                                 "' is reserved for future syntax and is not "
                                 "part of v0.1; " +
                                 kSyntaxSummary);
        }
    }
}

Item parse_item(const std::string& raw) {
    Item item;
    item.text = raw;

    const std::size_t colon = raw.find(':');
    const std::string_view view(raw);
    item.body = colon == std::string::npos ? raw : trim(view.substr(0, colon));

    if (item.body.empty()) {
        item_error(raw, std::string("the notation is empty; ") + kSyntaxSummary);
    }
    for (char c : item.body) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            item_error(raw, "whitespace is allowed around ',' and ':' only");
        }
    }
    reject_reserved_characters(raw, item.body);

    if (colon != std::string::npos) {
        item.weight = parse_weight(raw, view.substr(colon + 1));
    }
    return item;
}

// ---------------------------------------------------------------------------
// Bodies
// ---------------------------------------------------------------------------

bool looks_like_exact_hand(const std::string& body) {
    if (body.size() != 8) return false;
    for (std::size_t i = 0; i < 8; i += 2) {
        if (!try_parse_card(std::string_view(body).substr(i, 2))) return false;
    }
    return true;
}

std::uint64_t parse_exact_hand(const std::string& item, const std::string& body) {
    std::uint64_t mask = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const Card card = *try_parse_card(std::string_view(body).substr(i * 2, 2));
        const std::uint64_t bit = std::uint64_t{1} << card.id;
        if ((mask & bit) != 0) {
            item_error(item, "duplicate card \"" + card.to_string() +
                                 "\"; an exact PLO hand is 4 distinct cards");
        }
        mask |= bit;
    }
    return mask;
}

PloPattern parse_pattern(const std::string& item, const std::string& body) {
    std::string core = body;
    SuitQualifier suit = SuitQualifier::Any;

    // A suit suffix is recognized only when exactly 4 rank symbols precede
    // it. That is what turns "AAss" into the suit-letter diagnostic the spec
    // asks for (two symbols plus "ss" is not a valid suffix, so the 's' is
    // read as a stray suit letter) while "AAKKss" is the qualifier.
    //
    // Suffixes are case-INSENSITIVE, like every other letter in the notation:
    // rank symbols and card spellings both accept either case, so "AAKKDS"
    // must mean what "AAKKds" means (it used to be a suit-letter error).
    const auto four_symbols_then = [&body](std::size_t suffix_len) {
        if (body.size() != 4 + suffix_len) return false;
        for (std::size_t i = 0; i < 4; ++i) {
            if (!is_rank_symbol(body[i])) return false;
        }
        return true;
    };
    const auto suffix_is = [&body, &four_symbols_then](const char* lowercase) {
        const std::size_t len = std::char_traits<char>::length(lowercase);
        if (!four_symbols_then(len)) return false;
        for (std::size_t i = 0; i < len; ++i) {
            if (ascii_lower(body[4 + i]) != lowercase[i]) return false;
        }
        return true;
    };
    if (suffix_is("ds")) {
        suit = SuitQualifier::DoubleSuited;
        core = body.substr(0, 4);
    } else if (suffix_is("ss")) {
        suit = SuitQualifier::SingleSuited;
        core = body.substr(0, 4);
    } else if (suffix_is("r")) {
        suit = SuitQualifier::Rainbow;
        core = body.substr(0, 4);
    }

    // A bare suffix on its own ("r", "ds", "ss") is a suffix that lost its
    // pattern, not a hand; say so instead of falling through to the
    // unexpected-character / suit-letter diagnostics. Case-insensitive for
    // the same reason the suffix itself is; the message quotes the item as
    // the caller wrote it.
    const std::string core_lower = ascii_lower(core);
    if (core_lower == "r" || core_lower == "ds" || core_lower == "ss") {
        item_error(item, "\"" + core +
                             "\" is a suit suffix, not a hand; it must follow "
                             "4 rank symbols (e.g. \"AKQJ" +
                             core + "\")");
    }

    for (char c : core) {
        if (is_rank_symbol(c)) continue;
        if (is_suit_char(c)) {
            item_error(item, std::string("unexpected suit letter '") + c + "'; " + kSuitHint);
        }
        item_error(item, "unexpected character " + describe_char(c) + "; " + kSyntaxSummary);
    }
    if (core.size() < 4) {
        item_error(item, "\"" + core + "\" has " + std::to_string(core.size()) +
                             " rank symbol(s); " + kPadHint);
    }
    if (core.size() > 4) {
        item_error(item, "\"" + core + "\" has " + std::to_string(core.size()) +
                             " rank symbols; " + kTooManyHint);
    }

    std::array<std::uint8_t, 4> ranks{};
    for (std::size_t i = 0; i < 4; ++i) {
        ranks[i] = rank_symbol_value(core[i]);  // 0 or 2..14, the factory's domain
    }
    return make_plo_pattern(ranks, suit);
}

// ---------------------------------------------------------------------------
// Progressions
// ---------------------------------------------------------------------------

enum class ProgressionForm { None, Pair, Rundown };

// Decided on the CANONICAL pattern (descending, wildcards last), so the
// written order of a progression endpoint is as free as a plain pattern's:
// "*JJ*+" is the same progression as "JJ**+".
ProgressionForm progression_form(const PloPattern& p) {
    const std::array<std::uint8_t, 4>& r = p.ranks;
    if (r[0] != 0 && r[0] == r[1] && r[2] == 0 && r[3] == 0) {
        return ProgressionForm::Pair;
    }
    if (r[3] != 0 && r[1] + 1 == r[0] && r[2] + 2 == r[0] && r[3] + 3 == r[0]) {
        return ProgressionForm::Rundown;
    }
    return ProgressionForm::None;
}

const char* form_name(ProgressionForm form) {
    switch (form) {
        case ProgressionForm::Pair: return "a pair pattern (\"RR**\")";
        case ProgressionForm::Rundown: return "a rundown (\"JT98\")";
        case ProgressionForm::None: break;
    }
    return "none";
}

// Uniform shift of every literal rank; nullopt as soon as one would leave
// [2,14] (there is no ace-low wrap).
std::optional<PloPattern> shift_pattern(const PloPattern& p, int delta) {
    std::array<std::uint8_t, 4> ranks{};
    for (std::size_t i = 0; i < 4; ++i) {
        if (p.ranks[i] == 0) continue;  // wildcards do not move
        const int shifted = static_cast<int>(p.ranks[i]) + delta;
        if (shifted < 2 || shifted > 14) return std::nullopt;
        ranks[i] = static_cast<std::uint8_t>(shifted);
    }
    return make_plo_pattern(ranks, p.suit);
}

[[noreturn]] void degenerate_error(const std::string& item, const std::string& base) {
    item_error(item, "the progression expands to the single pattern \"" + base +
                         "\"; write the pattern on its own instead of using "
                         "'+' / '-'");
}

std::vector<PloPattern> expand_open_progression(const std::string& item,
                                                const std::string& base,
                                                int step) {
    if (base.empty()) {
        item_error(item, std::string("a progression needs a pattern before '") +
                             (step > 0 ? '+' : '-') + "'; " + kProgressionHint);
    }
    const PloPattern start = parse_pattern(item, base);
    if (progression_form(start) == ProgressionForm::None) {
        item_error(item, "\"" + base + "\" cannot carry a progression marker; " +
                             kProgressionHint);
    }

    std::vector<PloPattern> out;
    for (int delta = 0;; delta += step) {
        std::optional<PloPattern> shifted = shift_pattern(start, delta);
        if (!shifted) break;
        out.push_back(*shifted);
    }
    if (out.size() == 1) degenerate_error(item, base);
    return out;
}

std::vector<PloPattern> expand_span_progression(const std::string& item,
                                                const std::string& left_text,
                                                const std::string& right_text) {
    if (left_text.empty() || right_text.empty()) {
        item_error(item, "a '-' span needs a pattern on both sides (e.g. "
                         "\"99**-66**\"); " +
                             std::string(kProgressionHint));
    }
    if (right_text.find('-') != std::string::npos) {
        item_error(item, "a '-' span has exactly two endpoints; " +
                             std::string(kProgressionHint));
    }
    if (left_text.find('+') != std::string::npos ||
        right_text.find('+') != std::string::npos) {
        item_error(item, "a '-' span cannot also carry '+'; " +
                             std::string(kProgressionHint));
    }

    const PloPattern left = parse_pattern(item, left_text);
    const PloPattern right = parse_pattern(item, right_text);
    const ProgressionForm left_form = progression_form(left);
    const ProgressionForm right_form = progression_form(right);
    if (left_form == ProgressionForm::None || right_form == ProgressionForm::None) {
        item_error(item, "\"" + left_text + "-" + right_text +
                             "\" is not a progression; " + kProgressionHint);
    }
    if (left_form != right_form) {
        item_error(item, "span endpoints must have the same form, but \"" +
                             left_text + "\" is " + form_name(left_form) +
                             " while \"" + right_text + "\" is " +
                             form_name(right_form));
    }
    if (left.suit != right.suit) {
        item_error(item, "span endpoints must carry the same suit suffix "
                         "(\"ds\" / \"ss\" / \"r\" or none on both sides)");
    }

    const int delta = static_cast<int>(right.ranks[0]) - static_cast<int>(left.ranks[0]);
    if (delta > 0) {
        item_error(item, "a '-' span is written from the higher endpoint down "
                         "(e.g. \"99**-66**\", \"JT98-8765\")");
    }
    std::optional<PloPattern> reached = shift_pattern(left, delta);
    if (!reached || reached->ranks != right.ranks) {
        item_error(item, "span endpoints are not a uniform rank shift of each "
                         "other; " +
                             std::string(kProgressionHint));
    }

    std::vector<PloPattern> out;
    for (int d = 0; d >= delta; --d) {
        std::optional<PloPattern> shifted = shift_pattern(left, d);
        // Every intermediate rank lies between the two endpoints' ranks, and
        // both endpoints are in [2,14], so this never fails.
        assert(shifted.has_value());
        out.push_back(*shifted);
    }
    if (out.size() == 1) degenerate_error(item, left_text);
    return out;
}

// ---------------------------------------------------------------------------
// Item expansion
// ---------------------------------------------------------------------------

// May contain duplicates when a progression's steps overlap (pair
// progressions do: "JJ**+" holds JJQQ twice). The caller's merge map removes
// them, and they always carry the same weight, so they never conflict.
std::vector<std::uint64_t> expand_item(const Item& item) {
    const std::string& body = item.body;
    if (looks_like_exact_hand(body)) {
        return {parse_exact_hand(item.text, body)};
    }

    std::vector<PloPattern> patterns;
    if (body.back() == '+') {
        patterns = expand_open_progression(item.text, body.substr(0, body.size() - 1), +1);
    } else if (body.back() == '-') {
        patterns = expand_open_progression(item.text, body.substr(0, body.size() - 1), -1);
    } else {
        const std::size_t dash = body.find('-');
        if (dash != std::string::npos) {
            patterns = expand_span_progression(item.text, body.substr(0, dash),
                                               body.substr(dash + 1));
        } else {
            patterns.push_back(parse_pattern(item.text, body));
        }
    }

    std::vector<std::uint64_t> masks;
    for (const PloPattern& pattern : patterns) {
        const std::vector<std::uint64_t> one = expand_plo_pattern(pattern);
        masks.insert(masks.end(), one.begin(), one.end());
    }
    return masks;
}

}  // namespace

std::vector<Combo> parse_plo_range(std::string_view text) {
    // Lex and validate every item before expanding any of them: a syntax
    // error in the last item should not cost a full expansion of the first.
    const std::vector<std::string> raw_items = split_items(text);
    std::vector<Item> items;
    items.reserve(raw_items.size());
    for (const std::string& raw : raw_items) items.push_back(parse_item(raw));

    // Per-mask weight map. `item` records which item first claimed the mask so
    // a conflict can name both spellings.
    struct Entry {
        double weight;
        std::size_t item;
    };
    std::unordered_map<std::uint64_t, Entry> merged;

    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& item = items[i];
        for (std::uint64_t mask : expand_item(item)) {
            const auto inserted = merged.emplace(mask, Entry{item.weight, i});
            if (inserted.second) continue;
            // Exact comparison of the PARSED values, no epsilon: "0.5" and
            // ".50" merge, an omitted weight is exactly 1.0.
            if (inserted.first->second.weight == item.weight) continue;
            const Item& first = items[inserted.first->second.item];
            range_error("conflicting weights for combo " + internal::describe_mask(mask) +
                        ": item \"" + first.text + "\" gives weight " +
                        format_weight(first.weight) + " but item \"" + item.text +
                        "\" gives weight " + format_weight(item.weight) +
                        " (equal weights merge silently; conflicting ones are "
                        "an error)");
        }
    }

    std::vector<Combo> combos;
    combos.reserve(merged.size());
    for (const auto& entry : merged) {
        combos.push_back(Combo{entry.first, entry.second.weight});
    }
    std::sort(combos.begin(), combos.end(),
              [](const Combo& a, const Combo& b) { return a.mask < b.mask; });
    return combos;
}

} // namespace xiapl::internal
