#include "doctest.h"

#include <xiapl/canonicalize.h>
#include <xiapl/range.h>
#include <xiapl/simulation.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

// Validation of the checked-in generated table (apps/gen_preflop_rank.cpp).
// The generator's own gates (per-combo pair count, suit isomorphism, naive
// re-enumeration of AA / 76s / 32o) run offline; these tests are what keeps a
// corrupted or hand-edited artifact from reaching a release.
//
// Structural checks alone (uniqueness, combo coverage, descending order,
// equity encoding) do NOT pin which label owns which row: swapping the label
// strings of two adjacent rows passes all of them, and the Monte Carlo
// spot-check's 1e-2 tolerance is far wider than the gap between neighbours
// (A5s/A6s differ by 1.7e-4). Two further checks close that hole -- three
// exact (label, num) pins from the generator's --verify pass, and the global
// zero-sum identity, which binds every row at once.
//
// The table is included into an anonymous namespace so this translation unit
// never shares the definition with wherever the library includes it
// (src/core/preflop_rank.cpp): no ODR coupling in either direction. The .inc
// has no #include directives of its own, so nesting it is safe.
namespace {

#include "../src/core/preflop_rank_table.inc"

// C(50,5) * C(45,2) = the (villain, board) pairs behind every hero combo.
constexpr double kPairsPerCombo = 2097572400.0;
constexpr std::size_t kTableSize =
    sizeof(kPreflopRankTable) / sizeof(kPreflopRankTable[0]);

// 6 combos for a pair, 4 for suited, 12 for offsuit.
int expected_combo_count(const std::string& label) {
    if (label.size() == 2) return 6;
    if (label.size() == 3 && label[2] == 's') return 4;
    return 12;
}

}  // namespace

TEST_CASE("preflop rank table: 169 unique labels covering all 1326 combos") {
    static_assert(kTableSize == 169, "table must hold every canonical hand");

    std::set<std::string> labels;
    int total_combos = 0;

    for (std::size_t i = 0; i < kTableSize; ++i) {
        const std::string label = kPreflopRankTable[i].label;
        INFO("entry ", i, " label ", label);

        CHECK(labels.insert(label).second);  // no duplicates

        const xiapl::Range range = xiapl::Range::from_string(label);
        CHECK(range.size() == static_cast<std::size_t>(expected_combo_count(label)));
        total_combos += static_cast<int>(range.size());

        // Every combo of the label must canonicalize back to it: the table's
        // key is the same label space the rest of the library speaks.
        for (const xiapl::Combo& c : range.combos()) {
            CHECK(xiapl::canonicalize_hand_mask(c.mask) == label);
            CHECK(c.weight == doctest::Approx(1.0));
        }
    }

    CHECK(labels.size() == 169);
    CHECK(total_combos == 1326);
}

TEST_CASE("preflop rank table: ranking order and equity encoding") {
    CHECK(std::string(kPreflopRankTable[0].label) == "AA");
    CHECK(std::string(kPreflopRankTable[kTableSize - 1].label) == "32o");

    for (std::size_t i = 0; i < kTableSize; ++i) {
        const auto& e = kPreflopRankTable[i];
        INFO("entry ", i, " label ", std::string(e.label));

        // equity is exactly num / (2 * N) -- the literal in the file must
        // reparse to the same bits, not merely to something close.
        CHECK(e.equity == static_cast<double>(e.num) / (2.0 * kPairsPerCombo));

        // Sanity envelope: the weakest hand still wins ~32% and the best is
        // ~85%, so nothing may sit outside a generous bracket.
        CHECK(e.equity > 0.30);
        CHECK(e.equity < 0.90);
        CHECK(e.num <= static_cast<unsigned long long>(2.0 * kPairsPerCombo));

        if (i == 0) continue;
        const auto& prev = kPreflopRankTable[i - 1];
        // Documented order: num descending, label ascending on an exact tie.
        CHECK(prev.num >= e.num);
        if (prev.num == e.num) {
            CHECK(std::string(prev.label) < std::string(e.label));
        }
    }

    SUBCASE("no exact ties occur in practice, so the order is strict") {
        for (std::size_t i = 1; i < kTableSize; ++i) {
            INFO("entry ", i, " label ", std::string(kPreflopRankTable[i].label));
            CHECK(kPreflopRankTable[i - 1].num > kPreflopRankTable[i].num);
        }
    }
}

