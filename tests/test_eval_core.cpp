#include "doctest.h"
#include <xiapl/card.h>
#include <xiapl/eval.h>
#include <xiapl/hand_value.h>
#include <xiapl/utils.h>
#include "../src/core/eval_core.h"
#include <cstdint>
#include <initializer_list>
#include <sstream>
#include <string>

// Tests for the packed-score evaluator itself (src/core/eval_core.h).
//
// evaluate_mask() is now a thin decode_score(eval_score7()) wrapper, so
// "decode == evaluate_mask" would be a tautology and is deliberately NOT
// tested here. What is testable without restating the implementation:
//   1. decoded HandValues against hand-written expected values
//   2. that raw uint32 score ordering agrees with HandValue ordering
// The exhaustive cross-check against an independent reference evaluator
// lives in tests/test_eval_exhaustive.cpp (gates A / A6 / B / B6 / C).

using namespace xiapl;

namespace {

std::uint64_t mk(std::initializer_list<const char*> cs) {
  std::uint64_t m = 0;
  for (auto s : cs) m |= 1ULL << Card::from_string(s).id;
  return m;
}

std::string mask_hex(std::uint64_t m) {
  std::ostringstream o;
  o << "0x" << std::hex << m;
  return o.str();
}

// Simple xorshift64 (reproducible pseudo-random numbers within the test)
struct Xorshift {
  std::uint64_t s;
  explicit Xorshift(std::uint64_t seed) : s(seed) {}
  std::uint64_t operator()() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

std::uint64_t random_mask(Xorshift& rng, int n) {
  std::uint64_t m = 0;
  int c = 0;
  while (c < n) {
    int i = static_cast<int>(rng() % 52);
    if ((m >> i) & 1) continue;
    m |= 1ULL << i;
    ++c;
  }
  return m;
}

// Hand-written expectation: category, kicker_count and all 5 kicker slots
// (slots past kicker_count must be zero-filled). Ranks are 2..14, A = 14.
struct KnownHand {
  const char* label;
  std::uint64_t mask;
  HandCategory category;
  int kicker_count;
  std::uint8_t kickers[5];
};

void check_known_hand(const KnownHand& kh, const HandValue& hv,
                      const char* entry_point) {
  // std::string, not const char*: doctest stringifies a raw pointer as an address.
  INFO("hand = ", std::string(kh.label), " entry = ", std::string(entry_point),
       " mask = ", mask_hex(kh.mask), " got = ", describe_hand(hv));
  CHECK(hv.category == kh.category);
  CHECK(static_cast<int>(hv.kicker_count) == kh.kicker_count);
  for (int i = 0; i < 5; ++i) {
    CHECK(static_cast<int>(hv.kickers[static_cast<std::size_t>(i)]) ==
          static_cast<int>(kh.kickers[i]));
  }
}

}  // namespace

// One representative hand per category, checked against hand-written expected
// values (independent of the evaluator's own code path).
TEST_CASE("eval_core decode matches hand-written expectations") {
  const KnownHand hands[] = {
    {"royal flush",
     mk({"Ah","Kh","Qh","Jh","Th","2c","3d"}),
     HandCategory::StraightFlush, 1, {14, 0, 0, 0, 0}},
    {"steel wheel (A-5 straight flush)",
     mk({"Ah","2h","3h","4h","5h","Kc","Qd"}),
     HandCategory::StraightFlush, 1, {5, 0, 0, 0, 0}},
    {"quad 8s with ace kicker",
     mk({"8c","8d","8h","8s","2c","Ah","3d"}),
     HandCategory::FourOfAKind, 2, {8, 14, 0, 0, 0}},
    {"double trips -> nines full of fives",
     mk({"9c","9d","9h","5c","5d","5h","Ac"}),
     HandCategory::FullHouse, 2, {9, 5, 0, 0, 0}},
    {"seven-card flush (top 5 hearts)",
     mk({"2h","4h","6h","8h","Th","Qh","Ah"}),
     HandCategory::Flush, 5, {14, 12, 10, 8, 6}},
    {"wheel straight (mixed suits)",
     mk({"Ah","2c","3h","4d","5s","Kc","Qd"}),
     HandCategory::Straight, 1, {5, 0, 0, 0, 0}},
    {"aces and kings with queen kicker",
     mk({"Ac","Ad","Kc","Kd","Qc","Jh","9s"}),
     HandCategory::TwoPair, 3, {14, 13, 12, 0, 0}},
    {"pair of aces, K/Q/J kickers",
     mk({"Ac","Ad","Kc","Qd","Jc","9h","7s"}),
     HandCategory::OnePair, 4, {14, 13, 12, 11, 0}},
    {"ace-high no pair",
     mk({"Ac","Kd","Qc","Jd","9c","7h","5s"}),
     HandCategory::HighCard, 5, {14, 13, 12, 11, 9}},
    {"5 cards: aces full of kings",
     mk({"Ac","Ad","Ah","Kc","Kd"}),
     HandCategory::FullHouse, 2, {14, 13, 0, 0, 0}},
    {"6 cards: trip sevens, 4/3 kickers",
     mk({"7c","7d","7h","2c","3d","4h"}),
     HandCategory::ThreeOfAKind, 3, {7, 4, 3, 0, 0}},
  };
  for (const auto& kh : hands) {
    // Both entry points are checked against the same hand-written expectation:
    // the internal packed-score decode and the public API.
    check_known_hand(kh, internal::decode_score(internal::eval_score7(kh.mask)),
                     "decode_score(eval_score7)");
    check_known_hand(kh, evaluate_mask(kh.mask), "evaluate_mask");
  }
}

// The integer ordering of score matches HandValue's ordering (operator< / operator==)
TEST_CASE("eval_core score ordering matches HandValue ordering (random 7-card pairs)") {
  Xorshift rng(42);
  long long bad_lt = 0, bad_eq = 0;
  std::uint64_t first_bad_a = 0, first_bad_b = 0;
  for (int t = 0; t < 100000; ++t) {
    const std::uint64_t a = random_mask(rng, 7);
    const std::uint64_t b = random_mask(rng, 7);
    const HandValue ha = evaluate_mask(a), hb = evaluate_mask(b);
    const std::uint32_t sa = internal::eval_score7(a), sb = internal::eval_score7(b);
    bool ok = true;
    if ((ha < hb) != (sa < sb)) { ++bad_lt; ok = false; }
    if ((ha == hb) != (sa == sb)) { ++bad_eq; ok = false; }
    if (!ok && first_bad_a == 0) { first_bad_a = a; first_bad_b = b; }
  }
  INFO("first mismatch: a = ", mask_hex(first_bad_a), " b = ", mask_hex(first_bad_b));
  CHECK(bad_lt == 0);
  CHECK(bad_eq == 0);
}
