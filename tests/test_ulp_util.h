#pragma once

// Tests-only ULP (unit-in-the-last-place) distance helper for non-negative
// doubles.
//
// Ported from the ulp_diff() pattern established in test_equity_agreement.cpp
// (bit_cast-and-subtract distance for the HU-vs-multiway equity agreement
// gate) into a shared header so other tests can pin "close but not
// necessarily bit-identical" doubles without redefining the helper. Not
// wired into test_equity_agreement.cpp itself -- that file's own copy is
// left untouched to keep this task's diff confined to the std_error pins it
// is fixing.

#include "doctest.h"

#include <bit>
#include <cstdint>

// ULP distance between two doubles that are both guaranteed non-negative at
// the call site: for non-negative IEEE-754 doubles the raw bit pattern, read
// as an unsigned integer, is monotonic in the value, so integer subtraction
// of the bit patterns is exactly the ULP distance. Not valid across a sign
// change -- callers must only compare same-sign (here, non-negative)
// quantities.
//
// REQUIRE(a >= 0.0) enforces "non-negative-signed", not "non-negative bit
// pattern": IEEE-754 comparison treats -0.0 == 0.0, so -0.0 would pass this
// REQUIRE while its bit pattern (0x8000...) would break the monotonic-bits
// assumption above. Callers are responsible for ensuring their inputs cannot
// be -0.0 (see call sites for the argument in each case).
inline std::uint64_t ulp_distance(double a, double b) {
    REQUIRE(a >= 0.0);
    REQUIRE(b >= 0.0);
    const std::uint64_t ua = std::bit_cast<std::uint64_t>(a);
    const std::uint64_t ub = std::bit_cast<std::uint64_t>(b);
    return ua > ub ? ua - ub : ub - ua;
}
