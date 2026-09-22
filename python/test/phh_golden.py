"""Serialize xiapl.phh.PhhHand / PhhAction to JSON-native dicts.

Shared between test_phh.py (test_goldens_frozen, which reads the frozen
tests/fixtures/phh/*.expected.json files back and compares) and
gen_phh_golden.py (which writes them). This lives in python/test/, not
python/xiapl/, because it is a test-only concern -- not part of the
"binding-shaped" public xiapl.phh API.
"""
from __future__ import annotations

import math

from xiapl.card import Card
from xiapl.phh import PhhAction, PhhHand

# Local fallback tables, only used if xiapl.card.Card's string form ever
# stops being the 2-char "<rank><suit>" convention (see _cards_text).
RANKS = "23456789TJQKA"
SUITS = "cdhs"


def hand_to_dict(hand: PhhHand) -> dict[str, object]:
    """Serialize one PhhHand to a JSON-native record for golden comparison.

    Args:
        hand: The parsed hand.

    Returns:
        A dict with keys (in this order): variant, game, antes,
        blinds_or_straddles, min_bet, starting_stacks, players,
        finishing_stacks, winnings, collection_key, extra, actions.
    """
    return {
        "variant": hand.variant,
        "game": hand.game.name,
        "antes": [_float_or_inf(x) for x in hand.antes],
        "blinds_or_straddles": [_float_or_inf(x) for x in hand.blinds_or_straddles],
        "min_bet": _float_or_inf(hand.min_bet),
        "starting_stacks": [_float_or_inf(x) for x in hand.starting_stacks],
        "players": list(hand.players),
        "finishing_stacks": [_float_or_inf(x) for x in hand.finishing_stacks],
        "winnings": [_float_or_inf(x) for x in hand.winnings],
        "collection_key": hand.collection_key,
        "extra": {key: _json_native(value) for key, value in hand.extra.items()},
        "actions": [_action_to_dict(action) for action in hand.actions],
    }


def _action_to_dict(action: PhhAction) -> dict[str, object]:
    """Serialize one PhhAction to a JSON-native record.

    Args:
        action: The parsed action.

    Returns:
        A dict with keys exactly: verb, player, amount, cards, cards_text,
        unknown_count, has_cards, same_as_dealt, commentary.
    """
    return {
        "verb": action.verb,
        "player": action.player,
        "amount": action.amount,
        "cards": list(action.cards),
        "cards_text": _cards_text(action.cards),
        "unknown_count": action.unknown_count,
        "has_cards": action.has_cards,
        "same_as_dealt": action.same_as_dealt,
        "commentary": action.commentary,
    }


def _cards_text(card_ids: list[int]) -> str:
    """Render card IDs as a concatenated 2-char-per-card string, e.g. 'Ac2d'.

    Args:
        card_ids: Card IDs in the order they were written.

    Returns:
        The concatenated 2-char rendering of each card.
    """
    chars = []
    for card_id in card_ids:
        rendered = str(Card.from_id(card_id))
        if len(rendered) != 2:
            # xiapl.card.Card's string form is not "<rank><suit>" -- fall
            # back to the local rank/suit tables instead.
            rendered = f"{RANKS[card_id % 13]}{SUITS[card_id // 13]}"
        chars.append(rendered)
    return "".join(chars)


def _float_or_inf(value: float) -> float | str:
    """Render a float, mapping +/-inf to the string 'inf'/'-inf' (JSON has no infinity)."""
    if math.isinf(value):
        return "inf" if value > 0 else "-inf"
    return value


def _json_native(value: object) -> object:
    """Pass a JSON-native value through unchanged; stringify anything else."""
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, list):
        return [_json_native(item) for item in value]
    if isinstance(value, dict):
        return {key: _json_native(item) for key, item in value.items()}
    return str(value)