// The checks above are all permutation-blind: they constrain the multiset of
// labels and the sequence of `num` values independently, so relabelling two
// near-neighbour rows (A5s <-> A6s, 1.7e-4 apart in equity) satisfies every
// one of them, and the Monte Carlo spot-check's 1e-2 tolerance is ~60x too
// wide to see it. The two checks below pin the label -> num mapping itself.
TEST_CASE("preflop rank table: label-to-num binding") {
    SUBCASE("exact values cross-checked by the generator's --verify pass") {
        // These three labels were re-enumerated naively (all C(50,5) boards x
        // C(45,2) villains, no inclusion-exclusion) and matched the fast path
        // exactly, so their num is known independently of the fast counting
        // scheme: num = 2 * wins + ties.
        //   AA  : 2 * 1781508418 + 11402312  = 3574419148
        //   76s : 2 *  898379280 + 106652749 = 1903411309
        //   32o : 2 *  613318625 + 128529945 = 1355167195
        struct Pin {
            const char* label;
            unsigned long long num;
        };
        const Pin pins[] = {
            {"AA", 3574419148ULL},
            {"76s", 1903411309ULL},
            {"32o", 1355167195ULL},
        };

        for (const Pin& pin : pins) {
            const std::string wanted = pin.label;
            bool found = false;
            for (std::size_t i = 0; i < kTableSize; ++i) {
                if (wanted != kPreflopRankTable[i].label) continue;
                found = true;
                INFO("entry ", i, " label ", wanted);
                CHECK(kPreflopRankTable[i].num == pin.num);
                break;
            }
            INFO("label ", wanted);
            CHECK(found);
        }
    }

    SUBCASE("global zero-sum identity binds every row") {
        // Summed over all 1,326 hero combos, every (hero, villain, board)
        // configuration is counted once from each side: hero's win is
        // villain's loss and ties are symmetric, so
        //   sum_combos (2*wins + ties) = 1326 * N.
        // Per label the 6/4/12 combos are identical by suit isomorphism, so
        // the label-weighted sum must hit the same total. This is sensitive to
        // exactly the error class the per-combo pair-count gate cancels out
        // (a row carrying another hand's counts), and it constrains all 169
        // rows at once rather than the handful pinned above.
        std::uint64_t weighted_sum = 0;
        for (std::size_t i = 0; i < kTableSize; ++i) {
            const std::string label = kPreflopRankTable[i].label;
            weighted_sum +=
                static_cast<std::uint64_t>(expected_combo_count(label)) *
                static_cast<std::uint64_t>(kPreflopRankTable[i].num);
        }
        // 1326 * 2,097,572,400; ~2.8e12, far below the uint64 range.
        CHECK(weighted_sum == 1326ULL * 2097572400ULL);
        CHECK(weighted_sum == 2781381002400ULL);
    }

    SUBCASE("frozen label order (golden)") {
        // Neither check above resolves a swap between two same-shape
        // neighbours: the three pins do not cover those rows, and the
        // zero-sum weight is the combo count, which is equal for two suited
        // labels -- swapping "A5s" and "A6s" (1.7e-4 apart) leaves the total
        // at 2781381002400 exactly. Only the full order pins those rows.
        //
        // Golden, frozen 2026-08-10 after review: generator --verify
        // (AA / 76s / 32o re-enumerated naively), an independent
        // regeneration that reproduced the artifact byte-for-byte at several
        // thread counts, and 22 published vs-random reference points matched
        // to < 4.5e-4. Regenerating the table must not change this list; if
        // it does, the change needs review, not a silent update here.
        const char* const expected_order[] = {
        "AA", "KK", "QQ", "JJ", "TT", "99", "88", "AKs",
        "77", "AQs", "AJs", "AKo", "ATs", "AQo", "AJo", "KQs",
        "66", "A9s", "ATo", "KJs", "A8s", "KTs", "KQo", "A7s",
        "A9o", "KJo", "55", "QJs", "K9s", "A5s", "A6s", "A8o",
        "KTo", "QTs", "A4s", "A7o", "K8s", "A3s", "QJo", "K9o",
        "A5o", "A6o", "Q9s", "K7s", "JTs", "A2s", "QTo", "44",
        "A4o", "K6s", "K8o", "Q8s", "A3o", "K5s", "J9s", "Q9o",
        "JTo", "K7o", "A2o", "K4s", "Q7s", "K6o", "K3s", "T9s",
        "J8s", "33", "Q6s", "Q8o", "K5o", "J9o", "K2s", "Q5s",
        "T8s", "K4o", "J7s", "Q4s", "Q7o", "T9o", "J8o", "K3o",
        "Q6o", "Q3s", "98s", "T7s", "J6s", "K2o", "22", "Q2s",
        "Q5o", "J5s", "T8o", "J7o", "Q4o", "97s", "J4s", "T6s",
        "J3s", "Q3o", "98o", "87s", "T7o", "J6o", "96s", "J2s",
        "Q2o", "T5s", "J5o", "T4s", "97o", "86s", "J4o", "T6o",
        "95s", "T3s", "76s", "J3o", "87o", "T2s", "85s", "96o",
        "J2o", "T5o", "94s", "75s", "T4o", "93s", "86o", "65s",
        "84s", "95o", "T3o", "92s", "76o", "74s", "T2o", "54s",
        "85o", "64s", "83s", "94o", "75o", "82s", "73s", "93o",
        "65o", "53s", "63s", "84o", "92o", "43s", "74o", "72s",
        "54o", "64o", "52s", "62s", "83o", "42s", "82o", "73o",
        "53o", "63o", "32s", "43o", "72o", "52o", "62o", "42o",
        "32o",
        };
        REQUIRE(sizeof(expected_order) / sizeof(expected_order[0]) == kTableSize);

        for (std::size_t i = 0; i < kTableSize; ++i) {
            INFO("entry ", i, " expected ", std::string(expected_order[i]),
                 " actual ", std::string(kPreflopRankTable[i].label));
            CHECK(std::string(kPreflopRankTable[i].label) ==
                  std::string(expected_order[i]));
        }
    }
}

