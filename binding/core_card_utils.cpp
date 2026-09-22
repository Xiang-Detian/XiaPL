// xiapl.card and xiapl.utils, implemented against the C ABI waist
// (<xiapl/c_api.h>) instead of the C++ API.
//
// INCLUDE PURITY: see binding/core_common.h. Nothing but the waist header,
// pybind11 and the standard library may be included here.
//
// The Python surface is byte-compatible with the pre-waist binding: the same
// names, the same signatures (the .pyi stubs are the contract), the same
// repr/str text, the same exception types and the same exception messages --
// which come out identical because the waist carries the originating C++
// what() through verbatim.

#include "core_common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace py = pybind11;

namespace xiapl_py {
namespace {

std::string card_to_string(const PyCard& card) {
    return fetch_string([&](char* buf, std::int32_t cap, std::int32_t* len) {
        return xiapl_card_to_string(card.id, buf, cap, len);
    });
}

}  // namespace

void register_card_module(py::module_& parent) {
    auto card_mod = parent.def_submodule("card", "Card primitives");
    py::class_<PyCard>(card_mod, "Card")
        .def(py::init([](int rank, char suit_char) {
            // The suit letter is mapped through the public
            // xiapl::try_suit_from_char (c/C, d/D, h/H, s/S), which is the
            // same accepted set the previous tolower() switch had. Its
            // "not a suit letter" answer is not a waist failure, so the
            // binding owns the message -- and it is the same text
            // xiapl::Card's own get_suit_int throws.
            std::int32_t suit = 0;
            std::int32_t found = 0;
            check_status(xiapl_try_suit_from_char(suit_char, &suit, &found));
            if (found == 0) raise_value_error("Invalid suit char");
            // Card(int, int) behind this validates the rank and throws
            // "Invalid rank or suit"; the waist carries that text verbatim.
            std::uint8_t id = XIAPL_CARD_INVALID_ID;
            check_status(xiapl_card_from_rank_suit(static_cast<std::int32_t>(rank),
                                                   suit, &id));
            return PyCard(id);
        }), py::arg("rank"), py::arg("suit"))
        .def_property_readonly("rank", &PyCard::rank)
        .def_property_readonly("suit", [](const PyCard& c) {
            return suit_char_of(c);
        })
        .def_property_readonly("id", [](const PyCard& c) {
            return static_cast<int>(c.id);
        })
        // from_string / from_id take no py::arg, matching the previous
        // direct member binding: the runtime parameter is positional-only
        // ("arg0"), which card.pyi pins.
        .def_static("from_string", [](const std::string& s) {
            std::uint8_t id = XIAPL_CARD_INVALID_ID;
            check_status(xiapl_card_from_string(s.c_str(), &id));
            return PyCard(id);
        })
        .def_static("from_id", [](std::uint8_t id) {
            // Sentinel behaviour verbatim: an id outside [0, 51] yields the
            // Invalid card rather than an error (xiapl::Card::from_id).
            std::uint8_t out_id = XIAPL_CARD_INVALID_ID;
            check_status(xiapl_card_from_id(id, &out_id));
            return PyCard(out_id);
        })
        .def("to_string", &card_to_string)
        .def("__str__", &card_to_string)
        .def("__repr__", [](const PyCard& c) {
            return fetch_string([&](char* buf, std::int32_t cap,
                                    std::int32_t* len) {
                return xiapl_card_repr(c.id, buf, cap, len);
            });
        })
        .def("__eq__", &PyCard::operator==)
        .def("__lt__", &PyCard::operator<)
        .def("__hash__", [](const PyCard& c) {
            // std::hash<xiapl::Card> is the id itself; reproduced so that a
            // Card's hash is unchanged across the switch.
            return static_cast<std::size_t>(c.id);
        })
        .def("__reduce__", [](const PyCard& c) {
            // Reconstruct through the (rank, suit_char) constructor, exactly
            // as before: the callable is the live Card type object, so
            // pickling depends on Card.__module__ ("xiapl._xiapl.card")
            // staying importable (pinned by test_package_wrapper).
            return py::make_tuple(
                py::type::of(py::cast(c)),
                py::make_tuple(c.rank(), suit_char_of(c))
            );
        });
}

void register_utils_module(py::module_& parent) {
    auto utils_mod = parent.def_submodule("utils", "Mask utilities");

    utils_mod.def("card_to_mask", [](const PyCard& card) {
        std::uint64_t mask = 0;
        check_status(xiapl_card_to_mask(card.id, &mask));
        return mask;
    }, py::arg("card"));

    utils_mod.def("cards_to_mask", &py_cards_to_mask, py::arg("cards"));

    utils_mod.def("mask_to_cards", &mask_to_py_cards, py::arg("mask"));

    utils_mod.def("mask_to_ids", [](std::uint64_t mask) {
        std::uint8_t ids[kMaxCardIds];
        const std::int32_t count = fill_mask_ids(mask, ids);
        return std::vector<int>(ids, ids + count);
    }, py::arg("mask"));
}

}  // namespace xiapl_py
