// xiapl.eval and xiapl.canonicalize, implemented against the C ABI waist
// (<xiapl/c_api.h>) instead of the C++ API.
//
// INCLUDE PURITY: see binding/core_common.h. Nothing but the waist header,
// pybind11 and the standard library may be included here.
//
// The Python surface is byte-compatible with the pre-waist binding: the same
// names, the same signatures (the .pyi stubs are the contract), the same
// repr text, the same exception types and the same exception messages --
// which come out identical because the waist carries the originating C++
// what() through verbatim.

#include "core_common.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace xiapl_py {
namespace {

// ---------------------------------------------------------------------------
// The Python `HandValue` IS the waist's POD.
//
// There is exactly one representation of a hand value at this boundary, so
// the binding has nothing to decode and nothing to keep in step: category and
// kickers are read straight off the struct the evaluator filled. No py::init
// is bound, matching the previous binding -- an instance can only come out of
// evaluate_* -- so `HandValue()` keeps raising pybind's "No constructor
// defined!".
// ---------------------------------------------------------------------------
using PyHandValue = xiapl_hand_value_t;

// The Python `HandCategory`. Binding-local like PyGame, with the same
// underlying type as xiapl::HandCategory (uint8_t) so that the enum(int)
// constructor keeps rejecting 300 and -1 while building an unnamed value for
// 0 or 10. The values mirror xiapl_hand_category_t, which mirrors C++.
enum class PyHandCategory : std::uint8_t {
    HighCard      = XIAPL_HAND_HIGH_CARD,
    OnePair       = XIAPL_HAND_ONE_PAIR,
    TwoPair       = XIAPL_HAND_TWO_PAIR,
    ThreeOfAKind  = XIAPL_HAND_THREE_OF_A_KIND,
    Straight      = XIAPL_HAND_STRAIGHT,
    Flush         = XIAPL_HAND_FLUSH,
    FullHouse     = XIAPL_HAND_FULL_HOUSE,
    FourOfAKind   = XIAPL_HAND_FOUR_OF_A_KIND,
    StraightFlush = XIAPL_HAND_STRAIGHT_FLUSH,
};

std::string describe_hand_value(const PyHandValue& value) {
    return fetch_string([&](char* buf, std::int32_t cap, std::int32_t* len) {
        return xiapl_describe_hand(&value, buf, cap, len);
    });
}

// Winner slots covered without an allocation: the widest seat cap the library
// has (PLO's 32). A longer player list is still legal INPUT -- the seat cap is
// the C++ contract's verdict, and it is not even the first check judge makes
// -- so the waist has to be given a buffer as long as the list either way.
constexpr std::int32_t kMaxStackSeats = 32;

// ---------------------------------------------------------------------------
// Canonical situation enumeration handle
//
// Owns the handle for the duration of one call so that an exception raised
// while streaming the pairs out cannot leak the enumeration (the river one is
// ~2 GB).
// ---------------------------------------------------------------------------
struct SituationsDeleter {
    void operator()(xiapl_canonical_situations_t* situations) const {
        xiapl_canonical_situations_destroy(situations);
    }
};
using SituationsPtr =
    std::unique_ptr<xiapl_canonical_situations_t, SituationsDeleter>;

// Pairs copied out of the enumeration per fill call. Large enough that the
// per-call overhead is irrelevant against the 1.4M-row flop population, small
// enough (128 KB of scratch) to stay out of the way of the result itself.
constexpr std::int64_t kSituationChunk = 8192;

}  // namespace

