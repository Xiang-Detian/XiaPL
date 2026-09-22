// xiapl.simulation, implemented against the C ABI waist (<xiapl/c_api.h>)
// instead of the C++ API.
//
// INCLUDE PURITY: see binding/core_common.h. Nothing but the waist header,
// pybind11 and the standard library may be included here; tests/
// check_binding_includes.sh enforces it.
//
// This is the last of the seven public modules to switch, and the only one
// whose entry points RELEASE THE GIL. That makes the error protocol the
// interesting part of the file: raising needs the GIL, the waist reports a
// failure as a status code plus a thread-local message, and the message must
// be read on the calling thread before any other waist call can overwrite it
// (boundary convention 8(b)). See `Outcome` below for how the two are
// reconciled.

#include "core_common.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace py = pybind11;

namespace xiapl_py {
namespace {

// ---------------------------------------------------------------------------
// GIL-free error capture (boundary convention 8(b))
//
// check_status() raises immediately, which is only legal while the GIL is
// held; the two entry points below do their waist call with the GIL RELEASED
// so that other Python threads keep running through a multi-second Monte
// Carlo. So the failure is captured -- status code AND message -- into plain
// C++ storage inside the released region and re-raised after the GIL comes
// back.
//
// Releasing the GIL does not move the call to another OS thread, so the TLS
// read below is the same-thread read the convention requires. What the
// convention actually constrains is ORDER: the message must be read before any
// further waist call on this thread, which is why `capture` reads it right at
// the failing call rather than after the release scope ends.
// ---------------------------------------------------------------------------
struct Outcome {
    std::int32_t status = XIAPL_OK;
    std::string message;  // meaningful only when status != XIAPL_OK

