#include "doctest.h"
#include <xiapl/eval.h>
#include <xiapl/hand_value.h>
#include <xiapl/utils.h>
#include "../src/core/eval_internal.h"
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <sstream>

// Whole file is exhaustive-by-design (C(52,7)=133.8M + a 20.4M-hand naive
// cross-check); tagged "slow" so the fast CTest tier skips it. Fast-tier
// evaluator coverage lives in test_eval.cpp / test_eval_core.cpp.
TEST_SUITE_BEGIN("slow");

using namespace xiapl;

namespace {

// ---- Independent reference: direct 5-card evaluation + max over C(N,5) combos (N=6,7). Deliberately naive implementation ----
// kicker_count follows the convention fixed by the evaluator's decode side
// (eval_core.h decode_score: count of non-zero kicker nibbles):
//   StraightFlush/Straight=1, FourOfAKind/FullHouse=2, ThreeOfAKind/TwoPair=3, OnePair=4, Flush/HighCard=5
//
// The two entry points are cross-checked here without overlap, since both are
// now thin wrappers over the single evaluator in eval_core.h:
//   - public evaluate_mask(): gates A / A6 (exhaustive 5-card and 6-card)
//   - internal::evaluate_holdem_fast(): gates C (random 7-card) and B / B6
//     (golden hash over the full 7-card and 6-card enumerations)
struct Ref5 { int cat; int k[5]; int kicker_count; };  // cat: 0-8 in the same order as HandCategory (HandCategory starts at 1, so to_ref() subtracts 1)

Ref5 ref_eval5(const int ids[5]) {
  int rank[5], suit[5];
  for (int i = 0; i < 5; ++i) { rank[i] = ids[i] % 13 + 2; suit[i] = ids[i] / 13; }
  int cnt[15] = {0};
  for (int i = 0; i < 5; ++i) cnt[rank[i]]++;
  bool flush = (suit[0]==suit[1] && suit[0]==suit[2] && suit[0]==suit[3] && suit[0]==suit[4]);
  int sorted[5]; std::copy(rank, rank+5, sorted); std::sort(sorted, sorted+5, std::greater<int>());
  bool straight = false; int shigh = 0;
  if (sorted[0]-sorted[4]==4 && cnt[sorted[0]]==1 && cnt[sorted[1]]==1 && cnt[sorted[2]]==1
      && cnt[sorted[3]]==1 && cnt[sorted[4]]==1) { straight = true; shigh = sorted[0]; }
  if (sorted[0]==14 && sorted[1]==5 && sorted[2]==4 && sorted[3]==3 && sorted[4]==2) {
    straight = true; shigh = 5;  // wheel
  }
  Ref5 r{}; int pos = 0;
  auto push_by_count = [&](int c) {          // push ranks with count==c into k[] in descending order
    for (int v = 14; v >= 2; --v) if (cnt[v] == c) r.k[pos++] = v;
  };
  if (straight && flush) { r.cat = 8; r.k[0] = shigh; r.kicker_count = 1; return r; }
  int four = 0, three = 0, pairs = 0;
  for (int v = 2; v <= 14; ++v) { if (cnt[v]==4) four=v; else if (cnt[v]==3) three=v; else if (cnt[v]==2) pairs++; }
  if (four)               { r.cat = 7; push_by_count(4); push_by_count(1); r.kicker_count = 2; return r; }
  if (three && pairs)     { r.cat = 6; push_by_count(3); push_by_count(2); r.k[2]=0; r.kicker_count = 2; return r; }
  if (flush)              { r.cat = 5; push_by_count(1); r.kicker_count = 5; return r; }
  if (straight)           { r.cat = 4; r.k[0] = shigh; r.kicker_count = 1; return r; }
  if (three)              { r.cat = 3; push_by_count(3); push_by_count(1); r.kicker_count = 3; return r; }
  if (pairs == 2)         { r.cat = 2; push_by_count(2); push_by_count(1); r.kicker_count = 3; return r; }
  if (pairs == 1)         { r.cat = 1; push_by_count(2); push_by_count(1); r.kicker_count = 4; return r; }
  r.cat = 0; push_by_count(1); r.kicker_count = 5; return r;
}

// Compare by cat first, then lexicographically by k[] within the same cat
// (kicker_count is uniquely determined by cat, so it needs no comparison)
bool ref_less(const Ref5& a, const Ref5& b) {
  if (a.cat != b.cat) return a.cat < b.cat;
  for (int i = 0; i < 5; ++i) if (a.k[i] != b.k[i]) return a.k[i] < b.k[i];
  return false;
}

// All combinations of choosing 5 cards out of N (dropped-index approach: we need to
// enumerate the actual "5 cards kept", not the C(N,N-1)=N ways of dropping a single
// card, so combos is kept as an explicit table)
Ref5 ref_eval_best(const int ids[], const int (*combos)[5], int num_combos) {
  int sub0[5] = {ids[combos[0][0]], ids[combos[0][1]], ids[combos[0][2]], ids[combos[0][3]], ids[combos[0][4]]};
  Ref5 best = ref_eval5(sub0);
  for (int c = 1; c < num_combos; ++c) {
    int sub[5] = {ids[combos[c][0]], ids[combos[c][1]], ids[combos[c][2]], ids[combos[c][3]], ids[combos[c][4]]};
    Ref5 v = ref_eval5(sub);
    if (ref_less(best, v)) best = v;
  }
  return best;
}

Ref5 ref_eval7(const int ids[7]) {
  static const int C[21][5] = {
    {0,1,2,3,4},{0,1,2,3,5},{0,1,2,3,6},{0,1,2,4,5},{0,1,2,4,6},{0,1,2,5,6},
    {0,1,3,4,5},{0,1,3,4,6},{0,1,3,5,6},{0,1,4,5,6},{0,2,3,4,5},{0,2,3,4,6},
    {0,2,3,5,6},{0,2,4,5,6},{0,3,4,5,6},{1,2,3,4,5},{1,2,3,4,6},{1,2,3,5,6},
    {1,2,4,5,6},{1,3,4,5,6},{2,3,4,5,6}};
  return ref_eval_best(ids, C, 21);
}

// C(6,5)=6: the 5-card combinations obtained by removing exactly 1 card from 6
Ref5 ref_eval6(const int ids[6]) {
  static const int C[6][5] = {
    {1,2,3,4,5},{0,2,3,4,5},{0,1,3,4,5},{0,1,2,4,5},{0,1,2,3,5},{0,1,2,3,4}};
  return ref_eval_best(ids, C, 6);
}

// Normalize a HandValue into Ref5 form (compare with 0 past kicker_count)
// HandCategory starts at HighCard=1, so subtract 1 to align with ref_eval5/6/7's 0-based (0-8) range.
Ref5 to_ref(const HandValue& hv) {
  Ref5 r{}; r.cat = static_cast<int>(hv.category) - 1;
  r.kicker_count = hv.kicker_count;
  for (int i = 0; i < 5; ++i) r.k[i] = (i < hv.kicker_count) ? hv.kickers[i] : 0;
  return r;
}

// Cross-check against the independent reference (checks cat / kicker_count / all of kickers[])
bool ref_mismatch(const Ref5& got, const Ref5& want) {
  return got.cat != want.cat || got.kicker_count != want.kicker_count
      || !std::equal(got.k, got.k + 5, want.k);
}

std::uint64_t fnv1a(std::uint64_t h, std::uint64_t v) {
  h ^= v; h *= 1099511628211ULL; return h;
}

// HandValue -> normalized 64-bit value (hash input). Also folds in kicker_count
// (if a rewritten implementation over-reports kicker_count while returning
//  zero-filled kickers, category+kickers[] alone would let it slip through,
//  so kicker_count is folded in at the end as well)
std::uint64_t hv_word(const HandValue& hv) {
  std::uint64_t w = static_cast<std::uint64_t>(hv.category);
  for (int i = 0; i < 5; ++i) w = (w << 8) | static_cast<std::uint64_t>(i < hv.kicker_count ? hv.kickers[i] : 0);
  w = (w << 8) | static_cast<std::uint64_t>(hv.kicker_count);
  return w;
}

} // namespace

