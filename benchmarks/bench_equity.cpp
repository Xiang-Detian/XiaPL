// benchmarks/bench_equity.cpp
//
// Micro-benchmarks for xiapl_core equity API:
//   - exact HU equity time (preflop full enumeration)
//   - Monte Carlo HU equity iterations/sec
//   - range vs range equity time (preflop premium ranges)
//
// All times are measured with std::chrono::steady_clock. Results are
// printed in a stable format so they can be diffed across builds.
//
// Build (from project root):
//   cmake -S . -B build_bench -DXIAPL_BUILD_BENCHMARKS=ON \
//     -DCMAKE_BUILD_TYPE=Release
//   cmake --build build_bench --target bench_equity
//   ./build_bench/benchmarks/bench_equity

#include <xiapl/card.h>
#include <xiapl/range.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace xiapl;

namespace {

using clk = std::chrono::steady_clock;

std::uint64_t mk(const std::string& s) {
    return card_to_mask(Card::from_string(s));
}

struct Timing {
    double seconds;
    std::uint64_t ops;       // unit varies per measurement (iters, trials, calls)
    const char* unit;
};

void report(const std::string& label, Timing t) {
    double per = (t.ops > 0) ? (t.seconds / static_cast<double>(t.ops)) : 0.0;
    double ops_per_sec = (t.seconds > 0.0) ? (static_cast<double>(t.ops) / t.seconds) : 0.0;
    std::cout << "  " << std::left << std::setw(36) << label
              << "  " << std::right << std::setw(12) << t.ops << " " << t.unit
              << "  " << std::fixed << std::setprecision(4) << std::setw(9) << t.seconds << " s"
              << "  " << std::scientific << std::setprecision(2) << ops_per_sec << " " << t.unit << "/s"
              << "  " << std::fixed << std::setprecision(1) << (per * 1e9) << " ns/" << t.unit << "\n";
}

} // namespace

int main(int argc, char** argv) {
    int mc_iters     = 1'000'000;
    int re_iters     = 50'000;

    if (argc > 1) mc_iters = std::stoi(argv[1]);
    if (argc > 2) re_iters = std::stoi(argv[2]);

    std::cout << "xiapl bench_equity\n";
    std::cout << "  mc_iters=" << mc_iters
              << "  re_iters=" << re_iters << "\n\n";

    // -------------------------------------------------------------------
    // 1) Exact HU preflop equity: AsKs vs QdQc
    //    Full enumeration over C(48, 5) = 1,712,304 boards.
    // -------------------------------------------------------------------
    {
        std::vector<std::uint64_t> holes = {
            mk("As") | mk("Ks"),
            mk("Qd") | mk("Qc"),
        };
        SimulationOptions opt; // iterations=0 -> exact

        auto t0 = clk::now();
        auto r = calculate_equity(holes, 0, opt, GameType::Holdem);
        auto t1 = clk::now();
        double s = std::chrono::duration<double>(t1 - t0).count();

        std::cout << "[1] exact HU preflop equity (AsKs vs QdQc)\n";
        report("calculate_equity (exact)", { s, r.trials, "trial" });
        std::cout << "    P0.equity=" << std::fixed << std::setprecision(4)
                  << r.players[0].equity
                  << "  P1.equity=" << r.players[1].equity
                  << "  chop=" << r.chop_rate << "\n\n";
    }

    // -------------------------------------------------------------------
    // 2) Monte Carlo HU equity throughput: AsKs vs QdQc
    // -------------------------------------------------------------------
    {
        std::vector<std::uint64_t> holes = {
            mk("As") | mk("Ks"),
            mk("Qd") | mk("Qc"),
        };
        SimulationOptions opt;
        opt.iterations = mc_iters;
        opt.deterministic = true;
        opt.seed = 42;

        auto t0 = clk::now();
        auto r = calculate_equity(holes, 0, opt, GameType::Holdem);
        auto t1 = clk::now();
        double s = std::chrono::duration<double>(t1 - t0).count();

        std::cout << "[2] Monte Carlo HU equity (seed=42)\n";
        report("calculate_equity (MC)", { s, r.trials, "iter" });
        std::cout << "    P0.equity=" << std::fixed << std::setprecision(4)
                  << r.players[0].equity
                  << "  se=" << r.players[0].std_error << "\n\n";
    }

    // -------------------------------------------------------------------
    // 3) Range vs range equity (Monte Carlo): JJ+/AKs/AKo vs 99-22/AJs+/KQs/AQo+
    // -------------------------------------------------------------------
    {
        Range hero = Range::from_string("JJ+, AKs, AKo");
        Range vill = Range::from_string("99-22, AJs+, KQs, AQo+");
        SimulationOptions opt;
        opt.iterations = re_iters;
        opt.deterministic = true;
        opt.seed = 2026;

        std::cout << "[3] range vs range MC equity\n";
        std::cout << "    hero combos=" << hero.size()
                  << "  villain combos=" << vill.size() << "\n";

        auto t0 = clk::now();
        auto r = calculate_range_equity(hero, vill, 0, opt);
        auto t1 = clk::now();
        double s = std::chrono::duration<double>(t1 - t0).count();

        report("calculate_range_equity (MC)", { s, r.trials, "iter" });
        std::cout << "    hero_agg=" << std::fixed << std::setprecision(4)
                  << r.hero_aggregate_equity
                  << "  vill_agg=" << r.villain_aggregate_equity << "\n";
    }

    // -------------------------------------------------------------------
    // 4) Range vs range equity (exact preflop): AA vs KK
    // -------------------------------------------------------------------
    {
        Range hero = Range::from_string("AA");
        Range vill = Range::from_string("KK");
        SimulationOptions opt; // exact

        std::cout << "\n[4] range vs range exact preflop (AA vs KK)\n";

        auto t0 = clk::now();
        auto r = calculate_range_equity(hero, vill, 0, opt);
        auto t1 = clk::now();
        double s = std::chrono::duration<double>(t1 - t0).count();

        report("calculate_range_equity (exact)", { s, r.trials, "board" });
        std::cout << "    hero_agg=" << std::fixed << std::setprecision(4)
                  << r.hero_aggregate_equity
                  << "  vill_agg=" << r.villain_aggregate_equity << "\n";
    }

    return 0;
}
