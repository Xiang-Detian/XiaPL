// xiapl.deck, implemented against the C ABI waist (<xiapl/c_api.h>) instead
// of the C++ API.
//
// INCLUDE PURITY: see binding/core_common.h. Nothing but the waist header,
// pybind11 and the standard library may be included here.
//
// The Python surface is byte-compatible with the pre-waist binding: the same
// names, the same signatures (deck.pyi is the contract), the same repr text,
// the same exception types and the same exception messages -- which come out
// identical because the waist carries the originating C++ what() through
// verbatim.

#include "core_common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace py = pybind11;

namespace xiapl_py {
namespace {

// ---------------------------------------------------------------------------
// Deck -- owns one xiapl_deck_t handle for its lifetime.
//
// Non-copyable: the waist hands out single-ownership handles (one creator,
// one destroyer), and every method below binds by reference, so nothing
// needs a copy. pybind's default holder (std::unique_ptr<Deck>) places this
// with `new`, which does not require Deck to be copyable or movable.
// ---------------------------------------------------------------------------
class Deck {
public:
    // A full 52-card deck in ascending id order, NOT shuffled: deterministic
    // by default (xiapl_deck_create).
    Deck() { check_status(xiapl_deck_create(&handle_)); }

    // A deck holding exactly these cards, in this order. Validated before
    // anything is stored, so a rejected list leaves no partially-built deck
    // behind (xiapl_deck_create_from_cards delegates to Deck::set_cards,
    // which is what makes the invalid-id / duplicate-card message text below
    // identical to the pre-waist binding's).
    explicit Deck(const std::vector<PyCard>& cards) {
        const CardIdBuffer ids(cards);
        check_status(xiapl_deck_create_from_cards(ids.data(), ids.size(), &handle_));
    }

    Deck(const Deck&) = delete;
    Deck& operator=(const Deck&) = delete;

    ~Deck() { xiapl_deck_destroy(handle_); }

    xiapl_deck_t* handle() { return handle_; }
    const xiapl_deck_t* handle() const { return handle_; }

private:
    xiapl_deck_t* handle_ = nullptr;
};

// ---------------------------------------------------------------------------
// deal()'s output buffer.
//
// xiapl_deck_deal requires ids_capacity >= n and validates that BEFORE
// mutating the deck, unconditionally -- including when n <= 0, where a
// smaller (or query-derived) capacity would turn a no-op into
// XIAPL_ERR_INVALID_ARGUMENT. So capacity is sized from the requested count
// itself, not from how many cards the deck can actually supply: a deal()
// that asks for more than the deck holds is legal input whose buffer must
// still cover the ask, and the waist reports back how many were actually
// dealt. The stack array covers every deal a single 52-card deck can ever
// satisfy; a request wider than that (legal, if wasteful) falls back to the
// heap. out_ids must never be NULL -- true even at capacity 0, e.g. n <= 0 --
// which the stack array guarantees regardless of capacity.
class DealIdBuffer {
public:
    explicit DealIdBuffer(std::int32_t n) : capacity_(n > 0 ? n : 0) {
        if (capacity_ > kMaxCardIds) {
            heap_ids_.resize(static_cast<std::size_t>(capacity_));
        }
    }

    // data() hands out a pointer into one of two members, so copying a
    // buffer would silently change which (same rule as CardIdBuffer).
    DealIdBuffer(const DealIdBuffer&) = delete;
    DealIdBuffer& operator=(const DealIdBuffer&) = delete;

