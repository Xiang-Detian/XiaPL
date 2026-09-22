// Offline generator for src/core/preflop_rank_table.inc.
//
// It computes the EXACT equity of every canonical Hold'em starting hand
// ("AA", "AKs", "72o", ...) against ONE uniformly random opponent hand and a
// uniformly random five-card board, by full enumeration. No sampling, no
// floating point in the counting path: everything is integer counts, so the
// output is bit-identical on every platform and at every thread count.
//
// Counting scheme
// ---------------
// The naive statement of the problem is "for every hero combo, for every
// board disjoint from it (C(50,5)), for every villain holding disjoint from
// both (C(45,2)), compare" = 1326 * 2118760 * 990 / 2 comparisons. Instead we
// walk the C(52,5) = 2,598,960 boards once and, per board, evaluate all
// C(47,2) = 1,081 holdings of the remaining 47 cards a single time. Every one
// of those 1,081 holdings is simultaneously a hero holding (with the other
// 990 card-disjoint holdings as its villains) and a villain of others, so one
// evaluation is reused ~990 times.
//
// For a hero holding h = {a, b} on that board, with the 1,081 values sorted:
//
//   wins(h)   = total_less(h) - less_with_a(h) - less_with_b(h)
//   ties(h)   = total_eq(h)   - eq_with_a(h)   - eq_with_b(h)   + 1
//   losses(h) = total_gt(h)   - gt_with_a(h)   - gt_with_b(h)
//
// where `*_with_c` counts holdings that contain card c. This is plain
// inclusion-exclusion over the two "blocked" card sets: the only holding
// containing BOTH a and b is h itself. h is equal-valued, so it never appears
// in the less/greater terms (nothing to correct there), and it is
// double-subtracted in the equal terms, which the +1 restores -- leaving h
// itself correctly excluded from its own villain set.
//
// The per-card counts are not obtained by binary search per holding (that
// would be ~6 searches x 1,081 holdings x 2.6M boards). Instead the single
// sorted order is walked once in equal-value groups: at the start of a group
// every per-card counter already holds that card's strictly-less count, and a
// pre-pass over the group produces the per-card equal counts. Both fall out
// in O(1) per holding, so per board the cost is 1,081 evaluations + one sort
// + three linear passes.
//
// Correctness gate: --verify recomputes (wins, ties) for AA, 76s and 32o by
// the naive C(50,5) x 990 double loop and requires exact integer equality.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <xiapl/canonicalize.h>
#include <xiapl/card.h>
#include <xiapl/range.h>
#include <xiapl/utils.h>

// Internal fast evaluator (52-bit mask -> packed 24-bit score, order
// isomorphic to HandValue). Same access pattern as tests/test_eval_core.cpp.
#include "../src/core/eval_core.h"

