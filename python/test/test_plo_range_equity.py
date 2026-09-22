"""Python-facing smoke tests for PLO range-vs-range equity.

calculate_range_equity gained PLO support in plo-range-ws1 task 4
(src/core/equity_range.cpp, include/xiapl/simulation.h); the bindings themselves
were not touched (calculate_range_equity's lambda already forwards to the
tagged Range objects), so this file only needs to confirm the Python-facing
surface exercises the new dispatch: AggregateOnly MC on narrow PLO ranges,
the GameType tag-mismatch error, and PerCombo's 4-bit combo masks. The C++
behavior itself is covered exhaustively by tests/test_plo_range_equity.cpp.
"""

import pytest

from xiapl.range import Range
from xiapl.simulation import (
    GameType,
    RangeEquityMode,
    SimulationOptions,
    calculate_range_equity,
)

# Narrow enough to be a realistic PLO spot, wide enough to have several
# compatible combo pairs (no full-blocking risk for AggregateOnly's rejection
# guard).
_HERO_RANGE = "AAKKds"
_VILLAIN_RANGE = "QQJJds,JT98ds"


def test_plo_aggregate_only_returns_equity_in_unit_interval():
    """AggregateOnly MC on two narrow PLO ranges: equity in (0,1), trials ==
    iterations, exact is False (AggregateOnly MC never auto-falls back)."""
    hero = Range.from_string(_HERO_RANGE, game=GameType.Plo)
    villain = Range.from_string(_VILLAIN_RANGE, game=GameType.Plo)
    opts = SimulationOptions.mc_seeded(20_000, 7)

    result = calculate_range_equity(
        hero, villain, 0, opts, mode=RangeEquityMode.AggregateOnly
    )

    assert 0.0 < result.hero_aggregate_equity < 1.0
    assert 0.0 < result.villain_aggregate_equity < 1.0
    assert result.trials == 20_000
    assert result.exact is False


def test_plo_villain_range_returns_equity_in_unit_interval_too():
    """Same call, checking the villain side isn't a degenerate 0/1 constant."""
    hero = Range.from_string(_HERO_RANGE, game=GameType.Plo)
    villain = Range.from_string(_VILLAIN_RANGE, game=GameType.Plo)
    opts = SimulationOptions.mc_seeded(20_000, 11)

    result = calculate_range_equity(
        hero, villain, 0, opts, mode=RangeEquityMode.AggregateOnly
    )

    assert abs(result.hero_aggregate_equity + result.villain_aggregate_equity - 1.0) < 1e-9


def test_calculate_range_equity_hero_villain_game_tag_mismatch_raises_value_error():
    """A Hold'em hero against a PLO villain (or vice versa) must raise
    ValueError -- calculate_range_equity requires both ranges to carry the
    same GameType tag."""
    holdem_hero = Range.from_string("AA")
    plo_villain = Range.from_string("QQJJds", game=GameType.Plo)
    opts = SimulationOptions.mc_seeded(1_000, 7)

    with pytest.raises(ValueError):
        calculate_range_equity(holdem_hero, plo_villain, 0, opts)

    with pytest.raises(ValueError):
        calculate_range_equity(plo_villain, holdem_hero, 0, opts)


def test_plo_per_combo_exact_emits_4_bit_masks():
    """PerCombo (exact) on a PLO range: every combo mask must have exactly 4
    bits set (Hold'em combos have 2)."""
    hero = Range.from_string("AAKKds", game=GameType.Plo)
    villain = Range.from_string("QQJJds", game=GameType.Plo)
    opts = SimulationOptions.exact()

    result = calculate_range_equity(
        hero, villain, 0, opts, mode=RangeEquityMode.PerCombo
    )

    assert len(result.hero) > 0
    assert len(result.villain) > 0
    for entry in result.hero:
        assert bin(entry.combo_mask).count("1") == 4
    for entry in result.villain:
        assert bin(entry.combo_mask).count("1") == 4
