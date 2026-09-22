#pragma once

// Public equity-simulation API (Hold'em + PLO).
//
// Reproducible by construction:
//   - SimulationOptions::iterations == 0 -> exact enumeration.
//   - SimulationOptions::iterations  > 0 -> Monte Carlo;
//       deterministic + seed give bit-exact reproducibility. On the MC
//       sampling paths this holds for every `threads` value (chunked
//       substreams; see SimulationOptions::threads).
//   - The PerCombo evaluation path (calculate_range_equity's default mode)
//       is also bit-identical for every `threads` value, including serial.
//       Its two internal stages parallelize differently: the evaluation-
//       cache fill splits into worker-count slices where every (hand, board)
//       cell has exactly one owner, so any partition is bit-identical with
//       no reduction at all; the pair loop instead splits hero rows into a
//       small fixed NUMBER of chunks (a function of hero range width alone,
//       never of the worker count) and reduces the per-chunk accumulators in
//       a fixed chunk-index order. Either way the worker count never enters
//       the floating-point summation order. This applies whether PerCombo is
//       running exact enumeration (iterations == 0) or Monte Carlo
//       (iterations > 0) — both parallelize the same
//       evaluation-cache-fill + pair-loop code.

#include <cstdint>
#include <vector>

#include <xiapl/card.h>
#include <xiapl/game_type.h>
#include <xiapl/range.h>

namespace xiapl {

// Sampling mode for SimulationOptions. The mode is derived from
// (iterations, deterministic) for backward compatibility, but new code
// should set `mode` explicitly via `SimulationOptions::exact()` etc.
enum class SimulationMode : std::uint8_t {
    Exact = 0,                  // full enumeration; ignores seed
    MonteCarloRandom = 1,       // seed = random_device
    MonteCarloSeeded = 2,       // seed = options.seed (reproducible)
};

// Options controlling Monte Carlo / enumeration behavior.
//
// Recommended construction (forward-compatible):
//   auto opts = SimulationOptions::exact();
//   auto opts = SimulationOptions::mc_random(iterations);
//   auto opts = SimulationOptions::mc_seeded(iterations, seed);
//
// Legacy field-based construction is still supported:
//   - iterations == 0: exact enumeration (deterministic by construction).
//   - iterations  > 0 && deterministic == true:  seed-driven MC.
//   - iterations  > 0 && deterministic == false: random_device MC.
//
// Auto-fallback (calculate_equity and calculate_range_equity's PerCombo mode;
// see calculate_range_equity for the AggregateOnly exception): when
// iterations > 0 but iterations >= the number of distinct board completions
// (C(remaining_deck, num_to_draw)), the implementation silently runs exact
// enumeration instead — sampling with replacement past the exhaustive count
// is strictly slower and less accurate. EquityResult / RangeEquityResult.exact
// reflects what actually ran; callers can compare against options.iterations
// to detect a fallback.
//
// Note: calculate_equity's BOARD enumeration is the part that always runs
// single-threaded, on both the explicit-exact (iterations == 0) and the
// auto-fallback path above. Raising iterations past the enumeration
// threshold can therefore REDUCE calculate_equity's wall-clock parallelism
// while improving accuracy: the run switches from threaded MC sampling to
// serial exact board enumeration. Parallel exact board enumeration is a
// possible future extension (fixed combination-index ranges, no RNG).
// calculate_range_equity's exact path does NOT have this failure mode: it
// parallelizes its evaluation-cache fill and pair loop on options.threads in
// both PerCombo and AggregateOnly mode (see calculate_range_equity below;
// measured ~5x wall-clock at threads=8 on an 8-core dev machine, wide flop
// spot -- scales with core count and spot width). Both stages are
// bit-identical at every thread count, same PerCombo guarantee as above.
struct SimulationOptions {
    int iterations = 0;
    std::uint64_t seed = 0;
    bool deterministic = false;
    // Worker threads:
    //   0  = auto (see per-path heuristics below)
    //   1  = serial
    //   N  = exactly N workers
    //   <0 = treated as 1, on every path
    //
    // Monte Carlo SAMPLING paths (calculate_equity hand-vs-hand and multiway
    // MC; calculate_range_equity AggregateOnly MC): thread count NEVER changes
    // results. Trials are partitioned into fixed 65536-trial chunks, each with
    // an independent seed-derived RNG substream, reduced in chunk order — the
    // same seed gives bit-identical results at every threads value, including
    // serial. Auto parallelizes at iterations >= 200000 and honors the
    // XIAPL_NUM_THREADS environment variable (else hardware concurrency).
    //
    // The PerCombo per-combo evaluation path parallelizes work that is itself
    // deterministically seeded. Its pair loop splits hero rows into a small
    // fixed NUMBER of chunks (capped independent of `threads`; chunk count
    // and chunk boundaries are both pure functions of the hero range's
    // width) and reduces the per-chunk accumulators in a fixed chunk-index
    // order, so the summation order — and thus the last bits of the weighted
    // aggregates — is a function of hero range width only, never of the
    // worker count (the test suite pins this for bit equality). Its
    // auto mode uses its own work-size heuristic (>= 1e7 pair-board
    // evaluations) and does NOT read XIAPL_NUM_THREADS.
    //
    // Embedders running inside an outer thread pool (e.g. joblib) should pass
    // threads = 1. calculate_equity's board enumeration is serial on every
    // path regardless of this setting (see the auto-fallback note above);
    // calculate_range_equity's exact path is not serial — it parallelizes on
    // this field too (see calculate_range_equity below).
    int threads = 0;

