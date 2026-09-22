"""Parser and unvalidated writer for the Poker Hand History (PHH) file format.

Implements the NT (no-limit Texas hold'em) and PO (pot-limit Omaha) subset
of PHH 0.0.2 (https://phh.readthedocs.io/). Other variants raise
UnsupportedVariantError. The API is deliberately "binding-shaped": all
dataclass fields are int/float/str/bool/list[int] (plus GameType and dict for
the two fields that need them) so a future C++ port can be bound one-to-one
by pybind11 without surface changes.

A PHH document is parsed in two layers:

- TOML comments (``# ...`` on their own line, or trailing a line) are
  stripped by ``tomllib`` before this module ever sees the text -- they
  never appear as data.
- PHH commentary is a *separate* convention layered on top of individual
  action strings: a standalone ``"#"`` token inside an action string (e.g.
  ``"p1 f # gets shown"``) separates the grammar from free-text commentary,
  which this module surfaces via ``PhhAction.commentary``. A "glued" marker
  (``"#foo"``, no surrounding spaces) is rejected, matching pokerkit.

``parse_phh`` reads a single hand (one TOML table with no sub-tables).
``parse_phh_all`` reads a multi-hand ``.phhs`` collection (a TOML document
whose top level is entirely sub-tables, one per hand). ``read_phh`` and
``read_phh_all`` are pure-Python file-reading conveniences layered on top
of the two text parsers; they are never bound (a future C++ port binds the
text-parsing functions only, not file I/O).

``format_phh`` and ``format_phh_all`` are the writer counterparts: they
serialize a ``PhhHand`` (or a list of them) back to PHH/TOML text through a
minimal hand-rolled TOML emitter (``tomllib`` is read-only in the stdlib, so
there is no library to delegate to). ``write_phh`` and ``write_phh_all`` are
the matching file-writing conveniences, also never bound. The writer's
round-trip contract is semantic, not textual: ``parse_phh(format_phh(hand))``
reproduces the same fields as ``hand`` (``collection_key`` aside), but
byte-identity with an original hand-written .phh source is not a goal --
TOML comments are stripped on read and cannot be reproduced on write, and
the writer always emits its own canonical field order and spacing. See the
Writer section below (immediately above ``format_phh``) for what the writer
deliberately does -- and does not -- check.
"""
from __future__ import annotations

import datetime
import os
import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from xiapl.card import Card
from xiapl.simulation import GameType

# PHH 0.0.2 defines eleven variant codes; xiapl 0.1 only reads two of them.
_VARIANT_TO_GAME = {
    "NT": GameType.Holdem,
    "PO": GameType.Plo,
}
_ALL_PHH_VARIANTS = (
    "FT", "NT", "NS", "PO", "FO/8", "F7S", "F7S/8", "FR", "N2L1D", "F2L3D", "FB",
)

_REQUIRED_FIELDS = ("antes", "blinds_or_straddles", "min_bet", "starting_stacks", "actions")
_FORBIDDEN_FIELDS = ("bring_in", "small_bet", "big_bet")
_KNOWN_FIELDS = frozenset(
    {
        "variant",
        "antes",
        "blinds_or_straddles",
        "min_bet",
        "starting_stacks",
        "actions",
        "players",
        "finishing_stacks",
        "winnings",
    }
)

_PLAYER_TOKEN_RE = re.compile(r"^p([1-9][0-9]*)$")
_AMOUNT_RE = re.compile(r"^[0-9]+(\.[0-9]+)?$")

# Card-ID -> text rendering tables for the writer (the inverse of
# _parse_card_token's 2-char-per-card decoding below).
_RANKS = "23456789TJQKA"
_SUITS = "cdhs"

# A TOML "bare key" (unquoted): the writer uses this to decide whether an
# `extra` key can be written unquoted or needs basic-string quoting.
_BARE_KEY_RE = re.compile(r"^[A-Za-z0-9_-]+$")


class UnsupportedVariantError(ValueError):
    """A well-formed PHH hand in a variant xiapl 0.1 does not read."""


@dataclass
class PhhAction:
    """A single parsed PHH action-string entry.

    Attributes:
        verb: PHH verb token: one of 'db', 'dh', 'pb', 'cbr', 'cc', 'f',
            'sd', 'sm', or '' for a no-op / comment-only entry.
        player: 1-based player index as written in the action string. The
            receiving player for 'dh'; 0 for 'db' and no-ops.
        amount: The 'cbr' raise-to amount (the total the bet is raised to,
            not the increment).
        cards: Card IDs of the fully known cards, in the order written.
        unknown_count: Number of cards written with any '?' wildcard (e.g.
            '??', 'A?', '?d').
        has_cards: Whether a card argument was written at all. This is the
            only way to distinguish 'sd' stand-pat from 'sd ????', and 'sm'
            muck from 'sm ????'.
        same_as_dealt: Whether this is the 'sm -' form (show the hand as
            already dealt, without repeating the cards).
        commentary: Text after the standalone ' # ' separator inside the
            action string (PHH-level commentary, distinct from a TOML
            comment, which is stripped before this module sees the text).
        text: The raw, unmodified action string as written in the document.
    """

    verb: str = ""
    player: int = 0
    amount: float = 0.0
    cards: list[int] = field(default_factory=list)
    unknown_count: int = 0
    has_cards: bool = False
    same_as_dealt: bool = False
    commentary: str = ""
    text: str = ""


