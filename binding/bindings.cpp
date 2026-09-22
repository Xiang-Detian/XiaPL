// xiapl Python binding — EXPERIMENTAL. Build via `pip install -e .`.
// This module is not part of the stable `xiapl_core` contract; names and
// signatures may change across alpha releases.
//
// This file is the MODULE ROOT: it owns PYBIND11_MODULE, the exception
// translator and the registration ORDER of every submodule.
//
// The seven modules -- card, utils, eval, canonicalize, deck, range,
// simulation -- live in binding/core_*.cpp and reach the library only through
// the C ABI waist, <xiapl/c_api.h>. Their registration calls appear below in a
// load-bearing order, because pybind renders signature text and casts default
// argument values at BIND time.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

// The only C++ header this file needs: the module's own __version__ attribute.
// Everything else reaches the library through the waist, which is what
// tests/check_binding_includes.sh enforces.
#include <xiapl/version.h>

#include "core_common.h"

#include <exception>
#include <new>
#include <stdexcept>

namespace py = pybind11;

PYBIND11_MODULE(_xiapl, m) {
    m.doc() = "XiaPL (experimental Python binding for the modern C++ API).";
    m.attr("__version__") = XIAPL_VERSION_STRING;

    // Map C++ exception types to Python exceptions so callers get the
    // idiomatic ValueError / MemoryError instead of the catch-all
    // RuntimeError for every src/core failure.
    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p) std::rethrow_exception(p);
        } catch (const std::invalid_argument& e) {
            PyErr_SetString(PyExc_ValueError, e.what());
        } catch (const std::out_of_range& e) {
            PyErr_SetString(PyExc_IndexError, e.what());
        } catch (const std::bad_alloc& e) {
            PyErr_SetString(PyExc_MemoryError, e.what());
        }
        // Other exception types (std::runtime_error, etc.) fall through to
        // pybind11's default handler which maps them to RuntimeError.
    });

    // ---- xiapl.card ----
    // Implemented against the waist in binding/core_card_utils.cpp.
    //
    // Registered FIRST, ahead of every module that mentions a Card, and that
    // order is load-bearing: pybind renders a function's signature text at
    // BIND time, so a py::class_ that is not registered yet appears in the
    // docstring as its raw C++ type name. Verified by moving this call to the
    // end of this function: deck.set_cards, eval.evaluate_cards and
    // utils.cards_to_mask then advertise `list[xiapl_py::PyCard]` instead of
    // `list[xiapl._xiapl.card.Card]`, which fails the .pyi stubtest gate.
    // Same bind-time constraint that puts GameType ahead of its users below
    // (there it is a default argument value being cast, here the signature).
    xiapl_py::register_card_module(m);

    // ---- xiapl.deck ----
    // Implemented against the waist in binding/core_deck.cpp.
    xiapl_py::register_deck_module(m);

    // ---- xiapl.simulation (submodule + GameType only) ----
    // Implemented against the waist in binding/core_simulation.cpp. Split in
    // two because GameType has to be registered ahead of every module that
    // names it -- a `game` default argument value is py::cast at BIND time, so
    // eval's evaluate_hand/judge and range's Range.all/from_string both need
    // the enum's pybind type_info to exist already. The rest of
    // xiapl.simulation is registered further down.
    auto sim_mod = xiapl_py::register_simulation_gametype(m);

    // ---- xiapl.eval ----
    // Implemented against the waist in binding/core_eval_canon.cpp. The call
    // sits after the GameType registration it depends on (evaluate_hand/judge
    // cast a PyGame default argument at bind time).
    xiapl_py::register_eval_module(m);

    // ---- xiapl.range ----
    // Implemented against the waist in binding/core_range.cpp. Registered
    // AHEAD of the rest of xiapl.simulation: pybind renders signature text at
    // bind time, and calculate_range_equity takes two Ranges, so registering
    // range first is what makes its parameters render as
    // xiapl._xiapl.range.Range instead of the unregistered-type fallback name.
    // Nothing in the range module depends on the simulation types, only on
    // GameType above.
    xiapl_py::register_range_module(m);

    // ---- xiapl.simulation (the rest; sim_mod + GameType are above) ----
    // Implemented against the waist in binding/core_simulation.cpp.
    xiapl_py::register_simulation_module(sim_mod);

    // ---- xiapl.utils ----
    // Implemented against the waist in binding/core_card_utils.cpp.
    xiapl_py::register_utils_module(m);

    // ---- xiapl.canonicalize ----
    // Implemented against the waist in binding/core_eval_canon.cpp.
    xiapl_py::register_canonicalize_module(m);
}
