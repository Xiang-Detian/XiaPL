#!/bin/bash
set -euo pipefail

# Export surface check for the waist v1 shared library (XIAPL_BUILD_SHARED=ON).
#
# Builds nothing itself: takes the path to the built libxiapl_c_api shared
# object as $1 and asserts that its DEFINED, externally visible symbols are
# exactly the xiapl_* C ABI. CMakeLists.txt's xiapl_c_api_shared target
# enforces this two ways -- -fvisibility=hidden at compile time (via the
# XIAPL_API macro in <xiapl/c_api.h>) for this target's OWN object files, and a
# link-time export filter for the whole library, which is the one that actually
# matters here because xiapl_core (statically linked in) keeps its own default
# visibility. This script is the machine check that both together actually
# worked -- a library that also exported xiapl_core's C++ internals would
# collide with other C++ runtimes loaded into the same process (e.g. a
# Node/Dart host).
#
# Two checks, negative and positive:
#   1. No symbol outside the `xiapl_*` prefix is exported (catches a leaked
#      xiapl_core internal or a third-party symbol).
#   2. The exported SET equals the header's own `XIAPL_API`-declared function
#      set (catches the opposite failure: a waist function that got compiled
#      out, `#ifdef`-guarded off, or dropped from the export list -- (1) alone
#      cannot see a function that is simply MISSING). Check 2 assumes each
#      `^XIAPL_API` line declares exactly one function (true today: no line
#      declares two), and that the library exports no `xiapl_*` symbol that
#      ISN'T one of those functions (also true today: the header defines no
#      exported non-function xiapl_* symbol) -- if either assumption stops
#      holding, the set comparison needs rework.
#
# Both failures print the offending symbol names, not just counts: on Linux the
# first execution of this check is expected to be a CI run on a machine nobody
# can attach a debugger to, so the message has to be the whole diagnosis.
#
# Portable across the two platforms whose export filter CMakeLists.txt
# configures (it registers the ctest on exactly those):
#
#   macOS  -- BSD `nm -gU` lists global (-g) DEFINED (-U, i.e. not undefined)
#             symbols. The Mach-O ABI prefixes every C symbol with an extra
#             leading underscore, so `xiapl_foo` appears as `_xiapl_foo`.
#             Export filter: ld64 -exported_symbols_list.
#   Linux  -- GNU/LLVM nm, where `-U` has meant different things across
#             binutils releases, so only the unambiguous long options are used:
#             `--dynamic` (the .dynsym table, i.e. what a loader can actually
#             bind to -- NOT the static symtab that plain `nm` reads),
#             `--extern-only`, `--defined-only`. ELF has no leading-underscore
#             prefix. Export filter: GNU ld / lld --version-script.
#
# Usage: check_exports.sh <path-to-shared-library>

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <path-to-shared-library>" >&2
  exit 2
fi

LIB="$1"

if [ ! -f "$LIB" ]; then
  echo "check_exports: no such file: $LIB" >&2
  exit 2
fi

# The waist header, located relative to this script's own path (mirrors
# check_binding_includes.sh's ROOT-from-BASH_SOURCE pattern) rather than via
# an extra CLI argument, so existing callers (the xiapl_c_api_shared_exports
# ctest included) keep working unchanged.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
C_API_HEADER="$ROOT/include/xiapl/c_api.h"

if [ ! -f "$C_API_HEADER" ]; then
  echo "check_exports: no such file: $C_API_HEADER" >&2
  exit 2
fi

UNAME_S="$(uname -s)"
case "$UNAME_S" in
  Darwin)
    NM_ARGS=(-gU)
    # Mach-O's platform underscore prefix (see header comment).
    SYM_PREFIX="_"
    ;;
  Linux)
    NM_ARGS=(--dynamic --extern-only --defined-only)
    SYM_PREFIX=""
    ;;
  *)
    echo "check_exports: unsupported platform '$UNAME_S' (this check knows macOS and Linux; CMakeLists.txt registers the ctest only on those two)" >&2
    exit 2
    ;;
esac

# Checked explicitly rather than left to `set -e`, which would abort the whole
# script with no output at all: a wrong nm flavour, a nonexistent long option
# or a file nm cannot parse must say so, not vanish.
if ! NM_RAW="$(nm "${NM_ARGS[@]}" "$LIB")"; then
  echo "check_exports: 'nm ${NM_ARGS[*]} $LIB' failed -- is $LIB a shared library for this platform ($UNAME_S), and does nm accept these options?" >&2
  exit 2
