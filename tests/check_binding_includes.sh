#!/bin/bash
set -euo pipefail

# Include-purity check for the waist v1 Python binding (binding/).
#
# The point of the waist track is that the public half of the binding talks to
# the library ONLY through the C ABI. That is an architectural property with no
# compile-time enforcement of its own: adding `#include <xiapl/range.h>` to
# binding/core_range.cpp would build and pass every behavioural test while
# silently undoing the decoupling. This script is the machine check.
#
# Two rules, matching the two halves of binding/:
#
#   1. Every binding/core_*.h and binding/core_*.cpp -- the modules that
#      implement the Python surface (card, utils, eval, canonicalize, deck,
#      range, simulation) -- may include ONLY <xiapl/c_api.h>, pybind11
#      headers, binding-local headers and the standard library. Any other
#      project header is a violation.
#
#   2. binding/bindings.cpp is the module ROOT: it owns PYBIND11_MODULE, the
#      exception translator and the registration ORDER, and delegates every
#      module to a core_*.cpp. It must not drift back into including a core
#      header for one of them, so its <xiapl/...> includes are checked against
#      an explicit allowlist.
#
# Builds nothing and takes no arguments: it reads the sources. $1 may give the
# repository root (CMake passes ${CMAKE_SOURCE_DIR}); it defaults to this
# script's parent directory.
#
# Usage: check_binding_includes.sh [repo-root]

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
BINDING_DIR="$ROOT/binding"

if [ ! -d "$BINDING_DIR" ]; then
  echo "check_binding_includes: no such directory: $BINDING_DIR" >&2
  exit 2
fi

# The ONE waist header the public half may reach for.
WAIST_HEADER="xiapl/c_api.h"

# <xiapl/...> headers binding/bindings.cpp may include:
#   version.h  the module's own __version__ attribute
#   c_api.h    the waist header itself (currently reached transitively via
#              core_common.h rather than included directly, but it is never a
#              violation either way) -- the one xiapl/ include that should
#              always be allowed.
# Adding a module's C++ header here would be a decision to un-switch that
# module off the waist, so this list is meant to be argued about, not extended
# in passing.
BINDINGS_ALLOWED_XIAPL="version.h c_api.h"

VIOLATIONS=0
CHECKED=0

# Emits every include target in a file, one per line, with the surrounding
# <> or "" stripped. Commented-out lines are skipped the same way the
# preprocessor would not skip them -- deliberately: a `// #include <xiapl/x.h>`
# is not a violation, but neither is it worth special-casing, so only lines
# whose first non-blank token is #include are considered.
includes_of() {
  grep -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]' "$1" \
    | sed -E 's|^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]([^>"]+)[>"].*|\1|'
}

report() {
  echo "check_binding_includes: $1" >&2
  VIOLATIONS=$((VIOLATIONS + 1))
}

# ---- Rule 1: the public, waist-consuming half --------------------------------
# Globbed rather than hardcoding core_common.h by name, so a future core_*.h
# (a second binding-local header alongside core_common.h) is picked up
# automatically instead of silently going unchecked.
PUBLIC_FILES=()
while IFS= read -r file; do
  [ -n "$file" ] && PUBLIC_FILES+=("$file")
done < <(find "$BINDING_DIR" -maxdepth 1 \( -name 'core_*.h' -o -name 'core_*.cpp' \) | sort)

if [ "${#PUBLIC_FILES[@]}" -eq 0 ]; then
  echo "check_binding_includes: found no binding/core_* sources to check" >&2
  exit 1
fi

for file in "${PUBLIC_FILES[@]}"; do
  CHECKED=$((CHECKED + 1))
  while IFS= read -r target; do
    [ -z "$target" ] && continue
    case "$target" in
      "$WAIST_HEADER")  continue ;;   # the waist itself
      pybind11/*)       continue ;;   # the binding framework
      */*)              ;;            # any other path: fall through to the error
      *)                continue ;;   # no slash: a std header or a
                                      # binding-local one (core_common.h). This
                                      # is a residual gap by construction: an
                                      # unqualified #include "foo.h" carries no
                                      # "xiapl/" text to match against, so a
                                      # forbidden core header made reachable
                                      # this way (e.g. by adding its directory
                                      # to the include search path) would not
                                      # be caught here -- safety for this case
                                      # currently rests entirely on the
                                      # target_include_directories layout
                                      # (only include/, never include/xiapl/,
                                      # is ever on the search path).
    esac
    report "$(basename "$file") includes <$target>; the public half of the binding may include only <$WAIST_HEADER>, pybind11 headers and the standard library"
  done < <(includes_of "$file")
done

# ---- Rule 2: the module root -------------------------------------------------
BINDINGS_CPP="$BINDING_DIR/bindings.cpp"
if [ ! -f "$BINDINGS_CPP" ]; then
  echo "check_binding_includes: no such file: $BINDINGS_CPP" >&2
  exit 1
fi
CHECKED=$((CHECKED + 1))
while IFS= read -r target; do
  case "$target" in
    # Matches "xiapl/" ANYWHERE in the include path, not just a leading
    # anchor: a leading-only match would miss a path-spelled evasion like
    # "../include/xiapl/range.h" or "detail/../xiapl/range.h", which still
    # resolves to the same forbidden header via the include search path.
    *xiapl/*) ;;
    *) continue ;;
  esac
  # Strip everything up to and including the LAST "xiapl/" segment, so
  # `header` is the same bare "range.h"-style name regardless of how much
  # prefix leads up to it.
  header="${target##*xiapl/}"
  allowed=0
  for candidate in $BINDINGS_ALLOWED_XIAPL; do
    if [ "$header" = "$candidate" ]; then allowed=1; break; fi
  done
  if [ "$allowed" -eq 0 ]; then
    report "bindings.cpp includes <$target>, which is not on the module-root allowlist ($BINDINGS_ALLOWED_XIAPL); a module's C++ header here means that module is no longer going through the waist"
  fi
done < <(includes_of "$BINDINGS_CPP")

if [ "$VIOLATIONS" -ne 0 ]; then
  echo "check_binding_includes: FAIL -- $VIOLATIONS violation(s)" >&2
  exit 1
fi

echo "check_binding_includes: PASS -- $CHECKED file(s), include purity holds"
