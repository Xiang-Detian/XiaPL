#pragma once

// Internal error plumbing for the C ABI (waist v1). Private to src/api/.
//
// Two pieces:
//   * the thread-local slot behind xiapl_last_error_message(), and
//   * wrap(), which turns any escaping C++ exception into an xiapl_status_t
//     plus a message stored in that slot.
//
// The status classification IS the originating exception type (see "Status
// codes" in <xiapl/c_api.h>), and the message is the exception's what()
// carried through VERBATIM. Bindings surface that text as their own exception
// message, and the Python conformance gate compares it byte for byte against
// the C++ text, so no prefixing, reformatting or truncation may happen here.

#include <xiapl/c_api.h>

#include <cstdint>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace xiapl::waist {

// One slot per thread (boundary convention 8). It holds the message of the
// most recent NON-OK return ON THIS THREAD: a successful call deliberately
// leaves it alone, which is what makes xiapl_clear_last_error() meaningful --
// a caller clears the slot, makes a call, and knows any message it then reads
// belongs to that call.
//
// Declared `extern` here with the SOLE definition in waist_error.cpp -- not
// `inline thread_local` -- for two reasons:
//   1. AppleClang/ld64 (verified on this toolchain) mis-links an `inline
//      thread_local std::string` (non-trivial construction, so it needs a
//      per-TU "thread-local initialization routine" wrapper) once the
//      library is compiled with -fvisibility=hidden: the wrapper fails to
//      coalesce across translation units and ld reports "duplicate symbol
//      ... thread-local initialization routine ..." at link time. A trivial
//      thread_local (no dynamic init) or a single non-inline definition does
//      not hit this; std::string needs the former's opposite, so this file
//      takes the latter.
//   2. It also closes a would-be footgun for a future SHARED build: this
//      header is private to src/api/ and not installed, but any TU outside
//      the library that included it as `inline` would have silently gotten
//      its OWN hidden thread_local slot, disagreeing with the one
//      xiapl_last_error_message() reads. With `extern` + one definition, such
//      a TU fails to LINK instead (undefined symbol, since the definition
//      lives only in the compiled waist_error.cpp object) -- a loud failure
//      instead of a silent one. Tests link the waist's compiled objects (as
//      xiapl_c_api_tests already does), never re-define this symbol.
extern thread_local std::string g_last_error;

// Store `what` verbatim and return `status`. Never throws: if the copy itself
// fails (a bad_alloc while reporting a bad_alloc) the slot degrades to "" and
// the status code still reaches the caller.
inline std::int32_t fail(std::int32_t status, const char* what) noexcept {
    try {
        g_last_error.assign(what != nullptr ? what : "");
    } catch (...) {
        g_last_error.clear();  // noexcept, leaves the slot readable
    }
    return status;
}

// Boundary-side validation failure: the cases <xiapl/c_api.h> specifies for
// the waist itself (NULL pointer where one is required, negative count,
// out-of-range id) rather than inheriting from a C++ throw. These messages are
// the waist's own, so they name the C entry point.
inline std::int32_t invalid_argument(const char* what) noexcept {
    return fail(XIAPL_ERR_INVALID_ARGUMENT, what);
}

// Run `f` (an int32_t-returning callable) and translate anything that escapes.
// std::invalid_argument and std::out_of_range are unrelated siblings under
// std::logic_error, so the order of the two first handlers is a readability
// choice, not a correctness one; std::exception must stay last before (...).
template <class F>
inline std::int32_t wrap(F&& f) noexcept {
    try {
        return std::forward<F>(f)();
    } catch (const std::invalid_argument& e) {
        return fail(XIAPL_ERR_INVALID_ARGUMENT, e.what());
    } catch (const std::out_of_range& e) {
        return fail(XIAPL_ERR_OUT_OF_RANGE, e.what());
    } catch (const std::bad_alloc& e) {
        return fail(XIAPL_ERR_BAD_ALLOC, e.what());
    } catch (const std::exception& e) {
        return fail(XIAPL_ERR_RUNTIME, e.what());
    } catch (...) {
        // A non-std exception has no what() to carry; the class is still the
        // documented "anything else" bucket.
        return fail(XIAPL_ERR_RUNTIME, "unknown exception");
    }
}

}  // namespace xiapl::waist