// Gate A: full 5-card enumeration C(52,5)=2,598,960 cross-checked against the independent reference
TEST_CASE("exhaustive 5-card vs independent reference (public evaluate_mask)") {
  long long bad = 0, n = 0;
  int id[5];
  for (id[0]=0; id[0]<52; ++id[0]) for (id[1]=id[0]+1; id[1]<52; ++id[1])
  for (id[2]=id[1]+1; id[2]<52; ++id[2]) for (id[3]=id[2]+1; id[3]<52; ++id[3])
  for (id[4]=id[3]+1; id[4]<52; ++id[4]) {
    std::uint64_t m = 0; for (int i = 0; i < 5; ++i) m |= 1ULL << id[i];
    Ref5 want = ref_eval5(id);
    HandValue hv = evaluate_mask(m);
    if (ref_mismatch(to_ref(hv), want)) ++bad;
    ++n;
  }
  CHECK(n == 2598960);
  CHECK(bad == 0);
}

// Gate A6: full 6-card enumeration (equivalent to the turn street: board 4 + hole 2)
// C(52,6)=20,358,520 cross-checked against the independent reference
TEST_CASE("exhaustive 6-card vs independent reference (public evaluate_mask)") {
  long long bad = 0, n = 0;
  int id[6];
  for (id[0]=0; id[0]<52; ++id[0]) for (id[1]=id[0]+1; id[1]<52; ++id[1])
  for (id[2]=id[1]+1; id[2]<52; ++id[2]) for (id[3]=id[2]+1; id[3]<52; ++id[3])
  for (id[4]=id[3]+1; id[4]<52; ++id[4]) for (id[5]=id[4]+1; id[5]<52; ++id[5]) {
    std::uint64_t m = 0; for (int i = 0; i < 6; ++i) m |= 1ULL << id[i];
    Ref5 want = ref_eval6(id);
    HandValue hv = evaluate_mask(m);
    if (ref_mismatch(to_ref(hv), want)) ++bad;
    ++n;
  }
  CHECK(n == 20358520);
  CHECK(bad == 0);
}