    std::uint8_t* data() { return heap_ids_.empty() ? stack_ids_ : heap_ids_.data(); }
    std::int32_t capacity() const { return capacity_; }

private:
    std::uint8_t stack_ids_[kMaxCardIds];
    std::vector<std::uint8_t> heap_ids_;
    std::int32_t capacity_;
};

// The remaining cards, front to back (the next card dealt is the LAST
// entry), read with exactly one xiapl_deck_get_cards call. A deck can never
// hold more than 52 cards -- xiapl_deck_set_cards / _create_from_cards both
// require every id to be a distinct member of [0, 51] -- so the 52-slot
// stack buffer is always enough and no query-then-fill pass is needed.
std::int32_t read_card_ids(const Deck& deck, std::uint8_t (&out_ids)[kMaxCardIds]) {
    std::int32_t total = 0;
    check_status(xiapl_deck_get_cards(deck.handle(), out_ids, kMaxCardIds, &total));
    // The fill can never truncate at this capacity, so the clamp is only there
    // to keep a future widening of the id space from turning into an
    // out-of-bounds read in the callers (symmetry with fill_mask_ids).
    return total < kMaxCardIds ? total : kMaxCardIds;
}

std::vector<PyCard> read_py_cards(const Deck& deck) {
    std::uint8_t ids[kMaxCardIds];
    const std::int32_t total = read_card_ids(deck, ids);
    std::vector<PyCard> cards;
    cards.reserve(static_cast<std::size_t>(total));
    for (std::int32_t i = 0; i < total; ++i) cards.emplace_back(ids[i]);
    return cards;
}

}  // namespace

void register_deck_module(py::module_& parent) {
    auto deck_mod = parent.def_submodule("deck", "Deck operations");

    py::class_<Deck>(deck_mod, "Deck")
        .def(py::init<>())
        // No py::arg: positional-only, matching the previous binding
        // (deck.pyi pins `cards: Sequence[Card], /`).
        .def(py::init<const std::vector<PyCard>&>())
        .def("shuffle", [](Deck& d) {
            check_status(xiapl_deck_shuffle(d.handle()));
        })
        .def("shuffle", [](Deck& d, std::uint64_t seed) {
            check_status(xiapl_deck_shuffle_seeded(d.handle(), seed));
        }, py::arg("seed"))
        .def("deal", [](Deck& d, int n) {
            std::int32_t request = static_cast<std::int32_t>(n);
            // A deck can never hold more than kMaxCardIds (52) cards, and a
            // partial deal already stops at however many the deck actually
            // has -- xiapl_deck_deal's `n` past the deck's own size is legal
            // input, not an error, that simply dealt everything there was.
            // But xiapl_deck_deal also requires ids_capacity >= n BEFORE it
            // will look at the deck at all (see DealIdBuffer above), so an
            // oversized `n` used to force an oversized heap buffer too, even
            // though at most kMaxCardIds bytes of it could ever be written.
            // Clamping `n` itself to the deck's current size first -- one
            // extra xiapl_deck_size call, paid only in this already-rare
            // n > kMaxCardIds branch -- makes the requested and the
            // deliverable counts agree, so DealIdBuffer never has to size for
            // more than kMaxCardIds and stays on the stack. The dealt result
            // is unaffected either way: xiapl_deck_deal(deck, n_clamped, ...)
            // still deals min(n_clamped, deck size) == min(n, deck size)
            // cards, identical to the unclamped call.
            if (request > kMaxCardIds) {
                std::int32_t size = 0;
                check_status(xiapl_deck_size(d.handle(), &size));
                if (size < request) request = size;
            }
            DealIdBuffer buffer(request);
            std::int32_t count = 0;
            check_status(xiapl_deck_deal(d.handle(), request,
                                         buffer.data(), buffer.capacity(), &count));
            std::vector<PyCard> cards;
            cards.reserve(static_cast<std::size_t>(count));
            for (std::int32_t i = 0; i < count; ++i) cards.emplace_back(buffer.data()[i]);
            return cards;
        }, py::arg("n") = 1)
        // deal_one is deal(1) composed: a single-uint8_t buffer is exactly
        // xiapl_deck_deal's ids_capacity == 1 case. 0 written (an empty
        // deck) yields the default-constructed PyCard, i.e. the Invalid
        // card -- the same sentinel xiapl::Deck::deal_one() returns today
        // (a default Card()), reproduced verbatim rather than through a
        // waist call, since c_api.h documents this composition as the
        // reason no separate xiapl_deck_deal_one exists.
        .def("deal_one", [](Deck& d) {
            std::uint8_t id = XIAPL_CARD_INVALID_ID;
            std::int32_t count = 0;
            check_status(xiapl_deck_deal(d.handle(), 1, &id, 1, &count));
            return count > 0 ? PyCard(id) : PyCard();
        })
        .def("burn", [](Deck& d, int n) {
            check_status(xiapl_deck_burn(d.handle(), static_cast<std::int32_t>(n)));
        }, py::arg("n") = 1)
        // No py::arg: positional-only, matching the previous binding.
        .def("remove_cards", [](Deck& d, const std::vector<PyCard>& cards) {
            const CardIdBuffer ids(cards);
            check_status(xiapl_deck_remove_cards(d.handle(), ids.data(), ids.size()));
        })
        // reset's default is `true` here, NOT the C++ default of `false`
        // (xiapl::Deck::reset defaults to a deterministic, unshuffled
        // reset). That divergence predates the waist switch and is
        // preserved exactly: reset() called with no argument must keep
        // shuffling, and this default is what makes it do so.
        .def("reset", [](Deck& d, bool shuffle) {
            check_status(xiapl_deck_reset(d.handle(), shuffle ? 1 : 0));
        }, py::arg("shuffle") = true)
        // No py::arg: positional-only, matching the previous binding.
        .def("set_cards", [](Deck& d, const std::vector<PyCard>& cards) {
            const CardIdBuffer ids(cards);
            check_status(xiapl_deck_set_cards(d.handle(), ids.data(), ids.size()));
        })
        .def("size", [](const Deck& d) {
            std::int32_t size = 0;
            check_status(xiapl_deck_size(d.handle(), &size));
            return static_cast<int>(size);
        })
        .def("empty", [](const Deck& d) {
            std::int32_t out_empty = 0;
            check_status(xiapl_deck_empty(d.handle(), &out_empty));
            return out_empty != 0;
        })
        .def("has_cards", [](const Deck& d, int n) {
            std::int32_t out_has = 0;
            check_status(xiapl_deck_has_cards(d.handle(), static_cast<std::int32_t>(n),
                                              &out_has));
            return out_has != 0;
        }, py::arg("n") = 1)
        .def_property_readonly("cards", [](const Deck& d) {
            return read_py_cards(d);
        })
        .def_property_readonly("card_ids", [](const Deck& d) {
            std::uint8_t ids[kMaxCardIds];
            const std::int32_t total = read_card_ids(d, ids);
            return std::vector<int>(ids, ids + total);
        })
        .def("__repr__", [](const Deck& d) {
            return fetch_string([&](char* buf, std::int32_t cap, std::int32_t* len) {
                return xiapl_deck_repr(d.handle(), buf, cap, len);
            });
        })
        .def("__reduce__", [](const Deck& d) {
            // Reconstruct through the (cards) constructor, exactly as
            // before: the callable is the live Deck type object, so
            // pickling depends on Deck.__module__ ("xiapl._xiapl.deck")
            // staying importable.
            return py::make_tuple(
                py::type::of(py::cast(d)),
                py::make_tuple(read_py_cards(d))
            );
        });
}

}  // namespace xiapl_py
