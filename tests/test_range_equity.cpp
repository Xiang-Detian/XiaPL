#include "doctest.h"
#include "test_env_util.h"
#include "test_ulp_util.h"
#include <xiapl/card.h>
#include <xiapl/range.h>
#include <xiapl/simulation.h>
#include <xiapl/utils.h>

#include <cstdlib>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

using namespace xiapl;

namespace {

std::uint64_t mk(const std::string& s) {
    return card_to_mask(Card::from_string(s));
}

// Saves whatever XIAPL_NUM_THREADS the process already had on construction and
// restores it in the destructor, so a test that sweeps env values (or throws
// mid-sweep from a REQUIRE/failed call) can never leak an override into the
// tests that run after it — restoration happens via unwinding, not via a
// manual step at the end of the test body that a mid-loop failure could skip.
class XiaplNumThreadsEnvGuard {
 public:
    XiaplNumThreadsEnvGuard() {
        const char* raw = std::getenv("XIAPL_NUM_THREADS");
        had_env_ = (raw != nullptr);
        if (had_env_) saved_ = raw;
    }
    ~XiaplNumThreadsEnvGuard() {
        if (had_env_) {
            test_set_env("XIAPL_NUM_THREADS", saved_.c_str());
        } else {
            test_unset_env("XIAPL_NUM_THREADS");
        }
    }
    XiaplNumThreadsEnvGuard(const XiaplNumThreadsEnvGuard&) = delete;
    XiaplNumThreadsEnvGuard& operator=(const XiaplNumThreadsEnvGuard&) = delete;

 private:
    bool had_env_ = false;
    std::string saved_;
};

} // namespace

TEST_CASE("calculate_range_equity HU AA vs KK is reproducible") {
    Range hero = Range::from_string("AA");
    Range vill = Range::from_string("KK");

    SimulationOptions opt;
    opt.iterations = 5000;
    opt.deterministic = true;
    opt.seed = 42;

    auto r1 = calculate_range_equity(hero, vill, 0, opt);
    auto r2 = calculate_range_equity(hero, vill, 0, opt);

    CHECK(r1.exact == false);
    CHECK(r1.trials == 5000ULL);
    CHECK(r1.hero.size() == 6);     // 6 AA combos
    CHECK(r1.villain.size() == 6);  // 6 KK combos

    // Bit-exact reproducibility under same seed.
    CHECK(r1.hero_aggregate_equity == r2.hero_aggregate_equity);
    CHECK(r1.villain_aggregate_equity == r2.villain_aggregate_equity);

    // AA vs KK preflop is ~0.82 / 0.18
    CHECK(r1.hero_aggregate_equity > 0.75);
    CHECK(r1.hero_aggregate_equity < 0.90);
    // hero + villain should sum to ~1.0
    CHECK(r1.hero_aggregate_equity + r1.villain_aggregate_equity ==
          doctest::Approx(1.0).epsilon(0.005));
}

TEST_CASE("calculate_range_equity exact small ranges") {
    Range hero = Range::from_string("AsKs");  // single combo
    Range vill = Range::from_string("QdQc");  // single combo

    SimulationOptions opt;
    opt.iterations = 0; // exact

    auto r = calculate_range_equity(hero, vill, 0, opt);

    CHECK(r.exact == true);
    CHECK(r.trials == 2598960ULL);          // C(52, 5): global board pool
    REQUIRE(r.hero.size() == 1);
    REQUIRE(r.villain.size() == 1);
    CHECK(r.hero[0].equity + r.villain[0].equity ==
          doctest::Approx(1.0).epsilon(1e-9));
    CHECK(r.hero_aggregate_equity + r.villain_aggregate_equity ==
          doctest::Approx(1.0).epsilon(1e-9));
}

TEST_CASE("calculate_range_equity weighted villain shifts aggregate") {
    // hero combo AKs vs villain that is half AA, half 22.
    // Both should average between the two pure matchups.
    Range hero = Range::from_string("AsKs");
    Range vill_balanced = Range::from_string("AdAh, 2c2d");
    Range vill_heavy_aces = Range::from_string("AdAh:1.0, 2c2d:0.1");

    SimulationOptions opt;
    opt.iterations = 2000;
    opt.deterministic = true;
    opt.seed = 7;

    auto bal = calculate_range_equity(hero, vill_balanced, 0, opt);
    auto heavy = calculate_range_equity(hero, vill_heavy_aces, 0, opt);

    // Weighted toward AA villain should reduce hero equity vs balanced.
    CHECK(heavy.hero_aggregate_equity < bal.hero_aggregate_equity);
    CHECK(heavy.exact == false);
    CHECK(heavy.trials == 2000ULL);

    // weight is carried through into the result entries.
    REQUIRE(heavy.villain.size() == 2);
    bool found_aa = false, found_22 = false;
    for (const auto& e : heavy.villain) {
        if (std::abs(e.weight - 1.0) < 1e-12) found_aa = true;
        if (std::abs(e.weight - 0.1) < 1e-12) found_22 = true;
    }
    CHECK(found_aa);
    CHECK(found_22);
}

TEST_CASE("calculate_range_equity excludes board blockers") {
    // Hero range JJ+ on board with A → AA combos blocked by board.
    Range hero = Range::from_string("AA, KK, QQ, JJ");
    Range vill = Range::from_string("TT");

    SimulationOptions opt;
    opt.iterations = 0; // exact (flop -> 990 boards)

    std::uint64_t flop = mk("Ah") | mk("7d") | mk("2c");
    auto r = calculate_range_equity(hero, vill, flop, opt);

    CHECK(r.exact == true);
    CHECK(r.trials == 1176ULL);  // C(49, 2): global board pool excluding flop

    // AA combos containing Ah are blocked; AhAs, AhAc, AhAd are removed.
    // Remaining AA combos: AsAc, AsAd, AcAd → 3 combos.
    int aa_count = 0;
    std::uint64_t a_rank_mask = mk("As") | mk("Ah") | mk("Ac") | mk("Ad");
    for (const auto& e : r.hero) {
        std::uint64_t m = e.combo_mask;
        if (popcount64(m & a_rank_mask) == 2) ++aa_count;
    }
    CHECK(aa_count == 3);
}

