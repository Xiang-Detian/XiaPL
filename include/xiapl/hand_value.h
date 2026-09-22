#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace xiapl {

enum class HandCategory : std::uint8_t {
    HighCard = 1,
    OnePair,
    TwoPair,
    ThreeOfAKind,
    Straight,
    Flush,
    FullHouse,
    FourOfAKind,
    StraightFlush
};

struct HandValue {
    HandCategory category = HandCategory::HighCard;
    std::array<std::uint8_t, 5> kickers{};
    std::uint8_t kicker_count = 0;
};

bool operator==(const HandValue& lhs, const HandValue& rhs);
bool operator!=(const HandValue& lhs, const HandValue& rhs);
bool operator<(const HandValue& lhs, const HandValue& rhs);
bool operator>(const HandValue& lhs, const HandValue& rhs);
bool operator<=(const HandValue& lhs, const HandValue& rhs);
bool operator>=(const HandValue& lhs, const HandValue& rhs);

std::string to_string(HandCategory category);
std::string describe_hand(const HandValue& value);

HandValue make_hand_value(HandCategory category, const std::vector<int>& kickers);

} // namespace xiapl
