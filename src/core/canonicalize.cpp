#include <xiapl/canonicalize.h>

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>
#include <xiapl/deck.h>
#include <xiapl/utils.h>

namespace xiapl {

namespace {
// Lossless pack of (hero_mask, board_mask) into a single uint64.
//
// Layout:
//   bits  0..51 — combined = hero_mask | board_mask  (5..7 bits set)
//   bits 52..57 — hero_low_id  (low  bit position of hero_mask, 0..51)
//   bits 58..63 — hero_high_id (high bit position of hero_mask, 0..51)
//
// Hero and board never overlap, so combined uniquely determines the 7 cards
// and the two hero card IDs split them deterministically.
inline std::uint64_t pack_hero_board(std::uint64_t hero_mask,
                                     std::uint64_t board_mask) noexcept {
    const std::uint64_t combined = hero_mask | board_mask;
    const unsigned lo = static_cast<unsigned>(ctz64(hero_mask));
    const unsigned hi = 63u - static_cast<unsigned>(clz64(hero_mask));
    return combined |
           (static_cast<std::uint64_t>(lo) << 52) |
           (static_cast<std::uint64_t>(hi) << 58);
}

inline HeroBoardMask unpack_hero_board(std::uint64_t packed) noexcept {
    const std::uint64_t combined = packed & ((1ULL << 52) - 1ULL);
    const unsigned lo = static_cast<unsigned>((packed >> 52) & 0x3Fu);
    const unsigned hi = static_cast<unsigned>((packed >> 58) & 0x3Fu);
    const std::uint64_t hero_mask = (1ULL << lo) | (1ULL << hi);
    const std::uint64_t board_mask = combined ^ hero_mask;
    return {hero_mask, board_mask};
}

// 64-bit mix (Murmur3 finalizer). std::hash<uint64_t> is identity on
// libc++, which would cluster the low bits of `combined` heavily.
struct U64Mix {
    std::size_t operator()(std::uint64_t k) const noexcept {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return static_cast<std::size_t>(k);
    }
};

// Flat open-addressing dedup set keyed on packed canonical situations.
//
// unordered_set<uint64_t> allocates a node per insert (24 bytes + heap
// overhead) and the buckets store a separate pointer to each node, so
// every probe is a cache miss. For turn enumeration that's ~17M random
// allocations.
//
// This is a plain power-of-two-sized flat table. Insert hashes once,
// linearly probes from there, and writes the key in place. The empty
// sentinel is 0, which is never produced by pack_hero_board because the
// hero portion always has both bits set => combined > 0 => key > 0.
class FlatU64Set {
public:
    explicit FlatU64Set(std::size_t expected) {
        // Target ~0.5 load factor at full capacity.
        std::size_t cap = 1;
        while (cap < std::max<std::size_t>(expected * 2, 16)) cap <<= 1;
        table_.assign(cap, 0ULL);
        mask_ = cap - 1;
    }

    void insert(std::uint64_t key) {
        std::size_t pos = U64Mix{}(key) & mask_;
        while (true) {
            std::uint64_t v = table_[pos];
            if (v == 0)   { table_[pos] = key; ++count_; return; }
            if (v == key) return;
            pos = (pos + 1) & mask_;
        }
    }

    std::size_t size() const noexcept { return count_; }

    template <typename F>
    void for_each(F&& f) const {
        for (std::uint64_t v : table_) if (v) f(v);
    }