TEST_CASE("calculate_range_equity empty range returns empty result") {
    Range hero;  // empty
    Range vill = Range::from_string("AA");
    SimulationOptions opt;
    opt.iterations = 100;
    opt.deterministic = true;
    opt.seed = 1;

    auto r = calculate_range_equity(hero, vill, 0, opt);
    CHECK(r.hero.empty());
    CHECK(r.villain.empty());
    CHECK(r.hero_aggregate_equity == 0.0);
    CHECK(r.villain_aggregate_equity == 0.0);
}

TEST_CASE("calculate_range_equity aggregate is weighted by compatible pairs") {
    Range hero = Range::from_string("AsKs, QhQd");
    Range vill = Range::from_string("AsAh, KsKh, 7c7d");

    std::uint64_t board =
        mk("2c") | mk("3d") | mk("4h") | mk("8s") | mk("9c");

    SimulationOptions opt;
    opt.iterations = 0; // completed board, one board trial

    auto r = calculate_range_equity(hero, vill, board, opt);

    REQUIRE(r.hero.size() == 2);
    REQUIRE(r.villain.size() == 3);
    CHECK(r.exact == true);
    CHECK(r.trials == 1ULL);

    // Valid compatible pairs:
    //   AsKs vs 7c7d  -> hero loses
    //   QhQd vs AsAh  -> hero loses
    //   QhQd vs KsKh  -> hero loses
    //   QhQd vs 7c7d  -> hero wins
    //
    // Pair-weighted aggregate is therefore 1 / 4. A per-hero-combo average
    // would incorrectly produce (0 + 1/3) / 2.
    CHECK(r.hero_aggregate_equity == doctest::Approx(0.25));
    CHECK(r.villain_aggregate_equity == doctest::Approx(0.75));
}

TEST_CASE("calculate_range_equity rejects negative iterations") {
    Range hero = Range::from_string("AA");
    Range vill = Range::from_string("KK");
    SimulationOptions opt;
    opt.iterations = -1;
    CHECK_THROWS(calculate_range_equity(hero, vill, 0, opt));
}

// ---------------------------------------------------------------------------
// Phase C: wide range exact safety guard.
// Preflop exact (board==0) enumerates C(52,5) = 2,598,960 boards; the per-hand
// evaluation cache scales as unique_hands × num_boards × 4 B. Wide ranges
// must throw rather than silently allocate multiple GB.
// ---------------------------------------------------------------------------

TEST_CASE("calculate_range_equity throws on wide preflop exact") {
    // ~28% range each side; unique_hands ~ 300 -> cache ~ 3 GB > 512 MB limit.
    Range hero = Range::from_string("22+, A2s+, K2s+, Q2s+, J2s+, T2s+, A2o+, K9o+, QTo+, JTo");
    Range vill = Range::from_string("22+, A2s+, K2s+, Q2s+, J2s+, T2s+, A2o+, K9o+, QTo+, JTo");

    SimulationOptions opt;
    opt.iterations = 0; // exact

    CHECK_THROWS_AS(calculate_range_equity(hero, vill, 0, opt),
                    std::runtime_error);
}

TEST_CASE("calculate_range_equity wide preflop MC succeeds") {
    Range hero = Range::from_string("22+, A2s+, K2s+, Q2s+, J2s+, T2s+, A2o+, K9o+, QTo+, JTo");
    Range vill = Range::from_string("22+, A2s+, K2s+, Q2s+, J2s+, T2s+, A2o+, K9o+, QTo+, JTo");

    SimulationOptions opt;
    opt.iterations = 500;
    opt.deterministic = true;
    opt.seed = 42;

    auto r = calculate_range_equity(hero, vill, 0, opt);

    CHECK(r.exact == false);
    CHECK(r.trials == 500ULL);
    // Symmetric ranges -> hero equity ~= 0.5 (blockers cause minor noise).
    CHECK(r.hero_aggregate_equity > 0.4);
    CHECK(r.hero_aggregate_equity < 0.6);
}

TEST_CASE("calculate_range_equity small preflop exact still works") {
    // 6x6 combos, exact: cache = 12 unique × 2,598,960 × 4 B ~= 119 MB, under limit.
    Range hero = Range::from_string("AA");
    Range vill = Range::from_string("KK");
    SimulationOptions opt;
    opt.iterations = 0;

    auto r = calculate_range_equity(hero, vill, 0, opt);

    CHECK(r.exact == true);
    CHECK(r.trials == 2598960ULL);
    // AA vs KK preflop ~ 81% / 19%.
    CHECK(r.hero_aggregate_equity > 0.79);
    CHECK(r.hero_aggregate_equity < 0.83);
}

// ---------------------------------------------------------------------------
// 1-pass refactor regression tests: ensure the unified hero/villain accumulator
// preserves the semantics of the old two-call path.
// ---------------------------------------------------------------------------

TEST_CASE("calculate_range_equity MC different seeds differ") {
    Range hero = Range::from_string("JJ+, AKs");
    Range vill = Range::from_string("99-22, AJs+, KQs");

    SimulationOptions a;
    a.iterations = 2000;
    a.deterministic = true;
    a.seed = 1;

    SimulationOptions b = a;
    b.seed = 2;

    auto ra = calculate_range_equity(hero, vill, 0, a);
    auto rb = calculate_range_equity(hero, vill, 0, b);
    // Different seeds -> different sample sequences -> aggregate not bit-exact.
    CHECK(ra.hero_aggregate_equity != rb.hero_aggregate_equity);
    // But both should land in a believable equity window.
    CHECK(ra.hero_aggregate_equity > 0.3);
    CHECK(ra.hero_aggregate_equity < 0.9);
}

TEST_CASE("calculate_range_equity symmetric ranges -> 0.5 equity") {
    // Identical hero/villain ranges in exact mode should give exactly 0.5 each
    // since every (i, j) pair is mirrored by (j, i) with reversed equity.
    Range hero = Range::from_string("AsKs, KdQd");
    Range vill = Range::from_string("AsKs, KdQd");
    SimulationOptions opt;
    opt.iterations = 0; // exact preflop

    auto r = calculate_range_equity(hero, vill, 0, opt);
    CHECK(r.hero_aggregate_equity == doctest::Approx(0.5).epsilon(1e-9));
    CHECK(r.villain_aggregate_equity == doctest::Approx(0.5).epsilon(1e-9));
    // Each side returns the same set of per-combo entries.
    REQUIRE(r.hero.size() == r.villain.size());
}