@dataclass
class PhhHand:
    """A single parsed PHH hand.

    Attributes:
        variant: The raw PHH variant code, e.g. 'NT' or 'PO'.
        game: The xiapl.simulation.GameType corresponding to `variant`.
        antes: Per-player ante amounts, in seat order.
        blinds_or_straddles: Per-player blind/straddle amounts, in seat
            order.
        min_bet: The minimum bet/raise increment.
        starting_stacks: Per-player starting stack sizes, in seat order.
        actions: The parsed action sequence, in document order.
        players: Player display names, in seat order. Empty if the document
            does not have a 'players' field.
        finishing_stacks: Per-player ending stack sizes, in seat order.
            Empty if the document does not have a 'finishing_stacks' field.
        winnings: Per-player net winnings, in seat order. Empty if the
            document does not have a 'winnings' field.
        extra: Every other top-level TOML key, verbatim (raw tomllib
            values, uncoerced). Includes fields such as 'author', 'event',
            'year', 'currency', and 'ante_trimming_status'.
        collection_key: The key this hand was filed under in a multi-hand
            .phhs collection. Always '' for parse_phh(), which only reads
            single-hand documents; populated by parse_phh_all().
    """

    variant: str
    game: GameType
    antes: list[float]
    blinds_or_straddles: list[float]
    min_bet: float
    starting_stacks: list[float]
    actions: list[PhhAction]
    players: list[str] = field(default_factory=list)
    finishing_stacks: list[float] = field(default_factory=list)
    winnings: list[float] = field(default_factory=list)
    extra: dict[str, object] = field(default_factory=dict)
    collection_key: str = ""

    def hole_mask(self, player: int) -> int:
        """Return the 52-bit mask of hole cards dealt to `player`.

        Accumulates every 'dh' action addressed to this player, in action
        order. NT and PO deal exactly once per player, so this is usually
        a single action's cards; draw variants (out of scope for xiapl 0.1)
        would deal more than once.

        This raises rather than returning a partial mask: a mask missing
        cards would silently produce a wrong equity computation downstream,
        so callers who want to tolerate unknown cards should pre-check the
        relevant PhhAction.unknown_count themselves before calling this.

        Args:
            player: 1-based seat index.

        Returns:
            The 52-bit mask (bit `id` set means the card with that ID,
            `id = suit * 13 + (rank - 2)`, is in this player's hand).

        Raises:
            ValueError: If `player` is outside 1..len(starting_stacks), if
                no 'dh' action was addressed to this player, or if any of
                those actions dealt an unknown card ('?' wildcard).
        """
        num_players = len(self.starting_stacks)
        if not 1 <= player <= num_players:
            raise ValueError(f"PHH: player {player} out of range (1..{num_players})")
        dealt = [a for a in self.actions if a.verb == "dh" and a.player == player]
        if not dealt:
            raise ValueError(f"PHH: no cards were dealt to player {player}")
        if any(a.unknown_count > 0 for a in dealt):
            raise ValueError(f"PHH: player {player}'s hole cards include an unknown ('?') card")
        mask = 0
        for action in dealt:
            for card_id in action.cards:
                mask |= 1 << card_id
        return mask

    def board_mask(self, *, max_cards: int = 5) -> int:
        """Return the 52-bit mask of the first `max_cards` board cards.

        Concatenates every 'db' action's cards in action order (flop, then
        turn, then river) and keeps the first `max_cards` of them, so
        `max_cards` 0/3/4/5 selects preflop/flop/turn/river. Returns fewer
        than `max_cards` cards, without error, if the hand ended before
        that street was dealt.

        Args:
            max_cards: Number of board cards to include, 0..5. Defaults to
                5 (the full river board).

        Returns:
            The 52-bit mask of the first `max_cards` board cards.

        Raises:
            ValueError: If `max_cards` is outside 0..5, or if a board card
                within the first `max_cards` is unknown ('?' wildcard).
        """
        if not 0 <= max_cards <= 5:
            raise ValueError(f"PHH: max_cards must be in 0..5, got {max_cards}")
        cards: list[int] = []
        for action in self.actions:
            if action.verb != "db":
                continue
            if len(cards) >= max_cards:
                break
            if action.unknown_count > 0:
                raise ValueError("PHH: board includes an unknown ('?') card")
            cards.extend(action.cards)
        mask = 0
        for card_id in cards[:max_cards]:
            mask |= 1 << card_id
        return mask


def parse_phh(text: str) -> PhhHand:
    """Parse a single-hand PHH document.

    Args:
        text: The full contents of a .phh file (TOML source).

    Returns:
        The parsed hand.

    Raises:
        ValueError: If `text` is not valid TOML, is missing a required
            field, has a field that does not belong to its betting
            structure, or has an action string that fails to parse. (This
            includes tomllib.TOMLDecodeError, which is a ValueError
            subclass and is allowed to propagate unchanged.)
        UnsupportedVariantError: If the document's 'variant' is not 'NT' or
            'PO'. Also a ValueError subclass.
    """
    data = tomllib.loads(text)
    return _parse_hand_data(data)


