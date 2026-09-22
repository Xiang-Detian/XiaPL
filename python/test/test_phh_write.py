"""Tests for xiapl.phh's writer: format_phh / format_phh_all / write_phh / write_phh_all.

Covers round-tripping the transcribed real-hand fixtures and the .phhs
collection fixture through parse -> format -> parse, canonical
reconstruction of programmatically-built (empty-`text`) actions, the
verbatim-`text`-vs-canonical-rebuild distinction, file I/O via
write_phh/write_phh_all, a handful of TOML-emitter edge cases (inf, nested
extra dict, string escaping), the writer's three structural pre-write
checks, and format_phh_all's pinned empty-input behavior.
"""
from __future__ import annotations

from pathlib import Path

import pytest

import phh_golden
from xiapl import phh
from xiapl.phh import PhhAction, PhhHand
from xiapl.simulation import GameType

FIXTURES = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "phh"


def _minimal_hand(actions: list[PhhAction]) -> PhhHand:
    """Build a minimal 2-player NT PhhHand around the given actions."""
    return PhhHand(
        variant="NT",
        game=GameType.Holdem,
        antes=[0.0, 0.0],
        blinds_or_straddles=[1.0, 2.0],
        min_bet=2.0,
        starting_stacks=[200.0, 200.0],
        actions=actions,
    )


# ---------------------------------------------------------------------------
# Round trip: real fixtures and the .phhs collection
# ---------------------------------------------------------------------------


def test_round_trip_all_positive_fixtures() -> None:
    for name in ["dwan-ivey-2009.phh", "antonius-blom-2009.phh", "commentary.phh"]:
        original = phh.parse_phh((FIXTURES / name).read_text())
        rt = phh.parse_phh(phh.format_phh(original))
        assert phh_golden.hand_to_dict(rt) == phh_golden.hand_to_dict(original), name


def test_round_trip_phhs_collection() -> None:
    originals = phh.parse_phh_all((FIXTURES / "two_hands.phhs").read_text())
    rt = phh.parse_phh_all(phh.format_phh_all(originals))
    assert [phh_golden.hand_to_dict(h) for h in rt] == [
        phh_golden.hand_to_dict(h) for h in originals
    ]


# ---------------------------------------------------------------------------
# Programmatic construction: canonical action-string reconstruction
# ---------------------------------------------------------------------------


def test_format_phh_reconstructs_canonical_action_strings() -> None:
    actions = [
        PhhAction(verb="dh", player=1, unknown_count=2, has_cards=True),
        PhhAction(verb="cbr", player=1, amount=7000.0),
        PhhAction(verb="f", player=2),
        PhhAction(verb="cc", player=1, commentary="nice call"),
    ]
    hand = _minimal_hand(actions)
    formatted = phh.format_phh(hand)
    lines = formatted.splitlines()

    assert '  "d dh p1 ????",' in lines
    assert '  "p1 cbr 7000",' in lines
    assert '  "p2 f",' in lines
    assert '  "p1 cc # nice call",' in lines

    rt = phh.parse_phh(formatted)
    assert phh_golden.hand_to_dict(rt) == phh_golden.hand_to_dict(hand)


def test_format_phh_all_amount_non_integer_uses_shortest_repr() -> None:
    hand = _minimal_hand([PhhAction(verb="cbr", player=1, amount=1.5)])
    formatted = phh.format_phh(hand)
    assert '  "p1 cbr 1.5",' in formatted.splitlines()


# ---------------------------------------------------------------------------
# text verbatim vs. canonical rebuild
# ---------------------------------------------------------------------------