TEST_CASE("calculate_range_equity hero & villain per-combo are consistent") {
    // For each villain combo, its equity should equal 1 - weighted_avg of
    // hero equities (over compatible hero combos with hero-weight as weight).
    // Verify this invariant on an asymmetric weighted setup.
    Range hero = Range::from_string("AsKs:1.0, QhJh:0.5");
    Range vill = Range::from_string("AdAh:1.0, 2c2d:0.3");
    SimulationOptions opt;
    opt.iterations = 1000;
    opt.deterministic = true;
    opt.seed = 99;

    auto r = calculate_range_equity(hero, vill, 0, opt);
    REQUIRE(r.hero.size() == 2);
    REQUIRE(r.villain.size() == 2);

    // Recompute pair aggregate from villain side (must match hero side).
    // villain entry equity = sum(hero_weight * (1 - eq_hv)) / sum(hero_weight)
    // -> 1 - villain.equity = sum(hero_weight * eq_hv) / sum(hero_weight)
    // Since all hero combos are compatible (no overlap with these villains),
    // we can cross-check pair aggregate equivalence by recomputing from villains.
    double pair_num = 0.0, pair_den = 0.0;
    for (const auto& vc : r.villain) {
        // For each villain entry, (1 - vc.equity) is the hero-equity-weighted avg
        // over compatible hero combos. Sum hero_weight as denominator.
        double hero_weight_total = 0.0;
        for (const auto& hc : r.hero) {
            if ((hc.combo_mask & vc.combo_mask) == 0) hero_weight_total += hc.weight;
        }
        pair_num += vc.weight * hero_weight_total * (1.0 - vc.equity);
        pair_den += vc.weight * hero_weight_total;
    }
    REQUIRE(pair_den > 0.0);
    CHECK(pair_num / pair_den ==
          doctest::Approx(r.hero_aggregate_equity).epsilon(1e-9));
}

TEST_CASE("calculate_range_equity per-combo entries follow input order") {
    // Both sides should emit entries in the same order as input combos
    // (for combos that have at least one compatible opponent).
    Range hero = Range::from_string("AsKs, KdQd, 7c6c");
    Range vill = Range::from_string("AdAh, 2c2d");
    SimulationOptions opt;
    opt.iterations = 200;
    opt.deterministic = true;
    opt.seed = 5;

    auto r = calculate_range_equity(hero, vill, 0, opt);
    REQUIRE(r.hero.size() == 3);
    REQUIRE(r.villain.size() == 2);

    // hero[0] = AsKs, hero[1] = KdQd, hero[2] = 7c6c
    CHECK(r.hero[0].combo_mask == (mk("As") | mk("Ks")));
    CHECK(r.hero[1].combo_mask == (mk("Kd") | mk("Qd")));
    CHECK(r.hero[2].combo_mask == (mk("7c") | mk("6c")));
    // villain[0] = AdAh, villain[1] = 2c2d
    CHECK(r.villain[0].combo_mask == (mk("Ad") | mk("Ah")));
    CHECK(r.villain[1].combo_mask == (mk("2c") | mk("2d")));
}

// ---------------------------------------------------------------------------
// RangeEquityMode contract.
// Task 1 wired the mode through; Task 2 replaced the AggregateOnly Monte Carlo
// path (iterations > 0) with a sampled-pair estimator, so AggregateOnly MC no
// longer shares a random stream (or an estimator) with PerCombo. Exact mode
// (iterations == 0) still routes through the shared enumeration engine, so the
// exact tests below keep their bit-exact expectations.
// ---------------------------------------------------------------------------

TEST_CASE("range equity mode: default arg preserves existing behavior") {
    // Same call with and without explicit PerCombo must be bit-identical.
    auto hero = Range::from_string("AhKh");
    auto vill = Range::from_string("JJ+,AQs+,KQs");
    SimulationOptions opt;
    opt.iterations = 20000;
    opt.deterministic = true;
    opt.seed = 7;
    auto a = calculate_range_equity(hero, vill, 0, opt);
    auto b = calculate_range_equity(hero, vill, 0, opt, RangeEquityMode::PerCombo);
    CHECK(a.hero_aggregate_equity == b.hero_aggregate_equity);
    CHECK(a.hero.size() == b.hero.size());
    CHECK(a.trials == b.trials);
}

TEST_CASE("range equity mode: AggregateOnly strips breakdowns, keeps aggregates") {
    auto hero = Range::from_string("AhKh");
    auto vill = Range::from_string("JJ+,AQs+,KQs");
    SimulationOptions opt;
    opt.iterations = 20000;
    opt.deterministic = true;
    opt.seed = 7;
    auto full = calculate_range_equity(hero, vill, 0, opt, RangeEquityMode::PerCombo);
    auto agg = calculate_range_equity(hero, vill, 0, opt, RangeEquityMode::AggregateOnly);
    CHECK(agg.hero.empty());
    CHECK(agg.villain.empty());
    // Task 2: the two modes now run different MC estimators over different
    // random streams, so the aggregates only have to agree statistically.
    // PerCombo does not report a standard error, so we bound the combined SE
    // conservatively by 2 * se_agg: both estimators are averages of the same
    // bounded per-trial equity (in [0, 1]) over the same number of board
    // samples, and PerCombo additionally shares boards across pairs (common
    // random numbers), which cannot inflate its SE above the independent-pair
    // value. 2x therefore dominates sqrt(se_agg^2 + se_full^2).
    REQUIRE(agg.aggregate_std_error > 0.0);
    const double combined_se = 2.0 * agg.aggregate_std_error;
    CHECK(std::abs(agg.hero_aggregate_equity - full.hero_aggregate_equity) <=
          4.0 * combined_se);
    CHECK(agg.villain_aggregate_equity ==
          doctest::Approx(1.0 - agg.hero_aggregate_equity).epsilon(1e-12));
    // This preflop MC case has both sides equal to options.iterations by
    // coincidence, not because the two modes count the same thing: PerCombo's
    // trials is the board-sample-set size, AggregateOnly's is the number of
    // (hero combo, villain combo, board) triples. See simulation.h's
    // RangeEquityResult::trials doc for the semantics of each mode.
    CHECK(agg.trials == full.trials);
}

