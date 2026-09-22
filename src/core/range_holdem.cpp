#include "range_parse.h"

#include <xiapl/canonicalize.h>
#include <xiapl/card.h>
#include <xiapl/deck.h>
#include <xiapl/utils.h>

#include <algorithm>
#include <cctype>
#include <ios>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace xiapl::internal {

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

namespace {

// Locale-free digit test: std::isdigit's answer depends on the process
// locale, and this parser must not.
bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

// Length of the longest prefix of `text` that is a weight literal in the
// grammar documented on parse_weight_literal. 0 means "not a number at all".
std::size_t decimal_literal_prefix(const std::string& text) {
    std::size_t i = 0;
    std::size_t mantissa_digits = 0;
    while (i < text.size() && is_ascii_digit(text[i])) {
        ++i;
        ++mantissa_digits;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && is_ascii_digit(text[i])) {
            ++i;
            ++mantissa_digits;
        }
    }
    if (mantissa_digits == 0) return 0;  // "", ".", "+0.5", "abc", "nan"
    const std::size_t mantissa_end = i;

    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < text.size() && (text[j] == '+' || text[j] == '-')) ++j;
        std::size_t exponent_digits = 0;
        while (j < text.size() && is_ascii_digit(text[j])) {
            ++j;
            ++exponent_digits;
        }
        if (exponent_digits > 0) return j;
    }
    // A dangling "e" is trailing junk, not part of the literal ("1e" -> 1).
    return mantissa_end;
}

}  // namespace

WeightLiteral parse_weight_literal(const std::string& text, double& out) {
    const std::size_t end = decimal_literal_prefix(text);
    if (end == 0) return WeightLiteral::NotANumber;
    if (end != text.size()) return WeightLiteral::TrailingCharacters;

    // std::stod (the previous implementation) goes through strtod, which
    // honours the process-global C locale: under a comma-decimal locale
    // (de_DE, fr_FR) "0.5" stops at the '.' and EVERY weighted item is
    // rejected. An istringstream imbued with std::locale::classic() pins '.'
    // as the decimal separator whatever the embedding program set globally.
    // (std::from_chars<double> would be the leaner tool but is not reliably
    // available on AppleClang 17.)
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    double value = 0.0;
    in >> value;
    // The prefix scan already proved the whole token is a literal, so a
    // failure here means the value itself is unrepresentable ("1e999").
    if (in.fail()) return WeightLiteral::OutOfRange;
    if (!in.eof()) return WeightLiteral::TrailingCharacters;  // unreachable
    out = value;
    return WeightLiteral::Ok;
}