fi

# Column 3+ of nm's output is the symbol name (column 1 the address, column 2
# the one-letter type code). Some symbols (e.g. absolute) may have no address
# column, so take the LAST field rather than assuming a fixed column.
SYMBOLS="$(printf '%s\n' "$NM_RAW" | awk 'NF { print $NF }')"

if [ "$UNAME_S" = "Linux" ]; then
  # Drop the handful of symbols the LINKER itself defines in every ELF shared
  # object (crti.o's _init/_fini and ld's own section markers). They are not
  # produced by any compilation unit of ours, so they can never be the
  # xiapl_core leak this check exists to catch, and whether a `local: *;`
  # version-script node suppresses them varies by binutils version. The list is
  # CLOSED: it is not a wildcard escape hatch, and anything else showing up
  # -- including C++ runtime or libc symbols -- must fail check 1 loudly.
  SYMBOLS="$(printf '%s\n' "$SYMBOLS" \
    | grep -v -x -e '_init' -e '_fini' -e '_edata' -e '_end' -e '__bss_start' || true)"
fi

if [ -z "$SYMBOLS" ]; then
  echo "check_exports: $LIB exports no defined external symbols at all" >&2
  exit 1
fi

BAD="$(printf '%s\n' "$SYMBOLS" | grep -v "^${SYM_PREFIX}xiapl_" || true)"

if [ -n "$BAD" ]; then
  echo "check_exports: FAIL -- $LIB exports $(printf '%s\n' "$BAD" | wc -l | tr -d ' ') symbol(s) outside the xiapl_* ABI:" >&2
  printf '%s\n' "$BAD" | sed 's/^/  /' >&2
  echo "check_exports: the link-time export filter (cmake/xiapl_c_api_exports.txt on macOS, cmake/xiapl_c_api_exports.map on Linux) did not take effect, or does not cover these" >&2
  exit 1
fi

# Every symbol is known to carry the platform prefix now, so stripping it
# yields plain ABI names comparable against the header on both platforms.
# (With SYM_PREFIX empty this sed is a no-op, which is exactly right for ELF.)
EXPORTED="$(printf '%s\n' "$SYMBOLS" | sed "s/^${SYM_PREFIX}//" | sort -u)"

# First `xiapl_<name>(` on each ^XIAPL_API line is the declared function name;
# awk's match() is leftmost, so a return type or parameter that also starts
# with xiapl_ (e.g. `xiapl_deck_t*`) cannot win -- it is not followed by `(`.
DECLARED="$(awk '/^XIAPL_API/ {
                   if (match($0, /xiapl_[A-Za-z0-9_]*[ \t]*\(/)) {
                     s = substr($0, RSTART, RLENGTH)
                     sub(/[ \t]*\($/, "", s)
                     print s
                   }
                 }' "$C_API_HEADER" | sort -u)"

EXTRA="$(comm -23 <(printf '%s\n' "$EXPORTED") <(printf '%s\n' "$DECLARED"))"
MISSING="$(comm -13 <(printf '%s\n' "$EXPORTED") <(printf '%s\n' "$DECLARED"))"

if [ -n "$EXTRA" ] || [ -n "$MISSING" ]; then
  echo "check_exports: FAIL -- $LIB's export set does not match the $(printf '%s\n' "$DECLARED" | wc -l | tr -d ' ') XIAPL_API function(s) declared in $C_API_HEADER" >&2
  if [ -n "$EXTRA" ]; then
    echo "  exported but NOT declared in the header ($(printf '%s\n' "$EXTRA" | wc -l | tr -d ' ')):" >&2
    printf '%s\n' "$EXTRA" | sed 's/^/    /' >&2
  fi
  if [ -n "$MISSING" ]; then
    echo "  declared in the header but NOT exported ($(printf '%s\n' "$MISSING" | wc -l | tr -d ' ')):" >&2
    printf '%s\n' "$MISSING" | sed 's/^/    /' >&2
  fi
  exit 1
fi

echo "check_exports: PASS -- $(printf '%s\n' "$EXPORTED" | wc -l | tr -d ' ') exported symbol(s) on $UNAME_S, all ${SYM_PREFIX}xiapl_*, matching the header's XIAPL_API declarations exactly"
