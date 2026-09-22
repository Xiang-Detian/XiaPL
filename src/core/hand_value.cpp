#include <xiapl/hand_value.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace xiapl {

namespace {

int category_rank(HandCategory category) {
    return static_cast<int>(category);
}

bool is_valid_category(HandCategory category) {
    switch (category) {
    case HandCategory::HighCard:
    case HandCategory::OnePair:
    case HandCategory::TwoPair:
    case HandCategory::ThreeOfAKind:
    case HandCategory::Straight:
    case HandCategory::Flush:
    case HandCategory::FullHouse:
    case HandCategory::FourOfAKind:
    case HandCategory::StraightFlush:
        return true;
    }
    return false;
}

} // namespace

bool operator==(const HandValue& lhs, const HandValue& rhs) {
    return lhs.category == rhs.category &&
           lhs.kicker_count == rhs.kicker_count &&
           lhs.kickers == rhs.kickers;
}

bool operator!=(const HandValue& lhs, const HandValue& rhs) {
    return !(lhs == rhs);
}

bool operator<(const HandValue& lhs, const HandValue& rhs) {
    if (lhs.category != rhs.category) {
        return category_rank(lhs.category) < category_rank(rhs.category);
    }
    if (lhs.kickers != rhs.kickers) {
        return std::lexicographical_compare(
            lhs.kickers.begin(), lhs.kickers.end(),
            rhs.kickers.begin(), rhs.kickers.end()
        );
    }
    return lhs.kicker_count < rhs.kicker_count;
}

bool operator>(const HandValue& lhs, const HandValue& rhs) {
    return rhs < lhs;
}

bool operator<=(const HandValue& lhs, const HandValue& rhs) {
    return !(rhs < lhs);
}

bool operator>=(const HandValue& lhs, const HandValue& rhs) {
    return !(lhs < rhs);
}

std::string to_string(HandCategory category) {
    switch (category) {
    case HandCategory::HighCard:       return "High Card";
    case HandCategory::OnePair:        return "One Pair";
    case HandCategory::TwoPair:        return "Two Pair";
    case HandCategory::ThreeOfAKind:   return "Three of a Kind";
    case HandCategory::Straight:       return "Straight";
    case HandCategory::Flush:          return "Flush";
    case HandCategory::FullHouse:      return "Full House";
    case HandCategory::FourOfAKind:    return "Four of a Kind";
    case HandCategory::StraightFlush:  return "Straight Flush";
    }
    return "Unknown";
}

std::string describe_hand(const HandValue& value) {
    std::ostringstream out;
    out << to_string(value.category);
    if (value.kicker_count > 0) {
        out << " [";
        for (std::uint8_t i = 0; i < value.kicker_count; ++i) {
            if (i > 0) {
                out << ' ';
            }
            out << static_cast<int>(value.kickers[i]);
        }
        out << ']';
    }
    return out.str();
}

HandValue make_hand_value(HandCategory category, const std::vector<int>& kickers) {
    if (!is_valid_category(category)) {
        throw std::runtime_error("make_hand_value: invalid hand category");
    }
    if (kickers.size() > 5) {
        throw std::runtime_error("make_hand_value: too many kickers");
    }

    HandValue value;
    value.category = category;
    value.kicker_count = static_cast<std::uint8_t>(kickers.size());
    for (std::size_t i = 0; i < kickers.size(); ++i) {
        if (kickers[i] < 0 || kickers[i] > 14) {
            throw std::runtime_error("make_hand_value: invalid kicker");
        }
        value.kickers[i] = static_cast<std::uint8_t>(kickers[i]);
    }
    return value;
}

} // namespace xiapl
