# PHH Format Support (`xiapl.phh`)

`xiapl.phh` reads and writes the **Poker Hand History (PHH)** file format — a
TOML-based standard for recording poker hands, designed for both human
readability and machine parsing. It is a pure-Python addition to `xiapl`: not
part of the compiled `xiapl_core` extension, and stdlib-only (`tomllib`,
`dataclasses`, `re`, `pathlib`, `datetime`).

- Spec docs: <https://phh.readthedocs.io/>
- Format repo / reference implementation: <https://github.com/uoftcprg/phh-std>
  (and the sibling `uoftcprg/phh-dataset`, source of two of `xiapl`'s test
  fixtures — see [Fixtures and test oracle](#fixtures-and-test-oracle))
- Paper: *"PHH: A Human-Friendly and Machine-Readable Poker Hand History
  Format"*, <https://arxiv.org/abs/2312.11753>

This document covers `xiapl`'s subset (`python/xiapl/phh.py`), not the full
spec. Where this document and the module's own docstrings disagree, the
docstrings (and the tests pinning them) win.

## Supported subset

PHH 0.0.2 defines eleven variant codes. `xiapl.phh` reads and writes two:

| Code | Variant | `xiapl` |
|---|---|---|
| `NT` | No-limit Texas hold 'em | → `GameType.Holdem` |
| `PO` | Pot-limit Omaha hold 'em | → `GameType.Plo` |
| `FT` | Fixed-limit Texas hold 'em | unsupported |
| `NS` | No-limit short-deck hold 'em | unsupported |
| `FO/8` | Fixed-limit Omaha hi/lo eight-or-better | unsupported |
| `F7S` | Fixed-limit seven card stud | unsupported |
| `F7S/8` | Fixed-limit seven card stud hi/lo eight-or-better | unsupported |
| `FR` | Fixed-limit razz | unsupported |
| `N2L1D` | No-limit deuce-to-seven single draw | unsupported |
| `F2L3D` | Fixed-limit deuce-to-seven triple draw | unsupported |
| `FB` | Fixed-limit badugi | unsupported |

Any other `variant` raises `UnsupportedVariantError`, a `ValueError`
subclass — so a caller iterating a mixed collection can
`except phh.UnsupportedVariantError:` to skip unsupported hands while still
treating any other failure as real:

```pycon
>>> from xiapl import phh
>>> phh.read_phh("tests/fixtures/phh/alice-carol-wikipedia.phh")  # FB / badugi
xiapl.phh.UnsupportedVariantError: PHH variant 'FB' is not read by xiapl 0.1
(supported: 'NT' no-limit Texas hold 'em, 'PO' pot-limit Omaha hold 'em).
PHH 0.0.2 defines FT, NT, NS, PO, FO/8, F7S, F7S/8, FR, N2L1D, F2L3D, FB.
```

`NS` (short-deck) is excluded for a more structural reason than the other
nine: it's a different card universe, not just a different rule set. The
PHH spec itself notes short-deck's non-standard deck ("low ranks from
deuces to fives do not exist" — 36 cards, not 52), while `xiapl`'s core
(`Card`, `Deck`, the 52-bit mask convention used throughout
`calculate_equity`/`judge`) assumes a fixed 52-card deck. Reading `NS`
would need deck-level support outside `phh.py`, not just another
variant-code table entry.

## Reading

```python
def parse_phh(text: str) -> PhhHand: ...
def parse_phh_all(text: str) -> list[PhhHand]: ...
def read_phh(path: str | os.PathLike[str]) -> PhhHand: ...
def read_phh_all(path: str | os.PathLike[str]) -> list[PhhHand]: ...
```

`parse_phh` reads a single-hand document (one TOML table, no sub-tables —
what a plain `.phh` file normally contains). `parse_phh_all` reads a
`.phhs` **collection**: a document whose top level is entirely sub-tables,
one per hand (`[1]`, `[2]`, ...). `read_phh`/`read_phh_all` are thin
`Path.read_text(encoding="utf-8")` wrappers around the two text parsers;
pure-Python file I/O, never bound into other language surfaces (see
[Roadmap](#roadmap--non-goals)).

The `.phhs` numbering convention is **not part of the PHH spec itself** —
the spec only describes a single hand's fields. It's a de-facto convention
from `pokerkit` (the reference implementation phh-std ships alongside the
spec), followed here for interop:

```pycon
>>> hands = phh.read_phh_all("tests/fixtures/phh/two_hands.phhs")
>>> [(h.collection_key, h.variant) for h in hands]
[('1', 'NT'), ('2', 'PO')]
```

Calling the wrong entry point on the wrong document shape raises a
`ValueError` with a hint at the right one:

```pycon
>>> phh.parse_phh(open("tests/fixtures/phh/two_hands.phhs").read())
ValueError: PHH: missing required field 'variant'. This document's top level
is 2 tables (1, 2) and no fields -- it looks like a multi-hand .phhs
collection; use parse_phh_all().

>>> phh.parse_phh_all(open("tests/fixtures/phh/dwan-ivey-2009.phh").read())
ValueError: PHH: parse_phh_all() expects a .phhs collection of top-level
tables, but this document defines 'variant' at the top level -- it is a
single hand; use parse_phh().
```

## Data model

### `PhhHand`

| Field | Type | Meaning |
|---|---|---|
| `variant` | `str` | Raw PHH variant code, e.g. `'NT'` or `'PO'`. |
| `game` | `GameType` | The `xiapl.simulation.GameType` for `variant`. |
| `antes` | `list[float]` | Per-player ante amounts, seat order. |
| `blinds_or_straddles` | `list[float]` | Per-player blind/straddle amounts. |
| `min_bet` | `float` | Minimum bet/raise increment. |
| `starting_stacks` | `list[float]` | Per-player starting stacks (`inf` allowed — see [Known divergences](#known-divergences--spec-notes)). |
| `actions` | `list[PhhAction]` | Parsed action sequence, document order. |
| `players` | `list[str]` | Display names. `[]` if the document has no `players` field. |
| `finishing_stacks` | `list[float]` | Per-player ending stacks. `[]` if absent. |
| `winnings` | `list[float]` | Per-player net winnings. `[]` if absent. |
| `extra` | `dict[str, object]` | Every other top-level TOML key, verbatim: `author`, `event`, `year`, `currency`, `ante_trimming_status`, etc. |
| `collection_key` | `str` | The TOML key this hand was filed under in a `.phhs` collection. Always `''` for `parse_phh()`. |

`PhhHand` also has two interop methods — see
[Interop with the analysis engine](#interop-with-the-analysis-engine).

All money fields are `float`, including `PhhAction.amount`; TOML integers
in the source (e.g. `antes = [500, 500, 500]`) are coerced to `float`.

### `PhhAction`

| Field | Type | Meaning |
|---|---|---|
| `verb` | `str` | One of `'db'`, `'dh'`, `'pb'`, `'cbr'`, `'cc'`, `'f'`, `'sd'`, `'sm'`, or `''` for a no-op/comment-only entry. |
| `player` | `int` | 1-based seat index. Receiving player for `'dh'`; `0` for `'db'` and no-ops. |
| `amount` | `float` | The `'cbr'` raise-**to** amount (total, not increment). |
| `cards` | `list[int]` | Card IDs of fully known cards, order written. |
| `unknown_count` | `int` | Cards written with any `'?'` wildcard (`'??'`, `'A?'`, `'?d'`). |
| `has_cards` | `bool` | Whether a card argument was written at all — the muck-vs-show / stand-pat-vs-discard discriminator. |
| `same_as_dealt` | `bool` | Whether this is the `'sm -'` form (show as already dealt, cards not repeated). |
| `commentary` | `str` | Text after the standalone `' # '` separator (PHH-level, distinct from a TOML comment). |
| `text` | `str` | The raw, unmodified action string as written. |

`has_cards` disambiguates the two pairs of forms that otherwise look
identical: `'pN sm'` (no cards, `has_cards=False`, muck) vs.
`'pN sm ????'` (`has_cards=True`, show — cash-game etiquette for "showing,
but you don't get to see it"); and `'pN sd'` (stand pat) vs.
`'pN sd ????'` (discard).

### Card ID convention

Same as the rest of `xiapl`: `id = suit * 13 + (rank - 2)`, rank `2..14`
(`14` = ace), suit `0=C, 1=D, 2=H, 3=S`. E.g. `Ac` → `12`, `2d` → `13`.

## Action grammar

Every action is one whitespace-tokenized line from the `actions` array:

| Line | `verb` | `player` | `amount` | `cards` | `unknown_count` | `has_cards` | `same_as_dealt` |
|---|---|---|---|---|---|---|---|
| `"d dh p1 Ac2d"` | `dh` | `1` | `0.0` | `[12, 13]` | `0` | `True` | `False` |
| `"d dh p2 ????"` | `dh` | `2` | `0.0` | `[]` | `2` | `True` | `False` |
| `"d db Jc3d5c"` | `db` | `0` | `0.0` | `[9, 14, 3]` | `0` | `True` | `False` |
| `"p1 cbr 23000"` | `cbr` | `1` | `23000.0` | `[]` | `0` | `False` | `False` |
| `"p2 f"` | `f` | `2` | `0.0` | `[]` | `0` | `False` | `False` |
| `"p1 sm -"` | `sm` | `1` | `0.0` | `[]` | `0` | `False` | `True` |
| `"p1 sd"` | `sd` | `1` | `0.0` | `[]` | `0` | `False` | `False` |

Full verb set: `db` (deal board), `dh` (deal hole cards), `pb` (post
bring-in — accepted structurally, though `bring_in` is a forbidden field
for NT/PO, so this verb won't appear in a spec-conformant NT/PO hand),
`cbr` (complete/bet/raise-to), `cc` (check/call), `f` (fold), `sd` (stand
pat/discard), `sm` (show/muck). An action string with no verb tokens
(empty, whitespace, or a standalone `"# comment"`) parses as a no-op:
`verb=""`.

### PHH commentary vs. TOML comments

Two independent mechanisms that both use `#`; only one survives parsing:

```toml
actions = [
  "p1 f",  # TOML comment, stripped by tomllib -- gone before phh.py runs
  "p2 sm # PHH commentary, part of the notation",
]
```

```pycon
>>> a = phh.parse_phh(open("tests/fixtures/phh/commentary.phh").read()).actions
>>> a[2].commentary   # the trailing "# TOML comment..." line comment
''
>>> a[3].verb, a[3].has_cards, a[3].commentary
('sm', False, 'PHH commentary, part of the notation')
```

A standalone `'#'` token starts commentary; a `'#'` **glued** to other
characters (`"p1 f #glued"`, no space) is rejected, matching pokerkit:

```pycon
>>> phh.parse_phh(open("tests/fixtures/phh/bad/glued_comment.phh").read())
ValueError: PHH action 2 "p1 f #glued": glued comment marker '#glued' (PHH
commentary requires a standalone '#' token, e.g. "p1 f # comment")
```

`'cbr'` amounts match `^[0-9]+(\.[0-9]+)?$` — no sign, no exponent, no
digit grouping. `"1,000"` is rejected, a deliberate divergence from
pokerkit (which strips commas):

```pycon
>>> phh.parse_phh(open("tests/fixtures/phh/bad/comma_amount.phh").read())
ValueError: PHH action 2 "p1 cbr 1,000": malformed amount '1,000'
```

## Required and forbidden fields

For `NT`/`PO`, required: `antes`, `blinds_or_straddles`, `min_bet`,
`starting_stacks`, `actions` (plus `variant` itself). Forbidden:
`bring_in`, `small_bet`, `big_bet` (fixed-limit/stud-only fields — see
[Known divergences](#known-divergences--spec-notes) for why this exact
rule, not the spec's own summary table, is what's implemented). Both
violations raise `ValueError`:

```pycon
>>> phh.parse_phh(open("tests/fixtures/phh/bad/missing_min_bet.phh").read())
ValueError: PHH: missing required field 'min_bet'

>>> phh.parse_phh(open("tests/fixtures/phh/bad/nt_with_small_bet.phh").read())
ValueError: PHH: field 'small_bet' is not a feature of variant 'NT'
(fixed-limit/stud only); the variant tag and the betting structure disagree.
```

Every parse failure is a `ValueError` (or `UnsupportedVariantError`), with
messages naming the offending field/token and, where useful, the fix (the
mode-confusion hints above are the clearest example). Inside a `.phhs`
collection, a per-hand failure is wrapped with a `"PHH hand [key]: "`
prefix, and **the original exception type is preserved** — an
`UnsupportedVariantError` in hand `[2]` stays an `UnsupportedVariantError`
after wrapping, not a plain `ValueError`:

```pycon
>>> phh.parse_phh_all(collection_with_bad_hand_2)
ValueError: PHH hand [2]: missing required field 'min_bet'
```

## Interop with the analysis engine

`PhhHand.hole_mask(player)` and `PhhHand.board_mask(max_cards=)` convert
dealt cards into the 52-bit masks used by `calculate_equity`/`judge`. Both
**raise rather than return a partial mask** if any relevant card is
unknown (`'?'`) — a caller wanting to tolerate unknowns should pre-check
`PhhAction.unknown_count` first. `board_mask(max_cards=N)` takes the first
`N` board cards across all `'db'` actions in order, so `N=0/3/4/5` selects
preflop/flop/turn/river; it doesn't error if the hand ended earlier.

Worked example — `tests/fixtures/phh/dwan-ivey-2009.phh`, Ivey's `Ac2d` vs.
Dwan's `7h6h` after the turn (`Jc3d5c4h`):

```python
from xiapl import phh
from xiapl.simulation import calculate_equity, SimulationOptions

hand = phh.read_phh("tests/fixtures/phh/dwan-ivey-2009.phh")
ivey_mask = hand.hole_mask(1)
dwan_mask = hand.hole_mask(3)
turn_board = hand.board_mask(max_cards=4)  # flop + turn, river not yet dealt

result = calculate_equity([ivey_mask, dwan_mask], turn_board, SimulationOptions.exact())
[p.equity for p in result.players]
```

Actual output:

```pycon
>>> [p.equity for p in result.players]
[0.0, 1.0]
```

Ivey is drawing dead at the turn. `hole_mask(2)` (Antonius, `'d dh p2
????'`, never revealed) would raise `ValueError` if called.

## Writing

```python
def format_phh(hand: PhhHand) -> str: ...
def format_phh_all(hands: list[PhhHand]) -> str: ...
def write_phh(hand: PhhHand, path: str | os.PathLike[str]) -> None: ...
def write_phh_all(hands: list[PhhHand], path: str | os.PathLike[str]) -> None: ...
```

> **No game-rule legality validation is performed: action ordering, bet
> sizing, blind structure, street transitions and stack accounting are NOT
> checked. These functions guarantee well-formed PHH syntax only; producing
> a LEGAL hand history is the caller's responsibility.**
>
> — verbatim from the writer section comment in `python/xiapl/phh.py`; each
> writer function's docstring repeats the same statement with per-function
> ("This function guarantees ...") wording.

What *is* checked before any text is produced (structural, not legality):
`hand.variant` must be `'NT'`/`'PO'` (else `UnsupportedVariantError`);
`hand.variant` and `hand.game` must agree (`'NT'`↔`Holdem`, `'PO'`↔`Plo`,
else `ValueError`); every `hand.actions` element must be a `PhhAction`
(else `ValueError`); an action with empty `text` must carry a verb the
reconstruction grammar knows how to spell (else `ValueError` — there is no
PHH notation for an unknown verb).

**Canonical output form:** `format_phh` always emits, in order: `variant`,
the five required fields, then `players`/`finishing_stacks`/`winnings`
**only if non-empty**, then every `extra` key in its original insertion
order. `collection_key` is never written. `format_phh_all` renumbers a
list of hands as `[1]..[N]` in list order — `collection_key` is not
consulted, so round-tripping a `.phhs` collection renumbers it rather than
preserving the original keys.

**Action emission:** if `action.text` is non-empty, it is emitted
**verbatim** — keeping round-tripped hands byte-stable for their action
lines, including PHH commentary and exotic spellings like `'A?'`.
Otherwise the string is rebuilt from structured fields with canonical
single-space grammar. Caveat: a `PhhAction` whose `text` disagrees with
its own structured fields (e.g. mutated `.amount` without clearing `.text`)
serializes using the stale `text` — clear `text` to get the new fields to
take effect.

**Round-trip contract:** `parse_phh(format_phh(hand))` is guaranteed
**semantically** identical to `hand` (`collection_key` aside). Byte-
identity with an original `.phh` source is explicitly not a goal: TOML
comments are stripped on read and can't be reproduced, and the writer
always uses its own canonical field order/spacing. A related caveat:
whitespace runs inside a *rebuilt* action's `commentary` were already
collapsed to single spaces by the parser, so a rebuilt line keeps the
words but not the original spacing (verbatim `text` preserves it exactly).

**Write-time rejections:** non-`NT`/`PO` `hand.variant`
(`UnsupportedVariantError`); `hand.variant`/`hand.game` disagreement
(`ValueError`); a non-`PhhAction` entry in `hand.actions` (`ValueError`);
an unrecognized `verb` on an action with empty `text` (`ValueError` — the
reconstruction grammar cannot spell it); commentary built from structured
fields containing a
**glued** `'#'` (e.g. `'c#1'`) — rejected because `parse_phh` itself would
reject that string, and the writer refuses to produce unparseable output.
A **standalone** `'#'` inside commentary (e.g. `'a # b'`) round-trips fine:

```pycon
>>> hand.actions = [PhhAction(verb="cc", player=1, commentary="c#1 gotcha")]
>>> phh.format_phh(hand)
ValueError: PHH action 0 (verb 'cc'): commentary 'c#1 gotcha' contains glued
comment marker 'c#1' -- PHH grammar cannot represent a '#' glued to other
characters inside commentary (only a standalone '#' token is representable)
```

### Programmatic construction

Actions built from scratch (empty `text`) get canonical grammar:

```python
from xiapl.phh import PhhAction, PhhHand
from xiapl.simulation import GameType
from xiapl import phh

hand = PhhHand(
    variant="NT", game=GameType.Holdem,
    antes=[0.0, 0.0], blinds_or_straddles=[1.0, 2.0], min_bet=2.0,
    starting_stacks=[200.0, 200.0],
    actions=[
        PhhAction(verb="dh", player=1, unknown_count=2, has_cards=True),
        PhhAction(verb="dh", player=2, unknown_count=2, has_cards=True),
        PhhAction(verb="cbr", player=1, amount=6.0),
        PhhAction(verb="f", player=2),
    ],
)
print(phh.format_phh(hand))
```

Actual output:

```toml
variant = "NT"
antes = [0, 0]
blinds_or_straddles = [1, 2]
min_bet = 2
starting_stacks = [200, 200]
actions = [
  "d dh p1 ????",
  "d dh p2 ????",
  "p1 cbr 6",
  "p2 f",
]
```

Integer-valued floats print bare (`200.0` → `200`) via
`float.is_integer()`; a non-integer amount like `1.5` prints as `1.5`, and
`inf`/`-inf` starting stacks print as TOML's own `inf`/`-inf` literal.

## Known divergences & spec notes

- **Comma-grouped amounts rejected** (`"1,000"` → `ValueError`); pokerkit
  strips commas instead.
- **Per-variant required/forbidden fields follow `required.rst`, not
  `spec.rst`.** The spec's own two pages disagree: `spec.rst`'s summary
  table marks `bring_in`, `small_bet`, `big_bet`, and `min_bet` **all**
  `Required = yes` with no per-variant qualification (self-contradictory —
  the spec's own prose elsewhere says `bring_in` and
  `blinds_or_straddles` "must never be defined together"). `required.rst`
  gives the real per-variant breakdown, and that's what
  `_REQUIRED_FIELDS`/`_FORBIDDEN_FIELDS` implement for `NT`/`PO`.
- **TOML has no null**, so an "unknown" `starting_stacks` entry — typed by
  the spec's field table as "integers, floats, **or null**" — is written
  and read as the float `inf` instead (the spec's own prose: "Unknown
  stack values can be denoted as `inf`").
- **Partial-unknown cards accepted** (`"A?"` known rank/unknown suit,
  `"?d"` unknown rank/known suit), matching pokerkit. The spec's prose
  only shows the fully-unknown form ("An unknown card must be represented
  with two question mark characters: `??`"), but its own rank and suit
  character tables *each* list `?` as a valid character independently, so
  `"A?"`/`"?d"` are constructible from the formal per-character grammar
  despite the prose only mentioning `??`. `_parse_card_token` follows the
  permissive reading: any 2-character chunk containing `?` counts toward
  `unknown_count`, regardless of which half is the `?`.
- **`.phhs` collections are not part of the spec** — a `pokerkit`-ecosystem
  convention followed here for interop, not something the spec defines.

## Fixtures and test oracle

`dwan-ivey-2009.phh`, `antonius-blom-2009.phh`, and
`alice-carol-wikipedia.phh` are transcribed verbatim from
[`uoftcprg/phh-dataset`](https://github.com/uoftcprg/phh-dataset) (MIT
License, © 2024-2025 University of Toronto Computer Poker Research Group;
see `tests/fixtures/phh/README.md`). Everything else under
`tests/fixtures/phh/`, including `bad/` and `two_hands.phhs`, is
hand-written for `xiapl`.

Some fixtures carry a sibling `<name>.expected.json` — a frozen,
language-agnostic golden rendering of `parse_phh`/`parse_phh_all` on that
fixture (built by `python/test/phh_golden.py`, checked by
`test_goldens_frozen`). Being plain JSON, readable without Python, these
also double as the reference oracle for a possible future C++ port (see
[Roadmap](#roadmap--non-goals)).

`python/test/test_phh_oracle.py` cross-checks both the reader and the
writer against `pokerkit`'s own PHH state machine, comparing the resolved
hole/board cards against what `xiapl.phh` parsed. This is optional and
non-blocking: skipped via `pytest.importorskip("pokerkit")` when pokerkit
isn't installed; pokerkit is never an `xiapl` install requirement.

## Full-corpus validation

The checked-in fixtures pin behaviour; the corpus run below is what says the
parser survives real-world files. `xiapl.phh` was run over the **entire
public [`phh-dataset`](https://github.com/uoftcprg/phh-dataset)** —
**31,870 files, 21,616,107 hands**: the HandHQ anonymized online cash-game
logs (July 2009, six sites, 25NL to 1000NL), all 10,000 hands played by
Pluribus, the televised 2023 WSOP $50,000 Poker Players Championship final
table, and the assorted historical hands. Result (2026-09-09):

- **Every file parsed.** Zero parse errors; no unhandled exception of any
  kind escaped the parser on any file.
- **Every hand round-tripped semantically.** For each hand,
  `parse_phh(format_phh(hand))` was compared against the original parsed
  `PhhHand` — the data model, not the text — and for `.phhs` collections the
  same through `parse_phh_all`/`format_phh_all`. Zero mismatches.
- **The 68 files outside the supported subset were rejected cleanly**, each
  with `UnsupportedVariantError` and nothing else. Between them they cover
  **all nine** PHH variant codes `xiapl` does not read (`FT`, `NS`, `FO/8`,
  `F7S`, `F7S/8`, `FR`, `N2L1D`, `F2L3D`, `FB`), so the "iterate a mixed
  collection and skip what you can't read" pattern documented under
  [Supported subset](#supported-subset) is exercised against every code it
  can encounter, not just the fixture's badugi hand.

The corpus is not vendored into this repository (it is a separate MIT-licensed
project, and 21.6 M hands do not belong in a test suite); the run is a
pre-release audit, not part of `pytest`.

## Roadmap / non-goals

`xiapl.phh` is a **format layer**, not a rules engine: it does not track
active players, enforce action legality, compute pot/side-pot size, or
validate that an action sequence is a legal poker hand. That belongs to a
future game-engine layer built on top of parsed `PhhHand`/`PhhAction` data
— this is why the writer's non-validation is load-bearing, not a temporary
gap (see [Writing](#writing)).

The dataclasses are deliberately "binding-shaped" — every field is
`int`/`float`/`str`/`bool`/`list[int]` (plus `GameType` and `dict` for the
two fields needing them) — so a possible future C++ port could bind
one-to-one via pybind11 without surface changes. File I/O
(`read_phh`/`read_phh_all`/`write_phh`/`write_phh_all`) is excluded from
that plan on purpose — a port would bind only the text-in/text-out
functions. None of this is committed; it's a design constraint kept
available, not a promise. There is currently no C ABI exposure for PHH
(`xiapl.phh` is Python-only).
