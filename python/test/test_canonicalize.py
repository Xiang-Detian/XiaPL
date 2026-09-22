import random

import pytest
from xiapl.card import Card
from xiapl.deck import Deck
from xiapl.canonicalize import (
    canonicalize_board,
    canonicalize_board_mask,
    canonicalize_hand,
    canonicalize_hand_mask,
    canonicalize_hero_and_board,
    canonicalize_hero_and_board_masks,
    generate_canonical_situations,
)
from xiapl.utils import cards_to_mask


def to_cards(card_strs):
    return [Card.from_string(s) for s in card_strs]

def _cards_to_str_list(cards):
    return [c.to_string() for c in cards]

def mask_to_cards(mask: int):
    """Helper to restore a list of Card objects from a 52-bit mask (ascending ID order)."""
    cards = []
    for cid in range(52):
        if mask & (1 << cid):
            cards.append(Card.from_id(cid))
    return cards


def test_canonicalize_hero_and_board_suit_invariance():
    # Verify that (hero, board) pairs with different suit structure but equivalent meaning canonicalize to the same result
    hero1 = to_cards(["Ah", "Kh"])
    board1 = to_cards(["Qh", "Jh", "2c"])

    # Case with a consistent suit swap (e.g. a mapping like hearts→spades, clubs→diamonds)
    hero2 = to_cards(["As", "Ks"])
    board2 = to_cards(["Qs", "Js", "2d"])

    canon_hero1, canon_board1 = canonicalize_hero_and_board(hero1, board1)
    canon_hero2, canon_board2 = canonicalize_hero_and_board(hero2, board2)

    # Verify that the to_string lists match after canonicalization
    assert _cards_to_str_list(canon_hero1) == _cards_to_str_list(canon_hero2)
    assert _cards_to_str_list(canon_board1) == _cards_to_str_list(canon_board2)


def test_canonicalize_hero_and_board_board_priority():
    # Sanity-check that board suits are prioritized in the mapping
    hero = to_cards(["As", "Kd"])
    board = to_cards(["7c", "8c", "2d"])

    canon_hero, canon_board = canonicalize_hero_and_board(hero, board)

    # The board side should preserve same-suit relationships (7c and 8c end up the same suit)
    assert len(canon_board) == 3
    s0 = canon_board[0].suit
    s1 = canon_board[1].suit
    # 7c and 8c were originally the same suit, so they should stay the same suit value after canonicalization
    assert s0 == s1


def test_canonicalize_hero_and_board_masks_consistency_with_cards():
    """Verify that the card-based canonicalize_hero_and_board and the
    mask-based canonicalize_hero_and_board_masks produce corresponding
    results.
    """
    hero = to_cards(["Ah", "Kh"])
    board = to_cards(["Qh", "Jh", "2c"])

    # Canonicalization result via the card-based version
    canon_hero_cards, canon_board_cards = canonicalize_hero_and_board(hero, board)

    # Canonicalization result via the mask-based version
    hero_mask = cards_to_mask(hero)
    board_mask = cards_to_mask(board)
    canon_hero_mask, canon_board_mask = canonicalize_hero_and_board_masks(hero_mask, board_mask)

    # Convert masks back to cards for comparison (sort to avoid order effects)
    canon_hero_from_mask = mask_to_cards(canon_hero_mask)
    canon_board_from_mask = mask_to_cards(canon_board_mask)

    hero_str_from_cards = sorted(_cards_to_str_list(canon_hero_cards))
    hero_str_from_mask = sorted(_cards_to_str_list(canon_hero_from_mask))
    board_str_from_cards = sorted(_cards_to_str_list(canon_board_cards))
    board_str_from_mask = sorted(_cards_to_str_list(canon_board_from_mask))

    assert hero_str_from_cards == hero_str_from_mask
    assert board_str_from_cards == board_str_from_mask


