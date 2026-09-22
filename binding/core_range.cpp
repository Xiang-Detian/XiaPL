// xiapl.range, implemented against the C ABI waist (<xiapl/c_api.h>) instead
// of the C++ API.
//
// INCLUDE PURITY: see binding/core_common.h. Nothing but the waist header,
// pybind11 and the standard library may be included here.
//
// The Python surface is byte-compatible with the pre-waist binding: the same
// names, the same signatures (range.pyi is the contract), the same exception
// types and -- the point of this module -- the same exception MESSAGES. The
// range notation parsers produce by far the richest error text in the library
// (Hold'em token errors, PLO v0.1 item errors, the std::stod-derived weight
// errors), and every one of them arrives here through
// xiapl_last_error_message() verbatim rather than being reworded.

#include "core_common.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace py = pybind11;

namespace xiapl_py {
namespace {

// The game tag a handle carries, mapped straight back to the Python enum.
// Only used for the constructor paths whose C++ counterpart normalizes the tag
// itself (from_string, all, generate_top_percent_range, try_parse_range); the
// raw-combo constructor keeps the value it was given instead. See PyRange.
PyGame read_tag(const xiapl_range_t* range) {
    std::int32_t game = XIAPL_GAME_HOLDEM;
    check_status(xiapl_range_game(range, &game));
    return static_cast<PyGame>(game);
}

// The tag agreement that xiapl::range_union and friends check first, hoisted
// to the binding because the tag it has to read now lives here: a range built
// through the raw-combo constructor can carry a GameType outside
// {Holdem, Plo}, and the waist -- which sees only the normalized tag -- can no
// longer tell such a range apart from a Hold'em one. Checked ahead of every
// other validation, exactly as require_same_game is (src/core/range_setops.cpp),
// so a duplicated mask in an operand of a mismatched pair still reports the
// game first.
void require_same_game(const PyRange& a, const PyRange& b, const char* fn_name) {
    if (a.game() != b.game()) {
        const std::string message =
            std::string(fn_name) + ": left range is " + game_name(a.game()) +
            " but right range is " + game_name(b.game()) +
            "; both ranges must be the same game";
        raise_value_error(message.c_str());
    }
}

// The (masks, weights) columns a raw-combo Range constructor takes.
//
// Split into two contiguous arrays because that is the shape the waist speaks
// (parallel arrays, convention 4). No stack fast path: a combo list is
// unbounded -- a full PLO range is 270,725 entries -- so there is no size that
// would cover "every real call" the way CardIdBuffer's 52 slots do.
//
// Unlike CardIdBuffer, masks()/weights() MAY be NULL: std::vector::data() is
// free to return NULL for an empty vector. That is harmless here and needs no
// dummy allocation, because the only NULL check
// xiapl_range_create_from_combos performs is itself guarded by count > 0 --
// an empty combo list builds an empty range either way. (CardIdBuffer cannot
// take that shortcut: several waist entry points reject a NULL id pointer
// outright, regardless of count.)
class ComboColumns {
public:
    explicit ComboColumns(const std::vector<PyCombo>& combos)
        : masks_(combos.size()), weights_(combos.size()) {
        for (std::size_t i = 0; i < combos.size(); ++i) {
            masks_[i] = combos[i].mask;
            weights_[i] = combos[i].weight;
        }
    }

    // Both accessors hand out pointers into members; copying would silently
    // change which. Nothing needs a copy (same rule as CardIdBuffer).
    ComboColumns(const ComboColumns&) = delete;
    ComboColumns& operator=(const ComboColumns&) = delete;

