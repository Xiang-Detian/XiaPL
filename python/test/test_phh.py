"""Tests for xiapl.phh: the read-only PHH single- and multi-hand parser.

Covers the NT/PO happy paths against the transcribed phh-dataset fixtures
(dwan-ivey-2009, antonius-blom-2009), the unsupported-variant rejection
(alice-carol-wikipedia, an FB/badugi hand), the hand-written negative
fixtures under tests/fixtures/phh/bad/, a handful of inline-built minimal
NT hands for action-tokenizer edge cases (commentary, no-ops, unknown
variant codes) that don't warrant their own fixture file, and (added in
Task 2) .phhs collections, file readers, hole/board mask interop, and the
frozen golden JSON regression.
"""
from __future__ import annotations

import json
from pathlib import Path

import pytest

import phh_golden
from xiapl import phh
from xiapl.simulation import GameType

FIXTURES = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "phh"
BAD_FIXTURES = FIXTURES / "bad"


def _minimal_nt_hand(*extra_actions: str) -> str:
    """Build minimal NT-variant PHH text with extra action lines appended.

    Args:
        *extra_actions: Additional raw action strings appended after the two
            hole-card deals ("d dh p1 ????", "d dh p2 ????").

    Returns:
        TOML text for a two-player NT hand usable with phh.parse_phh().
    """
    actions = ['"d dh p1 ????"', '"d dh p2 ????"', *(repr(a) for a in extra_actions)]
    actions_block = ",\n  ".join(actions)
    return f'''variant = "NT"
antes = [0, 0]
blinds_or_straddles = [1, 2]
min_bet = 2
starting_stacks = [200, 200]
actions = [
  {actions_block},
]
'''


# ---------------------------------------------------------------------------
# Happy path: dwan-ivey-2009.phh (NT)
# ---------------------------------------------------------------------------


def test_parse_dwan_ivey_scalars() -> None:
    hand = phh.parse_phh((FIXTURES / "dwan-ivey-2009.phh").read_text())
    assert hand.variant == "NT"
    assert hand.game == GameType.Holdem
    assert hand.antes == [500.0, 500.0, 500.0]
    assert hand.blinds_or_straddles == [1000.0, 2000.0, 0.0]
    assert hand.min_bet == 2000.0
    assert hand.starting_stacks == [1125600.0, 2000000.0, 553500.0]
    assert hand.players == ["Phil Ivey", "Patrik Antonius", "Tom Dwan"]
    assert hand.finishing_stacks == []
    assert hand.winnings == []
    assert hand.collection_key == ""
    assert hand.extra["event"] == "Full Tilt Million Dollar Cash Game S4E12"
    assert hand.extra["year"] == 2009
    assert hand.extra["ante_trimming_status"] is True
    assert len(hand.actions) == 18


def test_parse_dwan_ivey_actions() -> None:
    a = phh.parse_phh((FIXTURES / "dwan-ivey-2009.phh").read_text()).actions
    assert (a[0].verb, a[0].player, a[0].cards) == ("dh", 1, [12, 13])  # Ac2d
    assert a[0].has_cards and a[0].unknown_count == 0
    assert (a[1].verb, a[1].player, a[1].cards, a[1].unknown_count) == ("dh", 2, [], 2)
    assert (a[2].verb, a[2].player, a[2].cards) == ("dh", 3, [31, 30])  # 7h6h
    assert (a[3].verb, a[3].player, a[3].amount) == ("cbr", 3, 7000.0)
    assert (a[5].verb, a[5].player) == ("f", 2)
    assert (a[6].verb, a[6].player) == ("cc", 3)
    assert (a[7].verb, a[7].player, a[7].cards) == ("db", 0, [9, 14, 3])  # Jc3d5c
    assert (a[10].verb, a[10].cards) == ("db", [28])  # 4h
    assert a[13].amount == 1067100.0
    assert (a[15].verb, a[15].player, a[15].cards) == ("sm", 1, [12, 13])
    assert not a[15].same_as_dealt
    assert (a[16].verb, a[16].player, a[16].cards) == ("sm", 3, [31, 30])
    assert (a[17].verb, a[17].cards) == ("db", [35])  # Jh (after sm!)
    assert all(x.commentary == "" for x in a)  # "# Ivey" etc. are TOML comments, stripped by tomllib
    assert a[3].text == "p3 cbr 7000"