def parse_phh_all(text: str) -> list[PhhHand]:
    """Parse a multi-hand .phhs collection document.

    A .phhs collection is a TOML document whose top level consists
    entirely of sub-tables, one per hand (e.g. ``[1]``, ``[2]``, ...); each
    sub-table is parsed exactly as ``parse_phh`` parses a whole document.
    Hands are returned in document order, which is the order tomllib
    inserts keys into the parsed dict.

    Args:
        text: The full contents of a .phhs file (TOML source).

    Returns:
        The parsed hands, in document order, each with `collection_key`
        set to the TOML key it was filed under (e.g. '1', '2').

    Raises:
        ValueError: If `text` is not valid TOML, if any top-level value is
            not a table (i.e. this document is actually a single hand; use
            `parse_phh` instead), or if a hand fails to parse (see
            `parse_phh`; the message is prefixed with the offending hand's
            collection key).
        UnsupportedVariantError: If a hand inside the collection has an
            unsupported `variant` (see `parse_phh`). Also a ValueError
            subclass, and preserved as this exact type -- not downgraded to
            plain `ValueError` -- when wrapped with the collection-key
            prefix, so `except UnsupportedVariantError:` still works inside
            a `.phhs` collection.
    """
    data = tomllib.loads(text)

    for key, value in data.items():
        if not isinstance(value, dict):
            raise ValueError(
                "PHH: parse_phh_all() expects a .phhs collection of "
                f"top-level tables, but this document defines '{key}' at "
                "the top level -- it is a single hand; use parse_phh()."
            )

    hands = []
    for key, table in data.items():
        try:
            hand = _parse_hand_data(table)
        except ValueError as exc:
            # Preserve the original exception type (e.g.
            # UnsupportedVariantError) rather than downgrading to plain
            # ValueError, so callers filtering on the specific type still
            # work inside a collection. Strip the inner 'PHH: ' prefix (if
            # present) so the wrapped message doesn't read "PHH hand [2]
            # PHH: ...".
            message = str(exc).removeprefix("PHH: ")
            raise type(exc)(f"PHH hand [{key}]: {message}") from exc
        hand.collection_key = key
        hands.append(hand)
    return hands


def read_phh(path: str | os.PathLike[str]) -> PhhHand:
    """Read and parse a single-hand .phh file.

    A pure-Python convenience wrapper around `parse_phh` for reading from
    disk; never bound to other language ports (a future C++ port binds the
    text-parsing functions only, not file I/O).

    Args:
        path: Filesystem path to a .phh file, UTF-8 encoded.

    Returns:
        The parsed hand.

    Raises:
        OSError: If `path` cannot be read.
        ValueError: See `parse_phh`.
        UnsupportedVariantError: See `parse_phh`.
    """
    return parse_phh(Path(path).read_text(encoding="utf-8"))


def read_phh_all(path: str | os.PathLike[str]) -> list[PhhHand]:
    """Read and parse a multi-hand .phhs collection file.

    A pure-Python convenience wrapper around `parse_phh_all` for reading
    from disk; never bound to other language ports (a future C++ port
    binds the text-parsing functions only, not file I/O).

    Args:
        path: Filesystem path to a .phhs file, UTF-8 encoded.

    Returns:
        The parsed hands, in document order.

    Raises:
        OSError: If `path` cannot be read.
        ValueError: See `parse_phh_all`.
        UnsupportedVariantError: See `parse_phh_all`.
    """
    return parse_phh_all(Path(path).read_text(encoding="utf-8"))


# -----------------------------------------------------------------------------
# Writer: format_phh / format_phh_all / write_phh / write_phh_all
#
# No game-rule legality validation is performed: action ordering, bet
# sizing, blind structure, street transitions and stack accounting are NOT
# checked. These functions guarantee well-formed PHH syntax only; producing
# a LEGAL hand history is the caller's responsibility.
# -----------------------------------------------------------------------------


def format_phh(hand: PhhHand) -> str:
    """Serialize a single hand to PHH/TOML text.

    Emits 'variant', the five required fields, then 'players' /
    'finishing_stacks' / 'winnings' only if non-empty, then every 'extra'
    key in insertion order; 'collection_key' is never written. Each action
    is `action.text` verbatim if it is set (this is what keeps
    round-tripping a parsed hand byte-stable for its action lines,
    including PHH commentary and exotic spellings like 'A?'), else rebuilt
    from structured fields with canonical PHH grammar and spacing -- see
    `PhhAction` for the field semantics that determine the rebuilt form.

    The result is not guaranteed to be byte-identical to any original
    source this hand may have been parsed from: TOML comments do not
    survive the round trip (tomllib strips them on read, and this writer
    has nothing to put back), and field order/spacing is always this
    function's canonical form. `parse_phh(format_phh(hand))` is guaranteed
    to be semantically identical to `hand` instead (same fields,
    `collection_key` aside).

    Args:
        hand: The hand to serialize.

    Returns:
        PHH/TOML text for `hand`, ending with a trailing newline.

    Raises:
        UnsupportedVariantError: If `hand.variant` is not 'NT' or 'PO' --
            the same subset `parse_phh` reads. Also a ValueError subclass.
        ValueError: If `hand.variant` and `hand.game` disagree (e.g.
            variant 'NT' with `game=GameType.Plo`), or if any element of
            `hand.actions` is not a `PhhAction`.

    Notes:
        No game-rule legality validation is performed: action ordering,
        bet sizing, blind structure, street transitions and stack
        accounting are NOT checked. This function guarantees well-formed
        PHH syntax only; producing a LEGAL hand history is the caller's
        responsibility.

        Reconstructed action text (built when an action's `text` is empty)
        also round-trips whitespace lossily: internal runs of
        spaces/tabs/newlines inside `commentary` were already collapsed to
        single spaces by the parser, so a rebuilt line keeps the words but
        not the original spacing.
    """
    return _format_hand_body(hand) + "\n"


