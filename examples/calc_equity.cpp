// examples/calc_equity.cpp
//
// Show xiapl_core's reproducible equity API:
//   - calculate_equity for Hold'em HU (exact + Monte Carlo + seed reproducibility)
//   - 3-way Hold'em equity
//   - PLO equity
//
// Build (from project root):
//   cmake -S . -B build -DXIAPL_BUILD_EXAMPLES=ON
//   cmake --build build --target calc_equity
//   ./build/examples/calc_equity
#include <xiapl/card.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace xiapl;

namespace {

std::uint64_t mk(const std::string& s) {
    return card_to_mask(Card::from_string(s));
}

void print_result(const std::string& label, const EquityResult& r) {
    std::cout << "  " << label << "\n";
    std::cout << "    exact=" << (r.exact ? "true" : "false")
              << "  trials=" << r.trials
              << "  chop=" << std::fixed << std::setprecision(4) << r.chop_rate << "\n";
    for (std::size_t i = 0; i < r.players.size(); ++i) {
        const auto& p = r.players[i];
        std::cout << "    P" << i
                  << "  win=" << std::setprecision(4) << p.winrate
                  << "  eq="  << p.equity
                  << "  se="  << p.std_error << "\n";
    }
}

} // namespace

int main() {
    // ------------------------------------------------------------------
    // 1) Hold'em HU: AsKs vs QdQc, exact preflop enumeration
    // ------------------------------------------------------------------
    {
        std::vector<std::uint64_t> holes = {
            mk("As") | mk("Ks"),
            mk("Qd") | mk("Qc"),
        };
        SimulationOptions opt;  // iterations=0 -> exact
        auto r = calculate_equity(holes, /*board=*/0, opt, GameType::Holdem);
        std::cout << "=== HU exact: AsKs vs QdQc (preflop) ===\n";
        print_result("AsKs vs QdQc", r);
    }

    // ------------------------------------------------------------------
    // 2) Same matchup, Monte Carlo with two seeds -> reproducible
    // ------------------------------------------------------------------
    {
        std::vector<std::uint64_t> holes = {
            mk("As") | mk("Ks"),
            mk("Qd") | mk("Qc"),
        };
        SimulationOptions a;
        a.iterations = 20000;
        a.deterministic = true;
        a.seed = 42;

        SimulationOptions b = a;             // same seed
        SimulationOptions c = a; c.seed = 7; // different seed

        auto ra = calculate_equity(holes, 0, a, GameType::Holdem);
        auto rb = calculate_equity(holes, 0, b, GameType::Holdem);
        auto rc = calculate_equity(holes, 0, c, GameType::Holdem);

        std::cout << "\n=== HU MC seed reproducibility (iters=20000) ===\n";
        std::cout << "  seed=42 P0.equity = " << ra.players[0].equity << "\n";
        std::cout << "  seed=42 P0.equity = " << rb.players[0].equity
                  << "   (same seed -> bit-exact: " << (ra.players[0].equity == rb.players[0].equity ? "yes" : "no") << ")\n";
        std::cout << "  seed=7  P0.equity = " << rc.players[0].equity
                  << "   (different seed -> differs: " << (ra.players[0].equity != rc.players[0].equity ? "yes" : "no") << ")\n";
    }

    // ------------------------------------------------------------------
    // 3) 3-way Hold'em equity on a flop
    // ------------------------------------------------------------------
    {
        std::vector<std::uint64_t> holes = {
            mk("As") | mk("Ks"),
            mk("Qd") | mk("Qc"),
            mk("7h") | mk("6h"),
        };
        std::uint64_t board = mk("Ah") | mk("7d") | mk("2c");

        SimulationOptions opt;  // exact: C(47, 2) = 1081 turn+river combos
        auto r = calculate_equity(holes, board, opt, GameType::Holdem);
        std::cout << "\n=== 3-way exact on Ah 7d 2c flop ===\n";
        print_result("AsKs / QdQc / 7h6h", r);
    }

    // ------------------------------------------------------------------
    // 4) PLO equity, Monte Carlo
    // ------------------------------------------------------------------
    {
        std::vector<std::uint64_t> holes = {
            mk("As") | mk("Ks") | mk("Qd") | mk("Jc"),
            mk("Ah") | mk("Ad") | mk("2c") | mk("3d"),
        };
        SimulationOptions opt;
        opt.iterations = 50000;
        opt.deterministic = true;
        opt.seed = 123;

        auto r = calculate_equity(holes, /*board=*/0, opt, GameType::Plo);
        std::cout << "\n=== PLO HU MC: AsKsQdJc vs AhAd2c3d ===\n";
        print_result("AsKsQdJc vs AhAd2c3d", r);
    }

    return 0;
}
