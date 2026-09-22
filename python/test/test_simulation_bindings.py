"""Smoke tests for the Python-facing pybind11 surface itself (Task 3 of the
2026-08-07-api-surface plan): enum casing, enum-value leakage into the
enclosing submodule, and the card-spelled mask-validation error message.

These are binding-layer regression tests, not behavioral ones -- see
test_range_equity_mode.py / test_equity.py for the semantics of the APIs
touched here.
"""

import pytest


def test_range_equity_mode_pascal_case_canonical():
    """PascalCase is the canonical spelling; the pre-0.1 SCREAMING_SNAKE
    names (PER_COMBO / AGGREGATE_ONLY) are kept as deprecated aliases of the
    same enum value, not separate values."""
    from xiapl.simulation import RangeEquityMode

    assert RangeEquityMode.PerCombo == RangeEquityMode.PER_COMBO
    assert RangeEquityMode.AggregateOnly == RangeEquityMode.AGGREGATE_ONLY


def test_enum_values_not_leaked_into_submodule():
    """.export_values() used to copy every enum member onto the enclosing
    submodule (e.g. xiapl.simulation.Holdem alongside xiapl.simulation.GameType
    .Holdem); dropping it keeps the submodule namespace to actual functions
    and types."""
    import xiapl.simulation as sim
    import xiapl.eval as ev

    for leaked in ("Holdem", "Plo", "Exact", "PER_COMBO", "HighCard"):
        assert not hasattr(sim, leaked), leaked
        assert not hasattr(ev, leaked), leaked


def test_combo_mask_error_spells_cards():
    """Range's combo-size validation error spells the offending mask as card
    names (matching the PLO parser's own error style) instead of printing
    the raw decimal mask value.

    mask=0b11 sets bits 0 and 1, which (id = suit*13 + (rank-2), suit 0=C)
    are "2c" (id 0) and "3c" (id 1) -- describe_mask renders them low-bit
    first, so the message contains "2c3c".
    """
    from xiapl.range import Range, Combo
    from xiapl.simulation import GameType

    bad = Combo()
    bad.mask = 0b11  # 2c3c -- two cards, invalid for PLO (needs 4)
    bad.weight = 1.0
    with pytest.raises(ValueError, match="2c3c"):
        Range([bad], GameType.Plo)


def test_combo_mask_error_falls_back_to_decimal_for_out_of_deck_bits():
    """A bit outside the 52-card deck cannot be spelled as a card; the error
    message falls back to a "#<decimal>" rendering instead of throwing while
    building its own message."""
    from xiapl.range import Range, Combo
    from xiapl.simulation import GameType

    bad = Combo()
    bad.mask = (1 << 55) | 0b111  # 3 real cards + 1 out-of-deck bit
    bad.weight = 1.0
    with pytest.raises(ValueError, match=r"#\d+"):
        Range([bad], GameType.Holdem)


# ---------------------------------------------------------------------------
# Task 4: the game-unified core API. One entry point per concept with a
# trailing `game`; the split-by-game functions are deleted (pre-publication,
# no aliases). The dispatch lives in the C++ library, so these tests are
# checking that the thin bindings expose it faithfully.
# ---------------------------------------------------------------------------

def _mask(*names):
    from xiapl.card import Card
    from xiapl.utils import cards_to_mask

    return cards_to_mask([Card.from_string(n) for n in names])


def test_eval_module_exposes_unified_api_only():
    import xiapl.eval as ev

    for name in ("evaluate_cards", "evaluate_mask", "evaluate_hand", "judge"):
        assert hasattr(ev, name), name

    for old_name in (
        "evaluate_best",
        "evaluate_holdem",
        "evaluate_plo",
        "determine_winners_holdem",
        "determine_winners_plo",
    ):
        assert not hasattr(ev, old_name), old_name


def test_range_all_replaces_the_per_game_factories():
    from xiapl.range import Range
    from xiapl.simulation import GameType

    assert not hasattr(Range, "all_holdem")
    assert not hasattr(Range, "all_plo")

    assert Range.all().size() == 1326
    assert Range.all().game == GameType.Holdem
    assert Range.all(GameType.Plo).size() == 270725
    assert Range.all(GameType.Plo).game == GameType.Plo