void register_eval_module(py::module_& parent) {
    auto eval_mod = parent.def_submodule("eval", "Hand evaluation");

    // Registered ahead of HandValue, whose `category` property returns one.
    py::enum_<PyHandCategory>(eval_mod, "HandCategory")
        .value("HighCard", PyHandCategory::HighCard)
        .value("OnePair", PyHandCategory::OnePair)
        .value("TwoPair", PyHandCategory::TwoPair)
        .value("ThreeOfAKind", PyHandCategory::ThreeOfAKind)
        .value("Straight", PyHandCategory::Straight)
        .value("Flush", PyHandCategory::Flush)
        .value("FullHouse", PyHandCategory::FullHouse)
        .value("FourOfAKind", PyHandCategory::FourOfAKind)
        .value("StraightFlush", PyHandCategory::StraightFlush);

    py::class_<PyHandValue>(eval_mod, "HandValue")
        .def_property_readonly("category", [](const PyHandValue& v) {
            return static_cast<PyHandCategory>(v.category);
        })
        .def_property_readonly("kickers", [](const PyHandValue& v) {
            // kicker_count is 0..5 for anything the evaluator produced, and
            // the remaining slots are zero by the ABI contract.
            return std::vector<int>(v.kickers, v.kickers + v.kicker_count);
        })
        .def("__repr__", &describe_hand_value);

    // `game` is keyword-only in Python: it is a rule selector, never something
    // a reader should have to positionally decode at a call site.
    eval_mod.def("evaluate_cards", [](const std::vector<PyCard>& cards) {
        const CardIdBuffer ids(cards);
        PyHandValue value{};
        check_status(xiapl_evaluate_cards(ids.data(), ids.size(), &value));
        return value;
    }, py::arg("cards"));

    eval_mod.def("evaluate_mask", [](std::uint64_t card_mask) {
        PyHandValue value{};
        check_status(xiapl_evaluate_mask(card_mask, &value));
        return value;
    }, py::arg("card_mask"));

    eval_mod.def("evaluate_hand", [](std::uint64_t board_mask,
                                     std::uint64_t hole_mask, PyGame game) {
        PyHandValue value{};
        check_status(xiapl_evaluate_hand(board_mask, hole_mask,
                                         game_code(game), &value));
        return value;
    }, py::arg("board_mask"), py::arg("hole_mask"),
       py::kw_only(), py::arg("game") = PyGame::Holdem);

    eval_mod.def("judge", [](const std::vector<std::uint64_t>& player_hole_masks,
                             std::uint64_t board_mask, PyGame game) {
        const std::int32_t num_players =
            static_cast<std::int32_t>(player_hole_masks.size());
        // The waist refuses to start unless the winners buffer can hold one
        // index per player, so it is sized from the list, not from a cap.
        std::int32_t stack_winners[kMaxStackSeats];
        std::vector<std::int32_t> heap_winners;
        std::int32_t capacity = kMaxStackSeats;
        if (num_players > kMaxStackSeats) {
            heap_winners.resize(static_cast<std::size_t>(num_players));
            capacity = num_players;
        }
        std::int32_t* winners =
            heap_winners.empty() ? stack_winners : heap_winners.data();
        std::int32_t count = 0;
        check_status(xiapl_judge(player_hole_masks.data(), num_players,
                                 board_mask, game_code(game), winners,
                                 capacity, &count));
        return std::vector<int>(winners, winners + count);
    }, py::arg("player_hole_masks"), py::arg("board_mask"),
       py::kw_only(), py::arg("game") = PyGame::Holdem);

    eval_mod.def("describe_hand", &describe_hand_value, py::arg("value"));
}

