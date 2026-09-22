// End-to-end tests for the PLO range-notation parser (task 3 of the
// plo-range-ws1 plan). Everything runs through the public surface
// Range::from_string(text, GameType::Plo) -- the internal pattern engine is
// deliberately NOT included here. Where a fixture count cannot be written
// down by hand (the union in the merge test), it is derived from an
// independent oracle that enumerates all C(52,4) hands and applies a
// hand-written predicate, so the parser is never checked against itself.
//
// The frozen count fixtures below are the task-2 engine table respelled as
// notation; their derivations are repeated in comments so this file can be
// audited without running anything.
#include "doctest.h"

#include <xiapl/card.h>
#include <xiapl/game_type.h>
#include <xiapl/range.h>
#include <xiapl/utils.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xiapl;

namespace {

Range plo(const std::string& text) {
    return Range::from_string(text, GameType::Plo);
}

std::size_t plo_count(const std::string& text) { return plo(text).size(); }

std::set<std::uint64_t> mask_set(const Range& range) {
    std::set<std::uint64_t> out;
    for (const Combo& c : range.combos()) out.insert(c.mask);
    return out;
}

std::set<std::uint64_t> plo_mask_set(const std::string& text) {
    return mask_set(plo(text));
}

// popcount == 4, strictly ascending (hence unique), weight in (0,1].
// Violations are counted and asserted once so a broken range does not emit
// hundreds of thousands of CHECK lines.
void check_wellformed(const Range& range) {
    std::size_t bad_popcount = 0;
    std::size_t not_ascending = 0;
    std::size_t bad_weight = 0;
    std::uint64_t previous = 0;
    bool first = true;
    for (const Combo& c : range.combos()) {
        if (popcount64(c.mask) != 4) ++bad_popcount;
        if (!first && c.mask <= previous) ++not_ascending;
        if (!(c.weight > 0.0 && c.weight <= 1.0)) ++bad_weight;
        previous = c.mask;
        first = false;
    }
    CHECK(bad_popcount == 0);
    CHECK(not_ascending == 0);
    CHECK(bad_weight == 0);
    CHECK(range.game() == GameType::Plo);
}

// Returns the what() of the std::invalid_argument the parser threw, or an
// empty string if it did not throw (which every caller treats as a failure).
// std::invalid_argument is the parser's only documented failure mode, so any
// other exception type fails the calling test right here rather than passing
// a "something threw" check.
std::string plo_error(const std::string& text) {
    try {
        Range::from_string(text, GameType::Plo);
    } catch (const std::invalid_argument& e) {
        return e.what();
    } catch (const std::exception& e) {
        FAIL_CHECK("wrong exception type for \"" << text << "\": " << e.what());
    }
    return std::string();
}

void check_throws_containing(const std::string& text, const std::string& needle) {
    const std::string message = plo_error(text);
    INFO("input=\"" << text << "\" message=\"" << message << "\"");
    REQUIRE_FALSE(message.empty());
    CHECK(message.find(needle) != std::string::npos);
}

// Parses `text` and checks every combo carries `expected` (the range must be
// non-empty, so an accidentally-empty expansion cannot pass vacuously).
void check_uniform_weight(const std::string& text, double expected) {
    Range range = plo(text);
    INFO("input=\"" << text << "\"");
    REQUIRE(range.size() > 0);
    std::size_t wrong = 0;
    for (const Combo& c : range.combos()) {
        if (c.weight != doctest::Approx(expected)) ++wrong;
    }
    CHECK(wrong == 0);
}

// Independent oracle: enumerate every 4-card hand and keep the ones the
// caller's predicate accepts. `pred` receives the hand's rank histogram
// (indexed by rank value 2..14) and its per-suit histogram. Written from
// scratch here; shares no code with the parser or the pattern engine.
template <typename Pred>
std::set<std::uint64_t> oracle_hands(Pred pred) {
    std::set<std::uint64_t> out;
    for (int c0 = 0; c0 < 52; ++c0) {
        for (int c1 = c0 + 1; c1 < 52; ++c1) {
            for (int c2 = c1 + 1; c2 < 52; ++c2) {
                for (int c3 = c2 + 1; c3 < 52; ++c3) {
                    const int ids[4] = {c0, c1, c2, c3};
                    std::array<int, 15> rank_count{};
                    std::array<int, 4> suit_count{};
                    for (int id : ids) {
                        rank_count[static_cast<std::size_t>(id % 13 + 2)] += 1;
                        suit_count[static_cast<std::size_t>(id / 13)] += 1;
                    }
                    if (!pred(rank_count, suit_count)) continue;
                    std::uint64_t mask = 0;
                    for (int id : ids) mask |= std::uint64_t{1} << id;
                    out.insert(mask);
                }
            }
        }
    }
    return out;
}

std::uint64_t hand4(const char* a, const char* b, const char* c, const char* d) {
    return cards_to_mask({Card::from_string(a), Card::from_string(b),
                          Card::from_string(c), Card::from_string(d)});
}

std::set<std::uint64_t> union_of(const std::vector<std::string>& texts) {
    std::set<std::uint64_t> out;
    for (const std::string& t : texts) {
        std::set<std::uint64_t> one = plo_mask_set(t);
        out.insert(one.begin(), one.end());
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Frozen count fixtures (task-2 engine table, respelled as notation)
// ---------------------------------------------------------------------------

TEST_CASE("plo_parser_frozen_pattern_counts") {
    // C(52,4).
    CHECK(plo_count("****") == 270725);
    // >= 2 aces: 2A C(4,2)C(48,2)=6*1128=6768; 3A C(4,3)*48=192; 4A 1.
    CHECK(plo_count("AA**") == 6961);
    // Containment discriminator: 270725 - 2*C(48,4) + C(44,4).
    // Positional-after-sort matching would give 16,208 instead.
    CHECK(plo_count("AK**") == 17316);
    // >= 1 ace: 270725 - C(48,4).
    CHECK(plo_count("A***") == 76145);
    // Two pairs of ranks, free suits: C(4,2)*C(4,2).
    CHECK(plo_count("AAKK") == 36);
    // {2,2} suit shape on AAKK: the two aces and the two kings must occupy
    // the same suit pair.
    CHECK(plo_count("AAKKds") == 6);
    // {2,1,1}: 36 total - 6 ds - 6 rainbow.
    CHECK(plo_count("AAKKss") == 24);
    // {1,1,1,1}: aces on a suit pair (6) x kings on the complement (1).
    CHECK(plo_count("AAKKr") == 6);
    // Four distinct ranks, free suits: 4^4.
    CHECK(plo_count("AKQJ") == 256);
    CHECK(plo_count("T987") == 256);
    // ds: C(4,2) suit pairs x C(4,2) rank splits.
    CHECK(plo_count("AKQJds") == 36);
    // ss: 4 doubled suits x C(4,2) ranks x 3*2 singleton suits.
    CHECK(plo_count("AKQJss") == 144);
    // r: 4!.
    CHECK(plo_count("AKQJr") == 24);
    // All four aces: exactly one hand.
    CHECK(plo_count("AAAA") == 1);

    check_wellformed(plo("AA**"));
    check_wellformed(plo("****"));
}

TEST_CASE("plo_parser_containment_includes_extra_pair") {
    // The AK** discriminator at hand level: AAKT has one ace "spare" and is
    // in the set under containment, out of it under positional matching.
    std::set<std::uint64_t> ak = plo_mask_set("AK**");
    CHECK(ak.count(hand4("As", "Ad", "Kh", "Tc")) == 1);
    CHECK(ak.count(hand4("As", "Kd", "Qh", "Jc")) == 1);
    CHECK(ak.count(hand4("As", "Qd", "Jh", "Tc")) == 0);
}

TEST_CASE("plo_parser_write_order_is_free") {
    // Patterns are canonicalized as a multiset, so the written order of the
    // four symbols (wildcards included) does not matter.
    CHECK(plo_mask_set("KQAJ") == plo_mask_set("AKQJ"));
    CHECK(plo_mask_set("*AA*") == plo_mask_set("AA**"));
    CHECK(plo_mask_set("**AA") == plo_mask_set("AA**"));
    CHECK(plo_mask_set("*A*Kr") == plo_mask_set("AK**r"));
}

TEST_CASE("plo_parser_rank_letters_are_case_insensitive") {
    // Inherited from the shared rank parser (xiapl::try_rank_from_char), the
    // same way the Hold'em notation accepts "aa".
    CHECK(plo_mask_set("aakkds") == plo_mask_set("AAKKds"));
    CHECK(plo_mask_set("jt98") == plo_mask_set("JT98"));
}

TEST_CASE("plo_parser_suit_suffixes_are_case_insensitive") {
    // Ranks were case-insensitive from the start but suffixes were not, so
    // "aakkds" parsed while "AAKKDS" was rejected as a stray suit letter --
    // an inconsistency inside one item. Both halves of the notation now fold
    // case, and so does a bare (pattern-less) suffix.
    CHECK(plo_mask_set("AAKKDS") == plo_mask_set("AAKKds"));
    CHECK(plo_mask_set("AAKKSS") == plo_mask_set("AAKKss"));
    CHECK(plo_mask_set("AAKKR") == plo_mask_set("AAKKr"));
    CHECK(plo_mask_set("aakkDs") == plo_mask_set("AAKKds"));
    CHECK(plo_mask_set("JT98DS-8765ds") == plo_mask_set("JT98ds-8765ds"));
    check_throws_containing("DS", "suit suffix");

    // Unchanged wart: five rank symbols plus a suffix is still read as a rank
    // pattern with a suit letter inside it, in either case, because the
    // suffix is only recognized after EXACTLY four symbols.
    check_throws_containing("AAKKKds", "unexpected suit letter 'd'");
    check_throws_containing("AAKKKDS", "unexpected suit letter 'D'");
}

// ---------------------------------------------------------------------------
// Exact 4-card hands
// ---------------------------------------------------------------------------

TEST_CASE("plo_parser_exact_hand") {
    Range range = plo("AsKsQhJd");
    REQUIRE(range.size() == 1);
    CHECK(range.combos()[0].mask == hand4("As", "Ks", "Qh", "Jd"));
    CHECK(range.combos()[0].weight == doctest::Approx(1.0));
    check_wellformed(range);

    // Order-free, like patterns.
    CHECK(plo_mask_set("JdQhKsAs") == plo_mask_set("AsKsQhJd"));
    // An exact hand is a subset of the pattern that describes it.
    CHECK(plo_mask_set("AsAdAhAc") == plo_mask_set("AAAA"));
}

TEST_CASE("plo_parser_exact_hand_duplicate_card_throws") {
    check_throws_containing("AsAsKhQd", "duplicate");
    check_throws_containing("AsKhQdAs", "duplicate");
}

// ---------------------------------------------------------------------------
// Progressions
// ---------------------------------------------------------------------------

TEST_CASE("plo_parser_pair_progression_up") {
    // "JJ**+" shifts the pair up until a rank would leave [2,14]:
    // JJ, QQ, KK, AA. Union size: 4*6961 - C(4,2) disjoint-pair overlaps of
    // 36 each (a hand with >=2 of rank R and >=2 of rank S is exactly
    // C(4,2)*C(4,2) = 36) = 27,844 - 216 = 27,628.
    CHECK(plo_mask_set("JJ**+") == plo_mask_set("JJ**,QQ**,KK**,AA**"));
    CHECK(plo_count("JJ**+") == 27628);
    check_wellformed(plo("JJ**+"));
}

TEST_CASE("plo_parser_pair_progression_down") {
    CHECK(plo_mask_set("44**-") == plo_mask_set("44**,33**,22**"));
}

TEST_CASE("plo_parser_pair_span") {
    CHECK(plo_mask_set("99**-66**") ==
          plo_mask_set("99**,88**,77**,66**"));
}

TEST_CASE("plo_parser_rundown_span") {
    // Closed span between two rundowns: JT98, T987, 9876, 8765.
    //
    // The four sets are pairwise DISJOINT: each rundown pins four distinct
    // ranks, so a hand matching two of them would need >= 5 distinct ranks in
    // 4 cards. Union = 4 * 4^4 = 1,024.
    //
    // NOTE: the plan/brief writes 768 for this fixture; 768 = 3 * 256 and
    // does not match the same brief's "union of the four explicit rundowns"
    // requirement. The set-equality assertion below is the frozen semantic
    // one, and the count is derived from it rather than from the typo.
    const std::set<std::uint64_t> explicit_union =
        union_of({"JT98", "T987", "9876", "8765"});
    CHECK(plo_mask_set("JT98-8765") == explicit_union);
    CHECK(explicit_union.size() == 1024);
    CHECK(plo_count("JT98-8765") == 1024);
    check_wellformed(plo("JT98-8765"));
}

TEST_CASE("plo_parser_rundown_progression_up_and_down") {
    // "+" shifts every rank up until one would leave [2,14]; "-" down.
    CHECK(plo_mask_set("JT98+") == plo_mask_set("JT98,QJT9,KQJT,AKQJ"));
    CHECK(plo_mask_set("8765-") == plo_mask_set("8765,7654,6543,5432"));
}

TEST_CASE("plo_parser_progression_keeps_suffix") {
    CHECK(plo_mask_set("JT98ds+") ==
          plo_mask_set("JT98ds,QJT9ds,KQJTds,AKQJds"));
    CHECK(plo_mask_set("JJ**r+") == plo_mask_set("JJ**r,QQ**r,KK**r,AA**r"));
    CHECK(plo_mask_set("JT98ss-9876ss") ==
          plo_mask_set("JT98ss,T987ss,9876ss"));
}

TEST_CASE("plo_parser_progression_errors") {
    // Not a progression-capable form (neither RR** nor a rundown).
    CHECK_FALSE(plo_error("AK**+").empty());
    CHECK_FALSE(plo_error("AAKK+").empty());
    CHECK_FALSE(plo_error("JJ*Q+").empty());
    // Degenerate span lint: expansion with exactly one element.
    CHECK_FALSE(plo_error("AA**+").empty());
    CHECK_FALSE(plo_error("22**-").empty());
    CHECK_FALSE(plo_error("AKQJ+").empty());
    CHECK_FALSE(plo_error("5432-").empty());
    CHECK_FALSE(plo_error("JJ**-JJ**").empty());
    // Mixed structural forms / mixed suffixes / wrong direction.
    CHECK_FALSE(plo_error("JJ**-AKQJ").empty());
    CHECK_FALSE(plo_error("JT98ds-8765").empty());
    CHECK_FALSE(plo_error("JT98-8765ss").empty());
    CHECK_FALSE(plo_error("66**-99**").empty());
    CHECK_FALSE(plo_error("8765-JT98").empty());
    // No ace-low wrap: A234 is not a rundown (A is high only), so it is a
    // plain 4-rank pattern and cannot carry a progression marker.
    CHECK(plo_count("A234") == 256);
    CHECK_FALSE(plo_error("A234+").empty());
    CHECK_FALSE(plo_error("5432-A234").empty());
}

// ---------------------------------------------------------------------------
// Merge rule
// ---------------------------------------------------------------------------

TEST_CASE("plo_parser_union_merges_overlapping_items") {
    // |AA** u AK**| from the independent oracle: hands with >= 2 aces, or
    // with >= 1 ace and >= 1 king. (By hand: 6,961 + 17,316 - 1,108.)
    const std::set<std::uint64_t> oracle = oracle_hands(
        [](const std::array<int, 15>& rank_count, const std::array<int, 4>&) {
            const int aces = rank_count[14];
            const int kings = rank_count[13];
            return aces >= 2 || (aces >= 1 && kings >= 1);
        });
    Range range = plo("AA**, AK**");
    CHECK(mask_set(range) == oracle);
    CHECK(range.size() == oracle.size());
    check_wellformed(range);

    std::size_t non_unit_weight = 0;
    for (const Combo& c : range.combos()) {
        if (c.weight != doctest::Approx(1.0)) ++non_unit_weight;
    }
    CHECK(non_unit_weight == 0);
}

TEST_CASE("plo_parser_equal_weights_merge_silently") {
    Range range = plo("AA**:0.5, AK**:0.5");
    CHECK(range.size() == plo_count("AA**, AK**"));
    std::size_t wrong_weight = 0;
    for (const Combo& c : range.combos()) {
        if (c.weight != doctest::Approx(0.5)) ++wrong_weight;
    }
    CHECK(wrong_weight == 0);
    check_wellformed(range);

    // Comparison is on the PARSED numeric value, not the spelling.
    CHECK_NOTHROW(plo("AA**:0.5, AK**:.50"));
    CHECK_NOTHROW(plo("AA**:1.0, AK**"));
    CHECK_NOTHROW(plo("AA**, AA**"));
}

TEST_CASE("plo_parser_conflicting_weights_throw_naming_both") {
    const std::string message = plo_error("AA**:0.5, AK**:0.3");
    INFO("message=\"" << message << "\"");
    REQUIRE_FALSE(message.empty());
    CHECK(message.find("AA**:0.5") != std::string::npos);
    CHECK(message.find("AK**:0.3") != std::string::npos);
    CHECK(message.find("0.5") != std::string::npos);
    CHECK(message.find("0.3") != std::string::npos);

    // An omitted weight is exactly 1.0 and conflicts with anything else.
    const std::string implicit = plo_error("AA**, AK**:0.3");
    REQUIRE_FALSE(implicit.empty());
    CHECK(implicit.find("AA**") != std::string::npos);
    CHECK(implicit.find("AK**:0.3") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Weights
// ---------------------------------------------------------------------------

TEST_CASE("plo_parser_weight_accepts_valid_values") {
    check_uniform_weight("AAKKds:1", 1.0);
    check_uniform_weight("AAKKds:1.0", 1.0);
    check_uniform_weight("AAKKds:0.25", 0.25);
    check_uniform_weight("AAKKds:.5", 0.5);
    check_uniform_weight("AAKKds : 0.5", 0.5);
    check_uniform_weight("AAKKds", 1.0);  // omitted weight is exactly 1.0
}

TEST_CASE("plo_parser_weight_rejects_out_of_range_and_non_numeric") {
    for (const char* text : {"AAKKds:0", "AAKKds:0.0", "AAKKds:1.01",
                             "AAKKds:-0.5", "AAKKds:nan", "AAKKds:inf",
                             "AAKKds:", "AAKKds:abc", "AAKKds:0.5:0.5"}) {
        INFO("input=" << text);
        CHECK_FALSE(plo_error(text).empty());
    }
}

TEST_CASE("plo_parser_weight_grammar_rejects_strtod_only_spellings") {
    // The weight literal is an unsigned decimal, deliberately narrower than
    // strtod's grammar: strtod reads "0x1" as the perfectly legal weight 1.0
    // and accepts a leading '+', so both used to be silently accepted. Every
    // rejection keeps the ':' guidance text.
    for (const char* text : {"AAKKds:0x1", "AAKKds:0X1", "AAKKds:+0.5",
                             "AAKKds: +1 ", "AAKKds:1e"}) {
        INFO("input=" << text);
        const std::string message = plo_error(text);
        REQUIRE_FALSE(message.empty());
        CHECK(message.find("weight separator") != std::string::npos);
    }
    // Exponent notation is a plain decimal literal and stays legal.
    check_uniform_weight("AAKKds:2.5e-1", 0.25);
    check_uniform_weight("AAKKds:1e-1", 0.1);
}

// ---------------------------------------------------------------------------
// Reserved tokens (teaching errors)
// ---------------------------------------------------------------------------

TEST_CASE("plo_parser_wrong_symbol_count_suggests_four_symbols") {
    // Too few: pad with '*'.
    check_throws_containing("AA", "AA**");
    check_throws_containing("AAK", "AA**");
    check_throws_containing("AA", "pad");
    // Too many: padding is backwards advice there, but the "AA**" shape
    // example stays in both halves.
    check_throws_containing("AAKKQ", "AA**");
    check_throws_containing("AAKKQ", "exactly 4 rank symbols");
    const std::string too_many = plo_error("AAKKQ");
    CHECK(too_many.find("pad") == std::string::npos);
}

TEST_CASE("plo_parser_bare_suit_suffix_says_it_needs_a_pattern") {
    for (const char* text : {"r", "ds", "ss"}) {
        INFO("input=" << text);
        check_throws_containing(text, "suffix");
        check_throws_containing(text, "4 rank symbols");
    }
}

TEST_CASE("plo_parser_x_suggests_wildcard_or_suffix") {
    check_throws_containing("AxKx", "'*'");
    check_throws_containing("AxKx", "ds");
    check_throws_containing("XX**", "'*'");
}

TEST_CASE("plo_parser_inline_suit_letter_suggests_suffix_or_exact_hand") {
    for (const char* text : {"AKs**", "AAss", "AAKKs", "AAKKd", "AhKh**"}) {
        INFO("input=" << text);
        const std::string message = plo_error(text);
        REQUIRE_FALSE(message.empty());
        CHECK(message.find("ds") != std::string::npos);
        CHECK(message.find("exact") != std::string::npos);
    }
}

TEST_CASE("plo_parser_percent_says_percentile_unsupported") {
    check_throws_containing("50%", "percentile");
    check_throws_containing("AA**%", "percentile");
}

TEST_CASE("plo_parser_reserved_symbols_say_reserved") {
    for (const char* text : {"AA**!", "$AA**", "AA**@", "(AA**)", "AA**)"}) {
        INFO("input=" << text);
        check_throws_containing(text, "reserved");
    }
}

TEST_CASE("plo_parser_non_ascii_input_keeps_messages_ascii") {
    // Diagnostics quote the input back, and the input is raw bytes. A raw
    // non-ASCII byte in what() makes the message invalid UTF-8; pybind11's
    // PyErr_SetString then raises UnicodeDecodeError instead of the
    // ValueError carrying the message, so the teaching error is lost exactly
    // where a copy-pasted en-dash needs it. Every message must be pure ASCII.
    const std::string en_dash = "JT98\xE2\x80\x93" "8765";  // "JT98-8765" with U+2013
    const std::string times_sign = "AKQJ\xC3\x97";          // "AKQJ" + U+00D7
    const std::string lone_high_byte = "AA*\x80";           // not valid UTF-8 at all

    for (const std::string& text : {en_dash, times_sign, lone_high_byte}) {
        const std::string message = plo_error(text);
        INFO("message=\"" << message << "\"");
        REQUIRE_FALSE(message.empty());
        std::size_t non_ascii_bytes = 0;
        for (char c : message) {
            if (static_cast<unsigned char>(c) >= 0x80) ++non_ascii_bytes;
        }
        CHECK(non_ascii_bytes == 0);
        // The offending byte is still named, hex-escaped.
        CHECK(message.find("\\x") != std::string::npos);
    }
    // Escaping keeps the guidance intact -- that is the whole point.
    check_throws_containing(en_dash, "4 rank symbols");
}

TEST_CASE("plo_parser_colon_explains_weight_separator") {
    const std::string guidance =
        "':' here is a weight separator; boolean AND is not supported in v0.1";
    check_throws_containing("AA**:AK**", guidance);
    check_throws_containing("AA**:abc", guidance);
    check_throws_containing("AA**:0", guidance);
    check_throws_containing("AA**:1.01", guidance);
}

// ---------------------------------------------------------------------------
// Lexical rules
// ---------------------------------------------------------------------------

TEST_CASE("plo_parser_whitespace_allowed_around_commas_and_colons_only") {
    CHECK(plo_mask_set("  AA** ,\tAK**  ") == plo_mask_set("AA**,AK**"));
    CHECK(plo_mask_set("AAKKds : 0.5") == plo_mask_set("AAKKds:0.5"));
    // Whitespace inside a body is not allowed.
    CHECK_FALSE(plo_error("AA **").empty());
    CHECK_FALSE(plo_error("A A**").empty());
    CHECK_FALSE(plo_error("As Ks Qh Jd").empty());
}

TEST_CASE("plo_parser_empty_items_are_errors") {
    CHECK_FALSE(plo_error("").empty());
    CHECK_FALSE(plo_error("   ").empty());
    CHECK_FALSE(plo_error("AA**,").empty());
    CHECK_FALSE(plo_error(",AA**").empty());
    CHECK_FALSE(plo_error("AA**,,AK**").empty());
}

TEST_CASE("plo_parser_unsatisfiable_item_yields_empty_range_not_error") {
    // Four cards of one rank are one per suit -> {1,1,1,1}, never {2,2}.
    // Task-2 contract: an unsatisfiable pattern expands to the empty set and
    // is not an error by itself.
    Range range = plo("AAAAds");
    CHECK(range.size() == 0);
    CHECK(range.empty());
    CHECK(range.game() == GameType::Plo);
}

// ---------------------------------------------------------------------------
// Range integration
// ---------------------------------------------------------------------------

TEST_CASE("plo_parser_all_wildcards_equals_range_all") {
    // Range::all(GameType::Plo) and the parser route must produce the same
    // combos in the SAME order: both emit ascending mask (colexicographic)
    // order, so a seeded MC stream does not depend on how the caller spelled
    // the range.
    const Range direct = Range::all(GameType::Plo);
    const Range parsed = Range::from_string("****", GameType::Plo);
    REQUIRE(direct.combos().size() == 270725);
    REQUIRE(parsed.combos().size() == 270725);
    for (std::size_t i = 0; i < direct.combos().size(); ++i) {
        REQUIRE(direct.combos()[i].mask == parsed.combos()[i].mask);
    }
    // Ascending-mask order is the contract, not an accident of either loop.
    for (std::size_t i = 1; i < direct.combos().size(); ++i) {
        REQUIRE(direct.combos()[i - 1].mask < direct.combos()[i].mask);
    }
}

TEST_CASE("plo_parser_holdem_notation_is_rejected_under_plo_tag") {
    // Hold'em spellings are not silently accepted: 2-symbol patterns and the
    // s/o suitedness letters are both errors under the PLO grammar.
    CHECK_FALSE(plo_error("AKs").empty());
    CHECK_FALSE(plo_error("AKo").empty());
    CHECK_FALSE(plo_error("JJ+").empty());
}

// ---------------------------------------------------------------------------
// Fuzz
// ---------------------------------------------------------------------------

TEST_CASE("plo_parser_fuzz_never_crashes") {
    // 10,000 random strings of <= 32 chars, fixed seed. Contract: the parser
    // either returns a well-formed range or throws std::invalid_argument /
    // std::runtime_error -- never anything else and never a crash.
    //
    // Three generators: printable ASCII (the required one), a
    // notation-biased alphabet, and a structured generator that assembles
    // items out of the grammar's own building blocks. The structured stream
    // is what makes the success path more than anecdotal: pure random ASCII
    // parses ~0.1% of the time, which would leave the well-formedness half
    // of the contract untested. It is kept to 5% of the iterations because
    // every successful parse costs a full C(52,4) reference expansion per
    // pattern.
    std::mt19937 rng(20260807u);
    std::string bytes;
    for (int b = 0x20; b <= 0x7E; ++b) bytes.push_back(static_cast<char>(b));
    // High bytes included: the parser must stay ASCII-clean (and
    // invalid_argument-clean) on arbitrary byte soup, not only on
    // well-formed UTF-8.
    for (int b = 0x80; b <= 0xFF; ++b) bytes.push_back(static_cast<char>(b));
    const std::string notation = "AKQJT98765432*dsrxo+-,: .%";

    const std::string rank_symbols = "AKQJT98765432*";
    const std::string suit_letters = "cdhs";
    const std::vector<std::string> suffixes = {"", "", "ds", "ss", "r"};
    const std::vector<std::string> markers = {"", "", "", "+", "-"};
    const std::vector<std::string> weights = {"", "", ":0.5", ":1", ":0.3",
                                              ":0", ":abc"};
    const auto pick = [&rng](const std::vector<std::string>& from) {
        std::uniform_int_distribution<std::size_t> d(0, from.size() - 1);
        return from[d(rng)];
    };
    const auto pick_char = [&rng](const std::string& from) {
        std::uniform_int_distribution<std::size_t> d(0, from.size() - 1);
        return from[d(rng)];
    };
    const auto random_pattern = [&]() {
        std::string out;
        for (int k = 0; k < 4; ++k) out.push_back(pick_char(rank_symbols));
        return out;
    };
    const auto random_structured = [&]() {
        std::uniform_int_distribution<int> item_count(1, 2);
        std::uniform_int_distribution<int> kind(0, 3);
        std::string out;
        const int n = item_count(rng);
        for (int k = 0; k < n; ++k) {
            if (k > 0) out += ",";
            switch (kind(rng)) {
                case 0:  // pattern (+ suffix, + progression marker)
                    out += random_pattern() + pick(suffixes) + pick(markers);
                    break;
                case 1: {  // exact hand, sometimes with a duplicate card
                    for (int c = 0; c < 4; ++c) {
                        out.push_back(pick_char(rank_symbols.substr(0, 13)));
                        out.push_back(pick_char(suit_letters));
                    }
                    break;
                }
                case 2:  // span
                    out += random_pattern() + "-" + random_pattern();
                    break;
                default:  // near-miss garbage
                    out += random_pattern().substr(0, 3) + pick_char(suit_letters);
                    break;
            }
            out += pick(weights);
        }
        return out.size() > 32 ? out.substr(0, 32) : out;
    };

    std::size_t parsed_ok = 0;
    std::size_t rejected = 0;
    std::size_t malformed_results = 0;
    std::size_t non_ascii_messages = 0;
    std::string first_bad;

    for (int i = 0; i < 10000; ++i) {
        std::string text;
        if (i % 20 == 19) {
            text = random_structured();
        } else {
            const std::string& alphabet = (i % 2 == 0) ? bytes : notation;
            std::uniform_int_distribution<int> len_dist(0, 32);
            std::uniform_int_distribution<std::size_t> char_dist(0, alphabet.size() - 1);
            const int len = len_dist(rng);
            text.reserve(static_cast<std::size_t>(len));
            for (int k = 0; k < len; ++k) text.push_back(alphabet[char_dist(rng)]);
        }

        try {
            Range range = Range::from_string(text, GameType::Plo);
            ++parsed_ok;
            std::uint64_t previous = 0;
            bool first = true;
            for (const Combo& c : range.combos()) {
                const bool ok = popcount64(c.mask) == 4 &&
                                (first || c.mask > previous) &&
                                c.weight > 0.0 && c.weight <= 1.0;
                if (!ok) {
                    ++malformed_results;
                    if (first_bad.empty()) first_bad = text;
                }
                previous = c.mask;
                first = false;
            }
        } catch (const std::invalid_argument& e) {
            ++rejected;
            // The message quotes the input back, so byte soup in must not
            // become byte soup out (see the ASCII-purity case above).
            for (const char* p = e.what(); *p != '\0'; ++p) {
                if (static_cast<unsigned char>(*p) >= 0x80) {
                    ++non_ascii_messages;
                    if (first_bad.empty()) first_bad = "non-ASCII message: " + text;
                    break;
                }
            }
        } catch (const std::runtime_error&) {
            ++rejected;
        } catch (const std::exception& e) {
            if (first_bad.empty()) {
                first_bad = "wrong exception type for \"" + text + "\": " + e.what();
            }
            ++malformed_results;
        }
    }

    INFO("parsed=" << parsed_ok << " rejected=" << rejected
                   << " non_ascii_messages=" << non_ascii_messages
                   << " first_bad=" << first_bad);
    CHECK(malformed_results == 0);
    CHECK(non_ascii_messages == 0);
    CHECK(first_bad.empty());
    CHECK(parsed_ok + rejected == 10000);
    // Guards the well-formedness half of the contract against a future
    // change that makes every input throw (measured: 96 successes; it was
    // 108 before the high bytes joined the alphabet, which shifts the draws).
    CHECK(parsed_ok >= 50);
}