def test_canonicalize_hero_and_board_masks_board_priority_shape():
    """Roughly verify that the mask version also canonicalizes so that
    same-suit board cards stay the same suit.
    """
    hero = to_cards(["As", "Kd"])
    board = to_cards(["7c", "8c", "2d"])

    hero_mask = cards_to_mask(hero)
    board_mask = cards_to_mask(board)

    canon_hero_mask, canon_board_mask = canonicalize_hero_and_board_masks(hero_mask, board_mask)
    canon_board = mask_to_cards(canon_board_mask)

    assert len(canon_board) == 3
    s0 = canon_board[0].suit
    s1 = canon_board[1].suit
    # 7c and 8c were originally the same suit, so they should stay the same suit value after canonicalization
    assert s0 == s1

# ---- Test for cards_to_mask (52-bit mask) ----
def test_cards_to_mask_single_card():

    card = Card.from_string("As")  # rank=14(A), suit=3(s)
    mask = cards_to_mask([card])

    # bit_index = suit * 13 + (rank - 2)
    # spade = 3, rank A = 14 → rr = 12 → bit = 3*13 + 12 = 51
    assert mask == (1 << 51)


def test_cards_to_mask_multiple_cards():

    # As (bit 51), Kd (rank13→rr11, suit1→bit=1*13+11=24)
    cards = to_cards(["As", "Kd"])
    mask = cards_to_mask(cards)

    expected = (1 << 51) | (1 << 24)
    assert mask == expected


def test_cards_to_mask_order_invariance():

    c1 = to_cards(["As", "Kd"])
    c2 = to_cards(["Kd", "As"])

    assert cards_to_mask(c1) == cards_to_mask(c2)


def test_cards_to_mask_no_overlap_with_rank2_encoding():
    """Assuming the card ID encoding (suit*13 + (rank-2), 0-51), check that
    the bitmask only ever sets bits in the range 0-51.
    """

    cards = to_cards(["As", "Kd", "2c", "5h"])
    mask = cards_to_mask(cards)

    # Bits beyond 52 must always be 0
    assert mask >> 52 == 0


def test_cards_to_mask_all_unique_cards():
    """Verify that bitmasking all 52 cards sets every bit."""

    deck = Deck()
    all_cards = deck.deal(52)
    mask = cards_to_mask(all_cards)

    # All 52 bits set → (1<<52)-1
    assert mask == (1 << 52) - 1


# Helper function: convert a card list to its string representation
def to_str(cards):
    return ",".join(str(c) for c in cards)

def test_canonicalization_stability_basic():
    """Basic order-independence test.
    For the same card composition, canonicalization results should match
    regardless of input order.
    """
    # Case 1: board is [Ah, Kd, Qs] (rank order)
    h1 = [Card(2, "c"), Card(3, "c")]
    b1 = [Card(14, "h"), Card(13, "d"), Card(12, "s")]

    # Case 2: board is [Kd, Qs, Ah] (scrambled)
    h2 = [Card(2, "c"), Card(3, "c")]
    b2 = [Card(13, "d"), Card(12, "s"), Card(14, "h")]

    # Run canonicalization
    canon1_h, canon1_b = canonicalize_hero_and_board(h1, b1)
    canon2_h, canon2_b = canonicalize_hero_and_board(h2, b2)

    # Verify
    assert to_str(canon1_b) == to_str(canon2_b), \
        f"Board mismatch! {to_str(canon1_b)} != {to_str(canon2_b)}"

    assert to_str(canon1_h) == to_str(canon2_h), \
        f"Hand mismatch! {to_str(canon1_h)} != {to_str(canon2_h)}"

@pytest.mark.parametrize("execution_number", range(100))
def test_canonicalization_random_permutation(execution_number):
    """Robustness test against random shuffles.
    Repeated 100 times for verification.
    """
    # Generate a random hand and board
    deck = Deck()
    cards = deck.deal(5)
    hand = cards[:2]
    board = cards[2:]

    # Baseline canonicalization result
    base_h, base_b = canonicalize_hero_and_board(hand, board)
    base_str = f"{to_str(base_h)}|{to_str(base_b)}"

    # Shuffle the board and re-test
    shuffled_board = list(board)
    random.shuffle(shuffled_board)

    test_h, test_b = canonicalize_hero_and_board(hand, shuffled_board)
    test_str = f"{to_str(test_h)}|{to_str(test_b)}"

    assert base_str == test_str, \
        f"Random shuffle failed!\nBase: {to_str(board)} -> {base_str}\nShuf: {to_str(shuffled_board)} -> {test_str}"