namespace {

using xiapl::internal::eval_score7;

constexpr int kDeckSize = 52;
constexpr int kRemaining = 47;                          // 52 - 5 board cards
constexpr int kHoldings = 1081;                         // C(47,2)
constexpr int kHoldingsWithCard = kRemaining - 1;       // 46
constexpr int kNumCombos = 1326;                        // C(52,2)
constexpr int kNumLabels = 169;                         // 13 * 13
constexpr std::uint64_t kTotalBoards = 2598960ULL;      // C(52,5)
constexpr std::uint64_t kBoardsPerCombo = 2118760ULL;   // C(50,5)
constexpr std::uint64_t kVillainsPerBoard = 990ULL;     // C(45,2)
// (villain, board) pairs behind every single hero combo. Identical for every
// combo, which is what makes the per-combo `num` values directly comparable.
constexpr std::uint64_t kPairsPerCombo = kBoardsPerCombo * kVillainsPerBoard;

// Holding index -> the two positions it occupies in the 47-card remainder
// array. The enumeration order (i ascending, j > i ascending) is fixed, so
// this inverse table is a compile-time constant.
struct HoldingPair {
    std::uint8_t i;
    std::uint8_t j;
};

constexpr std::array<HoldingPair, kHoldings> make_holding_pairs() {
    std::array<HoldingPair, kHoldings> table{};
    int h = 0;
    for (int i = 0; i < kRemaining; ++i) {
        for (int j = i + 1; j < kRemaining; ++j) {
            table[static_cast<std::size_t>(h)] = {static_cast<std::uint8_t>(i),
                                                  static_cast<std::uint8_t>(j)};
            ++h;
        }
    }
    return table;
}

inline constexpr std::array<HoldingPair, kHoldings> kHoldingPairs =
    make_holding_pairs();

// (card id, card id) -> dense combo index in [0, 1326), ascending pair order.
struct ComboIndex {
    std::array<std::array<std::uint16_t, kDeckSize>, kDeckSize> index{};
    std::array<std::uint8_t, kNumCombos> card_a{};
    std::array<std::uint8_t, kNumCombos> card_b{};
};

constexpr ComboIndex make_combo_index() {
    ComboIndex t{};
    int n = 0;
    for (int a = 0; a < kDeckSize; ++a) {
        for (int b = a + 1; b < kDeckSize; ++b) {
            t.index[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)] =
                static_cast<std::uint16_t>(n);
            t.index[static_cast<std::size_t>(b)][static_cast<std::size_t>(a)] =
                static_cast<std::uint16_t>(n);
            t.card_a[static_cast<std::size_t>(n)] = static_cast<std::uint8_t>(a);
            t.card_b[static_cast<std::size_t>(n)] = static_cast<std::uint8_t>(b);
            ++n;
        }
    }
    return t;
}

inline constexpr ComboIndex kCombos = make_combo_index();

// Hard failure: fires with or without NDEBUG, unlike assert(). Every gate in
// this generator is a correctness gate on a permanent artifact, so none of
// them may be compiled out.
[[noreturn]] void fail(const std::string& message) {
    std::fflush(stdout);
    std::fprintf(stderr, "\ngen_preflop_rank: FATAL: %s\n", message.c_str());
    std::abort();
}

void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

std::string u64_to_string(std::uint64_t v) {
    return std::to_string(v);
}

// ---------------------------------------------------------------------------
// Parallel driver
// ---------------------------------------------------------------------------

// Boards are handed out round-robin by global board index (`index % threads
// == worker`). That is a static partition -- no work stealing, no atomics on
// the hot path -- and it is exactly balanced, which a contiguous split by
// leading card would not be. Every worker accumulates into its own array and
// the reduction is integer addition, so the result never depends on the
// thread count.
template <typename Body>
void run_parallel(const char* tag, std::uint64_t total_units, int num_threads,
                  Body body) {
    std::atomic<std::uint64_t> progress{0};
    const auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(num_threads));
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() { body(t, num_threads, progress); });
    }

    for (;;) {
        const std::uint64_t done = progress.load(std::memory_order_relaxed);
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                .count();
        std::fprintf(stderr, "\r[%s] %llu / %llu (%.1f%%) %.1fs   ", tag,
                     static_cast<unsigned long long>(done),
                     static_cast<unsigned long long>(total_units),
                     100.0 * static_cast<double>(done) /
                         static_cast<double>(total_units),
                     elapsed);
        std::fflush(stderr);
        if (done >= total_units) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    for (auto& w : workers) w.join();
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    std::fprintf(stderr, "\r[%s] %llu / %llu (100.0%%) %.1fs\n", tag,
                 static_cast<unsigned long long>(total_units),
                 static_cast<unsigned long long>(total_units), elapsed);
}

// ---------------------------------------------------------------------------
// Fast path: one pass over all C(52,5) boards
// ---------------------------------------------------------------------------

struct Accum {
    std::uint64_t wins = 0;
    std::uint64_t ties = 0;
    std::uint64_t losses = 0;
    std::uint64_t boards = 0;
};

