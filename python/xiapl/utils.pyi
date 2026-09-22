"""Type stub for xiapl.utils (xiapl._xiapl.utils): 52-bit mask utilities.

Hand-written against the live binding (binding/core_card_utils.cpp,
register_utils_module) and verified with mypy.stubtest.
"""

from collections.abc import Sequence

from xiapl.card import Card

def card_to_mask(card: Card) -> int: ...
def cards_to_mask(cards: Sequence[Card]) -> int: ...
def mask_to_cards(mask: int) -> list[Card]: ...
def mask_to_ids(mask: int) -> list[int]: ...
