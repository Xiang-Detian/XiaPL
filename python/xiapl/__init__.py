"""xiapl: fast Texas Hold'em / PLO equity and hand-evaluation library.

Thin pure-Python wrapper around the compiled ``xiapl._xiapl`` extension. The
public submodules (``xiapl.card``, ``xiapl.simulation``, ...) are the pybind11
submodules of the extension, registered in ``sys.modules`` so that both
``import xiapl.card`` and ``from xiapl.card import Card`` work unchanged.
"""
import sys as _sys

from xiapl import _xiapl as _ext

__version__ = _ext.__version__

_SUBMODULES = (
    "card",
    "deck",
    "eval",
    "simulation",
    "range",
    "utils",
    "canonicalize",
)

for _name in _SUBMODULES:
    _mod = getattr(_ext, _name)
    globals()[_name] = _mod
    _sys.modules[f"{__name__}.{_name}"] = _mod

# xiapl.phh is pure Python (not a pybind11 submodule of the compiled
# extension), so it is imported directly rather than via the _SUBMODULES
# loop above. The import itself registers it in sys.modules and as an
# attribute of this package, matching the other submodules.
from xiapl import phh

__all__ = ["__version__", *_SUBMODULES, "phh"]