def test_judge_defaults_to_holdem_and_dispatches_to_plo():
    from xiapl.eval import judge
    from xiapl.simulation import GameType

    holdem_players = [_mask("2s", "3s"), _mask("4h", "5h")]
    holdem_board = _mask("6s", "7s", "8s", "9c", "Td")
    # Flush beats straight.
    assert judge(holdem_players, holdem_board) == [0]
    assert judge(holdem_players, holdem_board) == judge(
        holdem_players, holdem_board, game=GameType.Holdem)

    # PLO: aces-up beats kings-up over exactly 2 hole + 3 board.
    plo_players = [_mask("As", "Ah", "9d", "8c"), _mask("Ks", "Kh", "9h", "8d")]
    plo_board = _mask("Ac", "Kc", "2d", "3h", "5s")
    assert judge(plo_players, plo_board, game=GameType.Plo) == [0]


def test_evaluate_hand_defaults_to_holdem_and_dispatches_to_plo():
    from xiapl.eval import HandCategory, evaluate_hand
    from xiapl.simulation import GameType

    holdem_board = _mask("Ah", "Kd", "7s", "2c", "9h")
    holdem_hole = _mask("As", "Ac")
    assert evaluate_hand(holdem_board, holdem_hole).category == \
        HandCategory.ThreeOfAKind
    assert evaluate_hand(holdem_board, holdem_hole).category == \
        evaluate_hand(holdem_board, holdem_hole,
                      game=GameType.Holdem).category

    plo_board = _mask("Ac", "Kc", "2d", "3h", "3s")
    plo_hole = _mask("As", "Ah", "Kd", "Ks")
    plo = evaluate_hand(plo_board, plo_hole, game=GameType.Plo)
    assert plo.category == HandCategory.FullHouse
    assert plo.kickers[:2] == [14, 3]


def test_evaluate_cards_takes_loose_cards():
    from xiapl.card import Card
    from xiapl.eval import HandCategory, evaluate_cards

    hand = [Card.from_string(s) for s in ["As", "Ks", "Qs", "Js", "Ts"]]
    assert evaluate_cards(hand).category == HandCategory.StraightFlush


def test_game_is_keyword_only_on_the_eval_and_equity_entry_points():
    """`game` is a rule selector, never something a reader has to decode
    positionally, so it is keyword-only in Python."""
    from xiapl.eval import evaluate_hand, judge
    from xiapl.simulation import GameType

    players = [_mask("2s", "3s"), _mask("4h", "5h")]
    board = _mask("6s", "7s", "8s", "9c", "Td")
    with pytest.raises(TypeError):
        judge(players, board, GameType.Holdem)
    with pytest.raises(TypeError):
        evaluate_hand(board, _mask("As", "Ac"), GameType.Holdem)


def test_calculate_equity_takes_game_last_as_a_keyword():
    """The old signature led with the game (calculate_equity(game, holes,
    board, options)); it is now the trailing keyword-only selector."""
    from xiapl.simulation import (GameType, SimulationOptions,
                                calculate_equity)

    hero = _mask("As", "Ks")
    villain = _mask("2c", "3h")
    opts = SimulationOptions.mc_seeded(500, 1)

    # Old leading-game call form no longer type-checks.
    with pytest.raises(TypeError):
        calculate_equity(GameType.Holdem, [hero, villain], 0, opts)
    # Positional `game` is rejected too.
    with pytest.raises(TypeError):
        calculate_equity([hero, villain], 0, opts, GameType.Holdem)

    default = calculate_equity([hero, villain], 0, opts)
    explicit = calculate_equity([hero, villain], 0, opts,
                                game=GameType.Holdem)
    assert default.players[0].equity == explicit.players[0].equity

    plo_hero = _mask("As", "Ks", "Qs", "Js")
    plo_villain = _mask("2c", "3h", "7d", "8c")
    plo = calculate_equity([plo_hero, plo_villain], 0, opts,
                           game=GameType.Plo)
    assert 0.0 < plo.players[0].equity < 1.0