TEST_CASE("range equity mode: AggregateOnly MC converges to exact (river board)") {
    // River board => the exact ground truth is a single board evaluation, so
    // the only sampling dimension left for the fast path is the combo pair.
    auto hero = Range::from_string("AhKh");
    auto vill = Range::from_string("JJ+,AQs+");
    std::uint64_t board = mk("2c") | mk("3d") | mk("4h") | mk("8s") | mk("9c");

    SimulationOptions ex;  // iterations = 0 => exact
    auto truth = calculate_range_equity(hero, vill, board, ex,
                                        RangeEquityMode::PerCombo);

    SimulationOptions mc;
    mc.iterations = 200000;
    mc.deterministic = true;
    mc.seed = 11;
    auto agg = calculate_range_equity(hero, vill, board, mc,
                                      RangeEquityMode::AggregateOnly);

    CHECK(agg.hero.empty());
    CHECK(agg.exact == false);
    CHECK(agg.trials == 200000ULL);
    REQUIRE(agg.aggregate_std_error > 0.0);
    CHECK(std::abs(agg.hero_aggregate_equity - truth.hero_aggregate_equity) <=
          4.0 * agg.aggregate_std_error);
}

TEST_CASE("range equity mode: AggregateOnly respects combo weights") {
    // Board pairs the hero's king; villain AA beats it, villain QQ loses to it.
    // Per-pair equity is therefore deterministic (0 vs AA, 1 vs QQ) and the
    // aggregate is a pure function of the combo weights:
    //   weighted   = 6 / (6 + 3 * 0.25)   = 0.888...
    //   unweighted = 6 / (6 + 3)          = 0.666...
    // A sampler that ignored weights would miss by ~0.22, i.e. ~300 sigma.
    auto hero = Range::from_string("AhKh");
    auto vill = Range::from_string("AA:0.25,QQ");
    std::uint64_t board = mk("Kd") | mk("7c") | mk("2s") | mk("9h") | mk("4c");

    SimulationOptions ex;
    auto truth = calculate_range_equity(hero, vill, board, ex,
                                        RangeEquityMode::PerCombo);
    REQUIRE(truth.hero_aggregate_equity ==
            doctest::Approx(6.0 / 6.75).epsilon(1e-9));

    SimulationOptions mc;
    mc.iterations = 200000;
    mc.deterministic = true;
    mc.seed = 11;
    auto agg = calculate_range_equity(hero, vill, board, mc,
                                      RangeEquityMode::AggregateOnly);
    REQUIRE(agg.aggregate_std_error > 0.0);
    CHECK(std::abs(agg.hero_aggregate_equity - truth.hero_aggregate_equity) <=
          4.0 * agg.aggregate_std_error);
}

TEST_CASE("range equity mode: AggregateOnly handles hero/villain card conflicts") {
    // Asymmetric blockers: AhKh conflicts with 2 villain combos, AsKs with 1,
    // so the two hero combos see different villain sub-ranges (equity 6/7 vs
    // 6/8). The pair-level ground truth is 12/15 = 0.8; resampling only the
    // villain on a conflict would flatten the hero marginal and land on
    // (6/7 + 6/8)/2 = 0.8036 instead. 1M trials makes that ~9 sigma.
    auto hero = Range::from_string("AhKh,AsKs");
    auto vill = Range::from_string("AhAd,AhAc,AsAc,QQ");
    std::uint64_t board = mk("Kd") | mk("7c") | mk("2s") | mk("9h") | mk("4c");

    SimulationOptions ex;
    auto truth = calculate_range_equity(hero, vill, board, ex,
                                        RangeEquityMode::PerCombo);
    REQUIRE(truth.hero_aggregate_equity == doctest::Approx(0.8).epsilon(1e-9));

    SimulationOptions mc;
    mc.iterations = 1000000;
    mc.deterministic = true;
    mc.seed = 11;
    auto agg = calculate_range_equity(hero, vill, board, mc,
                                      RangeEquityMode::AggregateOnly);
    CHECK(agg.trials == 1000000ULL);  // rejected pairs are not counted
    REQUIRE(agg.aggregate_std_error > 0.0);
    CHECK(std::abs(agg.hero_aggregate_equity - truth.hero_aggregate_equity) <=
          4.0 * agg.aggregate_std_error);
}

TEST_CASE("range equity mode: AggregateOnly single-combo side (both orientations)") {
    // A side holding exactly one combo short-circuits the weighted draw. Check
    // both orientations against exact, on a flop so the board deal still runs
    // (the other single-hero tests are river spots with no cards to deal).
    // The hero AA combos containing Ah also conflict with the lone villain
    // combo, so pair rejection is exercised at the same time.
    auto range = Range::from_string("JJ+,AQs+");
    auto single = Range::from_string("AhKh");
    std::uint64_t flop = mk("2c") | mk("7d") | mk("9s");

    SimulationOptions ex;
    SimulationOptions mc;
    mc.iterations = 200000;
    mc.deterministic = true;
    mc.seed = 11;

    // villain is the single-combo side
    auto truth_v = calculate_range_equity(range, single, flop, ex,
                                          RangeEquityMode::PerCombo);
    auto agg_v = calculate_range_equity(range, single, flop, mc,
                                        RangeEquityMode::AggregateOnly);
    REQUIRE(agg_v.aggregate_std_error > 0.0);
    CHECK(agg_v.trials == 200000ULL);
    CHECK(std::abs(agg_v.hero_aggregate_equity - truth_v.hero_aggregate_equity) <=
          4.0 * agg_v.aggregate_std_error);

    // hero is the single-combo side
    auto truth_h = calculate_range_equity(single, range, flop, ex,
                                          RangeEquityMode::PerCombo);
    auto agg_h = calculate_range_equity(single, range, flop, mc,
                                        RangeEquityMode::AggregateOnly);
    REQUIRE(agg_h.aggregate_std_error > 0.0);
    CHECK(std::abs(agg_h.hero_aggregate_equity - truth_h.hero_aggregate_equity) <=
          4.0 * agg_h.aggregate_std_error);
}

