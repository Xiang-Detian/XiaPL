#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

namespace xiapl {
namespace internal {

// Environment lookup with a pre-rename fallback.
//
// Every library environment variable was renamed XPL_* -> XIAPL_* together
// with the library itself. Existing shells, run scripts and job files still
// export the old spelling, so a lookup that only honoured the new name would
// silently change the configuration of any such run (an unseen
// XPL_NUM_THREADS, for instance, would quietly fall back to the default
// thread count). Reading the legacy name as a fallback keeps those callers
// working unchanged.
//
// Precedence: `name` (the XIAPL_* spelling) wins whenever it is set to a
// non-empty value; only then is the XPL_* spelling consulted. When both are
// exported the new name decides, so a caller can always override a stale
// environment without unsetting anything.
//
// An empty value counts as unset, matching the pre-existing convention at
// every call site (an exported-but-empty path was never a usable path).
// Returns nullptr when neither spelling carries a value. The returned pointer
// is owned by the environment block, exactly as std::getenv's is.
inline const char* getenv_compat(const char* name) {
  if (const char* v = std::getenv(name)) {
    if (*v) return v;
  }
  static const char kPrefix[] = "XIAPL_";
  const std::size_t prefix_len = sizeof(kPrefix) - 1;
  if (std::strncmp(name, kPrefix, prefix_len) != 0) return nullptr;
  const std::string legacy = std::string("XPL_") + (name + prefix_len);
  if (const char* v = std::getenv(legacy.c_str())) {
    if (*v) return v;
  }
  return nullptr;
}

// True when `name` resolved through the legacy XPL_* spelling, i.e. the new
// name is unset and the old one carries the value. For call sites that want
// to point the operator at the current spelling.
inline bool getenv_used_legacy(const char* name) {
  if (const char* v = std::getenv(name)) {
    if (*v) return false;
  }
  return getenv_compat(name) != nullptr;
}

} // namespace internal
} // namespace xiapl