void count_all_boards(int worker, int num_threads, std::vector<Accum>& acc,
                      std::atomic<std::uint64_t>& progress) {
    int rem[kRemaining];
    // Packed sort key: score in the high bits, holding index (0..1080, 11
    // bits) in the low bits. Sorting the packed value keeps the whole grouping
    // walk on one contiguous array.
    std::uint64_t keys[kHoldings];
    int less_cnt[kRemaining];
    int group_cnt[kRemaining];
    std::memset(group_cnt, 0, sizeof(group_cnt));

    const auto stride = static_cast<std::uint64_t>(num_threads);
    const auto slot = static_cast<std::uint64_t>(worker);
    std::uint64_t board_index = 0;
    std::uint64_t since_report = 0;

    for (int c0 = 0; c0 < 48; ++c0) {
      for (int c1 = c0 + 1; c1 < 49; ++c1) {
        for (int c2 = c1 + 1; c2 < 50; ++c2) {
          for (int c3 = c2 + 1; c3 < 51; ++c3) {
            for (int c4 = c3 + 1; c4 < 52; ++c4) {
              if (board_index++ % stride != slot) continue;

              const std::uint64_t board =
                  (1ULL << c0) | (1ULL << c1) | (1ULL << c2) | (1ULL << c3) |
                  (1ULL << c4);

              int n = 0;
              std::uint64_t rest = xiapl::FULL_DECK_MASK & ~board;
              while (rest) {
                  rem[n++] = xiapl::ctz64(rest);
                  rest &= rest - 1;
              }

              int h = 0;
              for (int i = 0; i < kRemaining; ++i) {
                  const std::uint64_t with_i =
                      board | (1ULL << static_cast<unsigned>(rem[i]));
                  for (int j = i + 1; j < kRemaining; ++j) {
                      const std::uint32_t score = eval_score7(
                          with_i | (1ULL << static_cast<unsigned>(rem[j])));
                      keys[h] = (static_cast<std::uint64_t>(score) << 11) |
                                static_cast<std::uint64_t>(h);
                      ++h;
                  }
              }

              std::sort(keys, keys + kHoldings);
              std::memset(less_cnt, 0, sizeof(less_cnt));

              int s = 0;
              while (s < kHoldings) {
                  const std::uint64_t value = keys[s] >> 11;
                  int e = s + 1;
                  while (e < kHoldings && (keys[e] >> 11) == value) ++e;

                  // Pass 1: per-card counts inside this equal-value group.
                  for (int t = s; t < e; ++t) {
                      const HoldingPair p =
                          kHoldingPairs[static_cast<std::size_t>(keys[t] & 0x7FF)];
                      ++group_cnt[p.i];
                      ++group_cnt[p.j];
                  }

                  // Pass 2: emit counts for every holding in the group.
                  // less_cnt[] currently holds strictly-less counts (all
                  // earlier groups), group_cnt[] the equal counts.
                  const int total_eq = e - s;
                  const int total_gt = kHoldings - e;
                  for (int t = s; t < e; ++t) {
                      const HoldingPair p =
                          kHoldingPairs[static_cast<std::size_t>(keys[t] & 0x7FF)];
                      const int li = less_cnt[p.i];
                      const int lj = less_cnt[p.j];
                      const int ei = group_cnt[p.i];
                      const int ej = group_cnt[p.j];
                      const int wins = s - li - lj;
                      const int ties = total_eq - ei - ej + 1;
                      const int losses = total_gt - (kHoldingsWithCard - li - ei) -
                                         (kHoldingsWithCard - lj - ej);
                      Accum& a = acc[kCombos.index[static_cast<std::size_t>(rem[p.i])]
                                                  [static_cast<std::size_t>(rem[p.j])]];
                      a.wins += static_cast<std::uint64_t>(wins);
                      a.ties += static_cast<std::uint64_t>(ties);
                      a.losses += static_cast<std::uint64_t>(losses);
                      a.boards += 1;
                  }

                  // Pass 3: fold the group into less_cnt[] and clear
                  // group_cnt[] for the cards it touched (cheaper than
                  // clearing all 47 per group).
                  for (int t = s; t < e; ++t) {
                      const HoldingPair p =
                          kHoldingPairs[static_cast<std::size_t>(keys[t] & 0x7FF)];
                      ++less_cnt[p.i];
                      ++less_cnt[p.j];
                      group_cnt[p.i] = 0;
                      group_cnt[p.j] = 0;
                  }

                  s = e;
              }

              if (++since_report == 4096) {
                  progress.fetch_add(since_report, std::memory_order_relaxed);
                  since_report = 0;
              }
            }
          }
        }
      }
    }

    if (since_report != 0) {
        progress.fetch_add(since_report, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// Naive path (--verify): direct C(50,5) x 990 double loop for one hero combo
// ---------------------------------------------------------------------------

struct Counts {
    std::uint64_t wins = 0;
    std::uint64_t ties = 0;
    std::uint64_t losses = 0;
    std::uint64_t boards = 0;
};

void count_naive(int worker, int num_threads, std::uint64_t hero_mask,
                 Counts& out, std::atomic<std::uint64_t>& progress) {
    int deck[50];
    int n_deck = 0;
    std::uint64_t rest = xiapl::FULL_DECK_MASK & ~hero_mask;
    while (rest) {
        deck[n_deck++] = xiapl::ctz64(rest);
        rest &= rest - 1;
    }
    require(n_deck == 50, "naive: hero mask must hold exactly 2 cards");

    int villains[45];
    const auto stride = static_cast<std::uint64_t>(num_threads);
    const auto slot = static_cast<std::uint64_t>(worker);
    std::uint64_t board_index = 0;
    std::uint64_t since_report = 0;

    for (int i0 = 0; i0 < 46; ++i0) {
      for (int i1 = i0 + 1; i1 < 47; ++i1) {
        for (int i2 = i1 + 1; i2 < 48; ++i2) {
          for (int i3 = i2 + 1; i3 < 49; ++i3) {
            for (int i4 = i3 + 1; i4 < 50; ++i4) {
              if (board_index++ % stride != slot) continue;

              const std::uint64_t board =
                  (1ULL << static_cast<unsigned>(deck[i0])) |
                  (1ULL << static_cast<unsigned>(deck[i1])) |
                  (1ULL << static_cast<unsigned>(deck[i2])) |
                  (1ULL << static_cast<unsigned>(deck[i3])) |
                  (1ULL << static_cast<unsigned>(deck[i4]));

              const std::uint32_t hero_score = eval_score7(board | hero_mask);

              int n_villains = 0;
              std::uint64_t pool = xiapl::FULL_DECK_MASK & ~hero_mask & ~board;
              while (pool) {
                  villains[n_villains++] = xiapl::ctz64(pool);
                  pool &= pool - 1;
              }

              for (int a = 0; a < 45; ++a) {
                  const std::uint64_t with_a =
                      board | (1ULL << static_cast<unsigned>(villains[a]));
                  for (int b = a + 1; b < 45; ++b) {
                      const std::uint32_t v = eval_score7(
                          with_a | (1ULL << static_cast<unsigned>(villains[b])));
                      if (v < hero_score) {
                          ++out.wins;
                      } else if (v == hero_score) {
                          ++out.ties;
                      } else {
                          ++out.losses;
                      }
                  }
              }
              ++out.boards;

              if (++since_report == 4096) {
                  progress.fetch_add(since_report, std::memory_order_relaxed);
                  since_report = 0;
              }
            }
          }
        }
      }
    }

    if (since_report != 0) {
        progress.fetch_add(since_report, std::memory_order_relaxed);
    }
}

Counts naive_combo_counts(std::uint64_t hero_mask, int num_threads,
                          const char* tag) {
    std::vector<Counts> per_worker(static_cast<std::size_t>(num_threads));
    run_parallel(tag, kBoardsPerCombo, num_threads,
                 [&](int worker, int threads, std::atomic<std::uint64_t>& p) {
                     count_naive(worker, threads, hero_mask,
                                 per_worker[static_cast<std::size_t>(worker)], p);
                 });

    Counts total;
    for (const Counts& c : per_worker) {
        total.wins += c.wins;
        total.ties += c.ties;
        total.losses += c.losses;
        total.boards += c.boards;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Label reduction and output
// ---------------------------------------------------------------------------

struct LabelEntry {
    std::string label;
    std::uint64_t wins = 0;
    std::uint64_t ties = 0;
    std::uint64_t num = 0;   // 2 * wins + ties
    double equity = 0.0;     // num / (2 * kPairsPerCombo)
    int combos = 0;
};

int expected_combo_count(const std::string& label) {
    if (label.size() == 2) return 6;         // pair
    if (label.size() == 3 && label[2] == 's') return 4;
    return 12;                               // offsuit
}

void write_table(std::FILE* out, const std::vector<LabelEntry>& entries) {
    std::fprintf(out,
        "// Generated by apps/gen_preflop_rank.cpp -- do not edit by hand.\n"
        "// Exact equity of each canonical Hold'em starting hand vs one uniformly\n"
        "// random opponent hand and a uniformly random 5-card board, enumerated over\n"
        "// all C(50,5) boards x C(45,2) villain holdings per hero combo\n"
        "// (N = 2,097,572,400 (villain, board) pairs; identical for every hand).\n"
        "// num = 2*wins + ties (exact integer); equity = num / (2*N).\n"
        "// Sorted by num descending; ties (none expected) break by label ascending.\n"
        "struct PreflopRankEntry {\n"
        "    const char* label;\n"
        "    double equity;\n"
        "    unsigned long long num;\n"
        "};\n"
        "inline constexpr PreflopRankEntry kPreflopRankTable[169] = {\n");

    for (const LabelEntry& e : entries) {
        char quoted[16];
        std::snprintf(quoted, sizeof(quoted), "\"%s\",", e.label.c_str());
        // %.17g round-trips a double exactly, so the checked-in literal
        // reparses to the same bits as num / (2.0 * N).
        std::fprintf(out, "    {%-6s %.17g, %lluULL},\n", quoted, e.equity,
                     static_cast<unsigned long long>(e.num));
    }

    std::fprintf(out, "};\n");
}

void print_usage() {
    std::fprintf(stderr,
        "usage: gen_preflop_rank [--threads N] [--verify] [--out PATH]\n"
        "\n"
        "  --threads N   worker threads (default: hardware concurrency)\n"
        "  --verify      cross-check AA / 76s / 32o against a naive full\n"
        "                enumeration (exact integer equality required)\n"
        "  --out PATH    write the .inc file to PATH (default: stdout)\n");
}

}  // namespace

int main(int argc, char** argv) {
    int num_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (num_threads < 1) num_threads = 1;
    bool verify = false;
    std::string out_path;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc) {
            num_threads = std::atoi(argv[++i]);
            if (num_threads < 1) {
                std::fprintf(stderr, "gen_preflop_rank: --threads must be >= 1\n");
                return 1;
            }
        } else if (arg == "--verify") {
            verify = true;
        } else if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "gen_preflop_rank: unknown argument '%s'\n",
                         arg.c_str());
            print_usage();
            return 1;
        }
    }

    std::fprintf(stderr, "[gen] threads=%d verify=%s out=%s\n", num_threads,
                 verify ? "yes" : "no",
                 out_path.empty() ? "<stdout>" : out_path.c_str());

    // ---- Main pass -------------------------------------------------------
    std::vector<std::vector<Accum>> per_worker(
        static_cast<std::size_t>(num_threads),
        std::vector<Accum>(kNumCombos));

    const auto wall_start = std::chrono::steady_clock::now();
    run_parallel("boards", kTotalBoards, num_threads,
                 [&](int worker, int threads, std::atomic<std::uint64_t>& p) {
                     count_all_boards(worker, threads,
                                      per_worker[static_cast<std::size_t>(worker)], p);
                 });

    std::vector<Accum> combo(kNumCombos);
    for (const std::vector<Accum>& part : per_worker) {
        for (int c = 0; c < kNumCombos; ++c) {
            combo[static_cast<std::size_t>(c)].wins += part[static_cast<std::size_t>(c)].wins;
            combo[static_cast<std::size_t>(c)].ties += part[static_cast<std::size_t>(c)].ties;
            combo[static_cast<std::size_t>(c)].losses += part[static_cast<std::size_t>(c)].losses;
            combo[static_cast<std::size_t>(c)].boards += part[static_cast<std::size_t>(c)].boards;
        }
    }

    // Gate 1: every combo saw exactly C(50,5) boards and exactly
    // C(50,5) * 990 (villain, board) pairs, with none unaccounted for.
    for (int c = 0; c < kNumCombos; ++c) {
        const Accum& a = combo[static_cast<std::size_t>(c)];
        require(a.boards == kBoardsPerCombo,
                "combo " + u64_to_string(static_cast<std::uint64_t>(c)) +
                    ": board count " + u64_to_string(a.boards) + " != " +
                    u64_to_string(kBoardsPerCombo));
        require(a.wins + a.ties + a.losses == kPairsPerCombo,
                "combo " + u64_to_string(static_cast<std::uint64_t>(c)) +
                    ": wins+ties+losses " +
                    u64_to_string(a.wins + a.ties + a.losses) + " != " +
                    u64_to_string(kPairsPerCombo));
    }
    std::fprintf(stderr,
                 "[gen] pair-count gate: all %d combos hold exactly %llu "
                 "(villain, board) pairs\n",
                 kNumCombos, static_cast<unsigned long long>(kPairsPerCombo));

    // ---- Reduce combos -> 169 labels -------------------------------------
    std::map<std::string, LabelEntry> by_label;
    for (int c = 0; c < kNumCombos; ++c) {
        const Accum& a = combo[static_cast<std::size_t>(c)];
        const std::vector<xiapl::Card> hand = {
            xiapl::Card(kCombos.card_a[static_cast<std::size_t>(c)]),
            xiapl::Card(kCombos.card_b[static_cast<std::size_t>(c)])};
        const std::string label = xiapl::canonicalize_hand(hand);

        auto it = by_label.find(label);
        if (it == by_label.end()) {
            LabelEntry e;
            e.label = label;
            e.wins = a.wins;
            e.ties = a.ties;
            e.combos = 1;
            by_label.emplace(label, e);
            continue;
        }
        // Gate 2: suit isomorphism. Every combo of a label must have
        // bit-identical counts; a mismatch means the enumeration is wrong.
        require(it->second.wins == a.wins && it->second.ties == a.ties,
                "suit isomorphism violated for label " + label + ": " +
                    hand[0].to_string() + hand[1].to_string() + " has (" +
                    u64_to_string(a.wins) + ", " + u64_to_string(a.ties) +
                    ") but earlier combos have (" +
                    u64_to_string(it->second.wins) + ", " +
                    u64_to_string(it->second.ties) + ")");
        ++it->second.combos;
    }

    require(by_label.size() == static_cast<std::size_t>(kNumLabels),
            "expected 169 labels, got " +
                u64_to_string(static_cast<std::uint64_t>(by_label.size())));

    std::vector<LabelEntry> entries;
    entries.reserve(static_cast<std::size_t>(kNumLabels));
    int combo_total = 0;
    for (auto& kv : by_label) {
        LabelEntry e = kv.second;
        require(e.combos == expected_combo_count(e.label),
                "label " + e.label + " covers " +
                    u64_to_string(static_cast<std::uint64_t>(e.combos)) +
                    " combos, expected " +
                    u64_to_string(static_cast<std::uint64_t>(
                        expected_combo_count(e.label))));
        combo_total += e.combos;
        e.num = 2 * e.wins + e.ties;
        e.equity = static_cast<double>(e.num) /
                   (2.0 * static_cast<double>(kPairsPerCombo));
        entries.push_back(e);
    }
    require(combo_total == kNumCombos,
            "label combo counts sum to " +
                u64_to_string(static_cast<std::uint64_t>(combo_total)) +
                ", expected 1326");
    std::fprintf(stderr,
                 "[gen] isomorphism gate: 169 labels, 1326 combos, all combos of "
                 "a label identical\n");

    // Rank: num descending, label ascending on an exact tie.
    std::sort(entries.begin(), entries.end(),
              [](const LabelEntry& x, const LabelEntry& y) {
                  if (x.num != y.num) return x.num > y.num;
                  return x.label < y.label;
              });

    // ---- Optional naive cross-check --------------------------------------
    if (verify) {
        const char* labels[] = {"AA", "76s", "32o"};
        for (const char* label : labels) {
            const xiapl::Range range = xiapl::Range::from_string(label);
            require(!range.empty(), std::string("empty range for ") + label);
            const std::uint64_t hero_mask = range.combos().front().mask;
            require(xiapl::popcount64(hero_mask) == 2,
                    std::string("non-2-card combo for ") + label);

            const int card_lo = xiapl::ctz64(hero_mask);
            const int card_hi = xiapl::ctz64(hero_mask & (hero_mask - 1));
            const Accum& fast =
                combo[kCombos.index[static_cast<std::size_t>(card_lo)]
                                   [static_cast<std::size_t>(card_hi)]];

            std::fprintf(stderr, "[verify] %s = %s%s (naive C(50,5) x 990 pass)\n",
                         label, xiapl::Card(static_cast<std::uint8_t>(card_lo)).to_string().c_str(),
                         xiapl::Card(static_cast<std::uint8_t>(card_hi)).to_string().c_str());
            const Counts naive = naive_combo_counts(hero_mask, num_threads, "verify");

            require(naive.boards == kBoardsPerCombo,
                    std::string("naive board count mismatch for ") + label);
            const bool ok = naive.wins == fast.wins && naive.ties == fast.ties &&
                            naive.losses == fast.losses;
            std::printf(
                "[verify] %-4s fast(wins=%llu ties=%llu losses=%llu) "
                "naive(wins=%llu ties=%llu losses=%llu) -> %s\n",
                label, static_cast<unsigned long long>(fast.wins),
                static_cast<unsigned long long>(fast.ties),
                static_cast<unsigned long long>(fast.losses),
                static_cast<unsigned long long>(naive.wins),
                static_cast<unsigned long long>(naive.ties),
                static_cast<unsigned long long>(naive.losses),
                ok ? "EXACT MATCH" : "MISMATCH");
            std::fflush(stdout);
            require(ok, std::string("naive verification FAILED for ") + label);
        }
        std::printf("[verify] all 3 labels match the naive enumeration exactly\n");
        std::fflush(stdout);
    }

    // ---- Emit ------------------------------------------------------------
    if (out_path.empty()) {
        write_table(stdout, entries);
    } else {
        std::FILE* out = std::fopen(out_path.c_str(), "w");
        if (out == nullptr) {
            std::fprintf(stderr, "gen_preflop_rank: cannot open '%s' for writing\n",
                         out_path.c_str());
            return 1;
        }
        write_table(out, entries);
        std::fclose(out);
        std::fprintf(stderr, "[gen] wrote %s\n", out_path.c_str());
    }

    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start)
            .count();
    std::fprintf(stderr,
                 "[gen] top: %s (equity %.6f)  bottom: %s (equity %.6f)  "
                 "total %.1fs\n",
                 entries.front().label.c_str(), entries.front().equity,
                 entries.back().label.c_str(), entries.back().equity, wall);
    return 0;
}
