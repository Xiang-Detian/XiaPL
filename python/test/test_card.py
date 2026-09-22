# test_card.py
import copy
import pickle

from xiapl.card import Card


def test_card_to_string():
    card = Card(14, "s")  # As
    assert str(card) == "As"
    assert card.to_string() == "As"

def test_card_repr():
    card = Card(13, "h")  # Kh
    assert repr(card) == "<Card Kh>"

def test_card_from_string():
    card = Card.from_string("Qs")
    assert card.rank == 12
    assert card.suit == "s"

def test_card_equality():
    c1 = Card(10, "d")
    c2 = Card(10, "d")
    c3 = Card(10, "h")
    assert c1 == c2
    assert c1 != c3

def test_card_order():
    c1 = Card(10, "d")
    c2 = Card(11, "d")
    assert c1 < c2


def test_card_pickle():
    original = Card(9, "h")
    dumped = pickle.dumps(original)
    loaded = pickle.loads(dumped)
    assert original == loaded
    assert original.rank == loaded.rank
    assert original.suit == loaded.suit


def test_card_deepcopy():
    original = Card(7, "c")
    copied = copy.deepcopy(original)
    assert original == copied
    assert original.rank == copied.rank
    assert original.suit == copied.suit

def test_card_from_id_consistency():
    """Verify that a Card's id obtained from from_string() reconstructs an
    identical card when passed through from_id(id).
    """
    samples = ["As", "Kh", "Td", "7c", "2h", "Qd"]

    for s in samples:
        c1 = Card.from_string(s)
        cid = c1.id  # integer ID

        c2 = Card.from_id(cid)

        # rank, suit, and id should match
        assert c1.rank == c2.rank, f"rank mismatch for {s}"
        assert c1.suit == c2.suit, f"suit mismatch for {s}"
        assert c1.id == c2.id, f"id mismatch for {s}"

        # Also equal as Card objects
        assert c1 == c2