TEST_CASE("range equity mode: AggregateOnly is reproducible with same seed (incl. seed 0)") {
    auto hero = Range::from_string("AhKh");
    auto vill = Range::from_string("JJ+,AQs+,KQs");

    for (std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{7}}) {
        SimulationOptions opt;
        opt.iterations = 20000;
        opt.deterministic = true;
        opt.seed = seed;  // seed 0 must stay deterministic (FastRng(0) trap)
        auto a = calculate_range_equity(hero, vill, 0, opt,
                                        RangeEquityMode::AggregateOnly);
        auto b = calculate_range_equity(hero, vill, 0, opt,
                                        RangeEquityMode::AggregateOnly);
        CHECK(a.hero_aggregate_equity == b.hero_aggregate_equity);
        CHECK(a.villain_aggregate_equity == b.villain_aggregate_equity);
        CHECK(a.aggregate_std_error == b.aggregate_std_error);
        CHECK(a.trials == b.trials);

        // Thread count never changes the result: the trial stream is
        // partitioned into fixed-size chunks with seed-derived substreams and
        // reduced in chunk order, so the same seed is bit-identical at any
        // thread setting.
        SimulationOptions threaded = opt;
        threaded.threads = 4;
        auto c = calculate_range_equity(hero, vill, 0, threaded,
                                        RangeEquityMode::AggregateOnly);
        CHECK(c.hero_aggregate_equity == a.hero_aggregate_equity);
    }

    // Different seeds must actually move the estimate (guards against a
    // silently seed-independent path).
    SimulationOptions s1;
    s1.iterations = 20000;
    s1.deterministic = true;
    s1.seed = 1;
    SimulationOptions s2 = s1;
    s2.seed = 2;
    auto r1 = calculate_range_equity(hero, vill, 0, s1,
                                     RangeEquityMode::AggregateOnly);
    auto r2 = calculate_range_equity(hero, vill, 0, s2,
                                     RangeEquityMode::AggregateOnly);
    CHECK(r1.hero_aggregate_equity != r2.hero_aggregate_equity);
}

TEST_CASE("range equity mode: AggregateOnly trials semantics") {
    // AA vs AA: only 6 of the 36 combo pairs are card-compatible, so ~83% of
    // the sampled pairs are rejected. trials must still report the requested
    // iteration count (rejections are not trials).
    auto hero = Range::from_string("AA");
    auto vill = Range::from_string("AA");
    SimulationOptions opt;
    opt.iterations = 5000;
    opt.deterministic = true;
    opt.seed = 3;
    auto agg = calculate_range_equity(hero, vill, 0, opt,
                                      RangeEquityMode::AggregateOnly);
    CHECK(agg.trials == 5000ULL);
    CHECK(agg.exact == false);
    // Symmetric matchup: equity is 0.5 up to sampling noise.
    CHECK(agg.hero_aggregate_equity == doctest::Approx(0.5).epsilon(0.05));
    CHECK(agg.villain_aggregate_equity ==
          doctest::Approx(1.0 - agg.hero_aggregate_equity).epsilon(1e-12));
}

TEST_CASE("range equity mode: AggregateOnly MC rejects mutually blocking ranges") {
    // Every pair shares both cards, so no compatible pair exists and the
    // rejection guard must fire instead of looping forever. (PerCombo returns
    // zeroed aggregates for the same input; the modes differ here by design.)
    auto hero = Range::from_string("AhKh");
    auto vill = Range::from_string("AhKh");
    SimulationOptions opt;
    opt.iterations = 1000;
    opt.deterministic = true;
    opt.seed = 4;
    CHECK_THROWS_AS(
        calculate_range_equity(hero, vill, 0, opt,
                               RangeEquityMode::AggregateOnly),
        std::runtime_error);
}

// ---- Threading contract for the AggregateOnly Monte Carlo path ----
//
// The estimator splits its trials into fixed 65536-trial chunks, each seeded
// from an independent substream derived from the master seed, and reduces the
// per-chunk accumulators in chunk-index order. Both properties are load
// bearing: the substreams make chunk k's trials independent of which worker
// ran it, and the fixed reduction order makes the floating-point sum itself
// thread-count independent. The three tests below pin exactly that.

TEST_CASE("AggregateOnly: bit-identical across thread counts and chunk boundaries") {
    const Range hero = Range::from_string("AA,KK,QQ,AKs,AKo,T9s,55");
    const Range vill = Range::from_string("22+,A2s+,KTs+,QJs,JTs,ATo+,KQo");
    const std::uint64_t board = 0;  // preflop
    // Iteration counts straddle the chunk boundary (65536) so the matrix
    // covers a partial single chunk, an exactly-full chunk, a full chunk plus
    // a 1-trial remainder, and several chunks plus a remainder.
    for (int iters : {1, 65535, 65536, 65537, 3 * 65536 + 7}) {
        RangeEquityResult ref;
        bool have_ref = false;
        // 16 threads against iterations = 1 (a single chunk) must also work:
        // the resolver clamps the worker count to the chunk count.
        for (int threads : {1, 2, 3, 7, 8, 16}) {
            SimulationOptions o = SimulationOptions::mc_seeded(iters, 777);
            o.threads = threads;
            RangeEquityResult r = calculate_range_equity(
                hero, vill, board, o, RangeEquityMode::AggregateOnly);
            if (!have_ref) {
                ref = r;
                have_ref = true;
                continue;
            }
            INFO("iters = ", iters, " threads = ", threads);
            CHECK(r.hero_aggregate_equity == ref.hero_aggregate_equity);
            CHECK(r.aggregate_std_error == ref.aggregate_std_error);
            CHECK(r.trials == ref.trials);
        }
    }
}

TEST_CASE("AggregateOnly: mutually blocking ranges throw from threaded path") {
    // Single-combo ranges sharing a card: every pair draw collides, so every
    // chunk hits the rejection cap and throws. The per-worker exception_ptr
    // capture must surface that to the caller after the join instead of
    // terminating the process.
    const Range hero = Range::from_string("AsAh");
    const Range vill = Range::from_string("AsKs");
    // 200000 iterations => 4 chunks, and >= the auto-threading threshold.
    SimulationOptions o = SimulationOptions::mc_seeded(200000, 42);
    o.threads = 4;
    CHECK_THROWS_AS(calculate_range_equity(hero, vill, 0, o,
                                           RangeEquityMode::AggregateOnly),
                    std::runtime_error);
}

