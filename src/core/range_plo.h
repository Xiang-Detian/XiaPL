#pragma once

// Internal-only PLO range-notation parser. Not installed; consumers should
// use xiapl::Range::from_string(text, GameType::Plo) (include/xiapl/range.h).

#include <string_view>
#include <vector>

#include <xiapl/range.h>

namespace xiapl::internal {

// Parses the frozen v0.1 PLO range notation and returns the merged combo
// list, sorted ascending by mask, every mask popcount 4 and every weight in
// (0.0, 1.0]. Throws std::invalid_argument on malformed input.
std::vector<Combo> parse_plo_range(std::string_view text);

} // namespace xiapl::internal
