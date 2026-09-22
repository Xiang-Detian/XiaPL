// examples/weighted_range.cpp
//
// Show weighted Range parsing and how combo weights shift aggregate equity:
//   - Range::from_string supports ":weight" suffix
//   - try_parse_range returns std::optional<Range>
//   - calculate_range_equity weighs each villain combo by its parsed weight
//
// Build (from project root):
//   cmake -S . -B build -DXIAPL_BUILD_EXAMPLES=ON
//   cmake --build build --target weighted_range
//   ./build/examples/weighted_range
#include <xiapl/card.h>
#include <xiapl/range.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace xiapl;

namespace {

std::string mask_to_label(std::uint64_t mask) {
    auto cards = mask_to_cards(mask);
    std::string s;
    for (const auto& c : cards) s += c.to_string();
    return s;
}

void dump_range(const std::string& label, const Range& r) {
    std::cout << "  " << label << "  (size=" << r.size()
              << ", total_weight=" << std::fixed << std::setprecision(3)
              << r.total_weight() << ")\n";
    for (const auto& c : r.combos()) {
        std::cout << "    " << mask_to_label(c.mask)
                  << "  weight=" << c.weight << "\n";
    }
}

} // namespace

int main() {
    // ------------------------------------------------------------------
    // 1) Parse a weighted range
    // ------------------------------------------------------------------
    {
        Range r = Range::from_string("AdAh:1.0, AdAc:0.5, AsKs:0.25");
        std::cout << "=== Weighted Range parsing ===\n";
        dump_range("AdAh:1.0, AdAc:0.5, AsKs:0.25", r);
    }

    // ------------------------------------------------------------------
    // 2) try_parse_range: non-throwing parser
    // ------------------------------------------------------------------
    {
        std::cout << "\n=== try_parse_range ===\n";
        if (auto ok = try_parse_range("JJ+, AKs")) {
            std::cout << "  try_parse_range(\"JJ+, AKs\") -> size=" << ok->size() << "\n";
        }
        if (!try_parse_range("nope!").has_value()) {
            std::cout << "  try_parse_range(\"nope!\") -> nullopt (no throw)\n";
        }
    }

    // ------------------------------------------------------------------
    // 3) Same villain hands, different weights shift aggregate equity
    // ------------------------------------------------------------------
    {
        Range hero = Range::from_string("AsKs");
        Range vill_balanced   = Range::from_string("AdAh, 2c2d");
        Range vill_heavy_aces = Range::from_string("AdAh:1.0, 2c2d:0.1");
        Range vill_heavy_low  = Range::from_string("AdAh:0.1, 2c2d:1.0");

        SimulationOptions opt;
        opt.iterations = 20000;
        opt.deterministic = true;
        opt.seed = 7;

        auto bal   = calculate_range_equity(hero, vill_balanced,   0, opt);
        auto heavy = calculate_range_equity(hero, vill_heavy_aces, 0, opt);
        auto low   = calculate_range_equity(hero, vill_heavy_low,  0, opt);

        std::cout << "\n=== AsKs vs villain {AA, 22} with different weights ===\n";
        std::cout << std::fixed << std::setprecision(4);
        std::cout << "  balanced   AA:1.0 22:1.0 -> hero eq = "
                  << bal.hero_aggregate_equity << "\n";
        std::cout << "  heavy AA   AA:1.0 22:0.1 -> hero eq = "
                  << heavy.hero_aggregate_equity
                  << "  (lower: hero is dominated more often)\n";
        std::cout << "  heavy low  AA:0.1 22:1.0 -> hero eq = "
                  << low.hero_aggregate_equity
                  << "  (higher: hero dominates more often)\n";
    }

    // ------------------------------------------------------------------
    // 4) valid_combos: dead_mask removes any combo overlapping the mask
    // ------------------------------------------------------------------
    {
        Range r = Range::from_string("AA");
        std::uint64_t dead = card_to_mask(Card::from_string("As"));
        auto valid = r.valid_combos(dead);
        std::cout << "\n=== Range::valid_combos(dead = As) ===\n";
        std::cout << "  AA combos total       = " << r.size() << "\n";
        std::cout << "  AA combos w/o As card = " << valid.size()
                  << "  (3 expected: AhAd, AhAc, AdAc)\n";
    }

    return 0;
}
