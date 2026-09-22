"""Regenerate the frozen tests/fixtures/phh/*.expected.json golden files.

Run directly -- `python python/test/gen_phh_golden.py` -- not collected by
pytest. Overwrites the golden file for every fixture listed in _SOURCES
using the current xiapl.phh parser output via phh_golden.hand_to_dict.

After regenerating, human-review each `cards_text` field in the diff
against the corresponding fixture's action strings, then commit: from that
point on the golden files are the frozen regression baseline checked by
test_phh.py::test_goldens_frozen, not a live truth source.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

# python/ (for `xiapl`) and this script's own directory (for `phh_golden`),
# mirroring python/test/conftest.py's sys.path setup for pytest.
_PYTHON_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_PYTHON_DIR))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from phh_golden import hand_to_dict  # noqa: E402  (needs the sys.path insert above)

from xiapl import phh  # noqa: E402  (needs the sys.path insert above)

FIXTURES = _PYTHON_DIR.parent / "tests" / "fixtures" / "phh"

# The positive fixtures golden-tested by test_goldens_frozen. The bad/
# fixtures are excluded on purpose -- they exist to raise, not to parse.
_SOURCES = (
    "dwan-ivey-2009.phh",
    "antonius-blom-2009.phh",
    "two_hands.phhs",
    "commentary.phh",
)


def main() -> None:
    """Regenerate every golden file listed in _SOURCES."""
    for name in _SOURCES:
        src = FIXTURES / name
        if src.suffix == ".phhs":
            hands = phh.parse_phh_all(src.read_text())
        else:
            hands = [phh.parse_phh(src.read_text())]
        golden = [hand_to_dict(hand) for hand in hands]
        dest = FIXTURES / f"{name}.expected.json"
        dest.write_text(json.dumps(golden, indent=2) + "\n")
        print(f"wrote {dest}")


if __name__ == "__main__":
    main()
