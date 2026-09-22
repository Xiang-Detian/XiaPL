// Binding-local vocabulary shared by the waist-consuming halves of the xiapl
// Python extension.
//
// INCLUDE PURITY (waist v1 §D2): this header -- and every binding/core_*.cpp
// that includes it -- may include ONLY <xiapl/c_api.h>, pybind11 headers and
// the C++ standard library. Any other xiapl header would reintroduce the
// direct C++ coupling this track exists to remove. A ctest guard
// (tests/check_binding_includes.sh, Task 8) enforces it mechanically.
//
// What lives here:
//   * PyCard -- the Python-visible `Card` type, a binding-local value type
//     that is nothing but the uint8_t id the waist speaks,
//   * PyGame -- the Python-visible `GameType`, likewise binding-local,
//   * the card-id / card-mask marshalling every module needs, and
//   * the error plumbing that turns an xiapl_status_t plus the thread's
//     last_error message into the Python exception the frozen mapping calls
//     for.

#pragma once

#include <xiapl/c_api.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xiapl_py {

// ---------------------------------------------------------------------------
// Error plumbing
//
// The status code IS the originating C++ exception class (see "Status codes"
// in <xiapl/c_api.h>), so the mapping below is the same one the pybind
// exception translator applies to a direct C++ throw -- ValueError /
// IndexError / MemoryError / RuntimeError -- and the message is
// xiapl_last_error_message() carried through verbatim. That is what keeps the
// Python-visible exception text byte-identical across the switch.
//
// The message must be read on the CALLING thread immediately after the
// failing call (boundary convention 8), which is exactly what check_status()
// does. Callers that hold the GIL for the whole call use check_status();
// xiapl.simulation, which releases it around the waist call, captures the
// status and the message inside the released region and then re-raises
// through raise_status_message() with the GIL back in hand (see
// binding/core_simulation.cpp).
// ---------------------------------------------------------------------------

// Raise the Python exception a status code stands for, with `message` as its
// text. Split out of raise_status so that a caller which had to read the
// message earlier (GIL released at the time) re-uses the one classification
// table instead of copying it.
[[noreturn]] inline void raise_status_message(std::int32_t status,
                                              const char* message) {
    PyObject* exc_type = PyExc_RuntimeError;
    switch (status) {
    case XIAPL_ERR_INVALID_ARGUMENT: exc_type = PyExc_ValueError;  break;
    case XIAPL_ERR_OUT_OF_RANGE:     exc_type = PyExc_IndexError;  break;
    case XIAPL_ERR_BAD_ALLOC:        exc_type = PyExc_MemoryError; break;
    default:                         exc_type = PyExc_RuntimeError; break;
    }
    PyErr_SetString(exc_type, message != nullptr ? message : "");
    throw pybind11::error_already_set();
}

[[noreturn]] inline void raise_status(std::int32_t status) {
    raise_status_message(status, xiapl_last_error_message());
}

inline void check_status(std::int32_t status) {
    if (status != XIAPL_OK) raise_status(status);
}

// A validation failure the binding itself owns, i.e. one that has no waist
// call behind it to carry a message. Raised as ValueError to match the
// std::invalid_argument -> ValueError leg of the frozen mapping.
[[noreturn]] inline void raise_value_error(const char* what) {
    PyErr_SetString(PyExc_ValueError, what);
    throw pybind11::error_already_set();
}

// Same shape as raise_value_error, for a binding-owned failure that is a
// broken invariant rather than a bad input -- i.e. one that would map to
// RuntimeError even under the frozen std::exception mapping.
[[noreturn]] inline void raise_runtime_error(const char* what) {
    PyErr_SetString(PyExc_RuntimeError, what);
    throw pybind11::error_already_set();
}

// ---------------------------------------------------------------------------
// String output (boundary convention 7)
//
// `emit` is any of the waist's (out_buf, buf_capacity, out_length) writers.
// The stack buffer covers every string the card/deck/hand vocabulary can
// produce in one call; the heap path exists so a longer text can never be
// silently truncated.
// ---------------------------------------------------------------------------
template <class Emit>
inline std::string fetch_string(Emit&& emit) {
    char stack_buf[64];
    std::int32_t length = 0;
    check_status(emit(stack_buf, static_cast<std::int32_t>(sizeof(stack_buf)),
                      &length));
    if (length < static_cast<std::int32_t>(sizeof(stack_buf))) {
        // At most capacity-1 bytes are written plus a NUL, so a length that
        // fits strictly inside the buffer means the text is complete.
        return std::string(stack_buf, static_cast<std::size_t>(length));
    }
    std::string out(static_cast<std::size_t>(length), '\0');
    std::int32_t emitted = 0;
    check_status(emit(out.data(), length + 1, &emitted));
    // Size the result from the report of the call that actually filled the
    // buffer, not from the query's: a text that shrank between the two would
    // otherwise leave trailing NULs inside the returned string.
    if (emitted < length) out.resize(static_cast<std::size_t>(emitted));
    return out;
}