def test_canonicalization_hand_swap():
    """Swapping the order of cards within the hand should yield the same result."""
    h1 = [Card(14, "s"), Card(13, "s")] # As Ks
    h2 = [Card(13, "s"), Card(14, "s")] # Ks As
    board = [Card(2, "d"), Card(3, "d"), Card(4, "d")]

    c1_h, c1_b = canonicalize_hero_and_board(h1, board)
    c2_h, c2_b = canonicalize_hero_and_board(h2, board)

    assert to_str(c1_h) == to_str(c2_h)
    assert to_str(c1_b) == to_str(c2_b)

def test_canonicalization_isomorphism():
    """If the suits differ but the structure is the same (isomorphism), the
    result should be the same.
    """
    # Pattern A: hand=hearts, board=diamonds
    h1 = [Card(14, "h"), Card(13, "h")]
    b1 = [Card(2, "d"), Card(3, "d"), Card(4, "d")]

    # Pattern B: hand=spades, board=clubs (structurally identical)
    h2 = [Card(14, "s"), Card(13, "s")]
    b2 = [Card(2, "c"), Card(3, "c"), Card(4, "c")]

    c1_h, c1_b = canonicalize_hero_and_board(h1, b1)
    c2_h, c2_b = canonicalize_hero_and_board(h2, b2)

    # Results should match (e.g. both get unified to hand=s, board=d)
    assert to_str(c1_h) == to_str(c2_h), f"Isomorphism failed for hand: {to_str(c1_h)} != {to_str(c2_h)}"
    assert to_str(c1_b) == to_str(c2_b), f"Isomorphism failed for board: {to_str(c1_b)} != {to_str(c2_b)}"

def test_generate_canonical_situations_flop_masks_shape():
    """Verify that generate_canonical_situations(3) returns (hero_mask,
    board_mask) pairs with 2 / 3 bits set respectively (checks only the
    first few hundred entries).
    """
    situations = generate_canonical_situations(3)
    assert len(situations) > 0

    # Checking all entries could be expensive, so lightly check only the first 200
    for i, (hero_mask, board_mask) in enumerate(situations[:200]):
        # No card overlap between hero and board
        assert (hero_mask & board_mask) == 0

        # No bits set beyond 52
        assert hero_mask >> 52 == 0
        assert board_mask >> 52 == 0

        # Bit count (= number of cards) is as expected
        assert hero_mask.bit_count() == 2
        assert board_mask.bit_count() == 3

        # Also verify, just in case, that mask→card restoration doesn't fail
        hero_cards = mask_to_cards(hero_mask)
        board_cards = mask_to_cards(board_mask)
        assert len(hero_cards) == 2
        assert len(board_cards) == 3


# ===== Tests for board-only canonicalization =====

def test_canonicalize_board_order_invariance():
    """Verify that board-only canonicalization does not depend on input order."""
    b1 = [Card.from_string("Ah"), Card.from_string("Kd"), Card.from_string("Qs")]
    b2 = [Card.from_string("Qs"), Card.from_string("Ah"), Card.from_string("Kd")]

    canon1 = canonicalize_board(b1)
    canon2 = canonicalize_board(b2)

    assert _cards_to_str_list(canon1) == _cards_to_str_list(canon2)


def test_canonicalize_board_suit_isomorphism():
    """Verify that boards with the same suit structure match after
    board-only canonicalization. E.g. Ah Kd Qc and As Kh Qd.
    """
    b1 = to_cards(["Ah", "Kd", "Qc"])
    b2 = to_cards(["As", "Kh", "Qd"])  # isomorphic via suit swap

    canon1 = canonicalize_board(b1)
    canon2 = canonicalize_board(b2)

    assert _cards_to_str_list(canon1) == _cards_to_str_list(canon2)


def test_canonicalize_board_mask_consistency_with_cards():
    """Verify that canonicalize_board and canonicalize_board_mask produce
    corresponding results.
    """
    board = to_cards(["Ah", "Kd", "Qc"])

    # Card-based version
    canon_board_cards = canonicalize_board(board)

    # Mask-based version
    board_mask = cards_to_mask(board)
    canon_board_mask = canonicalize_board_mask(board_mask)
    canon_board_from_mask = mask_to_cards(canon_board_mask)

    board_str_from_cards = sorted(_cards_to_str_list(canon_board_cards))
    board_str_from_mask = sorted(_cards_to_str_list(canon_board_from_mask))

    assert board_str_from_cards == board_str_from_mask