def format_phh_all(hands: list[PhhHand]) -> str:
    """Serialize a list of hands to a multi-hand .phhs collection.

    Each hand becomes a top-level TOML table, numbered sequentially '[1]'
    .. '[N]' in list order. `hand.collection_key` (set by `parse_phh_all`
    on the hands it returns) is ignored on write, so round-tripping a
    collection through `parse_phh_all` then `format_phh_all` renumbers it
    canonically rather than preserving the original keys.

    Args:
        hands: The hands to serialize, in the order they should appear.

    Returns:
        PHH/TOML text for the whole collection, ending with a trailing
        newline -- or the empty string if `hands` is empty. (There is no
        TOML syntax for a valid-but-empty .phhs collection distinct from
        an empty document, so the empty string is the pinned canonical
        form for this case; `parse_phh_all("")` accepts it back as zero
        hands.)

    Raises:
        UnsupportedVariantError: See `format_phh`.
        ValueError: See `format_phh`.

    Notes:
        No game-rule legality validation is performed: action ordering,
        bet sizing, blind structure, street transitions and stack
        accounting are NOT checked. This function guarantees well-formed
        PHH syntax only; producing a LEGAL hand history is the caller's
        responsibility.

        Reconstructed action text (built when an action's `text` is empty)
        also round-trips whitespace lossily: internal runs of
        spaces/tabs/newlines inside `commentary` were already collapsed to
        single spaces by the parser, so a rebuilt line keeps the words but
        not the original spacing.
    """
    if not hands:
        return ""
    blocks = [f"[{index}]\n{_format_hand_body(hand)}" for index, hand in enumerate(hands, start=1)]
    return "\n\n".join(blocks) + "\n"


def write_phh(hand: PhhHand, path: str | os.PathLike[str]) -> None:
    """Serialize a single hand and write it to a .phh file, UTF-8 encoded.

    A pure-Python convenience wrapper around `format_phh` for writing to
    disk; never bound to other language ports (mirrors `read_phh`).

    Args:
        hand: The hand to serialize.
        path: Destination filesystem path. Overwritten if it already
            exists.

    Raises:
        UnsupportedVariantError: See `format_phh`.
        ValueError: See `format_phh`.
        OSError: If `path` cannot be written.

    Notes:
        No game-rule legality validation is performed: action ordering,
        bet sizing, blind structure, street transitions and stack
        accounting are NOT checked. This function guarantees well-formed
        PHH syntax only; producing a LEGAL hand history is the caller's
        responsibility.

        Reconstructed action text (built when an action's `text` is empty)
        also round-trips whitespace lossily: internal runs of
        spaces/tabs/newlines inside `commentary` were already collapsed to
        single spaces by the parser, so a rebuilt line keeps the words but
        not the original spacing.
    """
    Path(path).write_text(format_phh(hand), encoding="utf-8")


def write_phh_all(hands: list[PhhHand], path: str | os.PathLike[str]) -> None:
    """Serialize a list of hands and write them to a .phhs file, UTF-8 encoded.

    A pure-Python convenience wrapper around `format_phh_all` for writing
    to disk; never bound to other language ports (mirrors `read_phh_all`).

    Args:
        hands: The hands to serialize, in the order they should appear.
        path: Destination filesystem path. Overwritten if it already
            exists.

    Raises:
        UnsupportedVariantError: See `format_phh`.
        ValueError: See `format_phh`.
        OSError: If `path` cannot be written.

    Notes:
        No game-rule legality validation is performed: action ordering,
        bet sizing, blind structure, street transitions and stack
        accounting are NOT checked. This function guarantees well-formed
        PHH syntax only; producing a LEGAL hand history is the caller's
        responsibility.

        Reconstructed action text (built when an action's `text` is empty)
        also round-trips whitespace lossily: internal runs of
        spaces/tabs/newlines inside `commentary` were already collapsed to
        single spaces by the parser, so a rebuilt line keeps the words but
        not the original spacing.
    """
    Path(path).write_text(format_phh_all(hands), encoding="utf-8")


