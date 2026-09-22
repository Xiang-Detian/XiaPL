"""Type stub for the xiapl package (python/xiapl/__init__.py).

Submodules are pybind11 submodules of xiapl._xiapl, registered in sys.modules by
__init__.py so that both `import xiapl.card` and `from xiapl.card import Card`
resolve to the same object; re-exported here (redundant `as` aliases) so
`import xiapl; xiapl.card.Card` type-checks too.
"""

from xiapl import canonicalize as canonicalize
from xiapl import card as card
from xiapl import deck as deck
from xiapl import eval as eval
from xiapl import phh as phh
from xiapl import range as range
from xiapl import simulation as simulation
from xiapl import utils as utils

__version__: str

# Private (module-internal): the submodule registration list driving the
# sys.modules loop in __init__.py. Not part of the public API, but read
# directly by python/test/test_package_wrapper.py.
_SUBMODULES: tuple[str, ...]

__all__ = [
    "__version__",
    "canonicalize",
    "card",
    "deck",
    "eval",
    "phh",
    "range",
    "simulation",
    "utils",
]
