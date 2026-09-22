"""Cross-check xiapl.phh against pokerkit's own PHH state machine (oracle).

Replays the two transcribed real-hand fixtures (dwan-ivey-2009,
antonius-blom-2009) through pokerkit.HandHistory's state iteration and
compares hole cards and the final board against what xiapl.phh parsed,
catching transcription mistakes a hand-written parser could otherwise miss
silently. Also cross-checks the writer: feeds format_phh's re-serialized
output back through the same pokerkit replay to confirm pokerkit accepts
it and reads the same cards from it as from the original. This is an
optional, non-blocking cross-check, not a dependency: pokerkit is never
added to xiapl's install requirements, and the whole module is skipped via
pytest.importorskip when it isn't installed.
"""
from __future__ import annotations

import warnings
from pathlib import Path

import pytest

pokerkit = pytest.importorskip("pokerkit")

from xiapl import phh  # noqa: E402  (must follow importorskip)

FIXTURES = Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "phh"


def _snapshot_hole_and_board(text: str) -> tuple[list[list[str]], list[str]]:
    """Replay a PHH document through pokerkit and snapshot hole/board cards.

    Hole cards are captured immediately after the last 'd dh' action is
    applied to pokerkit's state, not after the full replay finishes.
    pokerkit's State is mutated in place as later actions replay onto the
    *same* object, and by showdown it has already mucked (cleared) the
    hole cards of every losing hand -- reading state.hole_cards only after
    the loop ends would see that post-mucking view instead of what was
    actually dealt.

    Args:
        text: The full contents of a .phh file (one hand).

    Returns:
        A (hole_cards, board_cards) pair. hole_cards[i] is player i's hole
        cards as 2-char strings (e.g. 'Ac'), with '??' for any card
        pokerkit has not resolved yet. board_cards is the full final
        board, also as 2-char strings, in deal order.
    """
    with warnings.catch_warnings():
        # pokerkit warns (UserWarning) on the PHH key
        # 'time_zone_abbreviation', which it does not recognize; neither
        # fixture below sets it, but silence it defensively anyway.
        warnings.simplefilter("ignore", UserWarning)
        hand_history = pokerkit.HandHistory.loads(text)
        hole_cards: list[list[str]] | None = None
        final_state = None
        for state, action in hand_history.state_actions:
            final_state = state
            if action is not None and action.startswith("d dh"):
                hole_cards = [
                    [f"{card.rank}{card.suit}" for card in hand] for hand in state.hole_cards
                ]
    assert hole_cards is not None, "fixture has no 'd dh' action"
    assert final_state is not None, "fixture produced no states"
    board_cards = [
        f"{card.rank}{card.suit}" for street in final_state.board_cards for card in street
    ]
    return hole_cards, board_cards


def test_dwan_ivey_oracle() -> None:
    text = (FIXTURES / "dwan-ivey-2009.phh").read_text()
    hole_cards, board_cards = _snapshot_hole_and_board(text)
    assert hole_cards == [["Ac", "2d"], ["??", "??"], ["7h", "6h"]]
    assert board_cards == ["Jc", "3d", "5c", "4h", "Jh"]
    # Both parsers accept the fixture.
    phh.parse_phh(text)
    pokerkit.HandHistory.loads(text)


def test_antonius_blom_oracle() -> None:
    text = (FIXTURES / "antonius-blom-2009.phh").read_text()
    hole_cards, board_cards = _snapshot_hole_and_board(text)
    assert hole_cards == [["Ah", "3s", "Ks", "Kh"], ["6d", "9s", "7d", "8h"]]
    assert board_cards == ["4s", "5c", "2h", "5h", "9c"]
    phh.parse_phh(text)
    pokerkit.HandHistory.loads(text)


def test_format_phh_output_accepted_by_pokerkit() -> None:
    # xiapl.phh's writer output isn't just accepted by pokerkit's PHH loader --
    # replaying it produces the same hole/board cards as the original
    # transcription, i.e. format_phh's canonical re-serialization is a
    # semantically faithful rewrite, not just syntactically valid TOML.
    text = (FIXTURES / "dwan-ivey-2009.phh").read_text()
    dwan_ivey_hand = phh.parse_phh(text)
    formatted = phh.format_phh(dwan_ivey_hand)

    hole_cards, board_cards = _snapshot_hole_and_board(formatted)
    assert hole_cards == [["Ac", "2d"], ["??", "??"], ["7h", "6h"]]
    assert board_cards == ["Jc", "3d", "5c", "4h", "Jh"]
    pokerkit.HandHistory.loads(formatted)