namespace {

int char_to_rank(char c) {
    if (auto v = try_rank_from_char(c)) return *v;
    throw std::invalid_argument("invalid rank char");
}

using internal::trim;

struct BasicHandPattern {
    int hi_rank;
    int lo_rank;
    bool is_pair;
    char type;      // 's', 'o', or 0 (both)
};

BasicHandPattern parse_basic_hand_notation(const std::string& notation) {
    if (notation.size() < 2) {
        throw std::invalid_argument("notation too short for starting hand: " + notation);
    }

    char c1 = notation[0];
    char c2 = notation[1];
    int r1 = char_to_rank(c1);
    int r2 = char_to_rank(c2);

    int hi_rank = r1;
    int lo_rank = r2;
    if (r1 < r2) {
        hi_rank = r2;
        lo_rank = r1;
    }

    bool is_pair = (hi_rank == lo_rank);
    char type = 0;

    if (!is_pair) {
        if (notation.size() >= 3) {
            char t = notation[2];
            if (t == 's' || t == 'S') {
                type = 's';
            } else if (t == 'o' || t == 'O') {
                type = 'o';
            } else {
                throw std::invalid_argument("non-pair starting hand must end with 's' or 'o': " + notation);
            }
        } else {
            type = 0;
        }
    }
    return BasicHandPattern{hi_rank, lo_rank, is_pair, type};
}

std::vector<std::string> expand_range_token_to_notations(const std::string& tok) {
    std::vector<std::string> notations;

    auto dash_pos = tok.find('-');
    if (dash_pos != std::string::npos) {
        std::string left = trim(tok.substr(0, dash_pos));
        std::string right = trim(tok.substr(dash_pos + 1));

        if (left.empty() || right.empty()) {
            throw std::invalid_argument("invalid '-' range token: " + tok);
        }
        if (left.find('+') != std::string::npos || right.find('+') != std::string::npos) {
            throw std::invalid_argument("'-' range must not contain '+': " + tok);
        }

        BasicHandPattern L = parse_basic_hand_notation(left);
        BasicHandPattern R = parse_basic_hand_notation(right);

        if (L.is_pair != R.is_pair) {
            throw std::invalid_argument("'-' range must be both pair or both non-pair: " + tok);
        }

        if (L.is_pair) {
            int hi = L.hi_rank;
            int lo = R.hi_rank;
            if (hi < lo) std::swap(hi, lo);

            for (int r = hi; r >= lo; --r) {
                char rc = rank_to_char(r);
                std::string n;
                n.push_back(rc);
                n.push_back(rc);
                notations.push_back(std::move(n));
            }
        } else {
            if (L.type != R.type) {
                throw std::invalid_argument("'-' range non-pair must have same suitedness (s/o): " + tok);
            }
            char type = L.type;
            if (type != 's' && type != 'o') {
                throw std::invalid_argument("'-' range non-pair must specify 's' or 'o': " + tok);
            }

            int hi1 = L.hi_rank, lo1 = L.lo_rank;
            int hi2 = R.hi_rank, lo2 = R.lo_rank;

            if (hi1 < hi2) {
                std::swap(hi1, hi2);
                std::swap(lo1, lo2);
            }

            if (hi1 == hi2) {
                int fixed_hi = hi1;
                int lo_start = lo1;
                int lo_end = lo2;
                if (lo_start < lo_end) std::swap(lo_start, lo_end);

                for (int lr = lo_start; lr >= lo_end; --lr) {
                    std::string n;
                    n.push_back(rank_to_char(fixed_hi));
                    n.push_back(rank_to_char(lr));
                    n.push_back(type);
                    notations.push_back(std::move(n));
                }
            } else {
                int gap1 = hi1 - lo1;
                int gap2 = hi2 - lo2;
                if (gap1 != gap2) {
                    throw std::invalid_argument("'-' range for connectors must preserve gap: " + tok);
                }
                int gap = gap1;
                for (int hr = hi1; hr >= hi2; --hr) {
                    int lr = hr - gap;
                    std::string n;
                    n.push_back(rank_to_char(hr));
                    n.push_back(rank_to_char(lr));
                    n.push_back(type);
                    notations.push_back(std::move(n));
                }
            }
        }
        return notations;
    }

    bool has_plus = (!tok.empty() && tok.back() == '+');
    std::string core = tok;
    if (has_plus) {
        core.pop_back();
    }

    BasicHandPattern P = parse_basic_hand_notation(core);

    if (P.is_pair) {
        if (!has_plus) {
            std::string n;
            char rc = rank_to_char(P.hi_rank);
            n.push_back(rc);
            n.push_back(rc);
            notations.push_back(std::move(n));
        } else {
            for (int r = P.hi_rank; r <= 14; ++r) {
                char rc = rank_to_char(r);
                std::string n;
                n.push_back(rc);
                n.push_back(rc);
                notations.push_back(std::move(n));
            }
        }
    } else {
        int hi = P.hi_rank;
        char hi_char = rank_to_char(hi);

        if (!has_plus) {
            if (P.type == 0) {
                std::string n1, n2;
                n1.push_back(hi_char);
                n1.push_back(rank_to_char(P.lo_rank));
                n1.push_back('s');
                n2.push_back(hi_char);
                n2.push_back(rank_to_char(P.lo_rank));
                n2.push_back('o');
                notations.push_back(std::move(n1));
                notations.push_back(std::move(n2));
            } else {
                std::string n;
                n.push_back(hi_char);
                n.push_back(rank_to_char(P.lo_rank));
                n.push_back(P.type);
                notations.push_back(std::move(n));
            }
        } else {
            if (P.type == 0) {
                if (hi != 14) {
                    throw std::invalid_argument("unsupported unsuffixed '+' range (only AK+ is supported): " + tok);
                }
                char lo_char = rank_to_char(P.lo_rank);
                {
                    std::string n;
                    n.push_back(hi_char);
                    n.push_back(lo_char);
                    n.push_back('s');
                    notations.push_back(std::move(n));
                }
                {
                    std::string n;
                    n.push_back(hi_char);
                    n.push_back(lo_char);
                    n.push_back('o');
                    notations.push_back(std::move(n));
                }
            } else {
                char type = P.type;

                if (hi == 14) {
                    for (int lr = P.lo_rank; lr < hi; ++lr) {
                        std::string n;
                        n.push_back(hi_char);
                        n.push_back(rank_to_char(lr));
                        n.push_back(type);
                        notations.push_back(std::move(n));
                    }
                }
                else if ((type == 's' || type == 'o') &&
                         (hi - P.lo_rank) == 1) {
                    // Connector '+' ladder: JTs+ → {JTs, QJs, KQs, AKs};
                    // symmetric for offsuit: JTo+ → {JTo, QJo, KQo, AKo}.
                    int gap = 1;
                    for (int h = hi; h <= 14; ++h) {
                        int lr = h - gap;
                        if (lr < 2) {
                            continue;
                        }
                        std::string n;
                        n.push_back(rank_to_char(h));
                        n.push_back(rank_to_char(lr));
                        n.push_back(type);
                        notations.push_back(std::move(n));
                    }
                }
                else {
                    for (int lr = P.lo_rank; lr < hi; ++lr) {
                        std::string n;
                        n.push_back(hi_char);
                        n.push_back(rank_to_char(lr));
                        n.push_back(type);
                        notations.push_back(std::move(n));
                    }
                }
            }
        }
    }

    return notations;
}

std::vector<std::uint64_t> expand_notation_to_masks(const std::string& notation) {
    if (notation.size() < 2) {
        throw std::invalid_argument("notation too short for starting hand");
    }

    int r1 = char_to_rank(notation[0]);
    int r2 = char_to_rank(notation[1]);

    int hi_rank = r1;
    int lo_rank = r2;
    if (r1 < r2) {
        hi_rank = r2;
        lo_rank = r1;
    }

    bool is_pair = (hi_rank == lo_rank);

    char type = '\0';
    if (!is_pair) {
        if (notation.size() < 3) {
            throw std::invalid_argument("non-pair starting hand notation must end with 's' or 'o'");
        }
        type = notation[2];
        if (type != 's' && type != 'S' && type != 'o' && type != 'O') {
            throw std::invalid_argument("non-pair starting hand must end with 's' or 'o'");
        }
    }

    std::vector<std::uint64_t> masks;
    constexpr int NUM_SUITS = 4;

    auto card_id = [](int rank, int suit) {
        return suit * 13 + (rank - 2);
    };

    if (is_pair) {
        masks.reserve(6);
        for (int s1 = 0; s1 < NUM_SUITS; ++s1) {
            for (int s2 = s1 + 1; s2 < NUM_SUITS; ++s2) {
                std::uint64_t mask = (std::uint64_t{1} << card_id(hi_rank, s1)) |
                                     (std::uint64_t{1} << card_id(hi_rank, s2));
                masks.push_back(mask);
            }
        }
    } else if (type == 's' || type == 'S') {
        masks.reserve(4);
        for (int s = 0; s < NUM_SUITS; ++s) {
            std::uint64_t mask = (std::uint64_t{1} << card_id(hi_rank, s)) |
                                 (std::uint64_t{1} << card_id(lo_rank, s));
            masks.push_back(mask);
        }
    } else {
        masks.reserve(12);
        for (int s1 = 0; s1 < NUM_SUITS; ++s1) {
            for (int s2 = 0; s2 < NUM_SUITS; ++s2) {
                if (s1 == s2) continue;
                std::uint64_t mask = (std::uint64_t{1} << card_id(hi_rank, s1)) |
                                     (std::uint64_t{1} << card_id(lo_rank, s2));
                masks.push_back(mask);
            }
        }
    }

    return masks;
}

} // anonymous namespace

std::vector<std::uint64_t> expand_range_token_to_masks(const std::string& token) {
    std::vector<std::string> notations = expand_range_token_to_notations(token);
    std::set<std::uint64_t> masks_set;
    for (const auto& n : notations) {
        for (std::uint64_t m : expand_notation_to_masks(n)) {
            masks_set.insert(m);
        }
    }
    return std::vector<std::uint64_t>(masks_set.begin(), masks_set.end());
}

} // namespace xiapl::internal