    const std::uint64_t* masks() const { return masks_.data(); }
    const double* weights() const { return weights_.data(); }
    std::int32_t size() const { return static_cast<std::int32_t>(masks_.size()); }

private:
    std::vector<std::uint64_t> masks_;
    std::vector<double> weights_;
};

// Query-then-fill (convention 4) around either of the waist's two combo
// emitters. `emit` is called twice: once with no buffers to learn the total,
// once to fill. The second call's own total is what sizes the result, so a
// list that shrank between the two passes cannot leave uninitialized entries
// behind -- the same defensive shape fetch_string uses for strings.
template <class Emit>
std::vector<PyCombo> fetch_combos(Emit&& emit) {
    std::int32_t total = 0;
    check_status(emit(nullptr, nullptr, 0, &total));
    if (total <= 0) return {};

    std::vector<std::uint64_t> masks(static_cast<std::size_t>(total));
    std::vector<double> weights(static_cast<std::size_t>(total));
    std::int32_t filled = 0;
    check_status(emit(masks.data(), weights.data(), total, &filled));
    if (filled > total) filled = total;
    if (filled < 0) filled = 0;

    std::vector<PyCombo> combos(static_cast<std::size_t>(filled));
    for (std::int32_t i = 0; i < filled; ++i) {
        combos[static_cast<std::size_t>(i)].mask = masks[static_cast<std::size_t>(i)];
        combos[static_cast<std::size_t>(i)].weight = weights[static_cast<std::size_t>(i)];
    }
    return combos;
}

// Owns a range handle for the window between "the waist created it" and "a
// PyRange took it over". Only the tag read sits in that window, and that read
// cannot fail on a non-NULL handle -- but a handle with no owner at all is the
// kind of gap that survives a later edit, so it gets an owner.
class RangeHandleGuard {
public:
    explicit RangeHandleGuard(xiapl_range_t* handle) : handle_(handle) {}
    RangeHandleGuard(const RangeHandleGuard&) = delete;
    RangeHandleGuard& operator=(const RangeHandleGuard&) = delete;
    ~RangeHandleGuard() { xiapl_range_destroy(handle_); }