    bool ok() const { return status == XIAPL_OK; }
};

Outcome capture(std::int32_t status) {
    Outcome outcome;
    outcome.status = status;
    if (status != XIAPL_OK) {
        const char* message = xiapl_last_error_message();
        if (message != nullptr) outcome.message = message;
    }
    return outcome;
}

// Re-raise a captured failure. Must be called with the GIL held.
void rethrow(const Outcome& outcome) {
    if (!outcome.ok()) raise_status_message(outcome.status, outcome.message.c_str());
}

// ---------------------------------------------------------------------------
// PySimOptions -- the Python `SimulationOptions`
//
// A binding-local value type with the same four mutable fields, and crucially
// with the same C++ FIELD TYPES as the xiapl::SimulationOptions it replaces
// (int / uint64_t / bool / int) rather than the ABI struct's own
// (int32_t / uint64_t / int32_t / int32_t). pybind renders and validates
// def_readwrite from the field type, so holding an xiapl_sim_options_t
// directly would turn `options.deterministic` from a bool into an int and
// change what the setter accepts.
//
// `reserved` is not exposed: the ABI requires it to be 0 and the C++ struct
// this replaces had no such member.
// ---------------------------------------------------------------------------
struct PySimOptions {
    int iterations = 0;
    std::uint64_t seed = 0;
    bool deterministic = false;
    int threads = 0;
};

xiapl_sim_options_t to_waist_options(const PySimOptions& options) {
    xiapl_sim_options_t out;
    out.seed = options.seed;
    out.iterations = static_cast<std::int32_t>(options.iterations);
    out.deterministic = options.deterministic ? 1 : 0;
    out.threads = static_cast<std::int32_t>(options.threads);
    out.reserved = 0;
    return out;
}

PySimOptions from_waist_options(const xiapl_sim_options_t& options) {
    PySimOptions out;
    out.seed = options.seed;
    out.iterations = static_cast<int>(options.iterations);
    out.deterministic = options.deterministic != 0;
    out.threads = static_cast<int>(options.threads);
    return out;
}

// The three factories go through the waist so that "what a well-formed exact /
// random / seeded options block looks like" has ONE definition (c_api.h,
// "Simulation options"). The default constructor deliberately does not: the
// ABI's permanent invariant is that a zero-initialized block IS the library
// default, which is exactly what PySimOptions' member initializers spell.
template <class Make>
PySimOptions make_options(Make&& make) {
    xiapl_sim_options_t raw = {};
    check_status(make(&raw));
    return from_waist_options(raw);
}

// ---------------------------------------------------------------------------
// Result types
//
// Binding-local mirrors of the C++ result structs, field for field and in the
// same order (simulation.pyi is the contract). They are filled from the
// waist's SoA columns plus its summary PODs; nothing here is a handle, so all
// four are plain values that pybind copies into Python.
// ---------------------------------------------------------------------------
struct PyPlayerEquity {
    double winrate = 0.0;
    double equity = 0.0;
    double std_error = 0.0;
};

struct PyEquityResult {
    std::vector<PyPlayerEquity> players;
    double chop_rate = 0.0;
    std::uint64_t trials = 0;
    bool exact = false;
};

struct PyRangeEquityEntry {
    std::uint64_t combo_mask = 0;
    double equity = 0.0;
    double weight = 1.0;
};

struct PyRangeEquityResult {
    std::vector<PyRangeEquityEntry> hero;
    std::vector<PyRangeEquityEntry> villain;
    double hero_aggregate_equity = 0.0;
    double villain_aggregate_equity = 0.0;
    std::uint64_t trials = 0;
    bool exact = false;
    double aggregate_std_error = 0.0;
};

// The Python-visible enums, binding-local for the same reason PyGame is, and
// with the same uint8_t underlying type as the xiapl:: enums they replace --
// which is what keeps SimulationMode(300) and RangeEquityMode(-1) rejected by
// pybind's integer caster while SimulationMode(255) builds an unnamed value.
enum class PySimMode : std::uint8_t {
    Exact            = XIAPL_SIM_EXACT,
    MonteCarloRandom = XIAPL_SIM_MONTE_CARLO_RANDOM,
    MonteCarloSeeded = XIAPL_SIM_MONTE_CARLO_SEEDED,
};

enum class PyRangeEquityMode : std::uint8_t {
    PerCombo      = XIAPL_RANGE_EQUITY_PER_COMBO,
    AggregateOnly = XIAPL_RANGE_EQUITY_AGGREGATE_ONLY,
};

// ---------------------------------------------------------------------------
// calculate_range_equity's game-agreement guard
//
// Hoisted into the binding for the same reason Task 7 hoisted the set-algebra
// one (binding/core_range.cpp): a PyRange carries its own Python-visible tag,
// which for the raw-combo constructor can be a GameType outside
// {Holdem, Plo}, while the handle it wraps carries only the normalized tag.
// The waist therefore cannot tell a GameType(2) range from a Hold'em one and
// can no longer make this call.
//
// The message is reproduced from src/core/equity_range.cpp verbatim, including
// the fact that game_name (binding/core_common.h) renders an unnamed tag as
// "Hold'em" -- so a GameType(2) hero against a Hold'em villain reports both
// sides as Hold'em, exactly as the C++ routine does today. Six fingerprint
// probes compare it byte for byte. Checked FIRST, ahead of the iterations and
// board validations, which is the order the C++ routine uses.
void require_same_game(const PyRange& hero, const PyRange& villain) {
    if (hero.game() != villain.game()) {
        const std::string message =
            std::string("calculate_range_equity: hero range is ") +
            game_name(hero.game()) + " but villain range is " +
            game_name(villain.game()) +
            "; both ranges must be the same game";
        raise_value_error(message.c_str());
    }
}

// ---------------------------------------------------------------------------
// Range equity result handle
//
// Unlike everything else at this boundary the range-equity result is a HANDLE:
// the per-combo breakdown's cardinality is not knowable before the computation
// and a query pass would mean running the simulation twice (c_api.h,
// "Range-vs-range equity"). It is owned for the length of one call only, and
// only inside the GIL-released region, so a scope guard is all it needs.
// ---------------------------------------------------------------------------
class RangeEquityGuard {
public:
    RangeEquityGuard() = default;
    RangeEquityGuard(const RangeEquityGuard&) = delete;
    RangeEquityGuard& operator=(const RangeEquityGuard&) = delete;
    ~RangeEquityGuard() { xiapl_range_equity_destroy(handle_); }