// ---------------------------------------------------------------------------
// PyCard -- the Python `Card`
//
// A card has no handle at the waist: it IS its uint8_t id, so this type holds
// nothing else. rank()/suit_index() inline the id arithmetic rather than
// calling xiapl_card_rank/xiapl_card_suit, which is explicitly permitted:
// `id = suit * 13 + (rank - 2)` is a frozen, published part of the card
// contract (see <xiapl/card.h> and boundary convention 1 in <xiapl/c_api.h>),
// so using it is use of a published contract, not a second implementation.
//
// Inlining is also what keeps the sentinel behaviour identical to the C++
// accessors this replaces: xiapl::Card::rank()/suit() do the same unchecked
// arithmetic, so Card.from_id(255).rank stays 10 and .suit stays '?' exactly
// as before, whereas xiapl_card_rank/xiapl_card_suit reject an id outside
// [0, 51] and would have turned a silent value into a raised exception.
// ---------------------------------------------------------------------------
struct PyCard {
    std::uint8_t id = XIAPL_CARD_INVALID_ID;

    PyCard() = default;
    explicit PyCard(std::uint8_t raw_id) : id(raw_id) {}

    int rank() const { return static_cast<int>(id % 13) + 2; }
    int suit_index() const { return static_cast<int>(id / 13); }

    bool operator==(const PyCard& other) const { return id == other.id; }
    bool operator<(const PyCard& other) const { return id < other.id; }
};

// The suit -> character table. The C++ API now publishes xiapl::suit_to_char
// (include/xiapl/utils.h), but the waist v1 include-purity rule confines this
// file to <xiapl/c_api.h> -- calling it directly would mean including
// <xiapl/utils.h> here, which check_binding_includes.sh rejects. Routing this
// through the C ABI is out of scope for the change that added the C++-side
// function, so the binding keeps the table it has always had -- including
// the '?' answer for an out-of-range suit index, which is what a
// default-constructed / INVALID_ID card produces.
inline char suit_char_of(const PyCard& card) {
    static const char kSuitChars[] = {'c', 'd', 'h', 's'};
    const int suit_index = card.suit_index();
    if (suit_index < 0 || suit_index > 3) return '?';
    return kSuitChars[suit_index];
}

// ---------------------------------------------------------------------------
// Card marshalling
//
// Two shapes cover every crossing: a Python list of Cards becomes the
// contiguous uint8_t id array the waist takes (CardIdBuffer), and a 52-bit
// mask becomes a Python list of Cards (mask_to_py_cards).
// ---------------------------------------------------------------------------

// Card ids a 52-bit mask can hold. xiapl_mask_to_ids ignores bits above 51,
// so this is the exact worst case for every id buffer below.
constexpr std::int32_t kMaxCardIds = 52;

// 52-bit mask -> ascending card ids, written into a caller-owned buffer;
// returns how many were written. Buffer-out rather than vector-return so a
// caller can build its own result with a single allocation.
inline std::int32_t fill_mask_ids(std::uint64_t mask,
                                  std::uint8_t (&out_ids)[kMaxCardIds]) {
    std::int32_t total = 0;
    check_status(xiapl_mask_to_ids(mask, out_ids, kMaxCardIds, &total));
    // The fill can never truncate at this capacity, so the clamp is only
    // there to keep a future widening of the id space from turning into an
    // out-of-bounds read in the callers.
    return total < kMaxCardIds ? total : kMaxCardIds;
}

inline std::vector<PyCard> mask_to_py_cards(std::uint64_t mask) {
    std::uint8_t ids[kMaxCardIds];
    const std::int32_t count = fill_mask_ids(mask, ids);
    std::vector<PyCard> cards;
    cards.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i) cards.emplace_back(ids[i]);
    return cards;
}