def _parse_hand_data(data: dict[str, object]) -> PhhHand:
    """Parse one already-tomllib-loaded hand table into a PhhHand.

    Shared by `parse_phh` (whole document is one hand) and `parse_phh_all`
    (each sub-table is one hand).

    Args:
        data: The TOML table for a single hand.

    Returns:
        The parsed hand (`collection_key` left at its default '').

    Raises:
        ValueError: See `parse_phh`.
        UnsupportedVariantError: See `parse_phh`. Also a ValueError
            subclass.
    """
    if "variant" not in data:
        raise ValueError(_missing_variant_message(data))
    variant = data["variant"]
    if variant not in _VARIANT_TO_GAME:
        raise UnsupportedVariantError(_unsupported_variant_message(variant))
    game = _VARIANT_TO_GAME[variant]

    for field_name in _REQUIRED_FIELDS:
        if field_name not in data:
            raise ValueError(f"PHH: missing required field '{field_name}'")
    for field_name in _FORBIDDEN_FIELDS:
        if field_name in data:
            raise ValueError(
                f"PHH: field '{field_name}' is not a feature of variant "
                f"'{variant}' (fixed-limit/stud only); the variant tag and "
                "the betting structure disagree."
            )

    antes = _coerce_float_list(data["antes"], "antes")
    blinds_or_straddles = _coerce_float_list(data["blinds_or_straddles"], "blinds_or_straddles")
    min_bet = _coerce_float(data["min_bet"], "min_bet")
    starting_stacks = _coerce_float_list(data["starting_stacks"], "starting_stacks")
    num_players = len(starting_stacks)

    raw_actions = data["actions"]
    if not isinstance(raw_actions, list):
        raise ValueError(
            f"PHH: field 'actions' must be a list, got {type(raw_actions).__name__}"
        )
    actions = [_parse_action(raw, index, num_players) for index, raw in enumerate(raw_actions)]

    players = _coerce_str_list(data.get("players", []), "players")
    finishing_stacks = _coerce_float_list(data.get("finishing_stacks", []), "finishing_stacks")
    winnings = _coerce_float_list(data.get("winnings", []), "winnings")
    extra = {key: value for key, value in data.items() if key not in _KNOWN_FIELDS}

    return PhhHand(
        variant=variant,
        game=game,
        antes=antes,
        blinds_or_straddles=blinds_or_straddles,
        min_bet=min_bet,
        starting_stacks=starting_stacks,
        actions=actions,
        players=players,
        finishing_stacks=finishing_stacks,
        winnings=winnings,
        extra=extra,
    )


def _missing_variant_message(data: dict[str, object]) -> str:
    """Build the 'missing variant' error, with a multi-hand-collection hint if it applies."""
    if data and all(isinstance(value, dict) for value in data.values()):
        keys = ", ".join(data.keys())
        return (
            "PHH: missing required field 'variant'. This document's top "
            f"level is {len(data)} tables ({keys}) and no fields -- it "
            "looks like a multi-hand .phhs collection; use parse_phh_all()."
        )
    return "PHH: missing required field 'variant'"


def _unsupported_variant_message(variant: object) -> str:
    return (
        f"PHH variant '{variant}' is not read by xiapl 0.1 (supported: 'NT' "
        "no-limit Texas hold 'em, 'PO' pot-limit Omaha hold 'em). PHH 0.0.2 "
        f"defines {', '.join(_ALL_PHH_VARIANTS)}."
    )


def _unsupported_variant_write_message(variant: object) -> str:
    return (
        f"PHH variant '{variant}' is not written by xiapl 0.1 (supported: 'NT' "
        "no-limit Texas hold 'em, 'PO' pot-limit Omaha hold 'em). PHH 0.0.2 "
        f"defines {', '.join(_ALL_PHH_VARIANTS)}."
    )