TEST_CASE("AggregateOnly: serial small runs keep the pre-chunking stream") {
    // Regression pin: iterations <= 65536 with threads = 1 is a single chunk
    // whose derived seed is the master seed itself (derive(s, 0) == s), so the
    // chunked implementation must reproduce the pre-chunking serial stream
    // bit-for-bit. The literals were measured at the Task-1 commit 68eb466
    // (alias sampler, before chunked substreams) and must never move.
    // Hex float literals, not decimals: the comparison is bit-exact.
    //
    // kPinnedStdError is compared via ulp_distance(..) <= 2 rather than ==:
    // equity_range.cpp derives std_error through
    // `var = equity_sum_sq / n - mean * mean`, and GCC's default
    // -ffp-contract=fast fuses that multiply-subtract into a single FMA,
    // shifting the final rounding by up to a couple of ULPs relative to the
    // two-step double rounding this literal was captured under (same
    // FMA-sensitive shape as the std_error pins in
    // test_equity_threading.cpp, measured on WSL Ubuntu / GCC 13 -O3). The
    // hex-float literal is kept as the recorded baseline; only the
    // comparison is loosened. Stream-invariance is still pinned bit-exact by
    // the trials/hero_aggregate_equity CHECKs alongside it -- cross-compiler
    // bit-identity of a derived (sqrt-based) statistic is not part of this
    // library's contract.
    const double kPinnedEquity = 0x1.e0e8p-2;             // 0.46963500976562500
    const double kPinnedStdError = 0x1.e345fd83468abp-10; // 0.00184354430044296
    const Range hero = Range::from_string("AhKh");
    const Range vill = Range::from_string("JJ+,AQs+,KQs");
    SimulationOptions o = SimulationOptions::mc_seeded(65536, 777);
    o.threads = 1;
    const RangeEquityResult r = calculate_range_equity(
        hero, vill, 0, o, RangeEquityMode::AggregateOnly);
    CHECK(r.trials == 65536ULL);
    CHECK(r.hero_aggregate_equity == kPinnedEquity);
    CHECK(ulp_distance(r.aggregate_std_error, kPinnedStdError) <= 2);
}

TEST_CASE("AggregateOnly: chunks past the first run distinct substreams") {
    // Sharp detector for a broken seed derivation. If chunk 1 replayed chunk
    // 0's stream, the 2-chunk run would be chunk 0's trials twice: its sum and
    // its trial count would both double, and mean = 2*sum / 2*n is bit-exactly
    // mean = sum/n (both scalings are exact powers of two). So equality of the
    // two means below is essentially a proof of duplicated substreams, and
    // inequality rules it out.
    const Range hero = Range::from_string("AhKh");
    const Range vill = Range::from_string("JJ+,AQs+,KQs");
    SimulationOptions one = SimulationOptions::mc_seeded(65536, 777);
    one.threads = 1;
    SimulationOptions two = SimulationOptions::mc_seeded(2 * 65536, 777);
    two.threads = 1;
    const RangeEquityResult r1 = calculate_range_equity(
        hero, vill, 0, one, RangeEquityMode::AggregateOnly);
    const RangeEquityResult r2 = calculate_range_equity(
        hero, vill, 0, two, RangeEquityMode::AggregateOnly);
    CHECK(r2.trials == 2ULL * 65536ULL);
    CHECK(r1.hero_aggregate_equity != r2.hero_aggregate_equity);
    // Both estimate the same quantity, so they must still agree to a few
    // standard errors (guards against chunk 1 sampling something else).
    CHECK(std::abs(r1.hero_aggregate_equity - r2.hero_aggregate_equity) <
          5.0 * r1.aggregate_std_error);
}

TEST_CASE("AggregateOnly: auto threads and XIAPL_NUM_THREADS never move the result") {
    // threads = 0 is the default, so the auto path (and its environment
    // override) has to satisfy the same bit-exactness contract as an explicit
    // worker count. Iterations are above the 200000 auto-threading threshold
    // so auto actually fans out.
    const Range hero = Range::from_string("AA,KK,QQ,AKs,T9s");
    const Range vill = Range::from_string("22+,A2s+,KTs+,QJs,ATo+");
    SimulationOptions serial = SimulationOptions::mc_seeded(250000, 99);
    serial.threads = 1;
    const RangeEquityResult ref = calculate_range_equity(
        hero, vill, 0, serial, RangeEquityMode::AggregateOnly);

    // Guard restores whatever the environment already had (in its
    // destructor) so this test leaves the process exactly as it found it —
    // an embedder's XIAPL_NUM_THREADS must not be clobbered for the tests that
    // run after this one, even if a CHECK/call below this point fails or
    // throws mid-sweep.
    const XiaplNumThreadsEnvGuard env_guard;

    SimulationOptions autos = serial;
    autos.threads = 0;
    // Includes a malformed value, which must be ignored (falling back to
    // hardware concurrency) rather than parsed into a bogus worker count.
    for (const char* env : {static_cast<const char*>(nullptr), "1", "3",
                            "not-a-number", "0", "-4"}) {
        if (env == nullptr) {
            test_unset_env("XIAPL_NUM_THREADS");
        } else {
            test_set_env("XIAPL_NUM_THREADS", env);
        }
        const RangeEquityResult r = calculate_range_equity(
            hero, vill, 0, autos, RangeEquityMode::AggregateOnly);
        INFO("XIAPL_NUM_THREADS = ", env == nullptr ? "(unset)" : env);
        CHECK(r.hero_aggregate_equity == ref.hero_aggregate_equity);
        CHECK(r.aggregate_std_error == ref.aggregate_std_error);
        CHECK(r.trials == ref.trials);
    }

    // Negative thread counts are documented as serial, not as an error.
    SimulationOptions negative = serial;
    negative.threads = -4;
    const RangeEquityResult neg = calculate_range_equity(
        hero, vill, 0, negative, RangeEquityMode::AggregateOnly);
    CHECK(neg.hero_aggregate_equity == ref.hero_aggregate_equity);
}

