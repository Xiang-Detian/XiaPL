"""Tests for the RangeEquityMode argument of calculate_range_equity.

Covers the Python-facing surface of the AggregateOnly mode: the
RangeEquityMode enum, the `mode` keyword argument on calculate_range_equity,
and the RangeEquityResult.aggregate_std_error field.
"""

from xiapl.range import Range
from xiapl.simulation import (
    RangeEquityMode,
    SimulationOptions,
    calculate_range_equity,
)

# Same hero/villain matchup used by the C++ contract tests (Task 1 plan
# example): narrow enough to be non-trivial, wide enough to have several
# compatible combo pairs.
_HERO_RANGE = "AhKh"
_VILLAIN_RANGE = "JJ+,AQs+,KQs"


def test_calculate_range_equity_default_mode_matches_explicit_per_combo():
    """Omitting `mode` must be bit-identical to passing PerCombo explicitly."""
    hero = Range.from_string(_HERO_RANGE)
    villain = Range.from_string(_VILLAIN_RANGE)
    opts = SimulationOptions.mc_seeded(20_000, 7)

    default_result = calculate_range_equity(hero, villain, 0, opts)
    explicit_result = calculate_range_equity(
        hero, villain, 0, opts, mode=RangeEquityMode.PerCombo
    )

    assert default_result.hero_aggregate_equity == explicit_result.hero_aggregate_equity
    assert default_result.villain_aggregate_equity == explicit_result.villain_aggregate_equity
    assert len(default_result.hero) == len(explicit_result.hero)
    assert default_result.trials == explicit_result.trials
    assert default_result.aggregate_std_error == 0.0
    assert explicit_result.aggregate_std_error == 0.0


def test_calculate_range_equity_aggregate_only_strips_breakdown():
    """AggregateOnly returns empty per-combo breakdowns and a sane aggregate."""
    hero = Range.from_string(_HERO_RANGE)
    villain = Range.from_string(_VILLAIN_RANGE)
    opts = SimulationOptions.mc_seeded(20_000, 7)

    result = calculate_range_equity(
        hero, villain, 0, opts, mode=RangeEquityMode.AggregateOnly
    )

    assert result.hero == []
    assert result.villain == []
    assert 0.44 <= result.hero_aggregate_equity <= 0.50


def test_calculate_range_equity_aggregate_only_std_error_positive():
    """MC AggregateOnly reports a positive standard error for the aggregate."""
    hero = Range.from_string(_HERO_RANGE)
    villain = Range.from_string(_VILLAIN_RANGE)
    opts = SimulationOptions.mc_seeded(20_000, 7)

    result = calculate_range_equity(
        hero, villain, 0, opts, mode=RangeEquityMode.AggregateOnly
    )

    assert result.aggregate_std_error > 0.0


def test_calculate_range_equity_aggregate_only_seed_reproducible():
    """Two AggregateOnly calls with the same seed give the same result."""
    hero = Range.from_string(_HERO_RANGE)
    villain = Range.from_string(_VILLAIN_RANGE)

    r1 = calculate_range_equity(
        hero, villain, 0, SimulationOptions.mc_seeded(20_000, 7),
        mode=RangeEquityMode.AggregateOnly,
    )
    r2 = calculate_range_equity(
        hero, villain, 0, SimulationOptions.mc_seeded(20_000, 7),
        mode=RangeEquityMode.AggregateOnly,
    )

    assert r1.hero_aggregate_equity == r2.hero_aggregate_equity
    assert r1.aggregate_std_error == r2.aggregate_std_error
    assert r1.trials == r2.trials


def test_calculate_range_equity_per_combo_std_error_is_zero():
    """PerCombo never populates aggregate_std_error, MC or exact."""
    hero = Range.from_string(_HERO_RANGE)
    villain = Range.from_string(_VILLAIN_RANGE)

    mc_result = calculate_range_equity(
        hero, villain, 0, SimulationOptions.mc_seeded(20_000, 7),
        mode=RangeEquityMode.PerCombo,
    )
    exact_result = calculate_range_equity(
        hero, villain, 0, SimulationOptions.exact(),
        mode=RangeEquityMode.PerCombo,
    )

    assert mc_result.aggregate_std_error == 0.0
    assert exact_result.aggregate_std_error == 0.0