def _coerce_float(value: object, field_name: str) -> float:
    """Coerce a TOML scalar to float, rejecting bool (not numeric) and other types."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(
            f"PHH: field '{field_name}' must be a number, got {type(value).__name__}"
        )
    return float(value)


def _coerce_float_list(value: object, field_name: str) -> list[float]:
    """Coerce a TOML array to list[float], element by element."""
    if not isinstance(value, list):
        raise ValueError(f"PHH: field '{field_name}' must be a list, got {type(value).__name__}")
    return [_coerce_float(item, field_name) for item in value]


def _coerce_str_list(value: object, field_name: str) -> list[str]:
    """Coerce a TOML array to list[str], rejecting non-list or non-string elements."""
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise ValueError(f"PHH: field '{field_name}' must be a list of strings")
    return list(value)


def _parse_action(raw: object, index: int, num_players: int) -> PhhAction:
    """Parse one action-array entry, wrapping any failure with positional context."""
    try:
        return _parse_action_body(raw, num_players)
    except ValueError as exc:
        raise ValueError(f'PHH action {index} "{raw}": {exc}') from exc


def _parse_action_body(raw: object, num_players: int) -> PhhAction:
    """Tokenize and dispatch one raw action string, without index/text context."""
    if not isinstance(raw, str):
        raise ValueError(f"action entry must be a string, got {type(raw).__name__}")

    tokens = raw.split()
    if not tokens:
        return PhhAction(text=raw)

    for token in tokens:
        if "#" in token and token != "#":
            raise ValueError(
                f"glued comment marker {token!r} (PHH commentary requires a "
                "standalone '#' token, e.g. \"p1 f # comment\")"
            )

    commentary = ""
    if "#" in tokens:
        hash_index = tokens.index("#")
        commentary = " ".join(tokens[hash_index + 1 :])
        tokens = tokens[:hash_index]

    if not tokens:
        return PhhAction(text=raw, commentary=commentary)

    action = _dispatch_grammar(tokens, num_players)
    action.text = raw
    action.commentary = commentary
    return action


def _dispatch_grammar(tokens: list[str], num_players: int) -> PhhAction:
    """Dispatch a tokenized action to its verb-specific parser.

    Grammar: 'd db C', 'd dh pN C', 'pN pb', 'pN cbr AMT', 'pN cc', 'pN f',
    'pN sd [C]', 'pN sm [C | -]'.
    """
    head = tokens[0]
    if head == "d":
        return _parse_deal(tokens, num_players)

    player = _parse_player_token(head, num_players)
    if len(tokens) < 2:
        raise ValueError(f"missing verb after {head!r}")
    verb = tokens[1]
    rest = tokens[2:]

    if verb == "pb":
        _require_arity(rest, 0, verb)
        return PhhAction(verb=verb, player=player)
    if verb == "cbr":
        _require_arity(rest, 1, verb)
        return PhhAction(verb=verb, player=player, amount=_parse_amount(rest[0]))
    if verb == "cc":
        _require_arity(rest, 0, verb)
        return PhhAction(verb=verb, player=player)
    if verb == "f":
        _require_arity(rest, 0, verb)
        return PhhAction(verb=verb, player=player)
    if verb == "sd":
        return _parse_sd(player, rest)
    if verb == "sm":
        return _parse_sm(player, rest)
    raise ValueError(f"unknown verb {verb!r}")


def _parse_deal(tokens: list[str], num_players: int) -> PhhAction:
    """Parse the 'd db C' (deal board) and 'd dh pN C' (deal hole cards) forms."""
    if len(tokens) == 3 and tokens[1] == "db":
        cards, unknown_count = _parse_card_token(tokens[2])
        return PhhAction(verb="db", player=0, cards=cards, unknown_count=unknown_count, has_cards=True)
    if len(tokens) == 4 and tokens[1] == "dh":
        player = _parse_player_token(tokens[2], num_players)
        cards, unknown_count = _parse_card_token(tokens[3])
        return PhhAction(
            verb="dh", player=player, cards=cards, unknown_count=unknown_count, has_cards=True
        )
    raise ValueError(f"malformed deal action {' '.join(tokens)!r}")


def _parse_sd(player: int, rest: list[str]) -> PhhAction:
    """Parse the 'pN sd [C]' (stand pat / discard) form; C is optional."""
    if not rest:
        return PhhAction(verb="sd", player=player)
    if len(rest) == 1:
        cards, unknown_count = _parse_card_token(rest[0])
        return PhhAction(
            verb="sd", player=player, cards=cards, unknown_count=unknown_count, has_cards=True
        )
    raise ValueError("malformed 'sd' action: too many arguments")


def _parse_sm(player: int, rest: list[str]) -> PhhAction:
    """Parse the 'pN sm [C | -]' (show / muck) form; C and '-' are optional."""
    if not rest:
        return PhhAction(verb="sm", player=player)
    if len(rest) == 1:
        if rest[0] == "-":
            return PhhAction(verb="sm", player=player, same_as_dealt=True)
        cards, unknown_count = _parse_card_token(rest[0])
        return PhhAction(
            verb="sm", player=player, cards=cards, unknown_count=unknown_count, has_cards=True
        )
    raise ValueError("malformed 'sm' action: too many arguments")


def _parse_player_token(token: str, num_players: int) -> int:
    """Parse and range-check a 'pN' token against the table's seat count."""
    match = _PLAYER_TOKEN_RE.match(token)
    if not match:
        raise ValueError(f"invalid player token {token!r} (expected 'pN')")
    player = int(match.group(1))
    if not 1 <= player <= num_players:
        raise ValueError(f"player index {player} out of range (1..{num_players})")
    return player


def _require_arity(rest: list[str], expected: int, verb: str) -> None:
    """Raise if a verb's argument count does not match the grammar."""
    if len(rest) != expected:
        raise ValueError(f"verb {verb!r} expects {expected} argument(s), got {len(rest)}")


def _parse_amount(token: str) -> float:
    """Parse a 'cbr' amount token. Rejects comma-grouped digits (diverges from pokerkit)."""
    if not _AMOUNT_RE.match(token):
        raise ValueError(f"malformed amount {token!r}")
    return float(token)


def _parse_card_token(token: str) -> tuple[list[int], int]:
    """Split a glued card token into 2-char chunks and resolve each to a card ID.

    Args:
        token: A run of concatenated 2-char card codes, e.g. 'Jc3d5c' or
            '????'. Any chunk containing '?' is an unknown card.

    Returns:
        A tuple of (known card IDs in written order, count of unknown cards).
    """
    if len(token) % 2 != 0:
        raise ValueError(f"card token {token!r} has odd length (cards are 2 characters each)")
    cards: list[int] = []
    unknown_count = 0
    for i in range(0, len(token), 2):
        chunk = token[i : i + 2]
        if "?" in chunk:
            unknown_count += 1
        else:
            try:
                cards.append(Card.from_string(chunk).id)
            except ValueError as exc:
                raise ValueError(f"invalid card {chunk!r}: {exc}") from exc
    return cards, unknown_count


def _check_hand_for_write(hand: PhhHand) -> None:
    """Check the writer's structural preconditions on `hand`.

    This is deliberately narrow -- see the Writer section comment above
    `format_phh` for what is NOT checked (every game-rule legality
    concern: action ordering, bet sizing, blind structure, street
    transitions, stack accounting).

    Args:
        hand: The hand to check.

    Raises:
        UnsupportedVariantError: If `hand.variant` is not 'NT' or 'PO'.
        ValueError: If `hand.variant` and `hand.game` disagree, or if any
            element of `hand.actions` is not a `PhhAction`.
    """
    if hand.variant not in _VARIANT_TO_GAME:
        raise UnsupportedVariantError(_unsupported_variant_write_message(hand.variant))
    expected_game = _VARIANT_TO_GAME[hand.variant]
    if hand.game != expected_game:
        raise ValueError(
            f"PHH: variant {hand.variant!r} implies game {expected_game.name!r}, "
            f"but hand.game is {hand.game.name!r}"
        )
    for index, action in enumerate(hand.actions):
        if not isinstance(action, PhhAction):
            raise ValueError(
                f"PHH: actions[{index}] must be a PhhAction, got {type(action).__name__}"
            )