    // Release the underlying buffer so the caller can drop peak RSS
    // after merging into a larger set.
    void clear_and_shrink() {
        std::vector<std::uint64_t>().swap(table_);
        mask_ = 0;
        count_ = 0;
    }

private:
    std::vector<std::uint64_t> table_;
    std::size_t mask_  = 0;
    std::size_t count_ = 0;
};
} // namespace




// The message a removed-generation artefact gets. Named once so the u32 and the
// string parser cannot drift, and so a caller reading either one is told the
// same thing: the number is refused because it cannot be honoured, not because
// it is unrecognised.
static std::string removed_legacy_message(const std::string& what) {
    return "canonicalization version " + what +
           " was removed: this build cannot reproduce its keys, so every "
           "lookup against a legacy-keyed artefact would miss. Rebuild the "
           "artefact with the strict (canon=2) form, or read it with a build "
           "from before the removal.";
}

const char* canon_version_name(CanonVersion v) {
    switch (v) {
    case CanonVersion::Strict: return "strict";
    }
    return "unknown";
}

CanonVersion canon_version_from_u32(std::uint32_t v) {
    if (v == 2) return CanonVersion::Strict;
    if (v == 1)
        throw std::invalid_argument(removed_legacy_message("1 (legacy)"));
    throw std::invalid_argument(
        "unknown canonicalization version " + std::to_string(v) +
        " (this build understands 2=strict)");
}

CanonVersion canon_version_from_string(const std::string& s) {
    if (s == "strict" || s == "2") return CanonVersion::Strict;
    if (s == "legacy" || s == "1")
        throw std::invalid_argument(
            removed_legacy_message("'" + s + "' (legacy)"));
    throw std::invalid_argument(
        "unknown canonicalization version '" + s + "' (expected 'strict')");
}

std::pair<std::vector<Card>, std::vector<Card>> canonicalize_hero_and_board_cards(
    std::vector<Card> hero_hand,
    std::vector<Card> board
) {
    // Convert to masks first, then call mask-based canonicalization
    std::uint64_t hero_mask  = cards_to_mask(hero_hand);
    std::uint64_t board_mask = cards_to_mask(board);

    auto [hero_mask_canon, board_mask_canon] =
        canonicalize_hero_and_board(hero_mask, board_mask);

    // Convert canonicalized masks back to Card arrays
    auto canon_hero  = mask_to_cards(hero_mask_canon);
    auto canon_board = mask_to_cards(board_mask_canon);

    return {canon_hero, canon_board};
}

std::pair<std::uint64_t, std::uint64_t> canonicalize_hero_and_board(
    std::uint64_t hero_mask,
    std::uint64_t board_mask
) {
    // Per-suit 13-bit rank patterns (bit r = rank (r+2) present).
    // Layout matches Card id = suit*13 + (rank-2), so suit s lives at
    // bits [13s, 13s+13).
    std::uint32_t pat_b[4];
    std::uint32_t pat_h[4];
    for (int s = 0; s < 4; ++s) {
        pat_b[s] = static_cast<std::uint32_t>((board_mask >> (s * 13)) & 0x1FFFu);
        pat_h[s] = static_cast<std::uint32_t>((hero_mask  >> (s * 13)) & 0x1FFFu);
    }

    // Suit priority key matches the original "register suits in card order"
    // logic exactly:
    //   - The original pre_sort goes through board cards by rank desc /
    //     suit asc, registering each new suit. The suit that owns the
    //     highest board rank wins; ties on max rank break by suit index asc
    //     (because pre_sort visits the lower-suit card first when ranks tie).
    //   - For suits absent from the board, the same rule applies to hero.
    //   - For suits absent from both, suit index asc keeps the identity map.
    // We collapse all three rules into one sort key per suit:
    //   (max_rank_b, max_rank_h, -suit) decreasing  ==  (b desc, h desc, suit asc)
    // where max_rank_* is -1 when the corresponding pattern is empty.
    auto top_rank = [](std::uint32_t p) -> int {
        return p ? (31 - clz32(p)) : -1;
    };

    struct Key {
        int max_b;
        int max_h;
        int suit;
    } keys[4];
    for (int s = 0; s < 4; ++s) {
        keys[s] = {top_rank(pat_b[s]), top_rank(pat_h[s]), s};
    }

    // 4-way insertion sort (faster than std::sort for n=4, no overhead).
    //
    // Priority ordering (matches the original card-by-card register loop):
    //   1. Suits that appear in the board come before suits that don't
    //      (board registration runs first).
    //   2. Within board-present suits: max_b desc, then suit asc.
    //      (rank-desc / suit-asc iteration registers higher max-rank suits
    //      first; ties on max rank fall back to the smaller suit index.)
    //   3. Within board-absent suits: max_h desc, then suit asc.
    //      (Same rule, run over hero cards.)
    auto prev_first = [](const Key& a, const Key& b) -> bool {
        bool a_in_b = a.max_b >= 0;
        bool b_in_b = b.max_b >= 0;
        if (a_in_b != b_in_b) return a_in_b;
        if (a_in_b) {
            if (a.max_b != b.max_b) return a.max_b > b.max_b;
            return a.suit < b.suit;
        }
        if (a.max_h != b.max_h) return a.max_h > b.max_h;
        return a.suit < b.suit;
    };
    for (int i = 1; i < 4; ++i) {
        Key cur = keys[i];
        int j = i;
        while (j > 0 && !prev_first(keys[j - 1], cur)) {
            keys[j] = keys[j - 1];
            --j;
        }
        keys[j] = cur;
    }

    // suit_map[orig_suit] = new_suit_position
    int suit_map[4];
    for (int i = 0; i < 4; ++i) {
        suit_map[keys[i].suit] = i;
    }

    std::uint64_t hero_mask_canon  = 0;
    std::uint64_t board_mask_canon = 0;
    for (int s = 0; s < 4; ++s) {
        const int shift = suit_map[s] * 13;
        board_mask_canon |= static_cast<std::uint64_t>(pat_b[s]) << shift;
        hero_mask_canon  |= static_cast<std::uint64_t>(pat_h[s]) << shift;
    }
    return {hero_mask_canon, board_mask_canon};
}

// CanonVersion::Strict. One 26-bit key per suit, (board_pattern << 13) |
// hero_pattern, sorted descending; the i-th largest key's suit becomes suit i.
//
// Why this is a canonical form and the legacy routine above is not: relabelling
// the suits permutes the four keys, so the SORTED key sequence is invariant --
// it is a complete invariant of the orbit.  Two situations therefore share a
// representative exactly when they are suit-isomorphic, where the legacy rule
// (compare only each suit's top board rank, break ties by suit index) keeps
// isomorphic situations apart whenever two suits share a top rank.
//
// The board pattern occupies the HIGH half of the key, so the board halves come
// out in descending order regardless of hero: the board component of the result
// is identical to canonicalize_board(board_mask).  That is what keeps the
// board-only marginal of a strict enumeration at exactly 1,755 flop classes.
//
// Ties are suits whose (board, hero) patterns are equal as a pair, i.e. genuinely
// interchangeable.  The sort is stable, so they keep ascending suit order; the
// canonical masks are the same whichever way they fall.
std::pair<std::uint64_t, std::uint64_t> canonicalize_hero_and_board_strict(
    std::uint64_t hero_mask,
    std::uint64_t board_mask
) {
    std::uint32_t pat_b[4];
    std::uint32_t pat_h[4];
    std::uint32_t key[4];
    for (int s = 0; s < 4; ++s) {
        pat_b[s] = static_cast<std::uint32_t>((board_mask >> (s * 13)) & 0x1FFFu);
        pat_h[s] = static_cast<std::uint32_t>((hero_mask  >> (s * 13)) & 0x1FFFu);
        key[s]   = (pat_b[s] << 13) | pat_h[s];
    }

    // 4-way stable insertion sort of the suit indices by key descending.
    int ord[4] = {0, 1, 2, 3};
    for (int i = 1; i < 4; ++i) {
        const int cur = ord[i];
        int j = i;
        while (j > 0 && key[ord[j - 1]] < key[cur]) {
            ord[j] = ord[j - 1];
            --j;
        }
        ord[j] = cur;
    }

    // suit_map[orig_suit] = new_suit_position
    int suit_map[4];
    for (int i = 0; i < 4; ++i) suit_map[ord[i]] = i;

    std::uint64_t hero_mask_canon  = 0;
    std::uint64_t board_mask_canon = 0;
    for (int s = 0; s < 4; ++s) {
        const int shift = suit_map[s] * 13;
        board_mask_canon |= static_cast<std::uint64_t>(pat_b[s]) << shift;
        hero_mask_canon  |= static_cast<std::uint64_t>(pat_h[s]) << shift;
    }
    return {hero_mask_canon, board_mask_canon};
}

std::pair<std::uint64_t, std::uint64_t> canonicalize_hero_and_board_v(
    std::uint64_t hero_mask,
    std::uint64_t board_mask,
    CanonVersion version
) {
    switch (version) {
    case CanonVersion::Strict:
        return canonicalize_hero_and_board_strict(hero_mask, board_mask);
    }
    // Not a legal enumerator: refuse rather than pick one, because picking one
    // would run a different card abstraction than the caller asked for.
    throw std::invalid_argument(
        "canonicalize_hero_and_board_v: unknown canonicalization version " +
        std::to_string(static_cast<std::uint32_t>(version)));
}

// Inner worker: enumerate all boards for the given (hero_lo, hero_hi)
// pair and insert canonicalized packed keys into `out_set`. Pure
// function of (hero_lo, hero_hi, board_size, version); safe to call from
// any thread on its own set.
static void enumerate_hero_pair(int hero_lo, int hero_hi, int board_size,
                                CanonVersion version,
                                FlatU64Set& out_set) {
    const std::uint64_t hero_mask_raw =
        (1ULL << hero_lo) | (1ULL << hero_hi);

    int remaining_ids[50];
    int n_rem = 0;
    for (int id = 0; id < 52; ++id) {
        if (id != hero_lo && id != hero_hi) {
            remaining_ids[n_rem++] = id;
        }
    }

    std::vector<int> b_idx(board_size);
    std::iota(b_idx.begin(), b_idx.end(), 0);

    // The version is hoisted out of the 26M-iteration inner loop: `canon` is a
    // direct call to one concrete routine, not a switch per situation.
    auto run = [&](auto canon) {
        std::vector<int> idx = b_idx;
        do {
            std::uint64_t board_mask_raw = 0ULL;
            for (int i = 0; i < board_size; ++i) {
                board_mask_raw |= (1ULL << remaining_ids[idx[i]]);
            }
            auto [hero_canon, board_canon] = canon(hero_mask_raw, board_mask_raw);
            out_set.insert(pack_hero_board(hero_canon, board_canon));
        } while (next_combination(idx, n_rem));
    };

    switch (version) {
    case CanonVersion::Strict:
        run([](std::uint64_t h, std::uint64_t b) {
            return canonicalize_hero_and_board_strict(h, b);
        });
        return;
    }
    throw std::invalid_argument(
        "generate_canonical_situations: unknown canonicalization version " +
        std::to_string(static_cast<std::uint32_t>(version)));
}

std::vector<HeroBoardMask> generate_canonical_situations(int board_size,
                                                        CanonVersion version) {
    if (board_size < 3 || board_size > 5) {
        throw std::invalid_argument(
            "generate_canonical_situations: board_size must be 3 (flop), 4 (turn), or 5 (river)");
    }
    // Reject an out-of-range version before spending minutes enumerating.
    (void)canon_version_from_u32(static_cast<std::uint32_t>(version));

    // Final-set reserve size: the exact Burnside orbit count for S4 acting on
    // the suits (flop 1,286,792 / turn 13,960,050 / river 123,156,254), rounded
    // up for headroom.
    const std::size_t final_reserve =
        (board_size == 3) ?   1'350'000ULL :
        (board_size == 4) ?  14'500'000ULL :
                            130'000'000ULL;

    // Enumerate the 52C2 = 1326 hero pairs once.
    struct HeroPair { int lo, hi; };
    std::vector<HeroPair> hero_pairs;
    hero_pairs.reserve(1326);
    for (int i = 0; i < 52; ++i) {
        for (int j = i + 1; j < 52; ++j) {
            hero_pairs.push_back({i, j});
        }
    }

    // Pick a thread count. Each thread owns a private set, so there's
    // no contention in the hot loop. The final merge is single-threaded.
    //
    // Cap at 4 even on 8+ core machines: the inner loop is dominated by
    // random-access unordered_set inserts, and going past 4 saturates
    // the memory subsystem (measured on M3: 8 threads is ~10% slower
    // than 4 on the turn enumeration).
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    int n_threads = static_cast<int>(std::min<unsigned>(hw, 4u));
    if (n_threads <= 0) n_threads = 1;

    // Per-thread flat sets. The previous version sized them to
    // final/n_threads, but that assumed *cross-thread* dedup matched the
    // observed final count. In practice each thread sees many of the
    // same canonical keys from different hero-pair partitions, so the
    // per-thread unique count can be much closer to the final count
    // than (final / n_threads). Sizing to final_reserve here trades a
    // few hundred MB of peak RSS for a guaranteed flat load factor and
    // avoids the probe-cascade that overflow triggers in open
    // addressing.
    const std::size_t per_thread_reserve = final_reserve;
    std::vector<FlatU64Set> local_sets;
    local_sets.reserve(static_cast<std::size_t>(n_threads));
    for (int t = 0; t < n_threads; ++t) {
        local_sets.emplace_back(per_thread_reserve);
    }

    const std::size_t total_pairs = hero_pairs.size();
    auto worker = [&](int tid, std::size_t begin, std::size_t end) {
        auto& s = local_sets[static_cast<std::size_t>(tid)];
        for (std::size_t p = begin; p < end; ++p) {
            enumerate_hero_pair(hero_pairs[p].lo, hero_pairs[p].hi,
                                board_size, version, s);
        }
    };

    if (n_threads == 1) {
        worker(0, 0, total_pairs);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(n_threads));
        const std::size_t base   = total_pairs / static_cast<std::size_t>(n_threads);
        const std::size_t excess = total_pairs % static_cast<std::size_t>(n_threads);
        std::size_t pos = 0;
        for (int t = 0; t < n_threads; ++t) {
            std::size_t take = base + (static_cast<std::size_t>(t) < excess ? 1 : 0);
            std::size_t begin = pos;
            std::size_t end   = pos + take;
            pos = end;
            threads.emplace_back(worker, t, begin, end);
        }
        for (auto& th : threads) th.join();
    }