TEST_CASE("PerCombo: negative threads means serial, not auto") {
    // SimulationOptions::threads documents "<0 = treated as 1" for every path.
    // PerCombo previously funnelled any n <= 0 into its auto branch, so a
    // negative value silently fanned out to hardware concurrency, which used
    // to only agree with serial to ~1e-9 (the old per-thread float-reduction
    // caveat). Fixed row-CHUNK-COUNT accumulation (kPerComboMaxRowChunks, see
    // equity_range.cpp) has since made PerCombo bit-identical at every threads
    // value -- which means this test can NO LONGER distinguish "negative maps
    // to serial" from "negative maps to auto" (or to any other resolved
    // worker count): every one of those now produces the identical result by
    // construction, so equality here is expected regardless of which N the
    // resolver actually picked. What this test still pins, honestly: negative
    // `threads` is ACCEPTED (no crash / no throw) and its result matches
    // every other threads value, which is real API-surface coverage on its
    // own (a naive `nthreads`-as-array-index bug, for instance, would not be
    // caught by the "PerCombo exact: bit-identical across thread counts"
    // tests above, none of which pass a negative value). The actual
    // mapping-level contract -- negative resolves to exactly 1, not to the
    // auto branch -- lives in the local `resolve_threads` lambda inside
    // enumerate_range_equity (equity_range.cpp, "if (hint < 0) return
    // 1;"), which has no standalone decision-table test of its own because it
    // is a private lambda, unlike the analogous MC-path resolver
    // (`resolve_mc_threads`, table-tested in tests/test_mc_chunking.cpp, row
    // "negative means serial by contract" -- a related but DIFFERENT
    // function on a different code path, not a direct guard for this one).
    // Ranges are deliberately wide: PerCombo's auto mode only fans out at
    // >= 1e7 pair-board evaluations, so a small spot would make this test
    // vacuous (auto would pick 1 worker anyway and old/new behavior would
    // agree). Here ~114 hero x ~110 villain combos after board blockers x
    // C(49,2) = 1176 boards (remain_mask strips only the 3 board cards)
    // is ~1.47e7, comfortably over the threshold.
    const Range hero = Range::from_string("22+,A2s+");
    const Range vill = Range::from_string("22+,K2s+");
    const std::uint64_t flop = mk("Th") | mk("5d") | mk("2c");

    SimulationOptions serial = SimulationOptions::exact();
    serial.threads = 1;
    SimulationOptions negative = SimulationOptions::exact();
    negative.threads = -4;

    const RangeEquityResult r1 = calculate_range_equity(hero, vill, flop, serial);
    const RangeEquityResult rn = calculate_range_equity(hero, vill, flop, negative);

    CHECK(rn.hero_aggregate_equity == r1.hero_aggregate_equity);
    CHECK(rn.villain_aggregate_equity == r1.villain_aggregate_equity);
    REQUIRE(rn.hero.size() == r1.hero.size());
    for (std::size_t i = 0; i < r1.hero.size(); ++i) {
        CHECK(rn.hero[i].combo_mask == r1.hero[i].combo_mask);
        CHECK(rn.hero[i].equity == r1.hero[i].equity);
    }
}

TEST_CASE("PerCombo exact: bit-identical across thread counts") {
    // Fixed row-CHUNK-COUNT accumulation (kPerComboMaxRowChunks in
    // equity_range.cpp: num_row_chunks = min(hero.size(), 64), rows per chunk
    // = ceil(hero.size() / num_row_chunks)) makes the float grouping a pure
    // function of hero.size() alone, never of the worker count -- unlike the
    // old per-thread split this replaced, which only agreed to ~1e-9 across
    // thread counts.
    //
    // Two spots, both hand-verified against the actual engine (via the
    // Python bindings, same equity_range.cpp code path) to exercise different
    // chunk-count regimes:
    //   - "small": 44 hero combos x 66 villain combos -> num_row_chunks ==
    //     44 (< the 64 cap), rows_per_chunk == 1, every chunk exactly 1 row
    //     (no partial chunk possible at rows_per_chunk == 1).
    //   - "wide, uneven last chunk": 249 hero combos x 69 villain combos ->
    //     num_row_chunks == 64 (capped), rows_per_chunk == 4, and 249 is not
    //     a multiple of 4 * 64 (or of any rows_per_chunk-sized boundary), so
    //     the 64th chunk holds only 1 row instead of 4 -- covers the
    //     ceil-division boundary arithmetic (chunk i0/i1 clamped by
    //     std::min) under assertion, not just chunk counting.
    // villain[j].equity is asserted too: villain_aggregate_equity is just
    // 1 - hero_aggregate_equity and carries no information about vill_acc,
    // which is written by the SAME chunked reduction as hero_acc and would
    // stay unverified without a direct per-combo check.
    struct Spot {
        const char* label;
        Range hero;
        Range vill;
        std::uint64_t board;
    };
    const std::uint64_t flop = mk("2h") | mk("7d") | mk("9s");
    const Spot spots[] = {
        {"small (44x66, 44 single-row chunks)",
         Range::from_string("AA,KK,QQ,JJ,AKs,AQs,AKo"),
         Range::from_string("TT+,AQs+,KQs,AQo+"), flop},
        {"wide, uneven last chunk (249x69, 64 chunks of 4 rows, last has 1)",
         Range::from_string("22+,A2s+,A2o+"), Range::from_string("22+"), flop},
    };
    for (const Spot& spot : spots) {
        CAPTURE(spot.label);
        SimulationOptions ref_opt = SimulationOptions::exact();
        ref_opt.threads = 1;
        const RangeEquityResult ref =
            calculate_range_equity(spot.hero, spot.vill, spot.board, ref_opt);
        for (int t : {2, 3, 8}) {
            SimulationOptions o = SimulationOptions::exact();
            o.threads = t;
            const RangeEquityResult r =
                calculate_range_equity(spot.hero, spot.vill, spot.board, o);
            REQUIRE(r.hero.size() == ref.hero.size());
            REQUIRE(r.villain.size() == ref.villain.size());
            for (std::size_t i = 0; i < ref.hero.size(); ++i) {
                CAPTURE(t); CAPTURE(i);
                CHECK(r.hero[i].equity == ref.hero[i].equity);  // bit equality
            }
            for (std::size_t j = 0; j < ref.villain.size(); ++j) {
                CAPTURE(t); CAPTURE(j);
                CHECK(r.villain[j].equity == ref.villain[j].equity);  // bit equality
            }
            CHECK(r.hero_aggregate_equity == ref.hero_aggregate_equity);
            CHECK(r.villain_aggregate_equity == ref.villain_aggregate_equity);
        }
    }
}