def _format_hand_body(hand: PhhHand) -> str:
    """Render one hand's fields as newline-joined 'key = value' lines.

    Shared by `format_phh` (the whole return value) and `format_phh_all`
    (one per '[N]' table). Runs `_check_hand_for_write` first, so an
    invalid hand raises before any text is produced.

    Args:
        hand: The hand to render.

    Returns:
        The field lines, newline-joined, without a table header or a
        trailing newline.

    Raises:
        UnsupportedVariantError: See `_check_hand_for_write`.
        ValueError: See `_check_hand_for_write`.
    """
    _check_hand_for_write(hand)
    lines = [
        f"variant = {_toml_value(hand.variant)}",
        f"antes = {_toml_value(hand.antes)}",
        f"blinds_or_straddles = {_toml_value(hand.blinds_or_straddles)}",
        f"min_bet = {_toml_value(hand.min_bet)}",
        f"starting_stacks = {_toml_value(hand.starting_stacks)}",
        _format_actions_field(hand.actions),
    ]
    if hand.players:
        lines.append(f"players = {_toml_value(hand.players)}")
    if hand.finishing_stacks:
        lines.append(f"finishing_stacks = {_toml_value(hand.finishing_stacks)}")
    if hand.winnings:
        lines.append(f"winnings = {_toml_value(hand.winnings)}")
    for key, value in hand.extra.items():
        lines.append(f"{_toml_key(key)} = {_toml_value(value)}")
    return "\n".join(lines)


def _format_actions_field(actions: list[PhhAction]) -> str:
    """Render the 'actions = [...]' field, always multiline (one action per line).

    Args:
        actions: The action sequence, in document order.

    Returns:
        The full 'actions = [...]' field, including its own 'actions = '
        prefix (unlike every other field line, which callers prefix
        themselves). '[]' single-line for an empty sequence.

    Raises:
        ValueError: See `_format_action`; wrapped with the action's index
            and verb for context.
    """
    if not actions:
        return "actions = []"
    lines = ["actions = ["]
    for index, action in enumerate(actions):
        lines.append(f"  {_toml_value(_format_action_at(action, index))},")
    lines.append("]")
    return "\n".join(lines)


def _format_action_at(action: PhhAction, index: int) -> str:
    """Render one action, wrapping any `_format_action` failure with index/verb context."""
    try:
        return _format_action(action)
    except ValueError as exc:
        raise ValueError(f"PHH action {index} (verb {action.verb!r}): {exc}") from exc


def _format_action(action: PhhAction) -> str:
    """Render one action to its PHH action-string body (no surrounding quotes).

    If `action.text` is non-empty, it is returned verbatim -- this is what
    keeps round-tripping a parsed hand byte-stable for its action lines,
    including PHH commentary and exotic spellings like 'A?'. Otherwise the
    string is rebuilt from the structured fields with canonical spacing.

    This implies a stale-`text` caveat: a `PhhAction` whose `text`
    disagrees with its own structured fields (e.g. built by mutating a
    parsed action's `.amount` without also clearing `.text`) serializes
    using `text`, not the structured fields. Callers who want the
    structured fields to take effect must clear `text` (or build a fresh
    `PhhAction` without one). There is also a whitespace-collapse caveat
    on the rebuilt path: `PhhAction.commentary` already has internal
    whitespace runs collapsed to single spaces by the parser (that
    collapsing happened before `commentary` was ever populated), so a
    rebuilt action's commentary round-trips its words but not its
    original spacing.

    Args:
        action: The action to render.

    Returns:
        The action-string body.

    Raises:
        ValueError: If `action.text` is empty and `action.verb` is not one
            of the PHH verbs this module parses -- there is no grammar to
            rebuild it from. Also if `action.text` is empty and
            `action.commentary` contains a token with a '#' glued to other
            characters (e.g. 'c#1') -- PHH grammar has no escape for that,
            and parsing the rebuilt string back would raise a glued-marker
            error, breaking the round-trip contract. A standalone '#'
            token inside commentary is fine and round-trips.
    """
    if action.text:
        return action.text
    body = _format_action_grammar(action)
    if not action.commentary:
        return body
    _check_commentary_for_write(action.commentary)
    return f"{body} # {action.commentary}" if body else f"# {action.commentary}"


def _check_commentary_for_write(commentary: str) -> None:
    """Reject commentary the parser could not read back (a glued '#' marker).

    The parser rejects any action-string token that contains '#' but isn't
    exactly '#' (see `_parse_action_body`'s glued-comment check) -- PHH
    grammar has no escape for a '#' glued to other characters. Since
    `commentary` is appended verbatim after ' # ' when an action is
    rebuilt from structured fields, a commentary token like 'c#1' would
    silently produce an action string `parse_phh` itself rejects. This
    check catches that at write time instead, with a clear error naming
    the offending token, rather than letting the caller find out only by
    round-tripping the output back through `parse_phh`.

    A standalone '#' token inside `commentary` (e.g. commentary 'a # b')
    is fine: it does not glue to anything and round-trips correctly, since
    the parser splits commentary from grammar on the *first* standalone
    '#' token and rejoins everything after it (including further '#'
    tokens) back into `commentary`.

    Args:
        commentary: The commentary text about to be appended to a
            reconstructed (non-verbatim-`text`) action string.

    Raises:
        ValueError: If any whitespace-separated token of `commentary`
            contains '#' but is not exactly '#'.
    """
    for token in commentary.split():
        if "#" in token and token != "#":
            raise ValueError(
                f"commentary {commentary!r} contains glued comment marker {token!r} "
                "-- PHH grammar cannot represent a '#' glued to other characters "
                "inside commentary (only a standalone '#' token is representable)"
            )