def test_text_verbatim_vs_canonical_rebuild() -> None:
    # Irregular internal spacing inside a parsed action's raw text...
    raw = (
        'variant = "NT"\n'
        "antes = [0, 0]\n"
        "blinds_or_straddles = [1, 2]\n"
        "min_bet = 2\n"
        "starting_stacks = [200, 200]\n"
        "actions = [\n"
        '  "d dh p1 ????",\n'
        '  "d dh p2 ????",\n'
        '  "p1   f   #   spaced   out   remark",\n'
        "]\n"
    )
    parsed = phh.parse_phh(raw)
    spaced_action = parsed.actions[2]
    assert spaced_action.verb == "f"
    assert spaced_action.text == "p1   f   #   spaced   out   remark"
    # The tokenizer collapses whitespace runs when extracting the
    # structured commentary field, even though `text` keeps it verbatim.
    assert spaced_action.commentary == "spaced out remark"

    # ...survives verbatim when re-emitted (because `text` is non-empty)...
    formatted_original = phh.format_phh(parsed)
    assert '  "p1   f   #   spaced   out   remark",' in formatted_original.splitlines()

    # ...but a rebuilt PhhAction with the same structured fields and no
    # `text` gets canonical single-space grammar instead.
    rebuilt_action = PhhAction(verb="cc", player=2, commentary=spaced_action.commentary)
    hand_with_rebuilt = _minimal_hand(
        [
            PhhAction(verb="dh", player=1, unknown_count=2, has_cards=True),
            PhhAction(verb="dh", player=2, unknown_count=2, has_cards=True),
            rebuilt_action,
        ]
    )
    formatted_rebuilt = phh.format_phh(hand_with_rebuilt)
    assert '  "p2 cc # spaced out remark",' in formatted_rebuilt.splitlines()


# ---------------------------------------------------------------------------
# Commentary containing '#': glued markers are rejected at write time,
# standalone '#' tokens and plain commentary round-trip.
# ---------------------------------------------------------------------------


def test_format_phh_rejects_glued_hash_in_commentary() -> None:
    # "c#1" is a glued comment marker: parse_phh itself would reject this
    # exact string if it ever appeared in a document, so format_phh must
    # refuse to produce it in the first place (round-trip contract).
    hand = _minimal_hand([PhhAction(verb="cc", player=1, commentary="c#1 gotcha")])
    with pytest.raises(ValueError, match=r"c#1"):
        phh.format_phh(hand)
    # The error also names the action's index and verb.
    with pytest.raises(ValueError, match=r"PHH action 0 \(verb 'cc'\)"):
        phh.format_phh(hand)


def test_format_phh_standalone_hash_in_commentary_round_trips() -> None:
    # A standalone '#' token inside commentary is not a glued marker --
    # it round-trips semantically (the words survive; see the
    # whitespace-collapse caveat test above for what does NOT survive).
    hand = _minimal_hand([PhhAction(verb="cc", player=1, commentary="a # b")])
    formatted = phh.format_phh(hand)
    assert '  "p1 cc # a # b",' in formatted.splitlines()

    rt = phh.parse_phh(formatted)
    assert rt.actions[0].commentary == "a # b"


def test_format_phh_plain_commentary_round_trips() -> None:
    hand = _minimal_hand([PhhAction(verb="cc", player=1, commentary="nice call")])
    formatted = phh.format_phh(hand)
    assert '  "p1 cc # nice call",' in formatted.splitlines()

    rt = phh.parse_phh(formatted)
    assert phh_golden.hand_to_dict(rt) == phh_golden.hand_to_dict(hand)


# ---------------------------------------------------------------------------
# File I/O
# ---------------------------------------------------------------------------


def test_write_phh_round_trips(tmp_path) -> None:
    original = phh.parse_phh((FIXTURES / "dwan-ivey-2009.phh").read_text())
    path = tmp_path / "out.phh"
    phh.write_phh(original, path)

    text = path.read_text(encoding="utf-8")
    assert text.endswith("\n") and not text.endswith("\n\n")

    rt = phh.read_phh(path)
    assert phh_golden.hand_to_dict(rt) == phh_golden.hand_to_dict(original)


def test_write_phh_all_round_trips(tmp_path) -> None:
    originals = phh.parse_phh_all((FIXTURES / "two_hands.phhs").read_text())
    path = tmp_path / "out.phhs"
    phh.write_phh_all(originals, path)

    text = path.read_text(encoding="utf-8")
    assert text.endswith("\n") and not text.endswith("\n\n")

    rt = phh.read_phh_all(path)
    assert [phh_golden.hand_to_dict(h) for h in rt] == [
        phh_golden.hand_to_dict(h) for h in originals
    ]