    xiapl_range_equity_t** slot() { return &handle_; }
    const xiapl_range_equity_t* get() const { return handle_; }

private:
    xiapl_range_equity_t* handle_ = nullptr;
};

// Query-then-fill (convention 4) around either breakdown emitter. Same shape
// as core_range.cpp's fetch_combos: the FILL pass's own total sizes the
// result, clamped to the buffers actually allocated, so a list that changed
// size between the passes cannot leave uninitialized entries behind. Returns
// the captured outcome instead of raising -- the caller runs without the GIL.
template <class Emit>
Outcome fetch_entries(Emit&& emit, std::vector<PyRangeEquityEntry>* out) {
    std::int32_t total = 0;
    Outcome outcome = capture(emit(nullptr, nullptr, nullptr, 0, &total));
    if (!outcome.ok() || total <= 0) return outcome;

    std::vector<std::uint64_t> masks(static_cast<std::size_t>(total));
    std::vector<double> equities(static_cast<std::size_t>(total));
    std::vector<double> weights(static_cast<std::size_t>(total));
    std::int32_t filled = 0;
    outcome = capture(emit(masks.data(), equities.data(), weights.data(),
                           total, &filled));
    if (!outcome.ok()) return outcome;
    filled = std::min(std::max(filled, 0), total);

    out->resize(static_cast<std::size_t>(filled));
    for (std::int32_t i = 0; i < filled; ++i) {
        const std::size_t index = static_cast<std::size_t>(i);
        (*out)[index].combo_mask = masks[index];
        (*out)[index].equity = equities[index];
        (*out)[index].weight = weights[index];
    }
    return outcome;
}

}  // namespace

py::module_ register_simulation_gametype(py::module_& parent) {
    auto sim_mod = parent.def_submodule("simulation", "Equity simulation");

    // GameType is registered here, at its home module, ahead of every module
    // that names it: pybind renders signature text and casts default argument
    // values at BIND time, so xiapl.eval's evaluate_hand/judge, xiapl.range's
    // Range.all/from_string and this module's own calculate_equity all need
    // the enum's type_info to exist already.
    py::enum_<PyGame>(sim_mod, "GameType")
        .value("Holdem", PyGame::Holdem)
        .value("Plo", PyGame::Plo);

    return sim_mod;
}

void register_simulation_module(py::module_& sim_mod) {
    py::enum_<PySimMode>(sim_mod, "SimulationMode")
        .value("Exact", PySimMode::Exact)
        .value("MonteCarloRandom", PySimMode::MonteCarloRandom)
        .value("MonteCarloSeeded", PySimMode::MonteCarloSeeded);

    py::class_<PySimOptions>(sim_mod, "SimulationOptions")
        .def(py::init<>())
        .def_readwrite("iterations", &PySimOptions::iterations)
        .def_readwrite("seed", &PySimOptions::seed)
        .def_readwrite("deterministic", &PySimOptions::deterministic)
        .def_readwrite("threads", &PySimOptions::threads)
        // Derived, never stored: there is no `mode` field precisely so that a
        // stored copy cannot contradict the fields it comes from.
        .def("effective_mode",
             [](const PySimOptions& options) {
                 const xiapl_sim_options_t raw = to_waist_options(options);
                 std::int32_t mode = XIAPL_SIM_EXACT;
                 check_status(xiapl_sim_options_effective_mode(&raw, &mode));
                 return static_cast<PySimMode>(mode);
             })
        .def_static("exact",
             []() {
                 return make_options([](xiapl_sim_options_t* out) {
                     return xiapl_sim_options_exact(out);
                 });
             })
        .def_static("mc_random",
             [](std::int32_t iterations) {
                 return make_options([&](xiapl_sim_options_t* out) {
                     return xiapl_sim_options_mc_random(iterations, out);
                 });
             },
             py::arg("iterations"))
        .def_static("mc_seeded",
             [](std::int32_t iterations, std::uint64_t seed) {
                 return make_options([&](xiapl_sim_options_t* out) {
                     return xiapl_sim_options_mc_seeded(iterations, seed, out);
                 });
             },
             py::arg("iterations"), py::arg("seed"));

    py::class_<PyPlayerEquity>(sim_mod, "PlayerEquity")
        .def_readonly("winrate", &PyPlayerEquity::winrate)
        .def_readonly("equity", &PyPlayerEquity::equity)
        .def_readonly("std_error", &PyPlayerEquity::std_error);

    py::class_<PyEquityResult>(sim_mod, "EquityResult")
        .def_readonly("players", &PyEquityResult::players)
        .def_readonly("chop_rate", &PyEquityResult::chop_rate)
        .def_readonly("trials", &PyEquityResult::trials)
        .def_readonly("exact", &PyEquityResult::exact);

    py::enum_<PyRangeEquityMode>(sim_mod, "RangeEquityMode")
        .value("PerCombo", PyRangeEquityMode::PerCombo)
        .value("AggregateOnly", PyRangeEquityMode::AggregateOnly)
        // Deprecated pre-0.1 spellings, kept as aliases; remove at 1.0.
        .value("PER_COMBO", PyRangeEquityMode::PerCombo)
        .value("AGGREGATE_ONLY", PyRangeEquityMode::AggregateOnly);

    py::class_<PyRangeEquityEntry>(sim_mod, "RangeEquityEntry")
        .def_readonly("combo_mask", &PyRangeEquityEntry::combo_mask)
        .def_readonly("equity", &PyRangeEquityEntry::equity)
        .def_readonly("weight", &PyRangeEquityEntry::weight);

    py::class_<PyRangeEquityResult>(sim_mod, "RangeEquityResult")
        .def_readonly("hero", &PyRangeEquityResult::hero)
        .def_readonly("villain", &PyRangeEquityResult::villain)
        .def_readonly("hero_aggregate_equity",
                      &PyRangeEquityResult::hero_aggregate_equity)
        .def_readonly("villain_aggregate_equity",
                      &PyRangeEquityResult::villain_aggregate_equity)
        .def_readonly("trials", &PyRangeEquityResult::trials)
        .def_readonly("exact", &PyRangeEquityResult::exact)
        .def_readonly("aggregate_std_error",
                      &PyRangeEquityResult::aggregate_std_error);

    // Both entry points can run for seconds (Monte Carlo with large
    // `iterations`) and both fan out over their own worker threads, so holding
    // the interpreter lock across them would stall every other Python thread
    // for no reason.
    //
    // The release is a SCOPE INSIDE the lambda rather than a
    // py::call_guard<py::gil_scoped_release> as it was before the switch,
    // because the failure path changed shape: the pre-waist binding let a C++
    // exception propagate out of the released region for pybind's translator
    // to convert with the GIL back in hand, whereas the waist reports failure
    // as a return value that this binding turns into a Python exception
    // itself -- and that needs the GIL. Everything that touches the waist,
    // including the result marshalling, sits inside the scope, so the released
    // window is the same one call_guard produced (pybind converts arguments
    // before the guard and the return value after it). The rendered signature
    // is unaffected: call_guard is not part of a function's signature text.
    sim_mod.def("calculate_equity",
                [](const std::vector<std::uint64_t>& hole_masks,
                   std::uint64_t board_mask, const PySimOptions& options,
                   PyGame game) {
                    PyEquityResult result;
                    Outcome outcome;
                    {
                        py::gil_scoped_release unlock;
                        const xiapl_sim_options_t raw = to_waist_options(options);
                        const std::int32_t players =
                            static_cast<std::int32_t>(hole_masks.size());
                        // Output cardinality is num_players, so there is no
                        // query pass: size the three columns up front and hand
                        // the waist all of them.
                        std::vector<double> winrate(hole_masks.size());
                        std::vector<double> equity(hole_masks.size());
                        std::vector<double> std_error(hole_masks.size());
                        xiapl_equity_summary_t summary{};
                        outcome = capture(xiapl_calculate_equity(
                            hole_masks.data(), players, board_mask, &raw,
                            game_code(game), winrate.data(), equity.data(),
                            std_error.data(), players, &summary));
                        if (outcome.ok()) {
                            result.players.resize(hole_masks.size());
                            for (std::size_t i = 0; i < hole_masks.size(); ++i) {
                                result.players[i].winrate = winrate[i];
                                result.players[i].equity = equity[i];
                                result.players[i].std_error = std_error[i];
                            }
                            result.chop_rate = summary.chop_rate;
                            result.trials = summary.trials;
                            result.exact = summary.exact != 0;
                        }
                    }
                    rethrow(outcome);
                    return result;
                },
                py::arg("hole_masks"), py::arg("board_mask"),
                py::arg("options"),
                py::kw_only(), py::arg("game") = PyGame::Holdem);

    sim_mod.def(
        "calculate_range_equity",
        [](const PyRange& hero_range, const PyRange& villain_range,
           std::uint64_t board_mask, const PySimOptions& options,
           PyRangeEquityMode mode) {
            // Ahead of the release, both because it raises and because the
            // C++ routine checks game agreement before anything else.
            require_same_game(hero_range, villain_range);

            PyRangeEquityResult result;
            Outcome outcome;
            {
                py::gil_scoped_release unlock;
                const xiapl_sim_options_t raw = to_waist_options(options);
                RangeEquityGuard guard;
                outcome = capture(xiapl_calculate_range_equity(
                    hero_range.handle(), villain_range.handle(), board_mask,
                    &raw, static_cast<std::int32_t>(mode), guard.slot()));
                if (outcome.ok()) {
                    xiapl_range_equity_summary_t summary{};
                    outcome = capture(
                        xiapl_range_equity_summary(guard.get(), &summary));
                    if (outcome.ok()) {
                        result.hero_aggregate_equity =
                            summary.hero_aggregate_equity;
                        result.villain_aggregate_equity =
                            summary.villain_aggregate_equity;
                        result.aggregate_std_error = summary.aggregate_std_error;
                        result.trials = summary.trials;
                        result.exact = summary.exact != 0;
                    }
                }
                if (outcome.ok()) {
                    outcome = fetch_entries(
                        [&](std::uint64_t* masks, double* equities,
                            double* weights, std::int32_t capacity,
                            std::int32_t* total) {
                            return xiapl_range_equity_hero(
                                guard.get(), masks, equities, weights,
                                capacity, total);
                        },
                        &result.hero);
                }
                if (outcome.ok()) {
                    outcome = fetch_entries(
                        [&](std::uint64_t* masks, double* equities,
                            double* weights, std::int32_t capacity,
                            std::int32_t* total) {
                            return xiapl_range_equity_villain(
                                guard.get(), masks, equities, weights,
                                capacity, total);
                        },
                        &result.villain);
                }
            }
            rethrow(outcome);
            return result;
        },
        py::arg("hero_range"), py::arg("villain_range"),
        py::arg("board_mask"), py::arg("options"),
        py::arg("mode") = PyRangeEquityMode::PerCombo);
}

}  // namespace xiapl_py
