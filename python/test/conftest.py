# test/conftest.py
#
# Prefer an in-tree build of the extension (pip install -e ., which drops
# python/xiapl/_xiapl.*.so) so the edit-compile-test loop exercises the
# working copy. When the extension has not been built in place, leave
# sys.path alone so `pip install . && pytest python/test/` tests the
# installed distribution instead of shadowing it with a source directory
# that has no compiled module.
import os
import sys
from glob import glob

_PYTHON_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

if glob(os.path.join(_PYTHON_DIR, "xiapl", "_xiapl*.so")) or glob(
    os.path.join(_PYTHON_DIR, "xiapl", "_xiapl*.pyd")
):
    sys.path.insert(0, _PYTHON_DIR)
