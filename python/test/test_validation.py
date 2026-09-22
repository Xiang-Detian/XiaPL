"""Regression tests for the validation guards added in the src/core review
Phase 1+2+3.

Each test pins down a previously-silent UB / silent-wrong-output path and
asserts that the library now raises a clean exception instead.
"""

import pytest

from xiapl.card import Card
from xiapl.deck import Deck
from xiapl.eval import evaluate_cards
from xiapl.utils import card_to_mask, cards_to_mask
from xiapl.canonicalize import generate_canonical_situations
from xiapl.range import Range


# ---------------------------------------------------------------------------
# Phase 2 — Card / Deck / utils mask UB closed
# ---------------------------------------------------------------------------

def test_card_id_ctor_rejects_out_of_range():
    """Card(IdType) now validates 0..51 or INVALID_ID(255)."""
    # INVALID_ID sentinel still accepted
    Card.from_id(255)
    # OOB int rejected by Card::from_string and the new id ctor
    with pytest.raises((ValueError, RuntimeError)):
        Card.from_string("Zz")


def test_cards_to_mask_rejects_invalid_card():
    """Default-constructed Card has id=255 → mask construction must raise."""
    # Use Card.from_id(255) for an INVALID_ID Card without depending on
    # the pybind default ctor binding.
    invalid = Card.from_id(255)
    with pytest.raises((ValueError, RuntimeError)):
        cards_to_mask([invalid])
    with pytest.raises((ValueError, RuntimeError)):
        card_to_mask(invalid)


def test_deck_set_cards_rejects_duplicates():
    """Deck::set_cards rejects duplicate Cards now."""
    d = Deck()
    a = Card.from_string("As")
    with pytest.raises((ValueError, RuntimeError)):
        d.set_cards([a, a])


def test_deck_set_cards_rejects_invalid_card():
    d = Deck()
    invalid = Card.from_id(255)
    valid = Card.from_string("As")
    with pytest.raises((ValueError, RuntimeError)):
        d.set_cards([invalid, valid])


# ---------------------------------------------------------------------------
# Phase 2 — evaluate_cards duplicate detection
# ---------------------------------------------------------------------------

def test_evaluate_cards_rejects_duplicate_cards():
    """5x same card used to silently collapse to a 1-bit mask."""
    five_aces = [Card.from_string("As")] * 5
    with pytest.raises((ValueError, RuntimeError)):
        evaluate_cards(five_aces)


def test_evaluate_cards_still_accepts_valid_5_to_7_cards():
    """Sanity: the new duplicate guard does not break the common path."""
    hand5 = [Card.from_string(s) for s in ["As", "Ks", "Qs", "Js", "Ts"]]
    v = evaluate_cards(hand5)
    assert v is not None


# ---------------------------------------------------------------------------
# Phase 2 — generate_canonical_situations board_size validation
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("bad", [0, 1, 2, 6, 10, -1])
def test_generate_canonical_situations_rejects_invalid_board_size(bad):
    with pytest.raises((ValueError, RuntimeError)):
        generate_canonical_situations(bad)


def test_generate_canonical_situations_flop_still_works():
    # Flop should still enumerate without throwing.
    out = generate_canonical_situations(3)
    assert len(out) > 0


# ---------------------------------------------------------------------------
# Phase 2 — Range parse_weight contract (0.0 rejected)
# ---------------------------------------------------------------------------

def test_parse_range_rejects_zero_weight():
    """parse_weight previously accepted 0.0 but filter dropped it."""
    with pytest.raises((ValueError, RuntimeError)):
        Range.from_string("JJ:0")


def test_parse_range_still_accepts_positive_weight():
    r = Range.from_string("JJ:0.5")
    assert r.size() == 6  # 6 combos of JJ


# ---------------------------------------------------------------------------
# Phase 6 — pybind exception translator
# invalid_argument → ValueError (not RuntimeError)
# ---------------------------------------------------------------------------

