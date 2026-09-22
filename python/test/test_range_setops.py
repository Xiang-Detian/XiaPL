"""Tests for the weighted set operations on xiapl.range.Range (Task 3 of the
phh-range-utils plan): union/intersection/difference methods and the
|/&/- operators. Mirrors the doctest highlights in
tests/test_range_setops.cpp plus the Python-specific surface (dunders,
TypeError/ValueError mapping).
"""

import pytest

from xiapl.range import Range
from xiapl.simulation import GameType


def test_union_matches_combined_notation():
    a = Range.from_string("AA")
    b = Range.from_string("KK")
    expected = Range.from_string("AA,KK")

    result = a.union(b)

    result_masks = sorted(c.mask for c in result.combos())
    expected_masks = sorted(c.mask for c in expected.combos())
    assert result_masks == expected_masks


def test_union_weight_is_max():
    a = Range.from_string("AA:0.3")
    b = Range.from_string("AA:0.7")

    result = a.union(b)

    assert len(result.combos()) == 6
    for combo in result.combos():
        assert combo.weight == pytest.approx(0.7)


def test_intersection_weight_is_min():
    a = Range.from_string("AA:0.3")
    b = Range.from_string("AA:0.7")

    result = a.intersection(b)

    assert len(result.combos()) == 6
    for combo in result.combos():
        assert combo.weight == pytest.approx(0.3)


def test_difference_weight_is_bounded_subtraction():
    a = Range.from_string("AA:0.7")
    b = Range.from_string("AA:0.3")

    result = a.difference(b)

    assert len(result.combos()) == 6
    for combo in result.combos():
        assert combo.weight == pytest.approx(0.4)


def test_difference_self_is_empty():
    a = Range.from_string("AA:0.7")

    result = a.difference(a)

    assert result.empty()


def test_intersection_disjoint_is_empty():
    a = Range.from_string("AA")
    b = Range.from_string("KK")

    assert a.intersection(b).empty()


def test_results_sorted_ascending_by_mask():
    # Hold'em from_string emits token order, not sorted -- "KK,AA" is the
    # reverse of ascending mask order. The set op output must still be
    # ascending regardless.
    a = Range.from_string("KK,AA")
    b = Range.from_string("QQ")

    result = a.union(b)

    masks = [c.mask for c in result.combos()]
    assert masks == sorted(masks)


def test_gametype_mismatch_raises_value_error():
    holdem = Range.from_string("AA")
    plo = Range.from_string("AAKKds", game=GameType.Plo)

    with pytest.raises(ValueError):
        holdem.intersection(plo)
    with pytest.raises(ValueError):
        holdem.union(plo)
    with pytest.raises(ValueError):
        holdem.difference(plo)
    with pytest.raises(ValueError):
        holdem | plo
    with pytest.raises(ValueError):
        holdem & plo
    with pytest.raises(ValueError):
        holdem - plo


def test_operator_and_method_are_equivalent():
    a = Range.from_string("AA:0.6,KK")
    b = Range.from_string("AA:0.4,QQ")

    def as_pairs(r):
        return [(c.mask, c.weight) for c in r.combos()]

    assert as_pairs(a.union(b)) == as_pairs(a | b)
    assert as_pairs(a.intersection(b)) == as_pairs(a & b)
    assert as_pairs(a.difference(b)) == as_pairs(a - b)


def test_operator_with_unsupported_type_raises_type_error():
    r = Range.from_string("AA")

    with pytest.raises(TypeError):
        r | 5
    with pytest.raises(TypeError):
        r & 5
    with pytest.raises(TypeError):
        r - 5


def test_plo_union_preserves_tag():
    a = Range.from_string("AAKKds", game=GameType.Plo)
    b = Range.from_string("AAQQds", game=GameType.Plo)

    result = a.union(b)

    assert result.game == GameType.Plo
    assert len(result.combos()) == len(a.combos()) + len(b.combos())