void register_canonicalize_module(py::module_& parent) {
    auto canon_mod = parent.def_submodule("canonicalize", "Suit canonicalization");

    // The Card-taking forms compose waist primitives, which is literally what
    // their C++ twins do (canonicalize_hero_and_board_cards and
    // canonicalize_board_cards are cards -> mask -> canonicalize -> mask ->
    // cards, src/core/canonicalize.cpp). Composing them here therefore
    // reproduces those functions bit for bit, invalid-card message included --
    // the hero side is converted first, so it is still the side that reports
    // a bad card. canonicalize_hand is the one exception; see below.
    canon_mod.def("canonicalize_hero_and_board",
                  [](const std::vector<PyCard>& hero_hand,
                     const std::vector<PyCard>& board) {
                      const std::uint64_t hero_mask = py_cards_to_mask(hero_hand);
                      const std::uint64_t board_mask = py_cards_to_mask(board);
                      std::uint64_t canon_hero = 0;
                      std::uint64_t canon_board = 0;
                      check_status(xiapl_canonicalize_hero_and_board(
                          hero_mask, board_mask, &canon_hero, &canon_board));
                      return std::make_pair(mask_to_py_cards(canon_hero),
                                            mask_to_py_cards(canon_board));
                  },
                  py::arg("hero_hand"), py::arg("board"));
    canon_mod.def("canonicalize_hero_and_board_masks",
                  [](std::uint64_t hero_mask, std::uint64_t board_mask) {
                      std::uint64_t canon_hero = 0;
                      std::uint64_t canon_board = 0;
                      check_status(xiapl_canonicalize_hero_and_board(
                          hero_mask, board_mask, &canon_hero, &canon_board));
                      return std::make_pair(canon_hero, canon_board);
                  },
                  py::arg("hero_mask"), py::arg("board_mask"));
    canon_mod.def("canonicalize_board",
                  [](const std::vector<PyCard>& board) {
                      std::uint64_t canon_board = 0;
                      check_status(xiapl_canonicalize_board(
                          py_cards_to_mask(board), &canon_board));
                      return mask_to_py_cards(canon_board);
                  },
                  py::arg("board"));
    canon_mod.def("canonicalize_board_mask",
                  [](std::uint64_t board_mask) {
                      std::uint64_t canon_board = 0;
                      check_status(xiapl_canonicalize_board(board_mask,
                                                            &canon_board));
                      return canon_board;
                  },
                  py::arg("board_mask"));
    // The ids form, NOT the mask form: a mask cannot represent a duplicated
    // card, so "exactly two cards" and "exactly two distinct cards" are
    // different questions. [As, As] answers "AA" here and is an arity error
    // through canonicalize_hand_mask, and that difference is the C++
    // behaviour this binding has always exposed.
    canon_mod.def("canonicalize_hand",
                  [](const std::vector<PyCard>& hand) {
                      const CardIdBuffer ids(hand);
                      return fetch_string([&](char* buf, std::int32_t cap,
                                              std::int32_t* len) {
                          return xiapl_canonicalize_hand_ids(
                              ids.data(), ids.size(), buf, cap, len);
                      });
                  },
                  py::arg("hand"));
    canon_mod.def("canonicalize_hand_mask",
                  [](std::uint64_t hero_mask) {
                      return fetch_string([&](char* buf, std::int32_t cap,
                                              std::int32_t* len) {
                          return xiapl_canonicalize_hand_mask(hero_mask, buf,
                                                              cap, len);
                      });
                  },
                  py::arg("hero_mask"));
    // Pinned to the strict (v2) canonicalization, on purpose, and NOT exposed
    // as a parameter: it is the only generation this build has. Widening this
    // surface would let a script change the card abstraction of a run without
    // any artefact recording that it did; a caller that genuinely needs to name
    // a version should go through the C API (xiapl_generate_canonical_situations
    // takes an xiapl_canon_version_t) and stamp it into what it writes.
    //
    // This used to be pinned to the legacy canonicalization, which was removed;
    // the flop population this returns is therefore 1,286,792 orbits rather than
    // the 1,420,796 legacy representatives.
    canon_mod.def("generate_canonical_situations",
                  [](int board_size) {
                      xiapl_canonical_situations_t* raw = nullptr;
                      check_status(xiapl_generate_canonical_situations(
                          static_cast<std::int32_t>(board_size),
                          XIAPL_CANON_STRICT, &raw));
                      SituationsPtr situations(raw);
                      std::int64_t total = 0;
                      check_status(xiapl_canonical_situations_count(
                          situations.get(), &total));
                      std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
                      out.reserve(static_cast<std::size_t>(total));
                      std::vector<std::uint64_t> hero_chunk(kSituationChunk);
                      std::vector<std::uint64_t> board_chunk(kSituationChunk);
                      for (std::int64_t offset = 0; offset < total;) {
                          std::int64_t written = 0;
                          check_status(xiapl_canonical_situations_fill(
                              situations.get(), offset, kSituationChunk,
                              hero_chunk.data(), board_chunk.data(), &written));
                          // A short fill (written < kSituationChunk) means
                          // the enumeration is exhausted, which is the normal
                          // way this loop ends. written <= 0 while offset <
                          // total is a different thing: the fill call
                          // reported it filled nothing even though the count
                          // said more remained, which is a broken invariant
                          // between xiapl_canonical_situations_count and
                          // xiapl_canonical_situations_fill, not a valid
                          // input this binding received. That is defensive
                          // and unreachable today (count and fill walk the
                          // same enumeration), so it must fail loudly rather
                          // than silently truncate the result.
                          if (written <= 0) {
                              raise_runtime_error(
                                  "canonical situations fill stalled");
                          }
                          for (std::int64_t i = 0; i < written; ++i) {
                              out.emplace_back(hero_chunk[static_cast<std::size_t>(i)],
                                               board_chunk[static_cast<std::size_t>(i)]);
                          }
                          offset += written;
                      }
                      // Release the enumeration before pybind converts `out`
                      // into a Python list: the river population is ~2 GB per
                      // copy, and there is no reason to hold two.
                      situations.reset();
                      return out;
                  },
                  py::arg("board_size"));
}

}  // namespace xiapl_py