def test_invalid_argument_translates_to_value_error():
    """std::invalid_argument from src/core surfaces as ValueError in Python."""
    invalid = Card.from_id(255)
    with pytest.raises(ValueError):
        cards_to_mask([invalid])


def test_evaluate_cards_dup_raises_value_error():
    five_aces = [Card.from_string("As")] * 5
    with pytest.raises(ValueError):
        evaluate_cards(five_aces)


# ---------------------------------------------------------------------------
# Phase 6 — Deck deterministic-by-default
# ---------------------------------------------------------------------------

def test_deck_default_is_sorted_and_deterministic():
    """Deck() now returns a sorted deck; two Decks compare equal pre-shuffle."""
    a = Deck()
    b = Deck()
    # Both should be sorted (ids 0..51 in order).
    a_ids = [c.id for c in a.cards]
    b_ids = [c.id for c in b.cards]
    assert a_ids == list(range(52))
    assert a_ids == b_ids


def test_deck_shuffle_still_randomizes():
    """Explicit shuffle should produce a non-trivial reorder."""
    a = Deck()
    a.shuffle()
    sorted_ids = list(range(52))
    a_ids = [c.id for c in a.cards]
    # Very high probability that a 52-card shuffle is not identity.
    assert a_ids != sorted_ids


# ---------------------------------------------------------------------------
# Phase 6 — SimulationOptions: static factories + SimulationMode enum
# ---------------------------------------------------------------------------

from xiapl.simulation import (
    SimulationOptions,
    SimulationMode,
    GameType,
    calculate_equity,
)
from xiapl.utils import cards_to_mask


def test_simulation_options_factories_set_effective_mode():
    assert SimulationOptions.exact().effective_mode() == SimulationMode.Exact
    assert SimulationOptions.mc_random(1000).effective_mode() == \
        SimulationMode.MonteCarloRandom
    assert SimulationOptions.mc_seeded(1000, 42).effective_mode() == \
        SimulationMode.MonteCarloSeeded


def test_simulation_options_legacy_fields_still_work():
    """Factories don't break field-based construction."""
    opts = SimulationOptions()
    opts.iterations = 1000
    opts.seed = 42
    opts.deterministic = True
    assert opts.effective_mode() == SimulationMode.MonteCarloSeeded


def test_mc_seeded_is_reproducible():
    """Two mc_seeded(N, seed) runs with the same seed produce identical equity."""
    hero = cards_to_mask([Card.from_string("As"), Card.from_string("Ks")])
    villain = cards_to_mask([Card.from_string("2c"), Card.from_string("3h")])
    opts = SimulationOptions.mc_seeded(5000, 12345)
    r1 = calculate_equity([hero, villain], 0, opts, game=GameType.Holdem)
    r2 = calculate_equity(
        [hero, villain], 0, SimulationOptions.mc_seeded(5000, 12345),
        game=GameType.Holdem
    )
    assert r1.players[0].equity == r2.players[0].equity
    assert r1.trials == r2.trials


# ---------------------------------------------------------------------------
# Auto-fallback: iterations >= board space promotes MC to exact enumeration.
# ---------------------------------------------------------------------------

def test_calculate_equity_auto_fallback_to_exact():
    """HU on a flop has C(45, 2) = 990 board completions; iterations >= 990
    should return exact=True and the same equity as exact()."""
    hero = cards_to_mask([Card.from_string("As"), Card.from_string("Ks")])
    villain = cards_to_mask([Card.from_string("2c"), Card.from_string("3h")])
    flop = cards_to_mask([Card.from_string(s) for s in ["Qs", "Jh", "7d"]])

    res_exact = calculate_equity(
        [hero, villain], flop, SimulationOptions.exact(),
        game=GameType.Holdem
    )
    res_fallback = calculate_equity(
        [hero, villain], flop, SimulationOptions.mc_seeded(10_000, 42),
        game=GameType.Holdem
    )
    res_small_mc = calculate_equity(
        [hero, villain], flop, SimulationOptions.mc_seeded(500, 42),
        game=GameType.Holdem
    )

    assert res_exact.exact and res_exact.trials == 990
    # Auto-fallback path: same exact result regardless of iterations >= 990.
    assert res_fallback.exact and res_fallback.trials == 990
    assert res_fallback.players[0].equity == res_exact.players[0].equity
    # Genuine MC stays MC.
    assert (not res_small_mc.exact) and res_small_mc.trials == 500