TEST_SUITE("slow") {
// Independent Monte Carlo cross-check of the exact table: same quantity, a
// completely different code path (calculate_range_equity's sampled-pair
// estimator vs. the offline enumeration). Hero = the label's combos, villain
// = every combo, empty board -- which samples the same uniform
// (villain, board) space the table enumerates.
//
// 200k trials give SE ~= 0.001 on the aggregate, so the 0.01 tolerance is
// ~10 sigma and cannot fire by chance. What it catches is a sampled row whose
// equity is wrong by more than a percentage point -- a stale regeneration, a
// wrong denominator, a grossly mismatched label. It deliberately does NOT
// resolve near-neighbour relabelling; that is the label-to-num binding test's
// job.
TEST_CASE("preflop rank table: Monte Carlo spot-check of spread entries") {
    const std::size_t probes[] = {0, 20, 40, 60, 80, 100, 120, 140, 160, 168};

    for (std::size_t idx : probes) {
        REQUIRE(idx < kTableSize);
        const auto& e = kPreflopRankTable[idx];

        const xiapl::Range hero = xiapl::Range::from_string(e.label);
        const xiapl::Range villain = xiapl::Range::all();

        xiapl::SimulationOptions opts =
            xiapl::SimulationOptions::mc_seeded(200000, 0xC0FFEEULL + idx);
        const xiapl::RangeEquityResult r = xiapl::calculate_range_equity(
            hero, villain, /*board_mask=*/0, opts,
            xiapl::RangeEquityMode::AggregateOnly);

        INFO("entry ", idx, " label ", std::string(e.label), " table ",
             e.equity, " mc ", r.hero_aggregate_equity);
        CHECK(std::abs(r.hero_aggregate_equity - e.equity) < 0.01);
    }
}
}  // TEST_SUITE("slow")
