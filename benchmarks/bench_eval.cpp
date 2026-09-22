// benchmarks/bench_eval.cpp
//
// Micro-benchmarks for xiapl_core hand evaluation:
//   - 7-card evaluation throughput  (evaluate_mask)
//   - Hold'em HU winner determination throughput (judge)
//   - PLO   HU winner determination throughput (judge, GameType::Plo)
//
// Method:
//   - Pre-generate N random valid card layouts to remove RNG / dealing cost
//     from the measured loop.
//   - Pin work into a tight loop, sum the result into a sink to defeat DCE.
//   - Report time, ops/sec, and ns/op.
//
// Build (from project root):
//   cmake -S . -B build_bench -DXIAPL_BUILD_BENCHMARKS=ON \
//     -DCMAKE_BUILD_TYPE=Release
//   cmake --build build_bench --target bench_eval
//   ./build_bench/benchmarks/bench_eval

#include <xiapl/card.h>
#include <xiapl/eval.h>
#include <xiapl/hand_value.h>
#include <xiapl/utils.h>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace xiapl;

namespace {

using clk = std::chrono::steady_clock;

struct Timing {
    double seconds;
    std::uint64_t ops;
};

void report(const std::string& label, Timing t) {
    double per = (t.ops > 0) ? (t.seconds / static_cast<double>(t.ops)) : 0.0;
    double ops_per_sec = (t.seconds > 0.0) ? (static_cast<double>(t.ops) / t.seconds) : 0.0;
    std::cout << "  " << std::left << std::setw(40) << label
              << "  " << std::right << std::setw(12) << t.ops << " ops"
              << "  " << std::fixed << std::setprecision(3) << std::setw(8) << t.seconds << " s"
              << "  " << std::scientific << std::setprecision(2) << ops_per_sec << " ops/s"
              << "  " << std::fixed << std::setprecision(1) << (per * 1e9) << " ns/op\n";
}

// Draw n distinct card ids from a 52-card deck using rng.
std::uint64_t sample_mask(std::mt19937_64& rng, int n) {
    std::uint64_t m = 0;
    int picked = 0;
    while (picked < n) {
        int id = static_cast<int>(rng() % 52);
        std::uint64_t bit = (1ULL << id);
        if ((m & bit) == 0) { m |= bit; ++picked; }
    }
    return m;
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 1234567ULL;
    int n_eval = 2'000'000;
    int n_hu   = 1'000'000;
    int n_plo  = 200'000;

    if (argc > 1) n_eval = std::stoi(argv[1]);
    if (argc > 2) n_hu   = std::stoi(argv[2]);
    if (argc > 3) n_plo  = std::stoi(argv[3]);

    std::cout << "xiapl bench_eval\n";
    std::cout << "  n_eval=" << n_eval
              << "  n_hu=" << n_hu
              << "  n_plo=" << n_plo
              << "  seed=" << seed << "\n\n";

    std::mt19937_64 rng(seed);

    // -------------------------------------------------------------------
    // 1) 7-card evaluation throughput
    //    Pre-generate n_eval random 7-card masks, then time the eval loop.
    // -------------------------------------------------------------------
    {
        std::vector<std::uint64_t> masks;
        masks.reserve(static_cast<std::size_t>(n_eval));
        for (int i = 0; i < n_eval; ++i) masks.push_back(sample_mask(rng, 7));

        std::uint64_t sink = 0;
        auto t0 = clk::now();
        for (std::uint64_t m : masks) {
            HandValue v = evaluate_mask(m);
            sink += static_cast<std::uint64_t>(v.category);
            sink += v.kickers[0];
        }
        auto t1 = clk::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        std::cout << "[1] 7-card hand evaluation\n";
        report("evaluate_mask (7 cards)", { s, static_cast<std::uint64_t>(n_eval) });
        std::cout << "    (sink=" << sink << ")\n\n";
    }

    // -------------------------------------------------------------------
    // 2) Hold'em HU winner determination throughput
    //    Pre-generate disjoint (board5, h0, h1) triples.
    // -------------------------------------------------------------------
    {
        struct Layout { std::uint64_t board; std::uint64_t h0; std::uint64_t h1; };
        std::vector<Layout> layouts;
        layouts.reserve(static_cast<std::size_t>(n_hu));
        for (int i = 0; i < n_hu; ++i) {
            std::uint64_t all = sample_mask(rng, 9); // 5 board + 2 + 2
            // Split: 5 lowest-set bits -> board, next 2 -> h0, last 2 -> h1.
            std::uint64_t m = all;
            std::uint64_t board = 0, h0 = 0, h1 = 0;
            int got = 0;
            while (m) {
                int b = __builtin_ctzll(m);
                m &= m - 1;
                std::uint64_t bit = 1ULL << b;
                if      (got < 5) board |= bit;
                else if (got < 7) h0    |= bit;
                else              h1    |= bit;
                ++got;
            }
            layouts.push_back({board, h0, h1});
        }

        std::uint64_t sink = 0;
        std::vector<std::uint64_t> holes(2);
        auto t0 = clk::now();
        for (const auto& L : layouts) {
            holes[0] = L.h0;
            holes[1] = L.h1;
            auto winners = judge(holes, L.board);
            sink += winners.size() ^ (winners.empty() ? 0u : winners.front());
        }
        auto t1 = clk::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        std::cout << "[2] Hold'em HU winner determination\n";
        report("judge Hold'em (HU)", { s, static_cast<std::uint64_t>(n_hu) });
        std::cout << "    (sink=" << sink << ")\n\n";
    }

    // -------------------------------------------------------------------
    // 3) PLO HU winner determination throughput
    //    Pre-generate disjoint (board5, h0[4], h1[4]) triples.
    // -------------------------------------------------------------------
    {
        struct Layout { std::uint64_t board; std::uint64_t h0; std::uint64_t h1; };
        std::vector<Layout> layouts;
        layouts.reserve(static_cast<std::size_t>(n_plo));
        for (int i = 0; i < n_plo; ++i) {
            std::uint64_t all = sample_mask(rng, 13); // 5 board + 4 + 4
            std::uint64_t m = all;
            std::uint64_t board = 0, h0 = 0, h1 = 0;
            int got = 0;
            while (m) {
                int b = __builtin_ctzll(m);
                m &= m - 1;
                std::uint64_t bit = 1ULL << b;
                if      (got < 5) board |= bit;
                else if (got < 9) h0    |= bit;
                else              h1    |= bit;
                ++got;
            }
            layouts.push_back({board, h0, h1});
        }

        std::uint64_t sink = 0;
        std::vector<std::uint64_t> holes(2);
        auto t0 = clk::now();
        for (const auto& L : layouts) {
            holes[0] = L.h0;
            holes[1] = L.h1;
            auto winners = judge(holes, L.board, GameType::Plo);
            sink += winners.size() ^ (winners.empty() ? 0u : winners.front());
        }
        auto t1 = clk::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        std::cout << "[3] PLO HU winner determination\n";
        report("judge PLO (HU)", { s, static_cast<std::uint64_t>(n_plo) });
        std::cout << "    (sink=" << sink << ")\n";
    }

    return 0;
}