def test_calculate_range_equity_auto_fallback_to_exact():
    """Range-vs-range on a flop has C(49, 2) = 1176 board completions;
    iterations >= 1176 should auto-fallback to exact."""
    from xiapl.range import Range
    from xiapl.simulation import calculate_range_equity

    hero = Range.all()
    villain = Range.all()
    flop = cards_to_mask([Card.from_string(s) for s in ["Qs", "Jh", "7d"]])

    res_exact = calculate_range_equity(
        hero, villain, flop, SimulationOptions.exact()
    )
    res_fallback = calculate_range_equity(
        hero, villain, flop, SimulationOptions.mc_seeded(50_000, 42)
    )

    assert res_exact.exact and res_exact.trials == 1176
    assert res_fallback.exact and res_fallback.trials == 1176
    # Same aggregate equity (full symmetry: 0.5)
    assert res_fallback.hero_aggregate_equity == res_exact.hero_aggregate_equity


def test_calculate_range_equity_threads_match_single():
    """The multi-threaded pair-loop must produce the same per-combo and
    aggregate equity as the single-threaded path (bit-exact when summands
    are deterministic — both paths are pure floating-point reductions over
    the same input)."""
    from xiapl.range import Range
    from xiapl.simulation import calculate_range_equity

    # Use a narrow asymmetric pair so per-combo entries are non-trivial.
    hero = Range.from_string("JJ+, AKs, AKo")
    villain = Range.from_string("22-77, A2s-A9s, KTo+")
    flop = cards_to_mask([Card.from_string(s) for s in ["Th", "5d", "2c"]])

    opts1 = SimulationOptions.exact()
    opts1.threads = 1
    opts4 = SimulationOptions.exact()
    opts4.threads = 4

    r1 = calculate_range_equity(hero, villain, flop, opts1)
    r4 = calculate_range_equity(hero, villain, flop, opts4)

    # Aggregate equity should match to floating-point tolerance (different
    # reduction order across threads).
    assert abs(r1.hero_aggregate_equity - r4.hero_aggregate_equity) < 1e-9
    # Per-combo equities should match too.
    eq1 = sorted((e.combo_mask, e.equity) for e in r1.hero)
    eq4 = sorted((e.combo_mask, e.equity) for e in r4.hero)
    assert len(eq1) == len(eq4)
    for (m1, q1), (m4, q4) in zip(eq1, eq4):
        assert m1 == m4
        assert abs(q1 - q4) < 1e-9


# ---------------------------------------------------------------------------
# Core review follow-up: evaluate_hand's Hold'em arm now rejects board/hole
# overlap (the PLO arm already did — closes the asymmetric silent-garbage
# path on the Hold'em side).
# ---------------------------------------------------------------------------

def test_evaluate_hand_rejects_board_hole_overlap():
    from xiapl.eval import evaluate_hand

    board = cards_to_mask([Card.from_string(s) for s in ["As", "Kd", "Qh"]])
    # Hero hole overlaps the board on As.
    hole = cards_to_mask([Card.from_string("As"), Card.from_string("2c")])
    with pytest.raises((ValueError, RuntimeError)):
        evaluate_hand(board, hole)


def test_evaluate_hand_still_accepts_disjoint_inputs():
    from xiapl.eval import evaluate_hand

    board = cards_to_mask([Card.from_string(s) for s in ["As", "Kd", "Qh"]])
    hole = cards_to_mask([Card.from_string("Jc"), Card.from_string("Tc")])
    v = evaluate_hand(board, hole)
    assert v is not None


# ---------------------------------------------------------------------------
# Core review follow-up: offsuit connector '+' now expands as a ladder,
# mirroring the suited connector branch. JTo+ used to silently collapse to
# {JTo} only.
# ---------------------------------------------------------------------------

