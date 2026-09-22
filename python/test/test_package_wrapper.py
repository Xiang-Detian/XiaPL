"""Regression tests for the python/xiapl/ package wrapper around the compiled
xiapl._xiapl extension (module rename xiapl -> xiapl._xiapl, __init__.py sys.modules
registration, __version__ sourced from XIAPL_VERSION_STRING).
"""

import pickle
import re
import subprocess
import sys
from pathlib import Path

import pytest

import xiapl
from xiapl.card import Card
from xiapl.deck import Deck


def test_version_matches_cpp_single_source_of_truth():
    """xiapl.__version__ is read from _xiapl.__version__ (bound from
    XIAPL_VERSION_STRING in include/xiapl/version.h)."""
    assert xiapl.__version__ == "0.1.0"


def test_version_matches_pyproject_toml():
    """Guard against version drift between pyproject.toml and the compiled
    extension's XIAPL_VERSION_STRING (surfaced as xiapl.__version__).

    Skipped when pyproject.toml is absent (e.g. running the installed-wheel
    test suite outside the source checkout, where only the package itself
    ships).

    Bump checklist — version literals to update together:
      1. include/xiapl/version.h  (XIAPL_VERSION_MAJOR/MINOR/PATCH, XIAPL_VERSION_STRING)
      2. pyproject.toml         (project.version)
      3. CMakeLists.txt         (project(xiapl VERSION ...))
      4. README.md              (top-of-file "**Version X.Y.Z**" badge line)
    """
    pyproject_path = Path(__file__).resolve().parents[2] / "pyproject.toml"
    if not pyproject_path.is_file():
        pytest.skip("pyproject.toml not found (installed-wheel context)")

    match = re.search(r'(?m)^version\s*=\s*"([^"]+)"', pyproject_path.read_text())
    assert match is not None, 'pyproject.toml missing a `version = "..."` line'
    assert match.group(1) == xiapl.__version__


def test_all_submodules_registered_in_sys_modules():
    """Every name in xiapl._SUBMODULES must be both an attribute of the
    package and a sys.modules entry, so `import xiapl.<sub>` and
    `from xiapl.<sub> import X` both work."""
    for name in xiapl._SUBMODULES:
        mod = getattr(xiapl, name)
        assert sys.modules[f"xiapl.{name}"] is mod


def test_module_syntax_import_matches_attribute():
    """`import xiapl.simulation` (module syntax, not from-import) must yield
    the same object as the attribute set by the package __init__."""
    import xiapl.simulation

    assert xiapl.simulation is sys.modules["xiapl.simulation"]


def test_from_import_still_works():
    """from xiapl.card import Card must keep working unchanged post-rename."""
    card = Card(9, "h")
    assert str(card) == "9h"


def test_card_pickle_survives_module_rename():
    """The __reduce__ callable is the live Card type object; pickling it
    relies on Card.__module__ resolving to an importable path. Pin the
    current qualified name so a future rename is caught here rather than
    surfacing as an opaque PicklingError."""
    assert Card.__module__ == "xiapl._xiapl.card"
    original = Card(9, "h")
    loaded = pickle.loads(pickle.dumps(original))
    assert original == loaded


def test_deck_pickle_survives_module_rename():
    assert Deck.__module__ == "xiapl._xiapl.deck"
    deck = Deck()
    loaded = pickle.loads(pickle.dumps(deck))
    assert len(loaded.cards) == len(deck.cards)


def test_fresh_interpreter_import_matrix():
    """Spawn a brand-new interpreter (empty sys.modules) and re-run the
    package's core import surface, since sys.modules registration only
    happens as a side effect of xiapl/__init__.py running once.

    The child runs from the directory that *contains* the xiapl package this
    process imported, so it resolves the same copy whether that is an in-tree
    build (python/) or an installed distribution (site-packages/)."""
    script = (
        "import xiapl, sys\n"
        "assert xiapl.__version__ == '0.1.0', xiapl.__version__\n"
        "import xiapl.simulation\n"
        "from xiapl.card import Card\n"
        "assert sys.modules['xiapl.simulation'] is xiapl.simulation\n"
        "print('wrapper ok')\n"
    )
    result = subprocess.run(
        [sys.executable, "-c", script],
        cwd=Path(xiapl.__file__).resolve().parent.parent,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert result.returncode == 0, result.stderr
    assert "wrapper ok" in result.stdout


def test_type_stubs_ship_next_to_the_package():
    """py.typed (PEP 561 marker) and the hand-written .pyi stubs (Task 2:
    stub layout + stubtest gate) must ship inside the installed package
    directory, not just in the source tree, or downstream mypy/IDE users get
    no type information."""
    package_dir = Path(xiapl.__file__).parent

    assert (package_dir / "py.typed").is_file()
    assert (package_dir / "simulation.pyi").is_file()

    # Full stub set: __init__ + one .pyi per submodule registered in
    # xiapl._SUBMODULES.
    assert (package_dir / "__init__.pyi").is_file()
    for name in xiapl._SUBMODULES:
        assert (package_dir / f"{name}.pyi").is_file()
