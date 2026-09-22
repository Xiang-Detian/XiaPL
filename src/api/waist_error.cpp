// Sole definition of xiapl::waist::g_last_error (declared `extern` in
// waist_error.h). See the comment on the declaration for why this is a plain
// non-inline TU-owned definition rather than an `inline thread_local`:
// AppleClang/ld64 duplicates the per-TU "thread-local initialization
// routine" wrapper an inline non-trivial thread_local requires once the
// library is compiled with -fvisibility=hidden, and a single definition here
// sidesteps that entirely.

#include "waist_error.h"

namespace xiapl::waist {

thread_local std::string g_last_error;

}  // namespace xiapl::waist