def test_write_phh_utf8(tmp_path) -> None:
    hand = _minimal_hand([PhhAction(verb="f", player=1)])
    hand.players = ["José"]
    path = tmp_path / "utf8.phh"
    phh.write_phh(hand, path)

    raw = path.read_bytes()
    assert "José".encode("utf-8") in raw

    rt = phh.read_phh(path)
    assert rt.players == ["José"]


# ---------------------------------------------------------------------------
# TOML emitter edge cases
# ---------------------------------------------------------------------------


def test_inf_starting_stack_round_trips() -> None:
    hand = _minimal_hand([PhhAction(verb="f", player=1)])
    hand.starting_stacks = [float("inf"), 200.0]

    formatted = phh.format_phh(hand)
    assert "starting_stacks = [inf, 200]" in formatted.splitlines()

    rt = phh.parse_phh(formatted)
    assert rt.starting_stacks[0] == float("inf")
    assert rt.starting_stacks[1] == 200.0


def test_format_phh_nested_extra_dict_round_trips() -> None:
    hand = _minimal_hand([PhhAction(verb="f", player=1)])
    hand.extra = {"meta": {"source": "unit-test", "count": 3}}

    formatted = phh.format_phh(hand)
    assert "meta = { " in formatted

    rt = phh.parse_phh(formatted)
    assert rt.extra == {"meta": {"source": "unit-test", "count": 3}}


def test_format_phh_string_escaping_round_trips() -> None:
    hand = _minimal_hand([PhhAction(verb="f", player=1)])
    hand.extra = {"note": 'back\\slash "quote"\ttab\nnewline'}

    formatted = phh.format_phh(hand)
    rt = phh.parse_phh(formatted)
    assert rt.extra["note"] == 'back\\slash "quote"\ttab\nnewline'


def test_format_phh_field_order_and_optional_omission() -> None:
    hand = phh.parse_phh((FIXTURES / "dwan-ivey-2009.phh").read_text())
    formatted = phh.format_phh(hand)

    keys_in_order = [line.split(" = ", 1)[0] for line in formatted.splitlines() if " = " in line]
    assert keys_in_order == [
        "variant",
        "antes",
        "blinds_or_straddles",
        "min_bet",
        "starting_stacks",
        "actions",
        "players",
        "ante_trimming_status",
        "author",
        "event",
        "year",
        "currency",
    ]
    assert "collection_key" not in formatted
    # finishing_stacks / winnings are empty on this fixture -- omitted entirely.
    assert "finishing_stacks" not in formatted
    assert "winnings" not in formatted


# ---------------------------------------------------------------------------
# Pre-write structural validation (NOT legality -- see module docstring)
# ---------------------------------------------------------------------------


def test_format_phh_rejects_unsupported_variant() -> None:
    hand = _minimal_hand([PhhAction(verb="f", player=1)])
    hand.variant = "FB"
    with pytest.raises(phh.UnsupportedVariantError):
        phh.format_phh(hand)


def test_format_phh_rejects_variant_game_mismatch() -> None:
    hand = _minimal_hand([PhhAction(verb="f", player=1)])
    hand.game = GameType.Plo  # variant stays "NT"
    with pytest.raises(ValueError) as exc_info:
        phh.format_phh(hand)
    assert not isinstance(exc_info.value, phh.UnsupportedVariantError)


def test_format_phh_rejects_non_phhaction_entries() -> None:
    hand = _minimal_hand([PhhAction(verb="f", player=1)])
    hand.actions.append("not a PhhAction")
    with pytest.raises(ValueError):
        phh.format_phh(hand)


# ---------------------------------------------------------------------------
# format_phh_all([]) -- pinned empty-input behavior
# ---------------------------------------------------------------------------


def test_format_phh_all_empty_returns_empty_string() -> None:
    assert phh.format_phh_all([]) == ""