def _format_action_grammar(action: PhhAction) -> str:
    """Rebuild the verb/player/cards/amount portion of an action string (no commentary).

    Mirrors `_dispatch_grammar`'s grammar in reverse: 'd db C', 'd dh pN
    C', 'pN pb', 'pN cbr AMT', 'pN cc', 'pN f', 'pN sd [C]', 'pN sm
    [C | -]'.

    Args:
        action: The action to render (`action.text` is ignored here; see
            `_format_action`).

    Returns:
        The verb/player/cards/amount portion, or '' for a no-op
        (`verb == ''`).

    Raises:
        ValueError: If `action.verb` is not a recognized PHH verb.
    """
    verb = action.verb
    if verb == "":
        return ""
    if verb == "db":
        return f"d db {_format_cards(action.cards, action.unknown_count)}"
    if verb == "dh":
        return f"d dh p{action.player} {_format_cards(action.cards, action.unknown_count)}"
    if verb == "pb":
        return f"p{action.player} pb"
    if verb == "cbr":
        return f"p{action.player} cbr {_format_float(action.amount)}"
    if verb == "cc":
        return f"p{action.player} cc"
    if verb == "f":
        return f"p{action.player} f"
    if verb == "sd":
        if not action.has_cards:
            return f"p{action.player} sd"
        return f"p{action.player} sd {_format_cards(action.cards, action.unknown_count)}"
    if verb == "sm":
        if action.same_as_dealt:
            return f"p{action.player} sm -"
        if not action.has_cards:
            return f"p{action.player} sm"
        return f"p{action.player} sm {_format_cards(action.cards, action.unknown_count)}"
    raise ValueError(f"PHH: cannot reconstruct an action string for unknown verb {verb!r}")


def _format_cards(cards: list[int], unknown_count: int) -> str:
    """Render card IDs (plus a trailing run of unknowns) as a glued PHH card token.

    The inverse of `_parse_card_token`.

    Args:
        cards: Known card IDs, in the order they should be written.
        unknown_count: Number of trailing '?' wildcard cards to append.

    Returns:
        The concatenated 2-char-per-card token, e.g. 'Jc3d5c' or '????'.
    """
    known = "".join(f"{_RANKS[card_id % 13]}{_SUITS[card_id // 13]}" for card_id in cards)
    return known + "??" * unknown_count


def _format_float(value: float) -> str:
    """Render a float as a TOML number literal.

    `float.is_integer()` covers ordinary chip amounts, so e.g. `7000.0`
    prints as `7000`, not `7000.0`. Everything else -- including
    `inf`/`-inf`, whose `repr()` is already the TOML literal spelling --
    falls through to `repr()`, Python's own shortest round-tripping form.

    Args:
        value: The float to render.

    Returns:
        The TOML-literal spelling of `value`.
    """
    if value.is_integer():
        return str(int(value))
    return repr(value)


def _toml_value(value: object) -> str:
    """Render one Python value as a TOML literal (scalar, or an inline collection).

    Handles every type `tomllib.loads` can produce -- and therefore every
    type `PhhHand.extra` can hold -- plus the dataclass fields' own
    int/float/str/list types.

    Args:
        value: The value to render.

    Returns:
        The TOML-literal spelling of `value`.

    Raises:
        ValueError: If `value`'s type has no TOML representation.
    """
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return _format_float(value)
    if isinstance(value, str):
        return _format_toml_string(value)
    if isinstance(value, (datetime.datetime, datetime.date, datetime.time)):
        return value.isoformat()
    if isinstance(value, list):
        return "[" + ", ".join(_toml_value(item) for item in value) + "]"
    if isinstance(value, dict):
        if not value:
            return "{}"
        inner = ", ".join(f"{_toml_key(key)} = {_toml_value(item)}" for key, item in value.items())
        return "{ " + inner + " }"
    raise ValueError(f"PHH: cannot serialize a value of type {type(value).__name__} to TOML")


def _format_toml_string(value: str) -> str:
    """Render a Python string as a double-quoted TOML basic string.

    Escapes backslash and double-quote with their TOML escapes, and any
    control character (codepoint < 0x20, plus DEL) as `\\uXXXX`; every
    other character -- including all non-ASCII text -- is written raw
    (TOML basic strings are UTF-8 by definition).

    Args:
        value: The string to render.

    Returns:
        The double-quoted TOML literal, including its surrounding quotes.
    """
    chars = ['"']
    for ch in value:
        if ch == "\\":
            chars.append("\\\\")
        elif ch == '"':
            chars.append('\\"')
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            chars.append(f"\\u{ord(ch):04X}")
        else:
            chars.append(ch)
    chars.append('"')
    return "".join(chars)


def _toml_key(key: str) -> str:
    """Render a TOML key: bare if `key` is a safe bare-key, else a quoted basic string."""
    if _BARE_KEY_RE.match(key):
        return key
    return _format_toml_string(key)