// Gate B: FNV hash of the score sequence over the full 7-card enumeration
// C(52,7)=133,784,560 (internal::evaluate_holdem_fast; category+kickers[]+kicker_count)
// GOLDEN_HASH_7CARD pins the evaluator's output bit-for-bit: it must never be
// edited to match a new implementation, only re-derived from a deliberate and
// reviewed change of hand-evaluation semantics.
static constexpr std::uint64_t GOLDEN_HASH_7CARD = 0xca2a37fd5fc8b2dbULL;  // fixed from measurement (hash includes kicker_count)
TEST_CASE("exhaustive 7-card golden hash (internal::evaluate_holdem_fast)") {
  std::uint64_t h = 14695981039346656037ULL;
  long long n = 0;
  int id[7];
  for (id[0]=0; id[0]<52; ++id[0]) for (id[1]=id[0]+1; id[1]<52; ++id[1])
  for (id[2]=id[1]+1; id[2]<52; ++id[2]) for (id[3]=id[2]+1; id[3]<52; ++id[3])
  for (id[4]=id[3]+1; id[4]<52; ++id[4]) for (id[5]=id[4]+1; id[5]<52; ++id[5])
  for (id[6]=id[5]+1; id[6]<52; ++id[6]) {
    std::uint64_t m = 0; for (int i = 0; i < 7; ++i) m |= 1ULL << id[i];
    HandValue hv; internal::evaluate_holdem_fast(m, 0, hv);
    h = fnv1a(h, hv_word(hv));
    ++n;
  }
  CHECK(n == 133784560);
  std::ostringstream hex_hash;
  hex_hash << "0x" << std::hex << h;
  MESSAGE("7-card golden hash = ", hex_hash.str());
  CHECK(h == GOLDEN_HASH_7CARD);
}

// Gate B6: FNV hash of the score sequence over the full 6-card enumeration
// C(52,6)=20,358,520 (internal::evaluate_holdem_fast). Same rule as GOLDEN_HASH_7CARD.
static constexpr std::uint64_t GOLDEN_HASH_6CARD = 0x3e5e1d6aeb16f1ULL;  // fixed from measurement (hash includes kicker_count)
TEST_CASE("exhaustive 6-card golden hash (internal::evaluate_holdem_fast)") {
  std::uint64_t h = 14695981039346656037ULL;
  long long n = 0;
  int id[6];
  for (id[0]=0; id[0]<52; ++id[0]) for (id[1]=id[0]+1; id[1]<52; ++id[1])
  for (id[2]=id[1]+1; id[2]<52; ++id[2]) for (id[3]=id[2]+1; id[3]<52; ++id[3])
  for (id[4]=id[3]+1; id[4]<52; ++id[4]) for (id[5]=id[4]+1; id[5]<52; ++id[5]) {
    std::uint64_t m = 0; for (int i = 0; i < 6; ++i) m |= 1ULL << id[i];
    HandValue hv; internal::evaluate_holdem_fast(m, 0, hv);
    h = fnv1a(h, hv_word(hv));
    ++n;
  }
  CHECK(n == 20358520);
  std::ostringstream hex_hash;
  hex_hash << "0x" << std::hex << h;
  MESSAGE("6-card golden hash = ", hex_hash.str());
  CHECK(h == GOLDEN_HASH_6CARD);
}

// Gate C: 200k random 7-card hands cross-checked against the independent reference.
// This is the only 7-card cross-check against the reference (gates B / B6 pin the
// full 7-card enumeration by hash, but a hash alone cannot prove correctness).
TEST_CASE("random 7-card vs independent reference (internal::evaluate_holdem_fast)") {
  std::uint64_t s = 0x9E3779B97F4A7C15ULL;  // simple splitmix64
  auto next = [&s]() { s += 0x9E3779B97F4A7C15ULL; std::uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31); };
  long long bad = 0;
  for (int t = 0; t < 200000; ++t) {
    std::uint64_t m = 0; int ids[7]; int c = 0;
    while (c < 7) { int i = static_cast<int>(next() % 52); if ((m >> i) & 1) continue; m |= 1ULL << i; ids[c++] = i; }
    std::sort(ids, ids + 7);
    Ref5 want = ref_eval7(ids);
    HandValue hv; internal::evaluate_holdem_fast(m, 0, hv);
    if (ref_mismatch(to_ref(hv), want)) ++bad;
  }
  CHECK(bad == 0);
}

TEST_SUITE_END();