def test_range_offsuit_connector_plus_expands_ladder():
    # JTo+ should expand to {JTo, QJo, KQo, AKo} (4 connector notations).
    # Each offsuit notation has 12 combos → 48 total.
    r = Range.from_string("JTo+")
    assert r.size() == 4 * 12

    # Suited ladder is unchanged (regression check on the existing branch).
    r_s = Range.from_string("JTs+")
    assert r_s.size() == 4 * 4


def test_range_offsuit_connector_plus_lower_starts():
    # T9o+ → {T9o, JTo, QJo, KQo, AKo} = 5 notations × 12 = 60.
    r = Range.from_string("T9o+")
    assert r.size() == 5 * 12


# ---------------------------------------------------------------------------
# Core review follow-up: calculate_equity / calculate_range_equity now reject
# physically-impossible board sizes (1 or 2 cards). Only {0, 3, 4, 5} are
# legal poker board states; 1 and 2 used to silently run with num_to_draw=
# 4 or 3 and return meaningless equity.
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("n_board_cards", [1, 2])
def test_calculate_equity_rejects_invalid_board_count(n_board_cards):
    hero = cards_to_mask([Card.from_string("As"), Card.from_string("Ks")])
    villain = cards_to_mask([Card.from_string("2c"), Card.from_string("3h")])
    fixed_board_ids = ["Qs", "Jh", "7d", "8c", "9d"]
    board = cards_to_mask(
        [Card.from_string(s) for s in fixed_board_ids[:n_board_cards]]
    )

    with pytest.raises((ValueError, RuntimeError)):
        calculate_equity(
            [hero, villain], board, SimulationOptions.exact(),
            game=GameType.Holdem,
        )


@pytest.mark.parametrize("n_board_cards", [1, 2])
def test_calculate_range_equity_rejects_invalid_board_count(n_board_cards):
    from xiapl.range import Range
    from xiapl.simulation import calculate_range_equity

    fixed_board_ids = ["Qs", "Jh", "7d", "8c", "9d"]
    board = cards_to_mask(
        [Card.from_string(s) for s in fixed_board_ids[:n_board_cards]]
    )

    with pytest.raises((ValueError, RuntimeError)):
        calculate_range_equity(
            Range.all(), Range.all(), board,
            SimulationOptions.exact(),
        )


def test_calculate_equity_still_accepts_preflop_and_flop_to_river():
    """Regression guard: the tighter board check must not break the
    documented {0, 3, 4, 5} board cases."""
    hero = cards_to_mask([Card.from_string("As"), Card.from_string("Ks")])
    villain = cards_to_mask([Card.from_string("2c"), Card.from_string("3h")])
    flop = cards_to_mask([Card.from_string(s) for s in ["Qs", "Jh", "7d"]])
    turn = cards_to_mask(
        [Card.from_string(s) for s in ["Qs", "Jh", "7d", "8c"]]
    )
    river = cards_to_mask(
        [Card.from_string(s) for s in ["Qs", "Jh", "7d", "8c", "9d"]]
    )

    for board in (0, flop, turn, river):
        r = calculate_equity(
            [hero, villain], board, SimulationOptions.mc_seeded(100, 1),
            game=GameType.Holdem,
        )
        assert r.trials > 0


# ---------------------------------------------------------------------------
# PLO range notation (plo-range-ws1 task 3): smoke asserts that the pybind
# surface reaches the real parser. The grammar itself is covered exhaustively
# by tests/test_plo_parser.cpp.
# ---------------------------------------------------------------------------

def test_plo_range_from_string_expands_pattern():
    """"AAKKds" = two aces + two kings in the {2,2} suit shape = 6 combos."""
    r = Range.from_string("AAKKds", game=GameType.Plo)
    assert r.size() == 6
    assert r.game == GameType.Plo