# ---------------------------------------------------------------------------
# Happy path: antonius-blom-2009.phh (PO)
# ---------------------------------------------------------------------------


def test_parse_antonius_blom_po() -> None:
    hand = phh.parse_phh((FIXTURES / "antonius-blom-2009.phh").read_text())
    assert hand.variant == "PO" and hand.game == GameType.Plo
    assert hand.starting_stacks == [1259450.25, 678473.5]
    a = hand.actions
    assert (a[0].verb, a[0].player, a[0].cards) == ("dh", 1, [38, 40, 50, 37])  # Ah3sKsKh
    # sm precedes the turn/river deals: money went in on the flop.
    verbs = [x.verb for x in a]
    assert verbs[-4:] == ["sm", "sm", "db", "db"]


# ---------------------------------------------------------------------------
# Unsupported variant: alice-carol-wikipedia.phh (FB / badugi)
# ---------------------------------------------------------------------------


def test_unsupported_variant() -> None:
    with pytest.raises(phh.UnsupportedVariantError, match="FB"):
        phh.parse_phh((FIXTURES / "alice-carol-wikipedia.phh").read_text())
    # UnsupportedVariantError is catchable as ValueError.
    assert issubclass(phh.UnsupportedVariantError, ValueError)


def test_unknown_variant_lists_supported() -> None:
    text = '''variant = "ZZ"
antes = [0, 0]
blinds_or_straddles = [1, 2]
min_bet = 2
starting_stacks = [200, 200]
actions = []
'''
    with pytest.raises(phh.UnsupportedVariantError, match="NT") as exc_info:
        phh.parse_phh(text)
    assert "PO" in str(exc_info.value)


# ---------------------------------------------------------------------------
# Hand-written negative fixtures (tests/fixtures/phh/bad/)
# ---------------------------------------------------------------------------


def test_nt_with_small_bet_raises() -> None:
    with pytest.raises(ValueError, match="small_bet"):
        phh.parse_phh((BAD_FIXTURES / "nt_with_small_bet.phh").read_text())


def test_missing_min_bet_raises() -> None:
    with pytest.raises(ValueError, match="min_bet"):
        phh.parse_phh((BAD_FIXTURES / "missing_min_bet.phh").read_text())


def test_comma_amount_raises() -> None:
    with pytest.raises(ValueError, match="amount"):
        phh.parse_phh((BAD_FIXTURES / "comma_amount.phh").read_text())


def test_glued_comment_raises() -> None:
    with pytest.raises(ValueError):
        phh.parse_phh((BAD_FIXTURES / "glued_comment.phh").read_text())


def test_odd_card_token_raises() -> None:
    with pytest.raises(ValueError):
        phh.parse_phh((BAD_FIXTURES / "odd_card_token.phh").read_text())


# ---------------------------------------------------------------------------
# Action-tokenizer edge cases (inline minimal hands; no fixture file needed)
# ---------------------------------------------------------------------------


def test_commentary_form() -> None:
    text = _minimal_nt_hand("p1 f # gets shown")
    action = phh.parse_phh(text).actions[-1]
    assert action.verb == "f"
    assert action.commentary == "gets shown"


def test_standalone_comment_action() -> None:
    text = _minimal_nt_hand("# note")
    action = phh.parse_phh(text).actions[-1]
    assert action.verb == ""
    assert action.commentary == "note"


def test_empty_action_is_noop() -> None:
    text = _minimal_nt_hand("")
    action = phh.parse_phh(text).actions[-1]
    assert action.verb == ""
    assert action.commentary == ""


# ---------------------------------------------------------------------------
# .phhs collections, file readers, masks, golden JSON (Task 2)
# ---------------------------------------------------------------------------


def test_parse_phhs_collection() -> None:
    hands = phh.parse_phh_all((FIXTURES / "two_hands.phhs").read_text())
    assert [h.collection_key for h in hands] == ["1", "2"]
    assert [h.variant for h in hands] == ["NT", "PO"]
    assert hands[0].blinds_or_straddles == [0.25, 0.5]
    assert hands[0].actions[0].unknown_count == 2
    assert hands[1].actions[0].unknown_count == 4
    # 'time' (TOML local time) and the unknown-to-pokerkit key stay in extra
    assert "time" in hands[0].extra and hands[0].extra["time_zone_abbreviation"] == "JST"


