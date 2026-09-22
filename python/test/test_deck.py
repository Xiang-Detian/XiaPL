import pytest
from xiapl.card import Card
from xiapl.deck import Deck


def test_deck_initialization():
    deck = Deck()
    assert len(deck.cards) == 52
    assert all(isinstance(card, Card) for card in deck.cards)

def test_deck_shuffle():
    deck1 = Deck()
    deck2 = Deck()
    deck1.shuffle()
    # After shuffle, deck order may differ
    assert deck1.cards != deck2.cards or set(deck1.cards) == set(deck2.cards)

def test_deck_shuffle_seed_reproducible():
    for seed in (0, 42):
        deck_a = Deck()
        deck_b = Deck()
        deck_a.shuffle(seed)
        deck_b.shuffle(seed)
        while deck_a.size() > 0:
            assert deck_a.deal_one().id == deck_b.deal_one().id

    deck_c = Deck()
    deck_d = Deck()
    deck_c.shuffle(1)
    deck_d.shuffle(2)
    any_diff = False
    while deck_c.size() > 0:
        if deck_c.deal_one().id != deck_d.deal_one().id:
            any_diff = True
            break
    assert any_diff


def test_deal_cards():
    deck = Deck()
    dealt = deck.deal(5)
    assert len(dealt) == 5
    assert len(deck.cards) == 47
    for card in dealt:
        assert isinstance(card, Card)

def test_deal_single_card():
    deck = Deck()
    single_card = deck.deal()
    assert isinstance(single_card, list)
    assert isinstance(single_card[0], Card)
    assert len(deck.cards) == 51

def test_remove_cards():
    deck = Deck()
    initial_size = len(deck.cards)
    to_remove = deck.cards[:3]
    deck.remove_cards(to_remove)
    assert len(deck.cards) == initial_size - 3
    for card in to_remove:
        assert card not in deck.cards


# Serialization tests
import copy
import pickle


def test_deck_pickle():
    deck = Deck()
    serialized = pickle.dumps(deck)
    deserialized = pickle.loads(serialized)
    assert isinstance(deserialized, Deck)
    assert len(deserialized.cards) == len(deck.cards)
    assert all(isinstance(card, Card) for card in deserialized.cards)

def test_deck_deepcopy():
    deck = Deck()
    copied = copy.deepcopy(deck)
    assert isinstance(copied, Deck)
    assert len(copied.cards) == len(deck.cards)
    assert all(isinstance(card, Card) for card in copied.cards)
def test_set_cards():
    deck = Deck()
    new_cards = [
        Card(14, "s"),  # Ace of spades
        Card(13, "h"),  # King of hearts
        Card(12, "d"),   # Queen of diamonds
    ]
    deck.set_cards(new_cards)
    assert len(deck.cards) == 3
    assert deck.cards[0].rank == 14 and deck.cards[0].suit == "s"
    assert deck.cards[1].rank == 13 and deck.cards[1].suit == "h"
    assert deck.cards[2].rank == 12 and deck.cards[2].suit == "d"

def test_deck_size_and_empty():
    deck = Deck()
    # Initially 52 cards, and empty is False
    assert deck.size() == 52
    assert not deck.empty()

    # empty becomes True once all 52 cards are dealt
    _ = deck.deal(52)
    assert deck.size() == 0
    assert deck.empty()


def test_deck_has_cards():
    deck = Deck()
    assert deck.has_cards()  # default n=1
    assert deck.has_cards(10)
    assert deck.has_cards(52)
    assert not deck.has_cards(53)

    _ = deck.deal(51)
    assert deck.has_cards(1)
    assert not deck.has_cards(2)


def test_deal_one():
    deck = Deck()
    before_size = deck.size()
    c = deck.deal_one()

    # One card is returned, and the deck's remaining count should decrease by 1
    assert isinstance(c, Card)
    assert deck.size() == before_size - 1


def test_burn_cards():
    deck = Deck()
    before_size = deck.size()

    # Burn one card
    deck.burn()
    assert deck.size() == before_size - 1

    # Burn several more cards
    deck.burn(2)
    assert deck.size() == before_size - 3


def test_card_ids_property():
    deck = Deck()
    ids = deck.card_ids

    # Initially should contain 52 cards, 0..51
    assert len(ids) == 52
    assert sorted(ids) == list(range(52))

    # Verify that card_ids corresponds to the ids of cards
    for card, cid in zip(deck.cards, ids, strict=False):
        assert card.id == cid

    # Sizes should still match after dealing some cards
    _ = deck.deal(5)
    assert len(deck.card_ids) == deck.size()