    // Merge thread-local flat sets into the final flat set (also dedups
    // across threads — many canonical keys are produced by more than
    // one unique hero pair).
    FlatU64Set unique_set(final_reserve);
    for (auto& s : local_sets) {
        s.for_each([&](std::uint64_t k) { unique_set.insert(k); });
        s.clear_and_shrink();
    }

    // Convert set to vector and return (unpack each key back to a pair)
    std::vector<HeroBoardMask> out;
    out.reserve(unique_set.size());
    unique_set.for_each([&](std::uint64_t packed) {
        out.push_back(unpack_hero_board(packed));
    });
    return out;
}

// Board-only canonical form: sort the four per-suit rank patterns descending
// and re-emit the i-th largest at suit i.  See canonicalize.h for why that is
// a canonical form and why the hero-less case of canonicalize_hero_and_board
// (which orders by top rank alone) is not.
//
// Ties are patterns that are equal as 13-bit words, i.e. suits that are
// genuinely interchangeable on this board.  The sort is stable, so they keep
// ascending suit order; the canonical mask is the same whichever way they fall,
// only suit_map differs, and fixing it deterministically keeps the permutation
// reproducible for callers that transport data through it.
static std::uint64_t canonicalize_board_core(std::uint64_t board_mask,
                                             int suit_map[4]) {
    std::uint32_t pat[4];
    for (int s = 0; s < 4; ++s) {
        pat[s] = static_cast<std::uint32_t>((board_mask >> (s * 13)) & 0x1FFFu);
    }

    // 4-way stable insertion sort of the suit indices by pattern descending.
    int ord[4] = {0, 1, 2, 3};
    for (int i = 1; i < 4; ++i) {
        const int cur = ord[i];
        int j = i;
        while (j > 0 && pat[ord[j - 1]] < pat[cur]) {
            ord[j] = ord[j - 1];
            --j;
        }
        ord[j] = cur;
    }

    for (int i = 0; i < 4; ++i) suit_map[ord[i]] = i;

    std::uint64_t canon = 0;
    for (int s = 0; s < 4; ++s) {
        canon |= static_cast<std::uint64_t>(pat[s]) << (suit_map[s] * 13);
    }
    return canon;
}

