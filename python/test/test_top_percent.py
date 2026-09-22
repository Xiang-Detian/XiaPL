"""Tests for xiapl.range.rank_starting_hands / generate_top_percent_range
(Task 5 of the phh-range-utils plan): the exact-table promotion of
top-percent starting-hand selection out of the C ABI's old MC-noise
implementation. Mirrors the doctest cases in tests/test_preflop_rank.cpp
plus the Python-specific surface (ValueError mapping, kw-only `game`).
"""

import pytest

from xiapl.range import Range, generate_top_percent_range, rank_starting_hands
from xiapl.simulation import GameType


def test_full_ranking_is_169_labels_aa_first():
    labels = rank_starting_hands(1.0)

    assert len(labels) == 169
    assert labels[0] == "AA"
    assert len(set(labels)) == 169


def test_default_top_percent_is_full_ranking():
    assert rank_starting_hands() == rank_starting_hands(1.0)


def test_floor_semantics_keep_33_labels_at_0_2():
    top20 = rank_starting_hands(0.2)
    assert len(top20) == 33


def test_below_one_over_169_is_empty():
    assert rank_starting_hands(0.005) == []


@pytest.mark.parametrize("bad", [0.0, 1.01, -1.0])
def test_top_percent_out_of_range_raises_value_error(bad):
    with pytest.raises(ValueError):
        rank_starting_hands(bad)
    with pytest.raises(ValueError):
        generate_top_percent_range(bad)


def test_plo_raises_value_error_naming_the_0_2_roadmap():
    with pytest.raises(ValueError, match="0.2"):
        rank_starting_hands(1.0, game=GameType.Plo)
    with pytest.raises(ValueError, match="0.2"):
        generate_top_percent_range(1.0, game=GameType.Plo)


def test_generate_top_percent_range_matches_label_expansion_at_0_2():
    labels = rank_starting_hands(0.2)
    assert len(labels) == 33

    expected_combos = sum(len(Range.from_string(label).combos()) for label in labels)

    result = generate_top_percent_range(0.2)

    assert len(result.combos()) == expected_combos
    assert result.game == GameType.Holdem
    for combo in result.combos():
        assert combo.weight == pytest.approx(1.0)

    masks = [c.mask for c in result.combos()]
    assert masks == sorted(masks)


def test_generate_top_percent_range_full_ranking_has_1326_combos():
    result = generate_top_percent_range(1.0)
    assert len(result.combos()) == 1326


def test_game_is_keyword_only():
    """`game` is a rule selector, never something a reader has to decode
    positionally, so it is keyword-only -- matching the eval/equity/set-op
    entry points (see test_simulation_bindings.py)."""
    with pytest.raises(TypeError):
        rank_starting_hands(1.0, GameType.Holdem)
    with pytest.raises(TypeError):
        generate_top_percent_range(0.2, GameType.Holdem)