def test_canonicalize_board_mask_agrees_with_empty_hero_when_top_ranks_differ():
    """canonicalize_board_mask and canonicalize_hero_and_board_masks(0, b)
    agree whenever no two suits share their top board rank.

    Both order suits by their 13-bit rank pattern; the pattern comparison is
    decided by the highest set bit, so the two rules can only part company at a
    tie on that bit (see the next test).
    """
    board = to_cards(["Ah", "Kd", "Qc"])
    board_mask = cards_to_mask(board)

    _, canon_board_mask_via_pair = canonicalize_hero_and_board_masks(0, board_mask)
    canon_board_mask_direct = canonicalize_board_mask(board_mask)

    assert canon_board_mask_via_pair == canon_board_mask_direct


def test_canonicalize_board_mask_is_canonical_where_empty_hero_is_not():
    """BEHAVIOUR CHANGE (2026-08-18). canonicalize_board_mask is a true
    canonical form; the hero-less case of canonicalize_hero_and_board_masks is
    not, and the two now disagree on boards whose top suits tie.

    Ah Kh Ad and Ad Kd Ah are the same board up to relabelling. Both have two
    ace-topped suits, so the pair routine's suit-index tie-break sends them to
    different representatives, while the board routine (full pattern order)
    merges them.
    """
    b1 = cards_to_mask(to_cards(["Ah", "Kh", "Ad"]))
    b2 = cards_to_mask(to_cards(["Ad", "Kd", "Ah"]))

    assert canonicalize_board_mask(b1) == canonicalize_board_mask(b2)

    _, pair1 = canonicalize_hero_and_board_masks(0, b1)
    _, pair2 = canonicalize_hero_and_board_masks(0, b2)
    assert pair1 != pair2


def _all_flop_masks():
    for a in range(52):
        for b in range(a + 1, 52):
            for c in range(b + 1, 52):
                yield (1 << a) | (1 << b) | (1 << c)


def test_canonicalize_board_mask_flop_class_count():
    """All 22,100 flops collapse to exactly the 1,755 suit-isomorphism
    classes. This is the property that separates a canonical form from a
    self-consistent but under-merging relabelling (the pre-2026-08-18
    behaviour produced 1,833)."""
    assert len({canonicalize_board_mask(m) for m in _all_flop_masks()}) == 1755


def _permute_suits(mask: int, perm) -> int:
    out = 0
    for s in range(4):
        pat = (mask >> (s * 13)) & 0x1FFF
        out |= pat << (perm[s] * 13)
    return out


def test_canonicalize_board_mask_suit_permutation_invariance():
    """Every flop, against all 24 suit permutations."""
    import itertools

    perms = list(itertools.permutations(range(4)))
    assert len(perms) == 24
    bad = 0
    for m in _all_flop_masks():
        base = canonicalize_board_mask(m)
        for p in perms:
            if canonicalize_board_mask(_permute_suits(m, p)) != base:
                bad += 1
    assert bad == 0


def test_canonicalize_board_mask_idempotent():
    """Canonicalizing a representative returns it unchanged."""
    bad = 0
    for m in _all_flop_masks():
        c = canonicalize_board_mask(m)
        if canonicalize_board_mask(c) != c:
            bad += 1
    assert bad == 0

def test_canonicalize_hand_pair():
    h = to_cards(["Td", "Tc"])
    assert canonicalize_hand(h) == "TT"

def test_canonicalize_hand_suited():
    h1 = to_cards(["Ah", "Kh"])
    h2 = to_cards(["Kh", "Ah"])  # reversed order
    assert canonicalize_hand(h1) == "AKs"
    assert canonicalize_hand(h2) == "AKs"

def test_canonicalize_hand_offsuit():
    h1 = to_cards(["Ah", "Kd"])
    h2 = to_cards(["Kd", "Ah"])  # reversed order
    # different suits, so offsuit
    assert canonicalize_hand(h1) == "AKo"
    assert canonicalize_hand(h2) == "AKo"

