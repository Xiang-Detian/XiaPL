#include <xiapl/range.h>

#include "range_parse.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

// Weighted set algebra over GameType-tagged ranges (range_union / range_
// intersection / range_difference). Split out of range.cpp: this never
// changes with the parser, and already has its own test file
// (tests/test_range_setops.cpp).

namespace xiapl {

namespace {

using internal::game_name;

// Game agreement is checked first, ahead of every other validation: a
// Hold'em range paired with a PLO one is a caller mistake whatever else is
// wrong with the call.
void require_same_game(const Range& a, const Range& b, const char* fn_name) {
    if (a.game() != b.game()) {
        // Message text is duplicated in binding/core_range.cpp:require_same_game; keep both in sync (grep binding/ before editing).
        throw std::invalid_argument(
            std::string(fn_name) + ": left range is " +
            game_name(a.game()) + " but right range is " +
            game_name(b.game()) + "; both ranges must be the same game");
    }
}

// Sorts `combos` ascending by mask in place and throws std::invalid_argument
// (naming the offending combo via describe_mask) if two combos share a
// mask. Only the raw-combo Range constructor can build such a duplicate --
// every parser path already rejects it -- and there is no honest answer for
// what a set operation should do with one.
void sort_and_check_duplicates(std::vector<Combo>& combos, const char* fn_name) {
    std::sort(combos.begin(), combos.end(),
               [](const Combo& lhs, const Combo& rhs) { return lhs.mask < rhs.mask; });
    for (std::size_t i = 1; i < combos.size(); ++i) {
        if (combos[i - 1].mask == combos[i].mask) {
            throw std::invalid_argument(
                std::string(fn_name) + ": duplicate combo " +
                internal::describe_mask(combos[i].mask) + " in operand");
        }
    }
}

// Linear merges below assume `a` and `b` are already mask-sorted and
// duplicate-free (sort_and_check_duplicates has run on both). Each drops
// results with weight <= 0.0, matching the (0.0, 1.0] weight contract used
// by the range-notation parsers.

std::vector<Combo> merge_union(const std::vector<Combo>& a, const std::vector<Combo>& b) {
    std::vector<Combo> result;
    result.reserve(a.size() + b.size());
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].mask < b[j].mask) {
            // Present only in `a`; union weight is max(wa, 0) == wa, so the
            // same weight <= 0.0 drop rule still applies (a raw-combo Range
            // can legally hold a non-positive weight).
            if (a[i].weight > 0.0) {
                result.push_back(a[i]);
            }
            ++i;
        } else if (b[j].mask < a[i].mask) {
            if (b[j].weight > 0.0) {
                result.push_back(b[j]);
            }
            ++j;
        } else {
            const double w = std::max(a[i].weight, b[j].weight);
            if (w > 0.0) {
                result.push_back(Combo{a[i].mask, w});
            }
            ++i;
            ++j;
        }
    }
    for (; i < a.size(); ++i) {
        if (a[i].weight > 0.0) {
            result.push_back(a[i]);
        }
    }
    for (; j < b.size(); ++j) {
        if (b[j].weight > 0.0) {
            result.push_back(b[j]);
        }
    }
    return result;
}

std::vector<Combo> merge_intersection(const std::vector<Combo>& a, const std::vector<Combo>& b) {
    std::vector<Combo> result;
    result.reserve(std::min(a.size(), b.size()));
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].mask < b[j].mask) {
            ++i;
        } else if (b[j].mask < a[i].mask) {
            ++j;
        } else {
            const double w = std::min(a[i].weight, b[j].weight);
            if (w > 0.0) {
                result.push_back(Combo{a[i].mask, w});
            }
            ++i;
            ++j;
        }
    }
    return result;
}

std::vector<Combo> merge_difference(const std::vector<Combo>& a, const std::vector<Combo>& b) {
    std::vector<Combo> result;
    result.reserve(a.size());
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].mask < b[j].mask) {
            // Present only in `a`, i.e. absent from the subtrahend; the
            // difference weight is max(0, wa - 0) == wa, so the same
            // weight <= 0.0 drop rule still applies.
            if (a[i].weight > 0.0) {
                result.push_back(a[i]);
            }
            ++i;
        } else if (b[j].mask < a[i].mask) {
            ++j;
        } else {
            const double w = std::max(0.0, a[i].weight - b[j].weight);
            if (w > 0.0) {
                result.push_back(Combo{a[i].mask, w});
            }
            ++i;
            ++j;
        }
    }
    for (; i < a.size(); ++i) {
        if (a[i].weight > 0.0) {
            result.push_back(a[i]);
        }
    }
    return result;
}

} // namespace

Range range_union(const Range& a, const Range& b) {
    require_same_game(a, b, "range_union");
    std::vector<Combo> left = a.combos();
    std::vector<Combo> right = b.combos();
    sort_and_check_duplicates(left, "range_union");
    sort_and_check_duplicates(right, "range_union");
    return Range(merge_union(left, right), a.game());
}

Range range_intersection(const Range& a, const Range& b) {
    require_same_game(a, b, "range_intersection");
    std::vector<Combo> left = a.combos();
    std::vector<Combo> right = b.combos();
    sort_and_check_duplicates(left, "range_intersection");
    sort_and_check_duplicates(right, "range_intersection");
    return Range(merge_intersection(left, right), a.game());
}

Range range_difference(const Range& a, const Range& b) {
    require_same_game(a, b, "range_difference");
    std::vector<Combo> left = a.combos();
    std::vector<Combo> right = b.combos();
    sort_and_check_duplicates(left, "range_difference");
    sort_and_check_duplicates(right, "range_difference");
    return Range(merge_difference(left, right), a.game());
}

} // namespace xiapl
