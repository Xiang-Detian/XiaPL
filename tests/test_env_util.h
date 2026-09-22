#pragma once

// Tests-only portability shim; library code never sets env vars.
//
// setenv()/unsetenv() are POSIX and are not available on MSVC. The MSVC
// equivalent is _putenv_s(), where setting an empty value string is how the
// variable gets deleted (there is no separate "unset" call).

#include <cstdlib>

#ifdef _WIN32

inline void test_set_env(const char* name, const char* value) {
    _putenv_s(name, value);
}

inline void test_unset_env(const char* name) {
    _putenv_s(name, "");
}

#else

inline void test_set_env(const char* name, const char* value) {
    ::setenv(name, value, 1);
}

inline void test_unset_env(const char* name) {
    ::unsetenv(name);
}

#endif