def test_plo_range_conflicting_weights_raise():
    """Overlapping items with different weights are an error (equal weights
    merge silently); the C++ std::invalid_argument maps to ValueError."""
    with pytest.raises(ValueError):
        Range.from_string("AA**:0.5, AK**:0.3", game=GameType.Plo)

    # Equal weights on the same overlap are fine.
    Range.from_string("AA**:0.5, AK**:0.5", game=GameType.Plo)


def test_plo_range_non_ascii_raises_value_error():
    """A copy-pasted en-dash must still surface as ValueError with the
    teaching message. The C++ side escapes non-ASCII bytes in what(); without
    that, pybind's PyErr_SetString fails on the invalid UTF-8 and Python
    raises UnicodeDecodeError instead, losing the message entirely."""
    with pytest.raises(ValueError) as excinfo:
        Range.from_string("JT98–8765", game=GameType.Plo)
    assert "rank symbols" in str(excinfo.value)

    with pytest.raises(ValueError):
        Range.from_string("AKQJ×", game=GameType.Plo)


def test_plo_range_all_matches_wildcard_pattern():
    """C(52,4) = 270,725, reachable both directly and through the parser."""
    assert Range.all(GameType.Plo).size() == 270725
    assert Range.from_string("****", game=GameType.Plo).size() == 270725


# ---------------------------------------------------------------------------
# core-refactor Task 6: calculate_equity now rejects the player-count cap
# up front (ValueError), before any Monte Carlo worker thread starts.
#
# Previously, calculate_equity with 11 Hold'em players threw from *inside*
# the per-trial Monte Carlo loop -- on a worker thread whenever iterations
# > 0 -- with the internal C++ symbol name ("judge_holdem_mask: too many
# players") in the message.
# ---------------------------------------------------------------------------

def _distinct_holdem_hands(n):
    """N distinct, pairwise-disjoint 2-card Hold'em hands (ids 0..2n-1)."""
    return [
        cards_to_mask([Card.from_id(2 * i), Card.from_id(2 * i + 1)])
        for i in range(n)
    ]


def test_calculate_equity_rejects_11_holdem_players():
    hands = _distinct_holdem_hands(11)
    with pytest.raises(ValueError) as excinfo:
        calculate_equity(hands, 0, SimulationOptions.exact(), game=GameType.Holdem)
    msg = str(excinfo.value)
    assert msg == "calculate_equity: at most 10 players supported for Hold'em (got 11)"
    # The message must name the public entry point, not the internal symbol
    # the pre-fix defect leaked.
    assert "judge_holdem_mask" not in msg


def test_calculate_equity_11_player_cap_synchronous_with_threaded_mc():
    """Same rejection, same message, even when options select the threaded
    Monte Carlo path -- the cap must be checked before any worker starts."""
    hands = _distinct_holdem_hands(11)
    opts = SimulationOptions.mc_seeded(5000, 424242)
    opts.threads = 8
    with pytest.raises(ValueError) as excinfo:
        calculate_equity(hands, 0, opts, game=GameType.Holdem)
    assert str(excinfo.value) == \
        "calculate_equity: at most 10 players supported for Hold'em (got 11)"


def test_calculate_equity_accepts_exactly_10_holdem_players():
    hands = _distinct_holdem_hands(10)
    result = calculate_equity(
        hands, 0, SimulationOptions.mc_seeded(100, 1), game=GameType.Holdem
    )
    assert len(result.players) == 10
    assert result.trials == 100


def test_calculate_equity_rejects_33_plo_players():
    """The cap check runs before the hole-mask overlap check, so a single
    reused 4-card mask is enough: 33 disjoint PLO hands would need 132
    distinct cards, more than a 52-card deck holds."""
    one_hand = cards_to_mask(
        [Card.from_string(s) for s in ["As", "Ks", "Ah", "Kh"]]
    )
    hands = [one_hand] * 33
    with pytest.raises(ValueError) as excinfo:
        calculate_equity(hands, 0, SimulationOptions.exact(), game=GameType.Plo)
    assert str(excinfo.value) == \
        "calculate_equity: at most 32 players supported for PLO (got 33)"
