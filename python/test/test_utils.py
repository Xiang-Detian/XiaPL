import pytest
from xiapl.card import Card
from xiapl.utils import card_to_mask, cards_to_mask, mask_to_cards, mask_to_ids


def to_cards(card_strs):
    return [Card.from_string(s) for s in card_strs]


def test_card_to_mask_single_bit():
    """Verify that card_to_mask sets exactly one bit."""
    c = Card.from_string("As")
    cid = c.id

    m = card_to_mask(c)

    # Exactly one bit is set
    assert m.bit_count() == 1
    # The set bit position matches card.id
    assert m == (1 << cid)


def test_cards_to_mask_multiple_cards():
    """Verify that cards_to_mask ORs multiple cards into a single mask."""
    cards = to_cards(["As", "Kh", "Td"])
    ids = [c.id for c in cards]

    m = cards_to_mask(cards)

    # Each card's bit is set
    for cid in ids:
        assert m & (1 << cid)

    # The number of set bits matches the number of cards
    assert m.bit_count() == len(ids)


def test_mask_to_cards_inverse_of_cards_to_mask():
    """Verify that cards_to_mask → mask_to_cards round-trips to the original card set."""
    original_cards = to_cards(["As", "Kh", "Td", "7c", "2h"])
    m = cards_to_mask(original_cards)

    restored_cards = mask_to_cards(m)

    orig_ids = sorted(c.id for c in original_cards)
    restored_ids = sorted(c.id for c in restored_cards)

    assert orig_ids == restored_ids


def test_mask_to_ids_inverse_of_cards_to_mask():
    """Verify that cards_to_mask → mask_to_ids round-trips to the original card ID set."""
    original_cards = to_cards(["As", "Kh", "Td", "7c", "2h"])
    m = cards_to_mask(original_cards)

    ids_from_mask = sorted(mask_to_ids(m))
    orig_ids = sorted(c.id for c in original_cards)

    assert ids_from_mask == orig_ids


def test_cards_to_mask_basic_properties():
    """Verify that cards_to_mask returns a 52-bit mask matching the OR of
    card_to_mask results.
    """
    cards = to_cards(["As", "Kh", "Td", "7c", "2h"])

    m1 = cards_to_mask(cards)
    # Should match the OR of card_to_mask results
    m2 = 0
    for c in cards:
        m2 |= card_to_mask(c)

    assert isinstance(m1, int)
    assert isinstance(m2, int)
    assert m1 == m2