def test_canonicalize_hand_mask_equivalence():
    h = to_cards(["Ah", "Kd"])
    mask = cards_to_mask(h)
    assert canonicalize_hand(h) == canonicalize_hand_mask(mask)


# ===== Phase 7: mask-based canonicalize equivalence vs Python ground truth =====
#
# Pins the suit-permutation rule the C++ rewrite must satisfy:
#   1. Board-present suits register first, ordered by max board rank desc
#      with suit index asc as tiebreak.
#   2. Board-absent suits register next, ordered by max hero rank desc
#      with suit index asc as tiebreak.
# This is what the old vector<Card>-based code did and what the new mask
# version is verified against.

import itertools as _itertools


def _ranks_suits(mask):
    out = []
    for cid in range(52):
        if mask & (1 << cid):
            out.append(((cid % 13) + 2, cid // 13))
    return out


def _to_mask(rank_suits):
    m = 0
    for r, s in rank_suits:
        m |= 1 << (s * 13 + (r - 2))
    return m


def _python_canon(hero_mask, board_mask):
    """Reference implementation: replicates the original card-by-card
    suit-registration loop. Used as ground truth for the mask rewrite."""
    hero = sorted(_ranks_suits(hero_mask), key=lambda x: (-x[0], x[1]))
    board = sorted(_ranks_suits(board_mask), key=lambda x: (-x[0], x[1]))
    suit_map = [-1, -1, -1, -1]
    nxt = 0
    for _, s in board:
        if suit_map[s] == -1:
            suit_map[s] = nxt
            nxt += 1
    for _, s in hero:
        if suit_map[s] == -1:
            suit_map[s] = nxt
            nxt += 1

    def remap(arr):
        out = []
        for r, s in arr:
            cs = suit_map[s] if suit_map[s] != -1 else s
            out.append((r, cs))
        out.sort(key=lambda x: (-x[0], x[1]))
        return out

    return _to_mask(remap(hero)), _to_mask(remap(board))


def test_canonicalize_hero_and_board_masks_matches_python_ground_truth_flop():
    """All flops × first 200 hands match the Python ground truth bit-for-bit."""
    mismatches = []
    hand_iter = _itertools.islice(_itertools.combinations(range(52), 2), 200)
    for h in hand_iter:
        hm = (1 << h[0]) | (1 << h[1])
        rest = [c for c in range(52) if c not in h]
        for b in _itertools.combinations(rest, 3):
            bm = (1 << b[0]) | (1 << b[1]) | (1 << b[2])
            new = canonicalize_hero_and_board_masks(hm, bm)
            old = _python_canon(hm, bm)
            if new != old:
                mismatches.append((hm, bm, old, new))
                if len(mismatches) > 3:
                    break
        if mismatches:
            break
    assert not mismatches, mismatches[:3]


def test_canonicalize_hero_and_board_masks_matches_python_ground_truth_turn():
    """Sample of turns: 50 hands × ~500 random turn boards (deterministic seed)."""
    rng = random.Random(20260614)
    hand_iter = list(_itertools.islice(_itertools.combinations(range(52), 2), 50))
    mismatches = 0
    checked = 0
    for h in hand_iter:
        hm = (1 << h[0]) | (1 << h[1])
        rest = [c for c in range(52) if c not in h]
        for _ in range(500):
            b = tuple(rng.sample(rest, 4))
            bm = sum(1 << c for c in b)
            new = canonicalize_hero_and_board_masks(hm, bm)
            old = _python_canon(hm, bm)
            checked += 1
            if new != old:
                mismatches += 1
    assert mismatches == 0, f"{mismatches} / {checked} mismatched"


def test_generate_canonical_situations_flop_count_stable():
    """The flop canonical-situation count is a structural invariant: any
    rewrite of the canonicalization must produce the same set size.

    The binding emits strict (canon_version=2) keys, so this is the exact
    Burnside orbit count for S4 acting on the suits. It was 1_420_796 while
    the binding was pinned to the legacy canonicalization, which has since
    been removed."""
    out = generate_canonical_situations(3)
    assert len(out) == 1286792
