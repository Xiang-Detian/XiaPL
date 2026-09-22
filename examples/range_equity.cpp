// examples/range_equity.cpp
//
// Show xiapl_core's range-vs-range equity API:
//   - parse ranges via Range::from_string
//   - compute hero/villain per-combo + aggregate equity via calculate_range_equity
//   - exact preflop enumeration and Monte Carlo with seed
//   - board blocker handling
//
// Build (from project root):
//   cmake -S . -B build -DXIAPL_BUILD_EXAMPLES=ON
//   cmake --build build --target range_equity
//   ./build/examples/range_equity
#include <xiapl/card.h>
#include <xiapl/range.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include <algorithm>
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

std::uint64_t mk(const std::string& s) {
    return card_to_mask(Card::from_string(s));
}

void print_top_combos(const std::string& label,
                      const std::vector<RangeEquityEntry>& entries,
                      std::size_t k = 5) {
    std::vector<RangeEquityEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const RangeEquityEntry& a, const RangeEquityEntry& b) {
                  return a.equity > b.equity;
              });
    std::cout << "  " << label << " (top " << std::min(k, sorted.size())
              << " of " << sorted.size() << "):\n";
    for (std::size_t i = 0; i < std::min(k, sorted.size()); ++i) {
        const auto& e = sorted[i];
        std::cout << "    " << mask_to_label(e.combo_mask)
                  << "  equity=" << std::fixed << std::setprecision(4) << e.equity
                  << "  weight=" << e.weight << "\n";
    }
}

} // namespace

int main() {
    // ------------------------------------------------------------------
    // 1) Premium vs premium, exact preflop
    // ------------------------------------------------------------------
    {
        Range hero = Range::from_string("AA");
        Range vill = Range::from_string("KK");
        SimulationOptions opt;  // iterations=0 -> exact

        auto r = calculate_range_equity(hero, vill, /*board=*/0, opt);
        std::cout << "=== AA vs KK exact preflop ===\n";
        std::cout << "  exact=" << (r.exact ? "true" : "false")
                  << "  trials=" << r.trials << "\n";
        std::cout << "  hero combos=" << r.hero.size()
                  << "  villain combos=" << r.villain.size() << "\n";
        std::cout << "  hero    aggregate equity = "
                  << std::fixed << std::setprecision(4) << r.hero_aggregate_equity << "\n";
        std::cout << "  villain aggregate equity = "
                  << r.villain_aggregate_equity << "\n";
    }

    // ------------------------------------------------------------------
    // 2) Wider ranges, Monte Carlo, seeded
    // ------------------------------------------------------------------
    {
        Range hero = Range::from_string("JJ+, AKs, AKo");
        Range vill = Range::from_string("99-22, AJs+, KQs, AQo+");
        SimulationOptions opt;
        opt.iterations = 30000;
        opt.deterministic = true;
        opt.seed = 2026;

        auto r = calculate_range_equity(hero, vill, /*board=*/0, opt);
        std::cout << "\n=== Wider ranges, MC iters=30000, seed=2026 ===\n";
        std::cout << "  hero hand counts: " << hero.size()
                  << "  vill hand counts: " << vill.size() << "\n";
        std::cout << "  hero    aggregate equity = "
                  << r.hero_aggregate_equity << "\n";
        std::cout << "  villain aggregate equity = "
                  << r.villain_aggregate_equity << "\n";
        print_top_combos("hero combos by equity", r.hero, 5);
        print_top_combos("villain combos by equity", r.villain, 5);
    }

    // ------------------------------------------------------------------
    // 3) Board blockers: AA combos blocked by an A on the board
    // ------------------------------------------------------------------
    {
        Range hero = Range::from_string("AA, KK, QQ");
        Range vill = Range::from_string("JJ");
        SimulationOptions opt;  // exact: C(49, 2) = 1176 turn+river combos

        std::uint64_t board = mk("Ah") | mk("7d") | mk("2c");
        auto r = calculate_range_equity(hero, vill, board, opt);
        std::cout << "\n=== AA/KK/QQ vs JJ on Ah 7d 2c (exact) ===\n";
        std::cout << "  trials=" << r.trials << "\n";
        std::cout << "  hero combos returned = " << r.hero.size()
                  << " (AA combos containing Ah are blocked)\n";
        std::cout << "  hero    aggregate equity = "
                  << std::fixed << std::setprecision(4) << r.hero_aggregate_equity << "\n";
        std::cout << "  villain aggregate equity = "
                  << r.villain_aggregate_equity << "\n";
    }

    return 0;
}