// A Card list rendered as the (ids, count) pair the waist takes.
//
// A list longer than a deck is legal input -- duplicates collapse in the
// waist, and an invalid id is the waist's verdict to make, not this type's --
// so the stack buffer that covers every real call has a heap fallback behind
// it. data() is never NULL, including for an empty list, because several
// waist entry points reject a NULL id pointer outright.
class CardIdBuffer {
public:
    explicit CardIdBuffer(const std::vector<PyCard>& cards)
        : size_(static_cast<std::int32_t>(cards.size())) {
        if (cards.size() > static_cast<std::size_t>(kMaxCardIds)) {
            heap_ids_.resize(cards.size());
        }
        std::uint8_t* ids = heap_ids_.empty() ? stack_ids_ : heap_ids_.data();
        for (std::size_t i = 0; i < cards.size(); ++i) ids[i] = cards[i].id;
    }

    // data() hands out a pointer into one of two members, so copying a
    // buffer would silently change which. Nothing needs to; make it explicit.
    CardIdBuffer(const CardIdBuffer&) = delete;
    CardIdBuffer& operator=(const CardIdBuffer&) = delete;

    const std::uint8_t* data() const {
        return heap_ids_.empty() ? stack_ids_ : heap_ids_.data();
    }
    std::int32_t size() const { return size_; }

private:
    std::uint8_t stack_ids_[kMaxCardIds];
    std::vector<std::uint8_t> heap_ids_;
    std::int32_t size_;
};

// Card list -> 52-bit mask. The waist owns the invalid-id verdict and its
// message ("cards_to_mask: invalid Card id").
inline std::uint64_t py_cards_to_mask(const std::vector<PyCard>& cards) {
    const CardIdBuffer ids(cards);
    std::uint64_t mask = 0;
    check_status(xiapl_cards_to_mask(ids.data(), ids.size(), &mask));
    return mask;
}

// ---------------------------------------------------------------------------
// PyGame -- the Python `GameType`
//
// Binding-local for the same reason PyCard is, and with the same underlying
// type as xiapl::GameType (uint8_t) rather than the ABI's int32_t. The
// underlying type is Python-visible through pybind's enum(int) constructor:
// with uint8_t, GameType(300) and GameType(-1) are rejected by the integer
// caster while GameType(2) builds an unnamed value. Widening it here would
// silently start accepting the first two.
// ---------------------------------------------------------------------------
enum class PyGame : std::uint8_t {
    Holdem = XIAPL_GAME_HOLDEM,
    Plo    = XIAPL_GAME_PLO,
};

// PyGame -> the int32_t game code the waist takes.
//
// Anything that is not Plo is Hold'em. That is not a normalization this
// binding invents: it is how the library itself classifies a GameType
// (`cards_per_hand` is `game == Plo ? 4 : 2`, and evaluate_hand / judge each
// dispatch as `game == GameType::Plo ? plo : holdem`), and it is what the
// pre-waist binding got by handing the enum straight to those functions. The
// waist's own contract for an unknown game code is XIAPL_ERR_INVALID_ARGUMENT,
// so without this the one value a caller can reach that is neither -- built
// through pybind's enum(int) constructor, e.g. GameType(2) -- would change
// from "treated as Hold'em" to a raised ValueError.
inline std::int32_t game_code(PyGame game) {
    return game == PyGame::Plo ? XIAPL_GAME_PLO : XIAPL_GAME_HOLDEM;
}

// Human-readable game name for binding-authored error messages: the
// set-algebra mismatch message in core_range.cpp and calculate_range_equity's
// game-agreement guard in core_simulation.cpp. Mirrors internal::game_name in
// src/core/range_parse.h, which is what produces the equivalent C++-side text
// today, including the fact that an unnamed tag (e.g. GameType(2)) renders as
// "Hold'em". Six "<op> holdem vs plo" / "plo vs holdem" fingerprint probes in
// python/bench/waist_fingerprint.py compare this reproduction against the
// recorded C++ output byte for byte.
inline const char* game_name(PyGame game) {
    return game == PyGame::Plo ? "PLO" : "Hold'em";
}

// ---------------------------------------------------------------------------
// PyCombo -- the Python `Combo`
//
// A weighted combo is a (mask, weight) pair and nothing else; it has no handle
// at the waist, which carries combos as parallel mask/weight columns (c_api.h,
// "A combo is NOT a struct at this boundary"). So this is a plain value type
// with the same two mutable fields and the same defaults the previous
// py::class_<xiapl::Combo> exposed.
//
// It lives here rather than in core_range.cpp because PyRange holds a
// std::vector<PyCombo> (see below), and PyRange in turn has to be complete in
// core_simulation.cpp, whose calculate_range_equity takes two of them.
// ---------------------------------------------------------------------------
struct PyCombo {
    std::uint64_t mask = 0;
    double weight = 1.0;
};