    const xiapl_range_t* get() const { return handle_; }
    xiapl_range_t* release() {
        xiapl_range_t* handle = handle_;
        handle_ = nullptr;
        return handle;
    }

private:
    xiapl_range_t* handle_;
};

// The handle out of one of the waist's handle-producing calls. Every entry
// point except xiapl_try_parse_range guarantees a non-NULL handle on
// XIAPL_OK; try_parse_range's own caller below checks for NULL first, so
// reaching this with one is a broken waist rather than a Python-level error.
template <class Create>
xiapl_range_t* create_handle(Create&& create) {
    xiapl_range_t* handle = nullptr;
    check_status(create(&handle));
    if (handle == nullptr) {
        raise_runtime_error("xiapl.range: waist returned a NULL range handle");
    }
    return handle;
}

// A new Range from one of the waist's handle-producing calls. `tag` is what
// the result should report as its game; `create` runs the waist call.
template <class Create>
PyRange make_range(PyGame tag, Create&& create) {
    return PyRange(create_handle(create), tag);
}

// Same, with the tag read back out of the finished handle -- the constructor
// paths whose C++ routine normalizes the tag internally.
template <class Create>
PyRange make_range_reading_tag(Create&& create) {
    RangeHandleGuard guard(create_handle(create));
    const PyGame tag = read_tag(guard.get());
    return PyRange(guard.release(), tag);
}

// The three set operations share everything but the waist entry point and the
// name that appears in the mismatch message. The result carries the LEFT
// operand's tag, which is what xiapl::range_union et al. do (`Range(merged,
// a.game())`) and is only distinguishable from reading the tag back out of
// the new handle for a range tagged outside {Holdem, Plo}.
using SetOp = std::int32_t (*)(const xiapl_range_t*, const xiapl_range_t*,
                               xiapl_range_t**);

PyRange apply_set_op(const PyRange& a, const PyRange& b, SetOp op,
                     const char* fn_name) {
    require_same_game(a, b, fn_name);
    return make_range(a.game(), [&](xiapl_range_t** out) {
        return op(a.handle(), b.handle(), out);
    });
}

// A range's combos read out of the waist, all of them or only those that miss
// `dead_mask`. Internal to this file since Task 8: bindings.cpp used to need
// the columns as well, to rebuild an xiapl::Range for the not-yet-switched
// simulation module, and that bridge is gone.
std::vector<PyCombo> read_combos(const xiapl_range_t* range) {
    return fetch_combos([&](std::uint64_t* masks, double* weights,
                            std::int32_t capacity, std::int32_t* total) {
        return xiapl_range_combos(range, masks, weights, capacity, total);
    });
}

std::vector<PyCombo> read_valid_combos(const xiapl_range_t* range,
                                       std::uint64_t dead_mask) {
    return fetch_combos([&](std::uint64_t* masks, double* weights,
                            std::int32_t capacity, std::int32_t* total) {
        return xiapl_range_valid_combos(range, dead_mask, masks, weights,
                                        capacity, total);
    });
}

}  // namespace

const std::vector<PyCombo>& PyRange::exposed_combos() const {
    if (!exposed_ready_) {
        exposed_ = read_combos(handle_);
        exposed_ready_ = true;
    }
    return exposed_;
}

void register_range_module(py::module_& parent) {
    auto range_mod = parent.def_submodule("range", "Range parsing");

    py::class_<PyCombo>(range_mod, "Combo")
        .def(py::init<>())
        .def_readwrite("mask", &PyCombo::mask)
        .def_readwrite("weight", &PyCombo::weight);

    py::class_<PyRange>(range_mod, "Range")
        .def(py::init<>())
        // Direct construction from an explicit combo list, bypassing the
        // notation parser -- e.g. for building a Range out of Combo objects
        // assembled elsewhere in Python. Validates combo.mask against
        // cards_per_hand(game) just like the parser paths do.
        //
        // This is the one path that keeps the game tag it was handed rather
        // than reading it back: see PyRange for why.
        .def(py::init([](const std::vector<PyCombo>& combos, PyGame game) {
                 const ComboColumns columns(combos);
                 return make_range(game, [&](xiapl_range_t** out) {
                     return xiapl_range_create_from_combos(
                         columns.masks(), columns.weights(), columns.size(),
                         game_code(game), out);
                 });
             }),
             py::arg("combos"), py::arg("game"))
        .def_static("from_string",
            [](const std::string& text, PyGame game) {
                return make_range_reading_tag([&](xiapl_range_t** out) {
                    return xiapl_range_create_from_string(text.c_str(),
                                                          game_code(game), out);
                });
            },
            py::arg("text"), py::arg("game") = PyGame::Holdem)
        .def_static("all",
            [](PyGame game) {
                return make_range_reading_tag([&](xiapl_range_t** out) {
                    return xiapl_range_create_all(game_code(game), out);
                });
            },
            py::arg("game") = PyGame::Holdem)
        // reference_internal, as before: the list is fresh each call but its
        // elements are the range's own stored combos, so `r.combos()[0] is
        // r.combos()[0]` stays True and each element keeps the range alive.
        .def("combos",
             [](const PyRange& r) -> const std::vector<PyCombo>& {
                 return r.exposed_combos();
             },
             py::return_value_policy::reference_internal)
        .def("valid_combos",
             [](const PyRange& r, std::uint64_t dead_mask) {
                 return read_valid_combos(r.handle(), dead_mask);
             },
             py::arg("dead_mask"))
        .def("total_weight",
             [](const PyRange& r, std::uint64_t dead_mask) {
                 double weight = 0.0;
                 check_status(xiapl_range_total_weight(r.handle(), dead_mask,
                                                       &weight));
                 return weight;
             },
             py::arg("dead_mask") = 0)
        .def("size",
             [](const PyRange& r) {
                 std::int32_t size = 0;
                 check_status(xiapl_range_size(r.handle(), &size));
                 return static_cast<int>(size);
             })
        .def("empty",
             [](const PyRange& r) {
                 std::int32_t empty = 0;
                 check_status(xiapl_range_empty(r.handle(), &empty));
                 return empty != 0;
             })
        .def_property_readonly("game", [](const PyRange& r) { return r.game(); })
        // Weighted set algebra (max/min lattice; see include/xiapl/range.h
        // for the frozen semantics). Method names and dunders are a
        // recorded commander adjudication -- do not rename.
        .def("union",
             [](const PyRange& a, const PyRange& b) {
                 return apply_set_op(a, b, &xiapl_range_union, "range_union");
             },
             py::arg("other"))
        .def("intersection",
             [](const PyRange& a, const PyRange& b) {
                 return apply_set_op(a, b, &xiapl_range_intersection,
                                     "range_intersection");
             },
             py::arg("other"))
        .def("difference",
             [](const PyRange& a, const PyRange& b) {
                 return apply_set_op(a, b, &xiapl_range_difference,
                                     "range_difference");
             },
             py::arg("other"))
        .def("__or__",
             [](const PyRange& a, const PyRange& b) {
                 return apply_set_op(a, b, &xiapl_range_union, "range_union");
             },
             py::is_operator())
        .def("__and__",
             [](const PyRange& a, const PyRange& b) {
                 return apply_set_op(a, b, &xiapl_range_intersection,
                                     "range_intersection");
             },
             py::is_operator())
        .def("__sub__",
             [](const PyRange& a, const PyRange& b) {
                 return apply_set_op(a, b, &xiapl_range_difference,
                                     "range_difference");
             },
             py::is_operator());

    // "This is not a range" is an answer, not a failure: the waist reports
    // XIAPL_OK with a NULL handle and records no error message.
    range_mod.def("try_parse_range",
        [](const std::string& text) -> std::optional<PyRange> {
            xiapl_range_t* handle = nullptr;
            check_status(xiapl_try_parse_range(text.c_str(), &handle));
            if (handle == nullptr) return std::nullopt;
            RangeHandleGuard guard(handle);
            const PyGame tag = read_tag(guard.get());
            return std::optional<PyRange>(PyRange(guard.release(), tag));
        },
        py::arg("text"));

    // Top-percent starting-hand selection (exact vs-random ranking table,
    // src/core/preflop_rank.cpp). `game` is kw-only so a positional call
    // cannot silently pass PLO where Hold'em was intended -- there is no PLO
    // ranking table yet (see the raised ValueError's roadmap note).
    range_mod.def("rank_starting_hands",
                  [](double top_percent, PyGame game) {
                      // Fixed-stride label slots (convention 4 plus
                      // XIAPL_STARTING_HAND_LABEL_SIZE): the query pass also
                      // performs the top_percent and game validation, so a
                      // rejected call raises before any buffer is sized.
                      std::int32_t total = 0;
                      check_status(xiapl_rank_starting_hands(
                          top_percent, game_code(game), nullptr, 0, &total));
                      if (total <= 0) return std::vector<std::string>();

                      std::vector<char> buffer(
                          static_cast<std::size_t>(total) *
                          XIAPL_STARTING_HAND_LABEL_SIZE);
                      std::int32_t filled = 0;
                      check_status(xiapl_rank_starting_hands(
                          top_percent, game_code(game), buffer.data(), total,
                          &filled));
                      // Clamp to what the buffer can actually hold: the
                      // second pass reports its own total, which a shorter
                      // list would make smaller and a longer one larger than
                      // the space just allocated.
                      if (filled > total) filled = total;
                      if (filled < 0) filled = 0;

                      std::vector<std::string> labels;
                      labels.reserve(static_cast<std::size_t>(filled));
                      for (std::int32_t i = 0; i < filled; ++i) {
                          const char* slot =
                              buffer.data() +
                              static_cast<std::size_t>(i) *
                                  XIAPL_STARTING_HAND_LABEL_SIZE;
                          // Every slot is NUL-filled by the waist, so the
                          // label ends at the first NUL or at the slot's end.
                          constexpr std::size_t kLabelRoom =
                              static_cast<std::size_t>(
                                  XIAPL_STARTING_HAND_LABEL_SIZE) - 1;
                          std::size_t length = 0;
                          while (length < kLabelRoom && slot[length] != '\0') {
                              ++length;
                          }
                          labels.emplace_back(slot, length);
                      }
                      return labels;
                  },
                  py::arg("top_percent") = 1.0,
                  py::kw_only(), py::arg("game") = PyGame::Holdem);
    range_mod.def("generate_top_percent_range",
                  [](double top_percent, PyGame game) {
                      return make_range_reading_tag([&](xiapl_range_t** out) {
                          return xiapl_generate_top_percent_range(
                              top_percent, game_code(game), out);
                      });
                  },
                  py::arg("top_percent"),
                  py::kw_only(), py::arg("game") = PyGame::Holdem);
}

}  // namespace xiapl_py
