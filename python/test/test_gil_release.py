"""Test that calculate_equity releases the GIL for the duration of the C++ call.

The pybind11 binding wraps its Monte Carlo simulation calls in
`py::gil_scoped_release` so other Python threads can keep running while a
long simulation is in flight in C++. This is a regression guard for that
contract: if a future refactor accidentally dropped the release guard, a
background Python thread would be starved for the whole call instead of
making steady progress, and the assertion below would fail.
"""

import threading
import time

from xiapl.card import Card
from xiapl.simulation import GameType, SimulationOptions, calculate_equity
from xiapl.utils import cards_to_mask

# PLO (not Hold'em) preflop HU: each PLO hand evaluation enumerates
# C(4,2) * C(board,3) card combinations, so per-trial cost is far higher than
# Hold'em's single 7-card evaluation. That lets a genuine Monte Carlo run
# (trials comfortably below the ~1.09M-board auto-fallback-to-exact
# threshold for this matchup) take >= 100ms on a single thread without
# requesting an implausibly large trial count.
_HERO = cards_to_mask([Card.from_string(c) for c in ["As", "Ks", "Qs", "Js"]])
_VILLAIN = cards_to_mask([Card.from_string(c) for c in ["2c", "3h", "4d", "5c"]])
_TRIALS = 250_000


def _count_forever(counter: list[int], stop: threading.Event) -> None:
    """Increment counter[0] roughly every 1ms until `stop` is set."""
    while not stop.is_set():
        counter[0] += 1
        time.sleep(0.001)


def test_calculate_equity_releases_gil_during_mc_call():
    """A background thread must keep advancing while calculate_equity runs.

    `_TRIALS` is well under the auto-fallback threshold for this preflop PLO
    HU matchup, so this is a genuine Monte Carlo run (not an exact-enumeration
    fallback), and threads=1 keeps the C++ side single-threaded so the whole
    wall-clock duration is attributable to one blocking C++ call.
    """
    opts = SimulationOptions.mc_seeded(_TRIALS, 12345)
    opts.threads = 1

    counter = [0]
    stop = threading.Event()
    counter_thread = threading.Thread(
        target=_count_forever, args=(counter, stop), daemon=True
    )
    counter_thread.start()
    try:
        before = counter[0]
        t0 = time.perf_counter()
        result = calculate_equity([_HERO, _VILLAIN], 0, opts,
                                  game=GameType.Plo)
        elapsed = time.perf_counter() - t0
        after = counter[0]
    finally:
        stop.set()
        counter_thread.join(timeout=1.0)

    # Sanity: this must actually be the long genuine-MC call we intended,
    # not a silent auto-fallback to a cheap exact enumeration.
    assert not result.exact
    assert result.trials == _TRIALS

    advanced = after - before
    assert advanced > 10, (
        f"counter thread only advanced by {advanced} during a "
        f"{elapsed * 1000:.1f}ms calculate_equity call; the GIL may not be "
        "released for the duration of the C++ call"
    )
