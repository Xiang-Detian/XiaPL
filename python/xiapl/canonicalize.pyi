"""Type stub for xiapl.canonicalize (xiapl._xiapl.canonicalize): suit
canonicalization.

Hand-written against the live binding (binding/core_eval_canon.cpp,
register_canonicalize_module) and verified with mypy.stubtest.
"""

from collections.abc import Sequence

from xiapl.card import Card

def canonicalize_hero_and_board(
    hero_hand: Sequence[Card], board: Sequence[Card]
) -> tuple[list[Card], list[Card]]: ...
def canonicalize_hero_and_board_masks(
    hero_mask: int, board_mask: int
) -> tuple[int, int]: ...
def canonicalize_board(board: Sequence[Card]) -> list[Card]: ...
def canonicalize_board_mask(board_mask: int) -> int: ...
def canonicalize_hand(hand: Sequence[Card]) -> str: ...
def canonicalize_hand_mask(hero_mask: int) -> str: ...

# board_size: 3 (Flop), 4 (Turn), 5 (River).
# Emits strict (canon_version=2) keys; the flop population is 1,286,792.
def generate_canonical_situations(board_size: int) -> list[tuple[int, int]]: ...