TEST_CASE("Hold'em PerCombo exact: narrow spot board-split agrees across thread counts") {
    // 1x1 combos with threads > unique hands forces the board-slice split
    // path for the Hold'em fill (split_boards in equity_range.cpp), which
    // previously had no direct test -- the PLO equivalent is "PLO PerCombo
    // exact: narrow spot agrees across thread counts" in
    // tests/test_plo_range_equity.cpp.
    const Range hero = Range::from_string("AsAh");
    const Range vill = Range::from_string("KsKh");
    const std::uint64_t flop = mk("2h") | mk("7d") | mk("9s");     // many boards
    const std::uint64_t river = flop | mk("Jc") | mk("3d");        // clamp path: 1 board
    for (std::uint64_t board : {flop, river}) {
        SimulationOptions o1 = SimulationOptions::exact();
        o1.threads = 1;
        SimulationOptions o8 = SimulationOptions::exact();
        o8.threads = 8;
        const RangeEquityResult a = calculate_range_equity(hero, vill, board, o1);
        const RangeEquityResult b = calculate_range_equity(hero, vill, board, o8);
        REQUIRE(a.hero.size() == 1);
        REQUIRE(b.hero.size() == 1);
        CHECK(a.hero[0].equity == b.hero[0].equity);
        CHECK(a.hero_aggregate_equity == b.hero_aggregate_equity);
        CHECK(a.villain_aggregate_equity == b.villain_aggregate_equity);
    }
}

TEST_CASE("range equity mode: AggregateOnly exact enumeration") {
    auto hero = Range::from_string("AhKh");
    auto vill = Range::from_string("QQ");
    // River board (5 cards) => tiny exact space; cards avoid Ah/Kh/Q ranks
    // so no hero/villain combos are blocked.
    std::uint64_t board =
        mk("2c") | mk("3d") | mk("4h") | mk("8s") | mk("9c");
    SimulationOptions opt;  // iterations = 0 => exact
    auto full = calculate_range_equity(hero, vill, board, opt, RangeEquityMode::PerCombo);
    auto agg = calculate_range_equity(hero, vill, board, opt, RangeEquityMode::AggregateOnly);
    CHECK(agg.exact);
    CHECK(agg.hero.empty());
    CHECK(agg.hero_aggregate_equity == full.hero_aggregate_equity);
    CHECK(agg.aggregate_std_error == 0.0);
}

namespace {

// Per-scenario result of the 100-seed aggregate-z sweep below: the pooled
// mean z-score and the count of per-seed outliers (|z| > 3).
struct BiasBatteryResult {
    double mean_z = 0.0;
    int outliers = 0;
};

// Sweeps the sampled-pair MC estimator over 100 seeds against a fixed exact
// ground truth and pools the per-seed z-scores (z_i = (mc_mean_i - exact) /
// mc_se_i) instead of asserting each run individually: asserting every run
// against a per-run bound (e.g. 4 sigma) has ~0.6% flake per run and is weak
// against a small systematic bias, since 100 independent per-run checks
// don't accumulate evidence with each other. Pooling into a mean-z and an
// outlier-count check is far more sensitive to a real (if small) bias while
// staying effectively zero-flake at a fixed seed sweep.
BiasBatteryResult run_bias_battery(const Range& hero, const Range& vill,
                                   std::uint64_t board, int mc_iterations) {
    SimulationOptions ex;  // iterations = 0 => exact
    auto truth = calculate_range_equity(hero, vill, board, ex,
                                        RangeEquityMode::PerCombo);
    const double exact_equity = truth.hero_aggregate_equity;

    const int kNumSeeds = 100;
    std::vector<double> z(kNumSeeds);
    for (int s = 0; s < kNumSeeds; ++s) {
        SimulationOptions mc;
        mc.iterations = mc_iterations;
        mc.deterministic = true;
        mc.seed = static_cast<std::uint64_t>(s + 1);  // avoid seed 0 special-case bias
        auto agg = calculate_range_equity(hero, vill, board, mc,
                                          RangeEquityMode::AggregateOnly);
        REQUIRE(agg.aggregate_std_error > 0.0);
        z[s] = (agg.hero_aggregate_equity - exact_equity) / agg.aggregate_std_error;
    }

    BiasBatteryResult result;
    double sum_z = 0.0;
    for (double zi : z) {
        sum_z += zi;
        if (std::abs(zi) > 3.0) ++result.outliers;
    }
    result.mean_z = sum_z / kNumSeeds;
    return result;
}

}  // namespace

// This exercises the alias-method sampler wired into
// sample_range_equity in place of the old WeightedComboPicker
// binary search: a correct replacement must reproduce the same weighted-pair
// distribution, so z should be centered at 0 with unit-ish spread across
// seeds regardless of which picker draws the pair.
//
// Two arms, both run through the same 100-seed battery:
//   - unweighted: villain combos are all weight 1.0 and hero is a single
//     combo, so BOTH pickers take AliasPicker's uniform bounded(n) fast
//     path and this arm never touches the weighted alias table at all.
//   - weighted: villain has several distinct explicit weights, forcing the
//     villain-side picker through the full alias-table construction and
//     pick() path this task actually added.
TEST_SUITE("slow") {
TEST_CASE("range equity mode: AggregateOnly bias battery (100-seed aggregate z)") {
    SUBCASE("unweighted villain range (uniform picker fast path)") {
        auto hero = Range::from_string("AhKh");
        auto vill = Range::from_string("JJ+,AQs+");
        std::uint64_t board = mk("2c") | mk("3d") | mk("4h") | mk("8s") | mk("9c");
        const BiasBatteryResult r = run_bias_battery(hero, vill, board, 20000);
        INFO("mean_z = ", r.mean_z, " outliers(|z|>3) = ", r.outliers);
        CHECK(std::abs(r.mean_z) < 0.3);
        CHECK(r.outliers <= 2);
    }

    SUBCASE("weighted villain range (exercises the alias table)") {
        auto hero = Range::from_string("AhKh");
        auto vill = Range::from_string("AA:0.25,QQ,JJ:0.6,AKs:0.35,T9s");
        // Low, disconnected board: avoids A/K/Q/J/T/9 ranks entirely so no
        // villain combo above is blocked (all weights stay in play) and
        // stays clear of hero's Ah/Kh.
        std::uint64_t board = mk("2c") | mk("3d") | mk("5h") | mk("7s") | mk("8c");
        const BiasBatteryResult r = run_bias_battery(hero, vill, board, 20000);
        INFO("mean_z = ", r.mean_z, " outliers(|z|>3) = ", r.outliers);
        CHECK(std::abs(r.mean_z) < 0.3);
        CHECK(r.outliers <= 2);
    }
}
}  // TEST_SUITE("slow")
