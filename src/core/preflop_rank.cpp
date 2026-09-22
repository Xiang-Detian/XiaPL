#include <xiapl/range.h>

#include <xiapl/game_type.h>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace xiapl {

namespace {

// preflop_rank_table.inc declares `struct PreflopRankEntry` and
// `kPreflopRankTable` unqualified with no include guard (by design, see
// apps/gen_preflop_rank.cpp / tests/test_preflop_rank_table.cpp). Nesting it
// inside this anonymous namespace keeps those names out of the global
// namespace of this public-API translation unit; the .inc has no #include
// directives of its own, so nesting it is safe.
#include "preflop_rank_table.inc"

constexpr std::size_t kTableSize =
    sizeof(kPreflopRankTable) / sizeof(kPreflopRankTable[0]);

void validate_top_percent(double top_percent, const char* fn_name) {
    if (!(top_percent > 0.0 && top_percent <= 1.0)) {
        throw std::invalid_argument(
            std::string(fn_name) + ": top_percent must be in (0.0, 1.0], got " +
            std::to_string(top_percent));
    }
}

// PLO percentile ranking is deferred to a future release (needs its own
// versioned table); named per-caller so the message identifies which
// function was called.
void require_holdem_ranking(GameType game, const char* fn_name) {
    if (game != GameType::Plo) return;
    throw std::invalid_argument(
        std::string(fn_name) +
        ": only Hold'em starting hands are ranked in xiapl 0.1 (PLO percentile "
        "selection needs a versioned ranking table, deferred to 0.2); pass "
        "game=GameType::Holdem or build the range with Range::from_string(text, "
        "GameType::Plo)");
}

} // namespace

std::vector<std::string> rank_starting_hands(double top_percent, GameType game) {
    require_holdem_ranking(game, "rank_starting_hands");
    validate_top_percent(top_percent, "rank_starting_hands");

    const std::size_t keep = static_cast<std::size_t>(
        static_cast<double>(kTableSize) * top_percent);

    std::vector<std::string> labels;
    labels.reserve(keep);
    for (std::size_t i = 0; i < keep; ++i) {
        labels.emplace_back(kPreflopRankTable[i].label);
    }
    return labels;
}

Range generate_top_percent_range(double top_percent, GameType game) {
    require_holdem_ranking(game, "generate_top_percent_range");
    validate_top_percent(top_percent, "generate_top_percent_range");

    const std::vector<std::string> labels = rank_starting_hands(top_percent, game);

    // Each label's combos are disjoint from every other label's (the 169
    // canonical labels partition the 1,326 combos), so a plain concatenation
    // never collides. rank_starting_hands orders labels by strength, not by
    // mask, so the concatenation is re-sorted below to match the
    // ascending-by-mask convention every other Range-producing entry point in
    // this header keeps (Range::all, the PLO parser, the set ops).
    std::vector<Combo> combos;
    for (const std::string& label : labels) {
        // Bind the Range to a name first: combos() returns a reference into
        // it, which would dangle if taken directly from the from_string(...)
        // temporary.
        const Range label_range = Range::from_string(label, game);
        const std::vector<Combo>& label_combos = label_range.combos();
        combos.insert(combos.end(), label_combos.begin(), label_combos.end());
    }
    std::sort(combos.begin(), combos.end(),
              [](const Combo& a, const Combo& b) { return a.mask < b.mask; });

    return Range(std::move(combos), game);
}

} // namespace xiapl
