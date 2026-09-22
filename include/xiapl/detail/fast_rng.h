#pragma once

// INTERNAL header -- not installed, no stability guarantee. Public API never
// exposes RNG types; use SimulationOptions::seed / Deck::shuffle(seed).

#include <cstdint>
#include <limits>
#include <random>

namespace xiapl {

// xoshiro256++ — fast PRNG suitable for Monte Carlo simulation.
// Passes BigCrush; ~5–8x faster than std::mt19937_64 per call.
// Implements the C++ UniformRandomBitGenerator concept so it can drop in
// wherever std::mt19937_64 is currently used.
//
// Reference: https://prng.di.unimi.it/xoshiro256plusplus.c
class FastRng {
public:
    using result_type = std::uint64_t;

    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }

    FastRng() { seed(0xDEADBEEFCAFEBABEULL); }

    // NOTE: seed 0 is treated as "pick a random seed" here, so this ctor is
    // NOT suitable for callers that must honour an explicit seed == 0 as a
    // deterministic seed. Those callers should default-construct and call
    // seed(s) directly, which has no zero-guard. (The guard is kept for
    // backwards compatibility with existing call sites.)
    explicit FastRng(std::uint64_t s) { seed(s ? s : std::random_device{}()); }

    void seed(std::uint64_t s) {
        // SplitMix64 to fill the 256-bit state from a 64-bit seed.
        for (int i = 0; i < 4; ++i) {
            s += 0x9E3779B97F4A7C15ULL;
            std::uint64_t z = s;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            state_[i] = z ^ (z >> 31);
        }
    }

    result_type operator()() {
        const std::uint64_t result = rotl(state_[0] + state_[3], 23) + state_[0];
        const std::uint64_t t = state_[1] << 17;
        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = rotl(state_[3], 45);
        return result;
    }

    // Named alias of operator() for call sites that read better as next().
    result_type next() { return (*this)(); }

    // Uniform integer in [0, n) via Lemire's multiply-shift, debiased with
    // rejection so the result is exactly uniform (no modulo bias).
    // The rejection branch is taken with probability < n / 2^32, i.e. never
    // in practice for the small n used here (deck sizes <= 52).
    // n == 0 returns 0 (the rejection branch, and its division by n, is not
    // entered because `l < n` is false).
    //
    // Reference: D. Lemire, "Fast Random Integer Generation in an Interval",
    // ACM TOMACS 29(1), 2019.
    std::uint32_t bounded(std::uint32_t n) {
        std::uint32_t x = static_cast<std::uint32_t>(next());
        std::uint64_t m = static_cast<std::uint64_t>(x) * n;
        auto l = static_cast<std::uint32_t>(m);
        if (l < n) {
            const std::uint32_t t = (0u - n) % n;
            while (l < t) {
                x = static_cast<std::uint32_t>(next());
                m = static_cast<std::uint64_t>(x) * n;
                l = static_cast<std::uint32_t>(m);
            }
        }
        return static_cast<std::uint32_t>(m >> 32);
    }

    // Direct uniform double in [0, 1). Avoids std::uniform_real_distribution
    // overhead (which is significant in hot inner loops).
    double next_double() {
        // Top 53 bits → uniform double in [0, 1).
        return static_cast<double>((*this)() >> 11) * (1.0 / 9007199254740992.0);
    }

    // Diagnostic-only: fold the 256-bit internal state into a 64-bit
    // fingerprint via FNV-1a. Does not consume a draw. Useful to verify that
    // two generators replaying the same seed stay in lockstep.
    std::uint64_t state_hash() const {
        const std::uint64_t FNV_OFFSET = 1469598103934665603ULL;
        const std::uint64_t FNV_PRIME  = 1099511628211ULL;
        std::uint64_t h = FNV_OFFSET;
        for (int i = 0; i < 4; ++i) {
            std::uint64_t w = state_[i];
            for (int b = 0; b < 8; ++b) {
                h ^= static_cast<unsigned char>((w >> (8 * b)) & 0xFF);
                h *= FNV_PRIME;
            }
        }
        return h;
    }

private:
    std::uint64_t state_[4];

    static std::uint64_t rotl(std::uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }
};

} // namespace xiapl