// Return canonicalized mask for board_mask alone
std::uint64_t canonicalize_board(std::uint64_t board_mask) {
    int suit_map[4];
    return canonicalize_board_core(board_mask, suit_map);
}

std::uint64_t canonicalize_board_with_suit_map(std::uint64_t board_mask,
                                               int suit_map_out[4]) {
    return canonicalize_board_core(board_mask, suit_map_out);
}

namespace detail {

std::uint64_t legacy_canonicalize_board(std::uint64_t board_mask) {
    // Deliberately routed through the untouched hero+board routine rather than
    // re-implemented: the frozen behaviour is *defined* as "that routine with
    // an empty hero", so delegation makes the two impossible to drift apart.
    return canonicalize_hero_and_board(0ULL, board_mask).second;
}

} // namespace detail

// Return canonicalized board as a Card array
std::vector<Card> canonicalize_board_cards(const std::vector<Card>& board) {
    // Convert to mask
    std::uint64_t board_mask      = cards_to_mask(board);
    std::uint64_t board_maskcanon = canonicalize_board(board_mask);

    // Convert canonicalized mask back to Card array
    return mask_to_cards(board_maskcanon);
}

// Normalize a 2-card hand to "AKs" / "AKo" / "TT" notation
std::string canonicalize_hand(const std::vector<Card>& hand) {
    if (hand.size() != 2) {
        throw std::invalid_argument("canonicalize_hand expects exactly 2 cards");
    }

    Card c1 = hand[0];
    Card c2 = hand[1];

    int r1 = c1.rank();
    int r2 = c2.rank();

    // Put the higher rank first
    int hi_rank = r1;
    int lo_rank = r2;
    int suit_hi = c1.suit();
    int suit_lo = c2.suit();

    if (r1 < r2) {
        hi_rank = r2;
        lo_rank = r1;
        suit_hi = c2.suit();
        suit_lo = c1.suit();
    }

    char hi_char = rank_to_char(hi_rank);
    char lo_char = rank_to_char(lo_rank);

    // Pairs are 2-char notation like "AA", "TT"
    if (hi_rank == lo_rank) {
        std::string res;
        res.push_back(hi_char);
        res.push_back(lo_char);
        return res;
    }

    // Check if suited
    bool suited = (suit_hi == suit_lo);

    std::string res;
    res.push_back(hi_char);
    res.push_back(lo_char);
    res.push_back(suited ? 's' : 'o');
    return res;
}

// Convert hero_mask (assumed 2 cards) to notation like "AKs"
std::string canonicalize_hand_mask(std::uint64_t hero_mask) {
    auto hero_cards = mask_to_cards(hero_mask);
    return canonicalize_hand(hero_cards);
}

} // namespace xiapl