def test_mode_confusion_hints() -> None:
    phhs_text = (FIXTURES / "two_hands.phhs").read_text()
    phh_text = (FIXTURES / "dwan-ivey-2009.phh").read_text()
    with pytest.raises(ValueError, match=r"parse_phh_all"):
        phh.parse_phh(phhs_text)
    with pytest.raises(ValueError, match=r"parse_phh\(\)"):
        phh.parse_phh_all(phh_text)


def test_collection_preserves_unsupported_variant_error_type() -> None:
    # A regression guard for a downgrading bug: a hand inside a .phhs
    # collection with an unsupported variant must still raise
    # UnsupportedVariantError (not plain ValueError), so callers filtering
    # on the specific type (e.g. to skip unsupported hands and keep the
    # rest of a mixed collection) keep working through parse_phh_all too.
    text = '''[1]
variant = "NT"
antes = [0, 0]
blinds_or_straddles = [1, 2]
min_bet = 2
starting_stacks = [200, 200]
actions = [
  "d dh p1 ????",
  "d dh p2 ????",
]

[2]
variant = "FB"
'''
    with pytest.raises(phh.UnsupportedVariantError, match=r"PHH hand \[2\]"):
        phh.parse_phh_all(text)


def test_collection_hand_error_has_key_prefix_no_double_phh() -> None:
    # Hand [2] is missing 'min_bet'; the wrapped message must carry the
    # collection-key prefix without doubling the inner error's own "PHH: "
    # prefix (i.e. "PHH hand [2]: missing ...", not "PHH hand [2] PHH: ...").
    text = '''[1]
variant = "NT"
antes = [0, 0]
blinds_or_straddles = [1, 2]
min_bet = 2
starting_stacks = [200, 200]
actions = [
  "d dh p1 ????",
  "d dh p2 ????",
]

[2]
variant = "NT"
antes = [0, 0]
blinds_or_straddles = [1, 2]
starting_stacks = [200, 200]
actions = []
'''
    with pytest.raises(ValueError, match=r"PHH hand \[2\]: missing required field 'min_bet'"):
        phh.parse_phh_all(text)


def test_commentary_vs_toml_comment() -> None:
    a = phh.parse_phh((FIXTURES / "commentary.phh").read_text()).actions
    assert a[2].commentary == ""                      # TOML comment: gone
    assert a[3].verb == "sm" and not a[3].has_cards   # muck ...
    assert a[3].commentary == "PHH commentary, part of the notation"


def test_hole_and_board_masks() -> None:
    hand = phh.parse_phh((FIXTURES / "dwan-ivey-2009.phh").read_text())
    assert hand.hole_mask(1) == (1 << 12) | (1 << 13)          # Ac2d
    assert hand.hole_mask(3) == (1 << 31) | (1 << 30)          # 7h6h
    with pytest.raises(ValueError):
        hand.hole_mask(2)                                      # ???? unknown
    with pytest.raises(ValueError):
        hand.hole_mask(4)                                      # out of range
    full = (1 << 9) | (1 << 14) | (1 << 3) | (1 << 28) | (1 << 35)  # Jc3d5c 4h Jh
    assert hand.board_mask() == full
    assert hand.board_mask(max_cards=3) == (1 << 9) | (1 << 14) | (1 << 3)
    assert hand.board_mask(max_cards=0) == 0
    with pytest.raises(ValueError):
        hand.board_mask(max_cards=6)


def test_read_functions(tmp_path) -> None:
    # read_phh/read_phh_all accept str and PathLike, utf-8
    p = FIXTURES / "dwan-ivey-2009.phh"
    assert phh.read_phh(p).variant == "NT"
    assert phh.read_phh(str(p)).variant == "NT"
    assert len(phh.read_phh_all(FIXTURES / "two_hands.phhs")) == 2


def test_goldens_frozen() -> None:
    goldens = sorted(FIXTURES.glob("*.expected.json"))
    assert goldens, "golden files missing"
    for path in goldens:
        src = FIXTURES / path.name.removesuffix(".expected.json")
        if src.suffix == ".phhs":
            hands = phh.parse_phh_all(src.read_text())
        else:
            hands = [phh.parse_phh(src.read_text())]
        assert [phh_golden.hand_to_dict(h) for h in hands] == json.loads(path.read_text()), src.name