// ---------------------------------------------------------------------------
// PyRange -- the Python `Range`
//
// One xiapl_range_t handle plus the Python-visible game tag, which is NOT
// always the tag the handle carries. The waist only accepts XIAPL_GAME_HOLDEM
// and XIAPL_GAME_PLO, but Python can build a GameType outside that pair
// (GameType(2), via pybind's enum(int) constructor) and the raw-combo
// constructor of xiapl::Range stores whatever it is handed -- so
// `Range(combos, GameType(2)).game` is GameType(2) today. game_code()
// normalizes that to Hold'em on the way in (which is what the C++ library
// itself does everywhere except this one field), so the raw value has to be
// remembered on this side to keep the round trip exact. Every other
// constructor path reads the tag back out of the handle, because those paths
// normalize inside the C++ routine as well.
//
// Movable so that std::optional<PyRange> works (try_parse_range's
// Optional[Range] return); non-copyable because the handle is
// single-ownership. A moved-from range holds a NULL handle, which
// xiapl_range_destroy documents as a no-op.
// ---------------------------------------------------------------------------
class PyRange {
public:
    // Empty Hold'em range (mirrors xiapl::Range's default constructor).
    PyRange() { check_status(xiapl_range_create(&handle_)); }

    // Adopts an already-created handle together with the tag it should
    // report. Ownership transfers: this object destroys the handle.
    PyRange(xiapl_range_t* handle, PyGame tag) : handle_(handle), tag_(tag) {}

    PyRange(const PyRange&) = delete;
    PyRange& operator=(const PyRange&) = delete;

    PyRange(PyRange&& other) noexcept
        : handle_(other.handle_), tag_(other.tag_),
          exposed_(std::move(other.exposed_)),
          exposed_ready_(other.exposed_ready_) {
        other.handle_ = nullptr;
        other.exposed_ready_ = false;
    }

    // No move ASSIGNMENT: nothing assigns to a live PyRange (std::optional's
    // Optional[Range] return only needs the constructor above), and an
    // assignment operator on a type with a lazily-filled cache is exactly the
    // kind of thing a later edit would reach for without noticing that the
    // cache and the handle have to move together. Left deleted implicitly --
    // declaring a move constructor already suppresses it -- and stated here so
    // the omission reads as deliberate.

    ~PyRange() { xiapl_range_destroy(handle_); }

    const xiapl_range_t* handle() const { return handle_; }
    PyGame game() const { return tag_; }

    // The combo list `Range.combos()` hands to Python, materialized once and
    // then kept. The previous binding returned a reference into the C++
    // Range's own vector (return_value_policy::reference_internal on
    // `const std::vector<Combo>&`), so repeated calls yielded the SAME Combo
    // objects and a write through one was visible to the next call. The waist
    // copies combos out by value, so this cache is what keeps both of those
    // observable properties: same storage, same addresses, same pybind
    // instances. Defined in core_range.cpp.
    const std::vector<PyCombo>& exposed_combos() const;

private:
    xiapl_range_t* handle_ = nullptr;
    PyGame tag_ = PyGame::Holdem;
    mutable std::vector<PyCombo> exposed_;
    mutable bool exposed_ready_ = false;
};

// ---------------------------------------------------------------------------
// Submodule registration entry points (implemented in binding/core_*.cpp)
//
// Each is called from bindings.cpp at the exact point its submodule used to
// be registered inline, because pybind renders signature text at BIND time:
// a type must already be registered when a later module's signature or
// default argument mentions it.
//
// xiapl.simulation is the one module registered in two steps, and for that
// same reason: GameType has to exist before xiapl.eval and xiapl.range are
// registered (their default arguments name it), while calculate_range_equity
// has to be bound after xiapl.range (its parameters name Range). So
// register_simulation_gametype() creates the submodule and registers the enum
// early, hands the submodule back, and register_simulation_module() fills in
// the rest later.
// ---------------------------------------------------------------------------
void register_card_module(pybind11::module_& parent);
void register_utils_module(pybind11::module_& parent);
void register_eval_module(pybind11::module_& parent);
void register_canonicalize_module(pybind11::module_& parent);
void register_deck_module(pybind11::module_& parent);
void register_range_module(pybind11::module_& parent);
pybind11::module_ register_simulation_gametype(pybind11::module_& parent);
void register_simulation_module(pybind11::module_& sim_mod);

}  // namespace xiapl_py