    // Returns the effective mode based on the (iterations, deterministic)
    // pair. New code should use the factories below instead.
    SimulationMode effective_mode() const noexcept {
        if (iterations <= 0) return SimulationMode::Exact;
        return deterministic ? SimulationMode::MonteCarloSeeded
                             : SimulationMode::MonteCarloRandom;
    }

    static SimulationOptions exact() {
        SimulationOptions o;
        o.iterations = 0;
        return o;
    }
    static SimulationOptions mc_random(int iterations) {
        SimulationOptions o;
        o.iterations = iterations;
        o.deterministic = false;
        return o;
    }
    static SimulationOptions mc_seeded(int iterations, std::uint64_t seed) {
        SimulationOptions o;
        o.iterations = iterations;
        o.deterministic = true;
        o.seed = seed;
        return o;
    }
};

struct PlayerEquity {
    double winrate = 0.0;    // outright win rate (no chop share)
    double equity = 0.0;     // expected pot share (includes chop)
    double std_error = 0.0;  // MC standard error of equity; 0.0 for exact
};

struct EquityResult {
    std::vector<PlayerEquity> players;
    double chop_rate = 0.0;
    std::uint64_t trials = 0;  // number of board completions actually evaluated
    // True when full enumeration actually ran: iterations == 0, OR the
    // auto-fallback described above (iterations > 0 but >= the number of
    // distinct board completions). Compare against options.iterations to
    // detect a fallback.
    bool exact = false;
};

// Reproducible equity calculation (Hold'em / PLO, 2+ players).
// hole_masks[i] is a 52-bit mask of player i's hole cards
// (Hold'em: 2 bits set, PLO: 4 bits set).
// board_mask is a 52-bit mask with 0..5 bits set (only {0, 3, 4, 5} are
// legal; 1 and 2 throw std::runtime_error).
// game selects the showdown rule and the expected hole-card width; it
// defaults to Hold'em.
// options.threads is a pure speed knob on the Monte Carlo path (both the
// hand-vs-hand and the multiway simulator run in seed-derived 65536-trial
// chunks, so a given seed produces bit-identical output at every thread
// count); exact enumeration ignores it and runs serially.
//
// Player-count cap: at most 10 players for Hold'em, at most 32 for PLO
// (hole_masks.size() past either throws std::invalid_argument up front,
// before any Monte Carlo trial runs -- both caps are checked synchronously
// on the caller's thread, never from inside a worker). The Hold'em cap
// covers full-ring NLHE (10 seats); the PLO cap is the internal winner-mask
// representation width and is not reachable in practice (a legal PLO deal
// runs out of cards well before it, since each hand needs 4 unique cards).
EquityResult calculate_equity(
    const std::vector<std::uint64_t>& hole_masks,
    std::uint64_t board_mask,
    const SimulationOptions& options,
    GameType game = GameType::Holdem
);

// Per-combo equity for a range-vs-range matchup.
struct RangeEquityEntry {
    // Hand mask: 2 bits set for Hold'em, 4 for PLO.
    std::uint64_t combo_mask = 0;
    double equity = 0.0;           // winrate + chop/2 vs the opposing range
    double weight = 1.0;           // input combo weight (carried through)
};

// Output mode for calculate_range_equity.
//   PerCombo (default) - per-combo breakdown + CRN board set.
//   AggregateOnly - aggregate equity only; breakdown vectors (hero/villain)
//     are returned empty. With iterations > 0 this runs a sampled-pair Monte
//     Carlo estimator (one hero combo, one villain combo and one board per
//     trial) instead of the enumerate-every-pair engine, so it does not share
//     a random stream with PerCombo: the two modes agree statistically, not
//     bit-exactly. With iterations == 0 both modes run the same exact
//     enumeration and agree bit-exactly.
enum class RangeEquityMode : std::uint8_t {
    PerCombo = 0,
    AggregateOnly = 1,
};

// Aggregate result for calculate_range_equity (Hold'em or PLO, HU).
// `hero` and `villain` list per-combo equities (only entries that have at least
// one valid pairing with the opposing range are included).
// `hero_aggregate_equity` / `villain_aggregate_equity` are weighted averages
// over all compatible hero/villain combo pairs, using
// hero_combo.weight * villain_combo.weight.
struct RangeEquityResult {
    std::vector<RangeEquityEntry> hero;
    std::vector<RangeEquityEntry> villain;
    double hero_aggregate_equity = 0.0;
    double villain_aggregate_equity = 0.0;
    // Size of the *global* board sample set (boards enumerated against the
    // deck minus the fixed board). Per-combo valid board counts may be lower
    // because samples that overlap a specific hero/villain combo are skipped.
    // For exact mode: trials == C(52 - board_count, 5 - board_count).
    // AggregateOnly Monte Carlo instead reports the number of sampled
    // (hero combo, villain combo, board) trials, which is exactly
    // options.iterations: pairs rejected for sharing a card are redrawn, not
    // counted.
    std::uint64_t trials = 0;
    // True when full enumeration actually ran: iterations == 0, OR (PerCombo
    // only) the auto-fallback described above calculate_range_equity.
    // AggregateOnly Monte Carlo never auto-falls back, so for that mode this
    // is false whenever iterations > 0.
    bool exact = false;
    // MC standard error of hero_aggregate_equity; only filled by
    // AggregateOnly Monte Carlo. 0.0 for exact and for PerCombo.
    double aggregate_std_error = 0.0;
};

// Reproducible range-vs-range equity, heads-up.
//
// - Both ranges must carry the same GameType tag (Range::game()); a mismatch
//   throws std::invalid_argument naming both games. Holdem x Holdem and
//   Plo x Plo are supported; the game selects the showdown rule (PLO scores
//   the best five cards over exactly two hole cards and exactly three board
//   cards) and the combo width the two ranges are filtered to (2 / 4 cards).
// - board_mask: 52-bit mask with 0..5 bits set.
// - options.iterations: 0 => exact enumeration, >0 => Monte Carlo.
// - options.deterministic + seed: bit-exact MC reproducibility at every
//   threads value, for both AggregateOnly and PerCombo — see
//   SimulationOptions::threads.
// - mode: PerCombo (default) returns per-combo breakdowns in hero/villain.
//   AggregateOnly returns hero/villain empty and only the aggregate fields
//   populated (including aggregate_std_error for MC); it is intended for
//   callers that only need the aggregate equity and want to skip the
//   per-combo bookkeeping cost.
//   AggregateOnly Monte Carlo never auto-falls back to exact enumeration:
//   the pair dimension is sampled even when the board is fully specified,
//   so `exact` stays false and `trials == options.iterations`.
// - options.threads applies to the AggregateOnly Monte Carlo path as well as
//   to PerCombo and AggregateOnly exact enumeration. For AggregateOnly MC it
//   is purely a speed knob: trials run in fixed 65536-trial chunks with
//   seed-derived substreams reduced in chunk order, so a given seed produces
//   bit-identical output at every thread count (see SimulationOptions).
// - AggregateOnly Monte Carlo throws std::runtime_error when it cannot find a
//   card-compatible hero/villain combo pair (10,000 consecutive rejections);
//   PerCombo instead returns zeroed aggregates for such mutually blocking
//   ranges. (Two independent PLO combos share a card ~28% of the time, so
//   ordinary PLO ranges are nowhere near this guard; only genuinely blocking
//   ranges reach it.)
//
// Exact mode is capped at ~512 MB of evaluation cache; wide preflop ranges
// (no board) that would exceed this throw std::runtime_error. Use
// iterations > 0 (Monte Carlo) for those cases. Postflop exact is unaffected.
//
// PLO specifics:
// - AggregateOnly + Monte Carlo (mc_seeded / mc_random) is the primary PLO
//   path: it is O(iterations) with no evaluation cache, so it scales to any
//   range width. It carries the same threading contract as Hold'em — fixed
//   65536-trial chunks with seed-derived substreams reduced in chunk order,
//   hence bit-identical output at every options.threads value.
// - PLO exact / PerCombo works, but a PLO range expands into 4-card combos,
//   so the evaluation cache (unique hands x boards x 4 B) reaches the 512 MB
//   cap at far narrower ranges than in Hold'em. Wide PLO exact requests
//   therefore throw by design; the message names AggregateOnly + mc_seeded as
//   the remedy. Narrow spots (a handful of combos, or a turn/river board)
//   stay exact-feasible.
RangeEquityResult calculate_range_equity(
    const Range& hero_range,
    const Range& villain_range,
    std::uint64_t board_mask,
    const SimulationOptions& options,
    RangeEquityMode mode = RangeEquityMode::PerCombo
);

} // namespace xiapl
