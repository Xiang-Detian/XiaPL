# XiaPL

A fast poker hand-evaluation and equity library for Texas Hold'em and
Pot-Limit Omaha, used from Python. The public surface is a typed Python
package (`xiapl`); underneath it is a zero-dependency C++20 core, which is
also usable directly from C++.

**Version 0.1.0** | [MIT License](https://github.com/Xiang-Detian/XiaPL/blob/main/LICENSE)

Three things set XiaPL apart:

- **Speed** — bitwise 52-card mask arithmetic throughout, exact enumeration
  where it's tractable and reproducible Monte Carlo elsewhere, and threaded
  evaluation paths that are bit-identical at every thread count. The C++
  core is the speed story, not a second product to learn — Python calls
  straight into it. Single-threaded 7-card evaluation costs 10.6 ns/eval in
  batch through the public entry point on an Apple M1 Pro; the head-to-head
  against PH Evaluator and OMPEval on two machines — method, correctness
  cross-check, and the contexts where XiaPL loses — is in
  [docs/benchmarks.md](https://github.com/Xiang-Detian/XiaPL/blob/main/docs/benchmarks.md).
- **PLO as a first-class citizen** — Hold'em and PLO are not two libraries
  glued together. Every public entry point (`evaluate_hand`, `judge`,
  `calculate_equity`, `Range.from_string`, ...) takes a single trailing
  `game` parameter instead of a `_holdem` / `_plo` split, and PLO gets its
  own weighted range grammar rather than an awkward reuse of Hold'em
  notation.
- **Usability** — a typed Python API (`py.typed` plus hand-written `.pyi`
  stubs verified against the binding with mypy stubtest), reproducible
  seeded Monte Carlo end-to-end, and parser error messages that teach the
  correct syntax instead of just rejecting input.

XiaPL is **0.x / alpha**: the Python API is the supported surface and the
one this README documents, but it may still evolve (rename, reshape,
regroup) before 1.0 — see [Pre-1.0 API notes](#pre-10-api-notes).

Also: best-of-5-to-7 hand evaluation with suit-isomorphism canonicalization,
exact (non-sampled) top-% starting-hand ranking, weighted range set algebra
(`|` / `&` / `-`), PHH hand-history read/write (`xiapl.phh`), and a
zero-external-dependency C++20 core that's source-portable across
macOS / Linux / Windows and arm64 / x86_64.

---

## Installation

```bash
pip install xiapl
```

is the planned route once a wheel is published to PyPI. Today, install from
a source checkout:

```bash
pip install .
```

This builds the C++ extension via pybind11 as part of the install (macOS
deployment target is handled automatically, no shell environment variable
needed) and installs `xiapl`, including a `py.typed` marker and hand-written
`.pyi` stubs for mypy / IDE support, and exposes `xiapl.__version__`.

Requires **Python >= 3.11** and a C++20 compiler (Apple clang, GCC, clang,
or MSVC — see [Platform support](#platform-support)). No other build
dependency is required beyond `pybind11`, which `pip install .` pulls in
automatically via `pyproject.toml`.

For iterative development (editable install — the flow used by the test
suite; Python-side edits are picked up immediately, C++-side edits need a
re-run of the command below):

```bash
pip install -e .
```

Every `pip install -e .` fully recompiles the C++ extension (setuptools'
editable-install hook builds in a fresh temp directory each time, so there is
no incremental object-file cache across invocations) — re-run it after
touching anything under `src/`, `include/`, or `binding/`. If you already
have `pybind11` and `setuptools` installed, skip pip's per-invocation build
sandbox with:

```bash
pip install -e . --no-build-isolation
```

(this does not make individual rebuilds faster, since every recompile is
still a full rebuild — it only skips reinstalling the build toolchain into a
throwaway environment on every call).

---

## Quick Start

### Cards & Deck

```python
from xiapl.card import Card
from xiapl.deck import Deck

ace_spades = Card.from_string("As")
print(ace_spades)          # As
print(ace_spades.rank, ace_spades.suit, ace_spades.id)  # 14 s 51

deck = Deck()
deck.shuffle(seed=7)       # reproducible; omit for a random shuffle
hole = deck.deal(2)
flop = deck.deal(3)
print([str(c) for c in hole])  # ['6c', '6h']
print([str(c) for c in flop])  # ['Qh', '8h', '3d']
```

### Hand Evaluation — `evaluate_cards` / `evaluate_hand` / `judge`

`evaluate_*` return a `HandValue` (category + tie-breaker ranks). The
surface is never split by game: one entry point per concept, with a
trailing keyword-only `game` that defaults to `GameType.Holdem`.

```python
from xiapl.card import Card
from xiapl.eval import evaluate_cards, evaluate_hand, judge, describe_hand
from xiapl.simulation import GameType
from xiapl.utils import cards_to_mask

# Best five out of 5..7 loose cards (no hole/board split)
cards = [Card.from_string(s) for s in ("As", "Ks", "Qs", "Js", "Ts", "2c", "7h")]
print(describe_hand(evaluate_cards(cards)))  # Straight Flush [14]

# Mask form: Hold'em (board + 2 hole)
board  = cards_to_mask([Card.from_string(s) for s in ("As", "Kd", "2c", "7h", "9s")])
hole_a = cards_to_mask([Card.from_string("Ah"), Card.from_string("Ac")])
hole_b = cards_to_mask([Card.from_string("Ks"), Card.from_string("Kh")])
print(describe_hand(evaluate_hand(board, hole_a)))  # Three of a Kind [14 13 9]

# Multiway showdown: indices of every player holding the best hand
# (more than one index means a chop)
print(judge([hole_a, hole_b], board))  # [0]

# PLO: board + 4 hole, same functions, game=GameType.Plo
plo_board = cards_to_mask([Card.from_string(s) for s in ("2h", "7d", "Jc", "4s", "9h")])
plo_hole  = cards_to_mask([Card.from_string(s) for s in ("As", "Ks", "Qd", "Jd")])
plo_value = evaluate_hand(plo_board, plo_hole, game=GameType.Plo)
print(describe_hand(plo_value))  # One Pair [11 14 9 7]
```

### Reproducible Equity — `calculate_equity` + `SimulationOptions`

`SimulationOptions.exact()` enumerates exhaustively; `.mc_seeded(n, seed)`
runs deterministic Monte Carlo — repeated calls with the same options are
bit-exact.

```python
from xiapl.card import Card
from xiapl.simulation import calculate_equity, SimulationOptions, GameType
from xiapl.utils import cards_to_mask

hole_a = cards_to_mask([Card.from_string("As"), Card.from_string("Ks")])
hole_b = cards_to_mask([Card.from_string("Qd"), Card.from_string("Qc")])

# 1) Exact preflop enumeration
r = calculate_equity([hole_a, hole_b], 0, SimulationOptions.exact())
print(r.exact, r.trials, [round(p.equity, 4) for p in r.players])
# True 1712304 [0.4621, 0.5379]

# 2) Reproducible Monte Carlo — bit-exact rerun
mc = SimulationOptions.mc_seeded(iterations=20_000, seed=42)
a = calculate_equity([hole_a, hole_b], 0, mc)
b = calculate_equity([hole_a, hole_b], 0, mc)
print(a.players[0].equity == b.players[0].equity)  # True

# 3) Multiway — up to 10 players for Hold'em (32 for PLO), same call
hole_c = cards_to_mask([Card.from_string("7h"), Card.from_string("6h")])
r3 = calculate_equity([hole_a, hole_b, hole_c], 0, SimulationOptions.exact())
print([round(p.equity, 4) for p in r3.players], round(r3.chop_rate, 4))
# [0.3779, 0.3957, 0.2263] 0.0015

# 4) PLO equity — same API, game=
plo_a = cards_to_mask([Card.from_string(s) for s in ("As", "Ks", "Qd", "Jd")])
plo_b = cards_to_mask([Card.from_string(s) for s in ("2c", "7c", "Jh", "9d")])
plo_r = calculate_equity([plo_a, plo_b], 0,
                          SimulationOptions.mc_seeded(50_000, 7), game=GameType.Plo)
print([round(p.equity, 4) for p in plo_r.players])  # [0.6371, 0.3629]
```

### Ranges — `Range.from_string`, set algebra, range-vs-range equity

```python
from xiapl.range import Range, rank_starting_hands, generate_top_percent_range
from xiapl.simulation import calculate_range_equity, SimulationOptions, GameType

# Standard notation, weighted combos, and Range.all(game=)
hero = Range.from_string("JJ+, AKs, AKo")
print(hero.size(), hero.total_weight())  # 40 40.0

full_holdem = Range.all(game=GameType.Holdem)
print(full_holdem.size())  # 1326  (all combos, weight 1.0 each)

full_plo = Range.all(game=GameType.Plo)
print(full_plo.size())  # 270725

# Range vs range equity — per-combo (sorted by input order) + weighted aggregates
h, v = Range.from_string("AA"), Range.from_string("KK")
res = calculate_range_equity(h, v, 0, SimulationOptions.exact())
print(round(res.hero_aggregate_equity, 4), round(res.villain_aggregate_equity, 4))
# 0.8195 0.1805
print(res.hero[0].combo_mask, round(res.hero[0].equity, 4))  # first hero combo

# Weighted set algebra: union w=max(wa,wb), intersection w=min(wa,wb),
# difference w=max(0, wa-wb) — both operators and named methods
a = Range.from_string("AA, KK")
b = Range.from_string("KK, QQ")
print((a | b).size(), (a & b).size(), (a - b).size())  # 18 6 6
assert (a | b).size() == a.union(b).size()

# Exact top-% starting-hand ranking (no sampling noise)
print(rank_starting_hands(0.10))
# ['AA', 'KK', 'QQ', 'JJ', 'TT', '99', '88', 'AKs', '77', 'AQs', 'AJs',
#  'AKo', 'ATs', 'AQo', 'AJo', 'KQs']
top10 = generate_top_percent_range(0.10)
print(top10.size(), top10.total_weight())  # 104 104.0
```

Pass `mode=RangeEquityMode.AggregateOnly` to `calculate_range_equity` when
only the weighted aggregate is needed — it skips the per-combo bookkeeping
and, for Monte Carlo, uses a sampled-pair estimator instead of the
enumerate-every-pair engine (see the PLO example below). Use `PerCombo`
(the default) for the per-combo breakdown or exact reproducibility parity
with earlier calls.

Both ranges must carry the same `game` (`Range.game`) — Hold'em vs Hold'em
and PLO vs PLO are supported; a mismatch raises `ValueError`. Multi-way
`calculate_equity` accepts up to 10 players for Hold'em / 32 for PLO, but
`calculate_range_equity` is heads-up only (both Hold'em and PLO — see
[Limitations](#limitations)).

### PLO Range Notation

PLO ranges use a separate, frozen **v0.1** grammar:
`Range.from_string(text, game=GameType.Plo)` — comma-separated items, each
one of a 4-symbol rank *pattern*, an *exact 4-card hand*, or a
*progression*, with an optional `:weight` suffix.

**Pattern cheat-sheet**

| Notation | Meaning | Count |
|---|---|---:|
| `****` | any 4-card hand | 270,725 |
| `AA**` | **at least** two aces (containment — includes trip/quad aces) | 6,961 |
| `AK**` | **at least** one ace *and* one king (containment, not "exactly") | 17,316 |
| `AAKKds` | AA + KK, double-suited (exact suit histogram `{2,2}`) | 6 |
| `AAKKss` | AA + KK, single-suited (`{2,1,1}`) | 24 |
| `AAKKr` | AA + KK, rainbow (`{1,1,1,1}`) | 6 |
| `AsKsQhJd` | exact 4-card hand | 1 |
| `JJ**+` | pair-pattern progression: `JJ**, QQ**, KK**, AA**` | 27,628 |
| `JT98-8765` | closed rundown progression: `JT98, T987, 9876, 8765` | 1,024 |
| `8765-` | open (trailing `-`) downward progression: `8765, 7654, 6543, 5432` — shifts every rank down until one would leave `[2,14]` | — |
| `AKQJds:0.5` | weight suffix, applied to every combo the item expands to | — |

Matching is **multiset containment**, not exact-slot equality: `AA**` reads
as "at least two aces", not "exactly two aces plus two unconstrained cards"
— `AAKQ` and `AAAK` both match. `AK**` = 17,316 (not the smaller "exactly
one ace, exactly one king, two others" count) is the standard worked
example. The suit qualifiers `ds` / `ss` / `r` are exact suit-histogram
constraints and do **not** partition the space in general — `AKQJds +
AKQJss + AKQJr` covers 204 of `AKQJ`'s 256 hands; the missing 52 (monotone +
three-one) have no v0.1 qualifier.

Items are unioned; a combo reached by two items with equal parsed weight
merges silently, conflicting weights raise `ValueError` naming both items
and both weights. A well-formed pattern that no hand can satisfy is **not**
an error — it expands to zero combos, so check `.size()` / `.empty()` if
that matters (e.g. `"AAAKds"`: three aces need three suits, which rules out
the double-suited `{2,2}` histogram):

```python
from xiapl.range import Range
from xiapl.simulation import GameType

z = Range.from_string("AAAKds", game=GameType.Plo)
print(z.size(), z.empty())  # 0 True
```

**Four teaching errors** (the parser's messages spell these out):

| Input | Why it's rejected | Fix |
|---|---|---|
| `"AA"` | 2 rank symbols — that's Hold'em notation; a PLO pattern needs exactly 4 | Pad with wildcards: `"AA**"` |
| `"AAxx"` | `x` is not part of the v0.1 alphabet | Use `*` for "any rank": `"AA**"` |
| `"AKs**"` | a suit letter after only 3 rank symbols isn't a valid `ds`/`ss`/`r` suffix position | Use a suffix after exactly 4 rank symbols (`"AKQJds"`) or an exact hand (`"AsKsQhJd"`) |
| `"15%"` (e.g. a top-15% percentile range) | `%` percentile ranges are not supported in v0.1 — there is no ranking table to define "top X%" against | Spell the hands out as explicit patterns instead (e.g. `"AA**, AKQJds, KQJTds"`); percentile ranges are planned once a versioned PLO hand-strength ordering exists to make "top X%" well-defined |

```pycon
>>> Range.from_string("AA", game=GameType.Plo)
ValueError: PLO range item "AA": "AA" has 2 rank symbol(s); a rank pattern
is exactly 4 symbols; pad the unknown cards with '*' (e.g. "AA" -> "AA**")
```

**Equity example** — `AggregateOnly` Monte Carlo is the primary PLO equity
path for wide ranges (exact mode's evaluation cache fills faster on 4-card
combos than on 2-card ones — see [Limitations](#limitations)):

```python
from xiapl.range import Range
from xiapl.simulation import calculate_range_equity, SimulationOptions, GameType, RangeEquityMode

hero    = Range.from_string("AAKKds", game=GameType.Plo)        # 6 combos
villain = Range.from_string("QQJJds,JT98ds", game=GameType.Plo) # 42 combos

mc = SimulationOptions.mc_seeded(200_000, 7)
agg = calculate_range_equity(hero, villain, 0, mc, RangeEquityMode.AggregateOnly)
print(round(agg.hero_aggregate_equity, 4), round(agg.aggregate_std_error, 5))
# 0.6442 0.00107
print(len(agg.hero))  # 0 -- per-combo breakdown is not computed in AggregateOnly
```

### PHH — reading real hand histories

`xiapl.phh` is a pure-Python addition for the Poker Hand History format. It
reads and writes the two variant codes the rest of the library models —
`NT` (no-limit hold'em) and `PO` (pot-limit Omaha) — and rejects the other
nine with a dedicated `UnsupportedVariantError`; more variants follow demand.
Parsing a real transcribed hand and feeding its hole/board masks straight
into `calculate_equity` (the "Ivey vs Dwan" televised million-dollar pot):

```python
from xiapl import phh
from xiapl.simulation import calculate_equity, SimulationOptions

hand = phh.read_phh("tests/fixtures/phh/dwan-ivey-2009.phh")
print(hand.variant, hand.players)
# NT ['Phil Ivey', 'Patrik Antonius', 'Tom Dwan']

ivey_hole = hand.hole_mask(1)               # player 1 = Ivey
dwan_hole = hand.hole_mask(3)                # player 3 = Dwan
flop = hand.board_mask(max_cards=3)          # board as dealt through the flop

r = calculate_equity([ivey_hole, dwan_hole], flop, SimulationOptions.exact())
print([round(p.equity, 4) for p in r.players])  # [0.6283, 0.3717]
```

`format_phh` / `write_phh` serialize a `PhhHand` back to PHH text or a file
(the writer guarantees well-formed PHH syntax, not game-legal action
sequences — see [docs/phh.md](https://github.com/Xiang-Detian/XiaPL/blob/main/docs/phh.md) for the full format
documentation: supported variant subset, data model, action grammar, writer
contract, and spec divergences).

The reader and writer are audited against the whole public
[`phh-dataset`](https://github.com/uoftcprg/phh-dataset) corpus — **31,870
files / 21,616,107 hands** of real online cash-game logs, all 10,000
Pluribus hands, and a televised 2023 WSOP final table. Every file parses,
every hand survives a semantic round-trip through `format_phh` unchanged,
and the 68 files in variants outside the supported subset are rejected with
`UnsupportedVariantError` rather than failing some other way
([details](https://github.com/Xiang-Detian/XiaPL/blob/main/docs/phh.md#full-corpus-validation)).

---

## Determinism & threads

`SimulationOptions` has four fields: `iterations` (`0` = exact enumeration,
`> 0` = Monte Carlo), `seed`, `deterministic`, and `threads`. The factory
methods set the sensible combinations: `.exact()`, `.mc_random(n)`
(non-reproducible), `.mc_seeded(n, seed)` (`deterministic=True` + the given
seed).

`threads` is a pure speed knob — it never changes results on any equity
path. `0` (the default) auto-parallelizes once a workload is large enough;
`1` is serial; a positive `N` runs exactly `N` workers. On every Monte Carlo
sampling path (`calculate_equity`, and `calculate_range_equity`'s
`AggregateOnly` mode), trials run in fixed 65536-trial chunks, each with its
own seed-derived RNG substream, reduced in a fixed chunk-index order — a
given seed therefore produces **bit-identical results at every `threads`
value**, serial or parallel. `calculate_range_equity`'s `PerCombo` path
parallelizes differently (splitting the evaluation cache and the hero row
loop across workers) but is bit-identical across `threads` values by the
same fixed-partition-and-reduce discipline. The one exception:
`calculate_equity`'s board enumeration in exact mode always runs
single-threaded regardless of `threads`.

`calculate_equity` and `calculate_range_equity` release the GIL for the
duration of the C++ call, so other Python threads keep running during a
long Monte Carlo call. Because the GIL is released, mutating a `Range` /
`Combo` object (e.g. `Combo.weight` via `Range.combos()`) from another
Python thread while a `calculate_range_equity` call is reading it is a data
race — don't do it.

---

## API overview

| Module | Key names |
|---|---|
| `xiapl.card` | `Card`, `Card.from_string`, `Card.from_id`, `.rank` / `.suit` / `.id` |
| `xiapl.deck` | `Deck`, `.shuffle()` / `.shuffle(seed)`, `.deal(n)`, `.deal_one()`, `.burn(n)`, `.remove_cards()`, `.reset()`, `.cards`, `.card_ids` |
| `xiapl.eval` | `HandCategory`, `HandValue`, `describe_hand`, `evaluate_cards`, `evaluate_mask`, `evaluate_hand(board, hole, *, game=)`, `judge(hole_masks, board, *, game=)` |
| `xiapl.simulation` | `GameType`, `SimulationMode`, `SimulationOptions` (`.exact()` / `.mc_random(n)` / `.mc_seeded(n, seed)`), `EquityResult`, `PlayerEquity`, `RangeEquityMode` (`PerCombo` / `AggregateOnly`), `RangeEquityResult`, `calculate_equity`, `calculate_range_equity` |
| `xiapl.range` | `Range`, `Combo`, `Range.from_string`, `Range.all(game=)`, `.combos()`, `.valid_combos(dead_mask)`, `.total_weight()`, `.union` / `.intersection` / `.difference` (`\|` / `&` / `-`), `try_parse_range`, `rank_starting_hands`, `generate_top_percent_range` |
| `xiapl.canonicalize` | `canonicalize_hero_and_board`, `canonicalize_hero_and_board_masks`, `canonicalize_board`, `canonicalize_board_mask`, `canonicalize_hand`, `canonicalize_hand_mask`, `generate_canonical_situations` |
| `xiapl.utils` | `card_to_mask`, `cards_to_mask`, `mask_to_cards`, `mask_to_ids` |
| `xiapl.phh` | `PhhHand`, `parse_phh` / `parse_phh_all`, `read_phh` / `read_phh_all`, `format_phh` / `format_phh_all`, `write_phh` / `write_phh_all`, `UnsupportedVariantError` |

Every extension module ships a hand-written `.pyi` stub under
`python/xiapl/`, verified against the compiled `_xiapl` extension with mypy
stubtest; `xiapl` installs `py.typed` so downstream `mypy`/IDE type-checking
picks them up automatically. `xiapl.phh` is the one pure-Python module (not
part of the compiled extension) and carries its annotations inline, so it
needs no stub.

---

## Pre-1.0 API notes

Breaking changes recorded here are exempt from a deprecation cycle while
XiaPL is at 0.x:

- **Starting-hand ranking is now exact**: preflop top-% ranking is backed by
  a compile-time table of exact vs-random equities (exhaustive enumeration
  over every (hero, villain, board) configuration — no RNG, no seed, no
  sampling noise), replacing an earlier Monte Carlo scoring approach. The
  label set returned for a given `top_percent` changes accordingly for the
  same input. Exposed to Python as `xiapl.range.rank_starting_hands` /
  `xiapl.range.generate_top_percent_range`, both `game` keyword-only.
- **Board-only canonicalization is now a true canonical form**:
  `canonicalize_board` / `canonicalize_board_mask` order the four suits by
  their full 13-bit rank pattern, where they previously ordered them by the
  single highest rank present in each suit. The old rule was not a canonical
  form — `Ah Kh Ad` and `Ad Kd Ah` are the same board up to relabelling, but
  both have two ace-topped suits, and the suit-index tie-break sent them to
  different representatives. It split the 22,100 flops into 1,833 classes;
  the new one produces exactly the 1,755 suit-isomorphism classes (turn:
  16,432, river: 134,459). Boards on which no two suits share their top rank
  — the large majority — canonicalize exactly as before. Any cache or table
  keyed on the old representative is still self-consistent but redundant, and
  will merge further if rebuilt.
  `canonicalize_hero_and_board` / `canonicalize_hero_and_board_masks` are
  unchanged and still use the top-rank rule.
- **`generate_canonical_situations` now enumerates the strict (hero+board)
  canonical form**, so the flop population is **1,286,792** where it used to
  be 1,420,796. Same reason as the board-only change one entry up, applied to
  the pair: the old rule ordered suits by their top *board* rank, and its
  tie-break split situations that are the same up to relabelling. 1,286,792 is
  the exact Burnside orbit count for the symmetric group on the four suits, so
  the new population has no redundancy left in it (turn: 13,960,050; river:
  123,156,254). The board halves of the result are exactly the 1,755 canonical
  flops. `canonicalize_hero_and_board` / `canonicalize_hero_and_board_masks`
  keep their old, frozen behaviour and are therefore no longer the map this
  enumeration uses; a cache keyed by the old population must be rebuilt.
- **PLO full-range enumeration order changed** from lowest-card-outermost
  lexicographic to colexicographic (ascending-mask) order, matching what
  `Range.from_string("****", game=GameType.Plo)` already produced —
  `Range.all` no longer disagrees with the parser on element order
  depending on how the caller spelled the same range. A pinned seeded MC
  stream built over `Range.all(GameType.Plo)` will change; one built over a
  parser-constructed full-range string was already in this order and is
  unaffected.
- **Python enum casing**: all four `py::enum_` bindings (`HandCategory`,
  `GameType`, `SimulationMode`, `RangeEquityMode`) dropped
  `.export_values()`, so enum members no longer leak onto the enclosing
  submodule (e.g. `xiapl.simulation.Holdem` alongside
  `xiapl.simulation.GameType.Holdem`) — always go through the enum type.
  `RangeEquityMode` is now PascalCase-canonical (`PerCombo` /
  `AggregateOnly`); the old `PER_COMBO` / `AGGREGATE_ONLY` spellings are
  kept as deprecated aliases of the same values, to be removed at 1.0.
- `Range`'s combo-size validation error now spells the offending combo as
  card names (`"Range: combo KdAs has 2 cards, expected 4 for this
  GameType"`) instead of a raw decimal mask value, matching the PLO
  parser's own diagnostic style; a mask bit outside the 52-card deck falls
  back to a `"#<decimal>"` rendering.
- **Game-unified API**: the public surface is never split by game — one
  entry point per concept takes a `game` parameter defaulting to
  `GameType.Holdem`, dispatched in the C++ library so Python sees the same
  behavior as C++. `game` is keyword-only on `evaluate_hand`, `judge`, and
  `calculate_equity` (`judge(masks, board, game=GameType.Plo)`); on
  `Range.all` / `Range.from_string` it stays positional-or-keyword. Older
  per-game C++ function pairs (`evaluate_holdem`/`evaluate_plo`, etc.) were
  deleted with no aliases, pre-publication.
- `Deck.shuffle(seed)` always reproduces the same permutation for the same
  seed, including seed `0`.

---

## Design notes

**Card ID encoding**

```
id = suit * 13 + (rank - 2)

rank: 2, 3, 4, ..., 9, T(10), J(11), Q(12), K(13), A(14)
suit: 0=clubs, 1=diamonds, 2=hearts, 3=spades
```

**52-bit masks** — cards are represented as bits in a 64-bit integer; bit
`i` corresponds to card ID `i`. This is what every mask-taking function
(`evaluate_hand`, `judge`, `calculate_equity`, `cards_to_mask`, ...) accepts
and returns, and it's what makes set operations (union, intersection,
population count) for deck manipulation and collision detection cheap.

**Iteration modes** — every simulation entry point takes an `iterations`
knob: `0` means exact full enumeration (deterministic by construction);
`> 0` means Monte Carlo sampling (pair with a fixed `seed` for
reproducibility).

**Range grammar, summarized** — Hold'em notation is the familiar
`"AKs"` / `"AKo"` / `"JJ+"` / `"TT-88"` / `"AQs+"` / `"76s-54s"` /
`"AdAh:0.5"` form, comma-separated, with an optional `:weight` suffix per
item. PLO notation (see [above](#plo-range-notation)) is a distinct,
frozen v0.1 grammar built from 4-symbol rank patterns, suit qualifiers
(`ds`/`ss`/`r`), and progressions — it is not a reuse of Hold'em notation
padded out to four cards.

---

## Using the C++ core directly

The Python package is a thin binding over `xiapl_core` (reached through the C
ABI adapter described in [The C ABI](#the-c-abi)); the C++ API mirrors
the Python surface (same type and function names — `Card`, `Deck`, `Range`,
`SimulationOptions`, `evaluate_hand`, `judge`, `calculate_equity`, ... —
modulo `::` vs `.` and PascalCase-vs-`snake_case` call conventions). If
you're building a C++ application directly on top of the core rather than
calling it from Python, this section is your entry point.

```bash
cmake -S . -B build \
  -DXIAPL_BUILD_CORE=ON \
  -DXIAPL_BUILD_TESTS=ON \
  -DXIAPL_BUILD_EXAMPLES=ON
cmake --build build -j
cmake --install build --prefix /usr/local
```

Use from an external CMake project:

```cmake
find_package(xiapl REQUIRED)
target_link_libraries(myapp PRIVATE xiapl::xiapl_core)

# C ABI / FFI adapter (see "The C ABI" below)
target_link_libraries(myffi PRIVATE xiapl::xiapl_c_api)
```

Build options:

| Option | Default | Effect |
|--------|---------|--------|
| `XIAPL_BUILD_CORE` | ON | Core library (`xiapl_core`) |
| `XIAPL_BUILD_FFI` | ON | C ABI library (`xiapl_c_api`) for FFI |
| `XIAPL_BUILD_SHARED` | OFF | Additionally build the C ABI as a shared library |
| `XIAPL_BUILD_TESTS` | OFF | Doctest-based C++ unit tests |
| `XIAPL_BUILD_EXAMPLES` | OFF | Runnable examples under `examples/` |
| `XIAPL_BUILD_BENCHMARKS` | OFF | Chrono-based microbenchmarks |

The umbrella header pulls in the whole installed surface:

```cpp
#include <xiapl/xiapl.h>
using namespace xiapl;
```

Runnable, self-contained examples for every Quick Start topic above
(cards/deck, evaluation, equity, weighted ranges, range-vs-range equity)
live under `examples/` (`evaluate_hand.cpp`, `calc_equity.cpp`,
`weighted_range.cpp`, `range_equity.cpp`), built with
`-DXIAPL_BUILD_EXAMPLES=ON`. Stable public headers install under
`${prefix}/include/xiapl/`; internals live under
`include/xiapl/detail/` and are not installed.

`benchmarks/` holds two chrono-based microbenchmarks (`bench_eval.cpp` for
raw hand evaluation, `bench_equity.cpp` for equity simulation) so the speed
claims at the top of this README are something you can measure on your own
machine rather than take on faith:

```bash
cmake -S . -B build -DXIAPL_BUILD_BENCHMARKS=ON
cmake --build build -j
./build/benchmarks/bench_eval && ./build/benchmarks/bench_equity
```

(both are compiled `-O3 -DNDEBUG` regardless of `CMAKE_BUILD_TYPE`, so the
numbers stay meaningful in a Debug configure.)

[docs/benchmarks.md](https://github.com/Xiang-Detian/XiaPL/blob/main/docs/benchmarks.md) has the published numbers: the
7-card evaluator head-to-head against PH Evaluator and OMPEval on an Apple
M1 Pro and an Intel Xeon, single-threaded, with the measurement method, the
correctness cross-check between the three libraries, and a note on which x86
compiler flags matter.

`apps/gen_preflop_rank.cpp` is the offline generator for the exact
preflop ranking table checked in at `src/core/preflop_rank_table.inc`
(the data behind `rank_starting_hands` / `generate_top_percent_range`).
The table is a committed artifact, so the generator exists for
auditability: anyone can re-derive it and byte-compare, and `--verify`
re-checks sample entries against a brute-force enumeration. It is not part
of the default build:

```bash
cmake --build build --target gen_preflop_rank
./build/gen_preflop_rank --threads 6 --verify --out src/core/preflop_rank_table.inc
```

The output is deterministic (integer counting, no RNG), so a regeneration
on any platform or thread count must reproduce the committed file exactly.

### The C ABI

XiaPL is an **hourglass**: one C++ core (`xiapl_core`), one narrow neck, many
bindings. The neck is `include/xiapl/c_api.h` — 77 `XIAPL_API` functions at
`XIAPL_C_ABI_VERSION` 4 — and it IS the contract every binding is written
against. The Python package in this repository is the first consumer: all
seven of its modules (`card`, `utils`, `eval`, `canonicalize`, `deck`,
`range`, `simulation`) are implemented against this header and nothing else,
which `tests/check_binding_includes.sh` enforces mechanically.

The C ABI is a mechanical, id/mask-level projection of the C++ public API — a
strict subset that invents no representation and performs no presentation
logic of its own (no sorting or formatting the C++ API does not already do).
That makes it the natural target for a Rust / Node / Go / WASM binding, in the
same way libraries such as SQLite or llama.cpp use their own C ABI as the one
thing every downstream binding is written against. The header's top-of-file
comment states the full frozen contract: the boundary conventions (ownership,
query-then-fill sizing, error propagation, thread contract, determinism) and
the ABI stability policy.

**Stability: unstable until 1.0.** The C ABI ships because it is the
substrate the Python binding is built on, and you are welcome to read and
use it — but during 0.x its functions and semantics may change between
releases without a deprecation cycle. `xiapl_c_abi_version()` /
`XIAPL_C_ABI_VERSION` exist so a consumer fails loudly instead of silently
when that happens. It is planned to graduate into a first-class,
stability-committed contract once the first non-Python binding ships; until
then, the Python package is the supported public surface.

It is built when `XIAPL_BUILD_FFI=ON` (the default) and exposed as
`xiapl::xiapl_c_api`. `XIAPL_BUILD_SHARED=ON` additionally builds it as a
shared library exporting the `xiapl_*` symbols and nothing else;
`tests/check_exports.sh` is the machine check for that (registered as the
`xiapl_c_api_shared_exports` ctest on macOS and Linux — the two platforms
whose link-time export filter is configured).

```c
#include <xiapl/c_api.h>

/* Once at binding init: catch a stale FFI struct declaration loudly. */
if (xiapl_c_abi_version() != XIAPL_C_ABI_VERSION) { /* refuse to run */ }

xiapl_sim_options_t options;                /* xiapl_sim_options_exact fully
                                             * populates every field, so no
                                             * {0} initializer is needed. */
if (xiapl_sim_options_exact(&options) != XIAPL_OK) { /* handle error */ }

const uint64_t holes[2] = { hero_mask, villain_mask };
xiapl_equity_summary_t summary = {0};
double equity[2] = {0};
int rc = xiapl_calculate_equity(holes, /*num_players=*/2, board_mask, &options,
                                XIAPL_GAME_HOLDEM, /*out_winrate=*/NULL, equity,
                                /*out_std_error=*/NULL, /*players_capacity=*/2,
                                &summary);
```

The C API mirrors the public equity surface: fixed-hand equity
(`xiapl_calculate_equity`), range-vs-range equity over opaque `xiapl_range_t`
handles (`xiapl_calculate_range_equity`, Hold'em and PLO alike — a range
carries its own game tag), and top-percent range generation
(`xiapl_generate_top_percent_range`), plus the card / deck / eval /
canonicalize primitives. See the header for the complete function list and
per-function contracts.

---

## Platform support

| OS | Arch | Compiler | Status |
|---|---|---|---|
| macOS | arm64 | Apple clang | Verified (local CI) |
| Linux | x86_64 / arm64 | clang / gcc | Source-portable (C++20 + portable bit-intrinsic wrappers); the core builds with clang on x86_64 and agrees with two independent evaluators over a million hands there (see [docs/benchmarks.md](https://github.com/Xiang-Detian/XiaPL/blob/main/docs/benchmarks.md)) |
| Windows | x86_64 | MSVC | Verified on real hardware (Ryzen 7 3700X, MSVC 19.51 / Visual Studio 2026, Python 3.12): `pip install .`, the full pytest suite, and ctest all pass (4/4), zero warnings |

Both `pip install .` and the CMake build compile the C++ core from source
on the host, so platform support is compiler support: all bit operations
route through portable wrappers (`ctz64`, `clz64`, `clz32`, `popcount64`)
that expand to GCC/Clang `__builtin_*`, MSVC `_BitScan*` / `__popcnt64`, or
a scalar fallback. SIMD is left to the compiler's auto-vectorizer (no
explicit NEON / SSE / AVX intrinsics). Any binary data the library produces
is little-endian, matching every supported target. On x86, building for a
baseline newer than generic `x86-64` is worth it — the flush check's
popcounts only become a single instruction from `x86-64-v2` / `-mpopcnt`
onwards ([docs/benchmarks.md](https://github.com/Xiang-Detian/XiaPL/blob/main/docs/benchmarks.md) quantifies it).

---

## Limitations

- `calculate_range_equity` with `iterations == 0` (exact mode) caps the
  per-hand evaluation cache at **~512 MB**. Wide preflop ranges (no board)
  easily exceed this — the call raises `RuntimeError` with a remediation
  hint instead of allocating multiple gigabytes silently. Use
  `iterations > 0` (Monte Carlo) for those cases. Exact mode on a flop /
  turn / river is unaffected.
- Multi-way `calculate_equity` accepts up to **10 players for Hold'em / 32
  for PLO** (`hole_masks.size()` past either raises `ValueError`), but
  `calculate_range_equity` is **heads-up only** (both Hold'em and PLO —
  multiway range-vs-range is not supported today, and is planned).
- XiaPL is 0.x / alpha: `Card`, `Deck`, `Range`, `SimulationOptions`,
  `calculate_equity`, `calculate_range_equity`, and the rest of the surface
  in the [API overview](#api-overview) are exposed today and exercised by
  the test suite, but signatures may still change before 1.0 (see
  [Pre-1.0 API notes](#pre-10-api-notes)).

---

## Testing

```bash
python -m pytest python/test/ -v
```

For contributors touching the C++ core, the corresponding C++ test suite:

```bash
cmake -S . -B build -DXIAPL_BUILD_TESTS=ON
cmake --build build --target xiapl_tests
./build/xiapl_tests
./build/xiapl_tests --test-suite-exclude=slow  # quick tier (or: ctest -LE slow)
```

`ctest` runs a little more than that binary. With
`-DXIAPL_BUILD_TESTS=ON -DXIAPL_BUILD_SHARED=ON` the full lane set is:

| ctest name | What it checks |
|---|---|
| `xiapl_tests_fast` / `xiapl_tests_slow` | the core C++ suite, split by doctest's `slow` suite |
| `xiapl_c_api_tests` | every C ABI entry point, its error classification and its ownership rules |
| `xiapl_c_api_abi_check` | that `<xiapl/c_api.h>` compiles as plain C11 under `-Wpedantic` |
| `xiapl_binding_include_purity` | that `binding/` reaches the library only through `<xiapl/c_api.h>` |
| `xiapl_c_api_shared_exports` | that the shared C ABI exports the `xiapl_*` functions and nothing else (macOS / Linux) |

The last two are shell-script guards rather than compiled test binaries, and
neither is gated on `XIAPL_BUILD_TESTS`: the include-purity check is
registered by every configure of this project, and the export check by every
configure with `XIAPL_BUILD_SHARED=ON` on a platform whose export filter is
configured. Both are skipped with a status message if no `bash` is found.
They guard architectural properties that have no compile-time enforcement of
their own.

---

## License

MIT — see [LICENSE](https://github.com/Xiang-Detian/XiaPL/blob/main/LICENSE) for details.
