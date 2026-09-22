#ifndef XIAPL_C_API_H
#define XIAPL_C_API_H

/* ===========================================================================
 * xiapl C ABI -- waist v1
 * ===========================================================================
 *
 * This header IS the contract. Every language binding (Python included) is
 * built on this file and on nothing else: there is no binding that sees the
 * C++ headers. The waist is a mechanical projection of the Tier-2 (id / mask)
 * integer contract described in the API architecture charter; it invents no
 * representation of its own and contains no presentation logic (no sorting,
 * no labelling, no formatting that the C++ API does not already perform).
 *
 * The waist is a SUBSET of the C++ public API. If a behaviour is not in
 * the public C++ headers, it is not here either.
 *
 * ---- ABI stability policy (0.x) -------------------------------------------
 *
 * While xiapl is at 0.x this ABI is UNSTABLE. Struct layouts, field order,
 * field meanings and function signatures may change on any 0.MINOR release.
 * There is no source compatibility promise and no binary compatibility
 * promise: the library is distributed as a static archive (and as a static
 * xcframework for iOS), so every consumer relinks against the exact headers it
 * was built with. We do not use a cbSize / struct-size negotiation protocol,
 * because the "old caller, new shared library" scenario it exists for does not
 * occur in this distribution model.
 *
 * What we DO promise, so that a break is loud instead of silent:
 *
 *   * XIAPL_C_ABI_VERSION is bumped on every layout or semantic change.
 *   * xiapl_c_abi_version() returns the value the linked library was built
 *     with.
 *   * FFI layers that redeclare these structs (Dart `dart:ffi`, hand-written
 *     Swift/Kotlin bridges, bindgen output pinned in a repo) MUST check
 *     XIAPL_C_ABI_VERSION == xiapl_c_abi_version() once at init and refuse to
 *     run on a mismatch. That single check is the supported way to detect a
 *     stale binding; without it a stale struct declaration is undefined
 *     behavior, not an error code.
 *   * Newly added struct fields are always chosen so that a zero-initialized
 *     struct (`xiapl_sim_options_t o = {0};`) selects the previous default
 *     behaviour. Zero-initialization is, and stays, the library default.
 *   * Struct layout convention: 8-byte-aligned members (uint64_t, double,
 *     pointers) come first, then 4-byte members, then explicit `reserved`
 *     padding so that no struct in this header has implicit interior or tail
 *     padding on any supported ABI. The pinned sizeof/offsetof values live in
 *     the pure-C ABI test.
 *   * Once the library is distributed as a SHARED object, breaking changes
 *     switch from "bump XIAPL_C_ABI_VERSION" to "rename the symbol"
 *     (`xiapl_foo_v2`), and removals of the old symbol are called out in the
 *     release notes.
 *
 * At 1.0 this section will be replaced by a real stability guarantee.
 *
 * ---- Boundary conventions --------------------------------------------------
 *
 * These hold for EVERY function below; per-function comments only note
 * departures.
 *
 * 1. Card representation. A card is a 52-bit `uint64_t` mask (bit `id` set) or
 *    a `uint8_t` id in [0, 51], with the frozen relation
 *        id = suit * 13 + (rank - 2),   rank in [2, 14], suit 0=c,1=d,2=h,3=s
 *    Masks are the primary currency; id arrays are used only where ORDER or
 *    MULTIPLICITY carries meaning that a mask cannot express (deck order,
 *    duplicate-card detection). There are no negative "empty slot" sentinels
 *    anywhere in this ABI; a variable number of cards is always
 *    `const uint64_t*`/`const uint8_t*` plus an explicit `int32_t` count.
 *    XIAPL_CARD_INVALID_ID (255) is not a slot sentinel: it is the C++
 *    `Card::INVALID_ID` value, and it appears only where the C++ API itself
 *    produces an invalid Card.
 *
 * 2. Return values. Every function returns `int32_t` (an xiapl_status_t
 *    value); results travel through `out_*` pointers. No function returns a
 *    struct by value. The four exceptions, all deliberate:
 *      - xiapl_c_abi_version() returns uint32_t (kept verbatim from ABI v2).
 *      - xiapl_last_error_message() returns const char* (SQLite convention).
 *      - xiapl_version_string() returns const char* (a static compile-time
 *        constant; an infallible query gains nothing from a buffer protocol).
 *      - the *_destroy family returns void: releasing a handle has no failure
 *        a caller could act on, and giving it a status would invite callers to
 *        branch on a value that is always XIAPL_OK.
 *
 * 3. Options. Input option blocks are plain POD structs passed by const
 *    pointer. A zero-initialized options struct always means "library
 *    default".
 *
 * 4. Variable-length output: SoA + caller-allocated + query-then-fill.
 *    The uniform shape is
 *        (..., <T>* out_array, int32_t <name>_capacity, int32_t* out_total)
 *    with these rules:
 *      - `*out_total` is ALWAYS set to the total number of available
 *        elements, whether or not a buffer was supplied.
 *      - The function writes min(capacity, *out_total) elements. Passing
 *        `NULL` / capacity 0 is the query call and is never an error.
 *      - A short buffer is NOT an error: the caller detects truncation by
 *        comparing capacity against `*out_total`.
 *    Parallel arrays (e.g. combo masks and combo weights) are separate
 *    pointers so each lands zero-copy in BigUint64Array / Float64Array /
 *    numpy / Float64List / UnsafeBufferPointer.
 *
 *    Two families cannot use query-then-fill and say so explicitly:
 *      - Functions whose output cardinality is already bounded by an input
 *        (per-player equity, showdown winners) simply require a big enough
 *        buffer; recomputing an expensive simulation for a query pass would be
 *        absurd.
 *      - Functions whose output cardinality is NOT known in advance AND whose
 *        computation is expensive (range equity, canonical-situation
 *        enumeration) return an opaque RESULT HANDLE. The caller queries and
 *        fills from the handle as many times as it likes, then destroys it.
 *      - Deck dealing MUTATES the deck, so a query pass would consume cards.
 *        Those functions validate the capacity BEFORE touching the deck.
 *
 * 5. Ownership. No ownership crosses this boundary except through opaque
 *    handles. Every buffer a function writes into is caller-allocated and
 *    caller-owned; the library never retains a pointer past the call. Every
 *    `xiapl_*_create*` / result-producing function that yields a handle is
 *    paired with exactly one `xiapl_*_destroy`.
 *
 * 6. Handles. Opaque pointers with explicit destroy. Garbage-collected
 *    languages MUST attach a finalizer (Dart `NativeFinalizer`, Node
 *    `napi_add_finalizer`, Swift `deinit`, Python `__del__`/capsule
 *    destructor); the library has no other way to learn that a handle died.
 *
 * 7. Strings.
 *    - Input: NUL-terminated UTF-8 `const char*`, owned by the caller for the
 *      duration of the call only.
 *    - Output: caller buffer + query, in the uniform shape
 *          (..., char* out_buf, int32_t buf_capacity, int32_t* out_length)
 *      `*out_length` is always the length in bytes EXCLUDING the terminating
 *      NUL. When `out_buf` is non-NULL and `buf_capacity > 0` the function
 *      writes at most `buf_capacity` bytes and always NUL-terminates
 *      (truncating if necessary). The query call is `out_buf == NULL` or
 *      `buf_capacity <= 0`; allocate `*out_length + 1` bytes and call again.
 *    - Error detail: xiapl_last_error_message(), see rule 8.
 *
 * 8. Errors. The return code carries the CLASS of failure (which maps 1:1 onto
 *    the originating C++ exception type); the human-readable detail lives in a
 *    thread-local slot readable with xiapl_last_error_message().
 *
 *    Thread contract, both halves of it:
 *      (a) Library side: when a function returns a non-OK status it has
 *          ALREADY stored the message in the THREAD-LOCAL slot OF THE CALLING
 *          THREAD. If the failure originated in an internal worker thread, the
 *          library aggregates it onto the calling thread before returning.
 *          There is no other thread on which the message can be observed.
 *      (b) Binding side: a binding MUST read the message on the SAME THREAD
 *          that received the non-OK status, before returning control upward or
 *          re-acquiring a runtime lock (for Python: before re-acquiring the
 *          GIL hands the thread back). Deferring the read across a thread
 *          switch reads a different slot.
 *    The returned pointer is owned by the library and stays valid until the
 *    next xiapl_* call ON THE SAME THREAD. It is never NULL: with no recorded
 *    error it points at "". Copy it if you need to keep it.
 *
 * 9. Callbacks. There are none, by design, and none will be added: they would
 *    force `NativeCallable.listener` + isolates on Dart and threadsafe
 *    functions on Node onto every binding. If cancellation is ever needed the
 *    mechanism is decided in advance: a `volatile int32_t*` flag the library
 *    polls, never a callback.
 *
 * 10. Types. Fixed-width only (`int32_t`, `int64_t`, `uint8_t`, `uint64_t`,
 *     `double`, `char`). Enums exist as named constants for readability, but
 *     every enum-valued FIELD and PARAMETER is declared `int32_t`, because the
 *     underlying type of a C enum is implementation-defined and binding
 *     generators disagree about it. There is no `bool`, no union, no bitfield
 *     and no variadic function in this ABI. Boolean values are `int32_t`,
 *     0 = false, non-zero = true; the library always WRITES 0 or 1.
 *
 * 11. Threading. Free functions are thread-safe: the library holds no mutable
 *     global state other than the thread-local error slot. Handles are not
 *     internally synchronized -- concurrent calls on the SAME handle require
 *     external synchronization if any of them mutates it. `xiapl_range_t` and
 *     the result handles are immutable after creation and may be read
 *     concurrently; `xiapl_deck_t` is mutable and may not.
 *
 *     `xiapl_sim_options_t::threads` is a pure speed knob and, when set to a
 *     non-zero value, ALWAYS overrides the XIAPL_NUM_THREADS environment
 *     variable. This is the contract, not an implementation detail: browsers
 *     and mobile embedders cannot set environment variables, so the explicit
 *     field has to win.
 *
 * 12. Determinism. A seeded Monte Carlo run is bit-identical at every
 *     `threads` value, including serial. This is a public cross-language
 *     contract, and it is what the golden cross-language conformance vectors
 *     are allowed to assume.
 *
 * ---- Naming ---------------------------------------------------------------
 *
 *   xiapl::foo(...)             ->  xiapl_foo(...)             (name verbatim)
 *   xiapl::Type::bar(...)       ->  xiapl_<type>_bar(...)      (receiver first)
 *   xiapl::Type::Type(...)      ->  xiapl_<type>_create[_...]
 *   (destructor)                ->  xiapl_<type>_destroy
 *   static Type Type::baz(...)  ->  xiapl_<type>_create_<baz>  (it yields a Type)
 *   overload sets               ->  suffix on the non-primary member
 *                                   (shuffle()/shuffle(seed) ->
 *                                    xiapl_deck_shuffle / xiapl_deck_shuffle_seeded)
 *
 * Output parameters are prefixed `out_`. Capacities are `<thing>_capacity`.
 * Types are `xiapl_<name>_t`; macros and enumerators are `XIAPL_<SCREAMING>`.
 * =========================================================================== */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Export decoration
 *
 * Static builds intentionally leave the decoration empty so C consumers can
 * link the installed static target without defining any xiapl macro. Shared
 * builds are compiled with -fvisibility=hidden and export ONLY the xiapl_*
 * symbols below; a .so that also exported the C++ core would collide with
 * other C++ runtimes loaded into the same Node/Dart process.
 * ------------------------------------------------------------------------ */
#if defined(_WIN32)
  #if defined(XIAPL_BUILD_SHARED)
    #if defined(XIAPL_C_API_EXPORTS)
      #define XIAPL_API __declspec(dllexport)
    #else
      #define XIAPL_API __declspec(dllimport)
    #endif
  #else
    #define XIAPL_API
  #endif
#elif defined(XIAPL_BUILD_SHARED)
  #define XIAPL_API __attribute__((visibility("default")))
#else
  #define XIAPL_API
#endif

/* ---------------------------------------------------------------------------
 * ABI revision
 *
 * Bumped on EVERY change to the layout or the meaning of anything below.
 * v3 is the first waist ABI; v2 was the app-shaped Flutter ABI it replaces.
 *
 * v4 keeps v3's layout and signatures and changes exactly one meaning: the
 * legacy hero+board canonicalization was removed from the C++ core, so
 * XIAPL_CANON_LEGACY (version code 1) is no longer a generation this
 * library can run and every function taking a version answers it with
 * XIAPL_ERR_INVALID_ARGUMENT. Nothing was renamed, renumbered or resized -- a
 * v3 caller recompiles unchanged, and only one that actually passed 1 sees a
 * behaviour change. It is a bump rather than a silent fix because a caller
 * that keyed artefacts by version 1 must find out at init, not at lookup time.
 * ------------------------------------------------------------------------ */
#define XIAPL_C_ABI_VERSION 4u

/* Returns the XIAPL_C_ABI_VERSION the linked library was built with.
 * Thread-safe, never fails. */
XIAPL_API uint32_t xiapl_c_abi_version(void);

/* ---------------------------------------------------------------------------
 * Frozen Tier-2 constants
 * ------------------------------------------------------------------------ */

/* All 52 bits set: (1ULL << 52) - 1. The public card-mask universe. */
#define XIAPL_FULL_DECK_MASK UINT64_C(0x000FFFFFFFFFFFFF)

/* The value of an invalid / default-constructed card id. Mirrors
 * xiapl::Card::INVALID_ID. It is produced (never consumed as a sentinel) by
 * xiapl_card_from_id and by an empty deck deal. */
#define XIAPL_CARD_INVALID_ID 255

/* Maximum kickers in a hand value (mirrors xiapl::HandValue::kickers). */
#define XIAPL_HAND_VALUE_MAX_KICKERS 5

/* Bytes per canonical starting-hand label slot, NUL included ("AKs\0"). */
#define XIAPL_STARTING_HAND_LABEL_SIZE 4

/* ---------------------------------------------------------------------------
 * Status codes
 *
 * The classification is exactly the originating C++ exception type, so every
 * binding can map it to its own idiom without the library knowing about that
 * idiom. The reference mapping (Python) is:
 *
 *   XIAPL_ERR_INVALID_ARGUMENT  <- std::invalid_argument  -> ValueError
 *   XIAPL_ERR_OUT_OF_RANGE      <- std::out_of_range      -> IndexError
 *   XIAPL_ERR_BAD_ALLOC         <- std::bad_alloc         -> MemoryError
 *   XIAPL_ERR_RUNTIME           <- anything else          -> RuntimeError
 *
 * A NULL pointer where one is required, and a negative count, are always
 * XIAPL_ERR_INVALID_ARGUMENT. A too-small output buffer is NOT an error
 * (see convention 4). Values are negative so that `rc < 0` is a valid
 * "did it fail" test in every language.
 * ------------------------------------------------------------------------ */
typedef enum xiapl_status_t {
  XIAPL_OK                   =  0,
  XIAPL_ERR_INVALID_ARGUMENT = -1,
  XIAPL_ERR_OUT_OF_RANGE     = -2,
  XIAPL_ERR_BAD_ALLOC        = -3,
  XIAPL_ERR_RUNTIME          = -4
} xiapl_status_t;

/* Detail message for the most recent non-OK status returned ON THIS THREAD.
 *
 * The message is the originating C++ exception's what() carried through
 * VERBATIM -- no prefixing, no reformatting, no truncation. Bindings that
 * surface it as an exception message therefore reproduce the C++ message
 * byte for byte, which is what the Python conformance gate pins.
 *
 * Never NULL ("" when nothing has failed on this thread). Owned by the
 * library; valid until the next xiapl_* call on this thread. See boundary
 * convention 8 for the full two-sided thread contract. */
XIAPL_API const char* xiapl_last_error_message(void);

/* Reset this thread's error slot to "". Optional; every non-OK return
 * overwrites the slot anyway. Provided so that a binding (or a test) can
 * prove a message it read belongs to the call it just made. */
XIAPL_API void xiapl_clear_last_error(void);

/* ---------------------------------------------------------------------------
 * Library version (mirrors <xiapl/version.h>)
 * ------------------------------------------------------------------------ */

/* Semantic version of the linked library. Any out pointer may be NULL.
 * Thread-safe, never fails. */
XIAPL_API int32_t xiapl_version(int32_t* out_major,
                                int32_t* out_minor,
                                int32_t* out_patch);

/* Version as a string, e.g. "0.1.0". Returns a pointer to a static
 * compile-time constant: never NULL, valid for the process lifetime, owned by
 * the library. One of the four documented return-convention exceptions
 * (convention 2). Thread-safe, never fails. */
XIAPL_API const char* xiapl_version_string(void);

/* ---------------------------------------------------------------------------
 * Enumerations
 *
 * Declared as C enums for readability only. Every parameter and every struct
 * field that carries one of these is declared int32_t (convention 10).
 * ------------------------------------------------------------------------ */

/* Poker variant. Selects hole-card width and showdown rule everywhere. */
typedef enum xiapl_game_t {
  XIAPL_GAME_HOLDEM = 0,
  XIAPL_GAME_PLO    = 1
} xiapl_game_t;

/* Five-card hand class, ascending in strength. Values mirror
 * xiapl::HandCategory and start at 1, not 0. */
typedef enum xiapl_hand_category_t {
  XIAPL_HAND_HIGH_CARD      = 1,
  XIAPL_HAND_ONE_PAIR       = 2,
  XIAPL_HAND_TWO_PAIR       = 3,
  XIAPL_HAND_THREE_OF_A_KIND= 4,
  XIAPL_HAND_STRAIGHT       = 5,
  XIAPL_HAND_FLUSH          = 6,
  XIAPL_HAND_FULL_HOUSE     = 7,
  XIAPL_HAND_FOUR_OF_A_KIND = 8,
  XIAPL_HAND_STRAIGHT_FLUSH = 9
} xiapl_hand_category_t;

/* Effective sampling mode of an options block. DERIVED from
 * (iterations, deterministic) -- see xiapl_sim_options_effective_mode. */
typedef enum xiapl_simulation_mode_t {
  XIAPL_SIM_EXACT              = 0, /* full enumeration; seed ignored       */
  XIAPL_SIM_MONTE_CARLO_RANDOM = 1, /* seed drawn from std::random_device   */
  XIAPL_SIM_MONTE_CARLO_SEEDED = 2  /* seed taken from options.seed         */
} xiapl_simulation_mode_t;

/* Output mode of xiapl_calculate_range_equity. */
typedef enum xiapl_range_equity_mode_t {
  XIAPL_RANGE_EQUITY_PER_COMBO      = 0,
  XIAPL_RANGE_EQUITY_AGGREGATE_ONLY = 1
} xiapl_range_equity_mode_t;

/* Which hero+board canonicalization an artefact or a lookup was built with.
 * These are ON-DISK values; they are never renumbered, and a code is never
 * reused once issued -- not even after the generation it names is gone.
 *
 * XIAPL_CANON_LEGACY is RETIRED as of ABI v4: the legacy generation was deleted
 * from the C++ core, so passing 1 to any function below is
 * XIAPL_ERR_INVALID_ARGUMENT. This build cannot reproduce legacy keys, and
 * quietly answering with strict keys instead would make every lookup against a
 * legacy-keyed artefact miss. The constant stays declared because 1 is still
 * stamped into artefacts on disk and an FFI layer needs a name to recognize it
 * by; reproducing such artefacts needs a build from before the removal.
 *
 * XIAPL_CANON_STRICT is therefore the only value this library accepts. */
typedef enum xiapl_canon_version_t {
  XIAPL_CANON_LEGACY = 1, /* RETIRED at ABI v4: always INVALID_ARGUMENT     */
  XIAPL_CANON_STRICT = 2  /* a true canonical form; the only one accepted   */
} xiapl_canon_version_t;

/* ---------------------------------------------------------------------------
 * Opaque handles
 * ------------------------------------------------------------------------ */

/* A parsed, immutable, GameType-tagged set of weighted combos. */
typedef struct xiapl_range xiapl_range_t;

/* A mutable, ordered stack of remaining cards. */
typedef struct xiapl_deck xiapl_deck_t;

/* The result of one range-vs-range equity computation. */
typedef struct xiapl_range_equity xiapl_range_equity_t;

/* The result of one canonical-situation enumeration. */
typedef struct xiapl_canonical_situations xiapl_canonical_situations_t;

/* ---------------------------------------------------------------------------
 * Plain-old-data structs
 * ------------------------------------------------------------------------ */

/* Best five-card value. Mirrors xiapl::HandValue field for field.
 *
 * Transparent on purpose: there is exactly ONE representation of a hand value
 * at this boundary, so no binding ever has to decode a packed score, and no
 * decode logic can drift between bindings. The library's internal packed
 * uint32 score is an implementation detail of the evaluator and is not part
 * of this ABI.
 *
 * `category` holds an xiapl_hand_category_t value. `kickers[0 .. kicker_count)`
 * are rank values in [2, 14], most significant first; the remaining slots are
 * zero. Zero-initializing this struct does NOT produce a meaningful hand
 * value -- it is an output type only. */
typedef struct xiapl_hand_value_t {
  int32_t category;                                 /*  0 */
  int32_t kicker_count;                             /*  4  0..5 */
  int32_t kickers[XIAPL_HAND_VALUE_MAX_KICKERS];    /*  8 */
} xiapl_hand_value_t;                               /* sizeof 28, alignof 4 */

/* Options controlling enumeration / Monte Carlo behaviour. Mirrors
 * xiapl::SimulationOptions.
 *
 * ZERO-INITIALIZED IS THE LIBRARY DEFAULT: {0} means exact enumeration on an
 * automatically chosen number of threads. That invariant is permanent.
 *
 * Field meanings:
 *   seed          Monte Carlo stream selector. Used only when
 *                 `deterministic != 0`. 0 is an ordinary, valid seed, not a
 *                 sentinel.
 *   iterations    0  = exact enumeration.
 *                 >0 = Monte Carlo trial count. If it meets or exceeds the
 *                      number of distinct board completions, the library
 *                      silently runs exact enumeration instead (sampling with
 *                      replacement past the exhaustive count is strictly worse
 *                      on both axes). The result reports what actually ran in
 *                      its `exact` field; compare against `iterations` to
 *                      detect the fallback. The AggregateOnly Monte Carlo path
 *                      of xiapl_calculate_range_equity is the one exception:
 *                      it samples the PAIR dimension and never falls back.
 *   deterministic 0 = seed from std::random_device, non-zero = use `seed`.
 *                 Ignored when iterations == 0.
 *   threads       0  = auto, 1 = serial, N = exactly N workers,
 *                 negative = treated as 1. Purely a speed knob: a seeded run
 *                 is bit-identical at every value (convention 12). A non-zero
 *                 value always overrides XIAPL_NUM_THREADS (convention 11).
 *                 Embedders running inside their own thread pool pass 1.
 *   reserved      MUST be 0. */
typedef struct xiapl_sim_options_t {
  uint64_t seed;           /*  0 */
  int32_t  iterations;     /*  8 */
  int32_t  deterministic;  /* 12  boolean */
  int32_t  threads;        /* 16 */
  int32_t  reserved;       /* 20  must be 0 */
} xiapl_sim_options_t;     /* sizeof 24, alignof 8 */

/* Non-per-player part of an equity result. Mirrors the scalar members of
 * xiapl::EquityResult. */
typedef struct xiapl_equity_summary_t {
  double   chop_rate;  /*  0  probability the hand chopped                  */
  uint64_t trials;     /*  8  board completions actually evaluated          */
  int32_t  exact;      /* 16  boolean: full enumeration actually ran        */
  int32_t  reserved;   /* 20 */
} xiapl_equity_summary_t; /* sizeof 24, alignof 8 */

/* Non-per-combo part of a range equity result. Mirrors the scalar members of
 * xiapl::RangeEquityResult. */
typedef struct xiapl_range_equity_summary_t {
  double   hero_aggregate_equity;    /*  0 */
  double   villain_aggregate_equity; /*  8 */
  double   aggregate_std_error;      /* 16  filled only by AggregateOnly MC */
  uint64_t trials;                   /* 24 */
  int32_t  exact;                    /* 32  boolean */
  int32_t  reserved;                 /* 36 */
} xiapl_range_equity_summary_t;      /* sizeof 40, alignof 8 */

/* ===========================================================================
 * Card  (mirrors <xiapl/card.h>)
 *
 * A card has no handle: it IS its uint8_t id. These functions exist so that no
 * binding has to re-implement parsing or formatting; the id<->rank/suit
 * arithmetic itself is a published Tier-2 contract, so a binding MAY inline it
 * in a hot loop instead of calling across the boundary.
 * ========================================================================= */

/* Build a card id from rank in [2, 14] and suit in [0, 3].
 * XIAPL_ERR_INVALID_ARGUMENT if either is out of range. */
XIAPL_API int32_t xiapl_card_from_rank_suit(int32_t rank,
                                            int32_t suit,
                                            uint8_t* out_id);

/* Parse a two-character card string, e.g. "As", "Td". Rank and suit letters
 * are accepted in either case. XIAPL_ERR_INVALID_ARGUMENT on any malformed
 * input; the message names which part failed. */
XIAPL_API int32_t xiapl_card_from_string(const char* text, uint8_t* out_id);

/* Non-throwing id factory. Mirrors xiapl::Card::from_id EXACTLY, including
 * its currently-unadjudicated contract: an id outside [0, 51] yields
 * XIAPL_CARD_INVALID_ID rather than an error. This function returns XIAPL_OK
 * for every input except a NULL out pointer. Callers that want validation
 * should test the result against XIAPL_CARD_INVALID_ID, or use
 * xiapl_card_from_rank_suit. */
XIAPL_API int32_t xiapl_card_from_id(uint8_t id, uint8_t* out_id);

/* Rank in [2, 14] / suit in [0, 3] of a card id.
 * XIAPL_ERR_INVALID_ARGUMENT if `id` is not in [0, 51]. */
XIAPL_API int32_t xiapl_card_rank(uint8_t id, int32_t* out_rank);
XIAPL_API int32_t xiapl_card_suit(uint8_t id, int32_t* out_suit);

/* Display form: "As", or "Invalid" for XIAPL_CARD_INVALID_ID. Buffer protocol
 * per convention 7. Mirrors xiapl::Card::to_string. */
XIAPL_API int32_t xiapl_card_to_string(uint8_t id,
                                       char* out_buf,
                                       int32_t buf_capacity,
                                       int32_t* out_length);

/* Debug form: "<Card As>", or "<Card Invalid>". Mirrors xiapl::Card::repr.
 * Present because it is a C++ public member; the waist carries no formatting
 * the C++ API does not already define. */
XIAPL_API int32_t xiapl_card_repr(uint8_t id,
                                  char* out_buf,
                                  int32_t buf_capacity,
                                  int32_t* out_length);

/* Non-throwing character parsers (xiapl::try_rank_from_char /
 * try_suit_from_char). `*out_found` is set to 1 on success and 0 on an
 * unrecognized character, and the status is XIAPL_OK either way: "not a rank
 * character" is an answer, not a failure. `*out_rank` / `*out_suit` are set
 * to 0 when not found. Upper and lower case are both accepted. */
XIAPL_API int32_t xiapl_try_rank_from_char(char c,
                                           int32_t* out_rank,
                                           int32_t* out_found);
XIAPL_API int32_t xiapl_try_suit_from_char(char c,
                                           int32_t* out_suit,
                                           int32_t* out_found);

/* Rank in [2, 14] -> '2'..'9', 'T', 'J', 'Q', 'K', 'A'.
 * XIAPL_ERR_INVALID_ARGUMENT outside that range. */
XIAPL_API int32_t xiapl_rank_to_char(int32_t rank, char* out_char);

/* ===========================================================================
 * Mask utilities  (mirrors <xiapl/utils.h>)
 * ========================================================================= */

/* Single card id -> 52-bit mask. XIAPL_ERR_INVALID_ARGUMENT if the id is not
 * in [0, 51] (XIAPL_CARD_INVALID_ID included). */
XIAPL_API int32_t xiapl_card_to_mask(uint8_t id, uint64_t* out_mask);

/* Card ids -> 52-bit mask (bitwise OR; duplicates collapse silently, exactly
 * as in C++). XIAPL_ERR_INVALID_ARGUMENT on any id not in [0, 51], or on a
 * negative count. `ids` may be NULL only when `count` is 0.
 *
 * This single function covers both xiapl::cards_to_mask and the validating
 * half of xiapl::ids_to_mask: at this boundary a card IS an id, so the two
 * collapse. The "-1 means empty slot" behaviour of the C++ int-array overload
 * is deliberately NOT carried across (convention 1: no negative sentinels). */
XIAPL_API int32_t xiapl_cards_to_mask(const uint8_t* ids,
                                      int32_t count,
                                      uint64_t* out_mask);

/* 52-bit mask -> ascending card ids. Query-then-fill per convention 4.
 * Bits above 51 are ignored. This is the waist form of BOTH
 * xiapl::mask_to_cards and xiapl::mask_to_ids -- they differ only in the
 * language-side wrapper type. */
XIAPL_API int32_t xiapl_mask_to_ids(uint64_t mask,
                                    uint8_t* out_ids,
                                    int32_t ids_capacity,
                                    int32_t* out_total);

/* Hole cards dealt per player for `game`: 2 for Hold'em, 4 for PLO.
 * XIAPL_ERR_INVALID_ARGUMENT on an unknown game. */
XIAPL_API int32_t xiapl_cards_per_hand(int32_t game, int32_t* out_count);

/* ===========================================================================
 * Hand evaluation  (mirrors <xiapl/eval.h>, <xiapl/hand_value.h>)
 * ========================================================================= */

/* Best five-card value out of 5 to 7 loose cards, given as ids. Game-agnostic:
 * there is no hole/board split, so every card is usable.
 *
 * The id-array form is kept alongside the mask form because it can see
 * information a mask cannot: XIAPL_ERR_INVALID_ARGUMENT on a DUPLICATE card,
 * XIAPL_ERR_RUNTIME on a bad card count. `ids` may not be NULL. */
XIAPL_API int32_t xiapl_evaluate_cards(const uint8_t* ids,
                                       int32_t count,
                                       xiapl_hand_value_t* out_value);

/* Same, taking a 52-bit mask with 5 to 7 bits set.
 * XIAPL_ERR_RUNTIME on a bad popcount. */
XIAPL_API int32_t xiapl_evaluate_mask(uint64_t card_mask,
                                      xiapl_hand_value_t* out_value);

/* Best five-card value for one player's hole cards against a board, under
 * `game`'s showdown rule (Hold'em: best five of 2 hole + 3..5 board, any mix;
 * PLO: best five of exactly 2 hole + exactly 3 board).
 *
 * `board_mask` must have 3 to 5 bits set, `hole_mask` exactly
 * xiapl_cards_per_hand(game) bits, and the two must not overlap. */
XIAPL_API int32_t xiapl_evaluate_hand(uint64_t board_mask,
                                      uint64_t hole_mask,
                                      int32_t game,
                                      xiapl_hand_value_t* out_value);

/* Showdown. Writes the ASCENDING indices of every player holding the best hand
 * under `game`'s rule; more than one index means a chop.
 *
 * Output cardinality is bounded by `num_players`, so there is no query pass:
 * `winners_capacity` must be >= num_players or the call fails with
 * XIAPL_ERR_INVALID_ARGUMENT before doing any work. `*out_count` receives the
 * number of winners written.
 *
 * `board_mask` must have 3 to 5 bits set; every player_hole_masks[i] must have
 * xiapl_cards_per_hand(game) bits set and must not overlap the board or
 * another player. An empty player list is an error -- there is no showdown
 * without players. Hold'em additionally rejects more than 10 players
 * (full-ring seat cap); PLO's cap is 32. All of these are XIAPL_ERR_RUNTIME,
 * matching the C++ contract. */
XIAPL_API int32_t xiapl_judge(const uint64_t* player_hole_masks,
                              int32_t num_players,
                              uint64_t board_mask,
                              int32_t game,
                              int32_t* out_winners,
                              int32_t winners_capacity,
                              int32_t* out_count);

/* Human-readable description of a hand value: the category name, followed by
 * the kicker ranks in brackets when there are any, e.g. "Full House [13 2]".
 * Buffer protocol per convention 7. Mirrors xiapl::describe_hand. */
XIAPL_API int32_t xiapl_describe_hand(const xiapl_hand_value_t* value,
                                      char* out_buf,
                                      int32_t buf_capacity,
                                      int32_t* out_length);

/* Name of a hand category, e.g. "Full House". Mirrors
 * xiapl::to_string(HandCategory). XIAPL_ERR_INVALID_ARGUMENT on an unknown
 * category. Buffer protocol per convention 7. */
XIAPL_API int32_t xiapl_hand_category_name(int32_t category,
                                           char* out_buf,
                                           int32_t buf_capacity,
                                           int32_t* out_length);

/* ===========================================================================
 * Simulation options  (mirrors xiapl::SimulationOptions)
 *
 * The three factories exist so that "what the library considers a well-formed
 * exact / random / seeded options block" has exactly one definition, shared by
 * every binding. Filling the struct by hand is equally supported.
 * ========================================================================= */

XIAPL_API int32_t xiapl_sim_options_exact(xiapl_sim_options_t* out_options);

XIAPL_API int32_t xiapl_sim_options_mc_random(int32_t iterations,
                                              xiapl_sim_options_t* out_options);

XIAPL_API int32_t xiapl_sim_options_mc_seeded(int32_t iterations,
                                              uint64_t seed,
                                              xiapl_sim_options_t* out_options);

/* The effective xiapl_simulation_mode_t implied by (iterations,
 * deterministic). There is deliberately no `mode` FIELD: the mode is derived,
 * and a stored copy could contradict the fields it is derived from. */
XIAPL_API int32_t xiapl_sim_options_effective_mode(
    const xiapl_sim_options_t* options,
    int32_t* out_mode);

/* ===========================================================================
 * Equity: fixed hands vs a board  (mirrors xiapl::calculate_equity)
 * ========================================================================= */

/* Reproducible equity for 2..N fixed hands against a fixed board.
 *
 * hole_masks[i] is player i's hole cards (2 bits for Hold'em, 4 for PLO).
 * board_mask has 0, 3, 4 or 5 bits set; 1 and 2 are XIAPL_ERR_RUNTIME.
 * Player-count caps: 10 for Hold'em, 32 for PLO; exceeding either is
 * XIAPL_ERR_INVALID_ARGUMENT raised up front, on the calling thread, before
 * any trial runs.
 *
 * Output is SoA and its cardinality equals `num_players`, so there is no query
 * pass: each of `out_winrate` / `out_equity` / `out_std_error` may be NULL if
 * that column is not wanted, but any non-NULL one must have room for
 * `num_players` doubles (`players_capacity` >= num_players, otherwise
 * XIAPL_ERR_INVALID_ARGUMENT before any work). `out_summary` may be NULL.
 *
 *   out_winrate[i]    outright win rate, no chop share
 *   out_equity[i]     expected pot share, chop included
 *   out_std_error[i]  Monte Carlo standard error of equity; 0.0 when exact
 *
 * Note that exact BOARD enumeration always runs single-threaded here; raising
 * `iterations` past the enumeration threshold can therefore reduce wall-clock
 * parallelism while improving accuracy. */
XIAPL_API int32_t xiapl_calculate_equity(const uint64_t* hole_masks,
                                         int32_t num_players,
                                         uint64_t board_mask,
                                         const xiapl_sim_options_t* options,
                                         int32_t game,
                                         double* out_winrate,
                                         double* out_equity,
                                         double* out_std_error,
                                         int32_t players_capacity,
                                         xiapl_equity_summary_t* out_summary);

/* ===========================================================================
 * Range  (mirrors <xiapl/range.h>)
 *
 * A range is an immutable, GameType-tagged, mask-ascending set of weighted
 * combos. Every constructor validates that each combo mask has exactly
 * xiapl_cards_per_hand(game) bits set.
 *
 * A combo is NOT a struct at this boundary: combos travel as the parallel
 * arrays (uint64_t mask[], double weight[]) so that each column lands
 * zero-copy in a typed array on the other side.
 * ========================================================================= */

/* Empty Hold'em range (mirrors the default constructor). */
XIAPL_API int32_t xiapl_range_create(xiapl_range_t** out_range);

/* Range from an explicit combo list, bypassing the notation parser.
 * `masks` and `weights` are parallel arrays of `count` entries; `weights` may
 * be NULL, meaning weight 1.0 for every combo. XIAPL_ERR_INVALID_ARGUMENT on
 * the first mask whose popcount is not xiapl_cards_per_hand(game); the message
 * names the offending mask. */
XIAPL_API int32_t xiapl_range_create_from_combos(const uint64_t* masks,
                                                 const double* weights,
                                                 int32_t count,
                                                 int32_t game,
                                                 xiapl_range_t** out_range);

/* Parse range notation. Hold'em notation for XIAPL_GAME_HOLDEM, the frozen
 * v0.1 four-rank PLO notation for XIAPL_GAME_PLO.
 *
 * XIAPL_ERR_INVALID_ARGUMENT on malformed input; the parser's own message
 * (always pure ASCII) is carried verbatim in xiapl_last_error_message(), which
 * is the whole reason this ABI has a message channel at all. A well-formed but
 * unsatisfiable pattern is NOT malformed: it yields an EMPTY range, so callers
 * that care must check xiapl_range_size / xiapl_range_empty. */
XIAPL_API int32_t xiapl_range_create_from_string(const char* text,
                                                 int32_t game,
                                                 xiapl_range_t** out_range);

/* Every combo of `game` at weight 1.0: 1,326 for Hold'em, 270,725 for PLO,
 * in ascending mask order. */
XIAPL_API int32_t xiapl_range_create_all(int32_t game,
                                         xiapl_range_t** out_range);

/* Non-throwing Hold'em parse (mirrors xiapl::try_parse_range, which returns
 * std::optional). On a parse failure the status is XIAPL_OK and
 * `*out_range` is set to NULL -- "this is not a range" is an answer, not a
 * failure, and no error message is recorded. `*out_range` is NULL on every
 * non-OK status too, so `rc == XIAPL_OK && *out_range != NULL` is the single
 * success test. */
XIAPL_API int32_t xiapl_try_parse_range(const char* text,
                                        xiapl_range_t** out_range);

/* Release a range handle. NULL is a no-op. */
XIAPL_API void xiapl_range_destroy(xiapl_range_t* range);

/* Number of combos / emptiness / game tag. */
XIAPL_API int32_t xiapl_range_size(const xiapl_range_t* range,
                                   int32_t* out_size);
XIAPL_API int32_t xiapl_range_empty(const xiapl_range_t* range,
                                    int32_t* out_empty);
XIAPL_API int32_t xiapl_range_game(const xiapl_range_t* range,
                                   int32_t* out_game);

/* All combos, ascending by mask. Query-then-fill per convention 4; either
 * column pointer may independently be NULL. `*out_total` always equals
 * xiapl_range_size. */
XIAPL_API int32_t xiapl_range_combos(const xiapl_range_t* range,
                                     uint64_t* out_masks,
                                     double* out_weights,
                                     int32_t combos_capacity,
                                     int32_t* out_total);

/* The combos that do not intersect `dead_mask`, ascending by mask.
 * Query-then-fill; a query pass is cheap (it is a popcount filter, not a
 * simulation). */
XIAPL_API int32_t xiapl_range_valid_combos(const xiapl_range_t* range,
                                           uint64_t dead_mask,
                                           uint64_t* out_masks,
                                           double* out_weights,
                                           int32_t combos_capacity,
                                           int32_t* out_total);

/* Summed weight of the combos that do not intersect `dead_mask`
 * (dead_mask = 0 sums the whole range). */
XIAPL_API int32_t xiapl_range_total_weight(const xiapl_range_t* range,
                                           uint64_t dead_mask,
                                           double* out_weight);

/* ---------------------------------------------------------------------------
 * Weighted set algebra (max/min lattice, mirrors xiapl::range_union etc.)
 *
 *   union         w = max(wa, wb)
 *   intersection  w = min(wa, wb)   (combo must be present in both)
 *   difference    w = max(0, wa - wb)   (combo dropped when it reaches 0)
 *
 * PURE FUNCTIONS: each returns a NEW handle and never mutates either operand.
 * There is no in-place form and none will be added -- a range is an immutable
 * value, and the C++ operations return a new Range.
 *
 * Both operands must carry the same game tag; a mismatch is
 * XIAPL_ERR_INVALID_ARGUMENT naming both games. The result is sorted ascending
 * by mask, so downstream seeded Monte Carlo never depends on how the operands
 * were spelled. A duplicated mask inside either operand is
 * XIAPL_ERR_INVALID_ARGUMENT: only the raw-combo constructor can build one and
 * there is no honest answer for what a set operation should do with it.
 *
 * The caller owns the returned handle and must destroy it.
 * ------------------------------------------------------------------------ */
XIAPL_API int32_t xiapl_range_union(const xiapl_range_t* a,
                                    const xiapl_range_t* b,
                                    xiapl_range_t** out_range);
XIAPL_API int32_t xiapl_range_intersection(const xiapl_range_t* a,
                                           const xiapl_range_t* b,
                                           xiapl_range_t** out_range);
XIAPL_API int32_t xiapl_range_difference(const xiapl_range_t* a,
                                         const xiapl_range_t* b,
                                         xiapl_range_t** out_range);

/* ---------------------------------------------------------------------------
 * Top-percent starting hands
 *
 * Canonical Hold'em starting-hand LABELS ("AA", "AKs", "AQo", ...) ranked by
 * EXACT equity against a uniformly random opponent and a uniformly random
 * board, strongest first, truncated to floor(169 * top_percent) labels.
 * Deterministic: backed by a compile-time table, no RNG and no platform
 * variance.
 *
 * Honest reading of the metric: vs-random equity is a WEAK notion of preflop
 * strength -- it over-ranks small pairs and offsuit aces and under-ranks
 * suited connectors. This is hand strength in a vacuum, not an opening range,
 * and bindings must not present it as one.
 *
 * top_percent must be in (0.0, 1.0]; anything else is
 * XIAPL_ERR_INVALID_ARGUMENT. Only XIAPL_GAME_HOLDEM is ranked in 0.1;
 * XIAPL_GAME_PLO is XIAPL_ERR_INVALID_ARGUMENT naming the roadmap item.
 * ------------------------------------------------------------------------ */

/* Labels are written into a fixed-stride buffer: slot i occupies
 * out_labels[i * XIAPL_STARTING_HAND_LABEL_SIZE ...] and is NUL-terminated
 * inside its slot. `out_labels` must therefore have room for
 * `labels_capacity * XIAPL_STARTING_HAND_LABEL_SIZE` bytes. Query-then-fill
 * per convention 4 (`*out_total` is the untruncated label count). */
XIAPL_API int32_t xiapl_rank_starting_hands(double top_percent,
                                            int32_t game,
                                            char* out_labels,
                                            int32_t labels_capacity,
                                            int32_t* out_total);

/* The same selection as a Range at weight 1.0 -- every combo of every kept
 * label. Equivalent to parsing the comma-joined output of
 * xiapl_rank_starting_hands. The caller owns the returned handle. */
XIAPL_API int32_t xiapl_generate_top_percent_range(double top_percent,
                                                   int32_t game,
                                                   xiapl_range_t** out_range);

/* ===========================================================================
 * Range-vs-range equity  (mirrors xiapl::calculate_range_equity)
 *
 * This is the one place where the query-then-fill rule would be actively
 * harmful: the per-combo breakdown's cardinality is not known before the
 * computation (only combos with at least one valid pairing appear), and a
 * query pass would mean running the simulation twice. So the computation
 * yields a RESULT HANDLE instead, which the caller queries and fills from as
 * many times as it likes and then destroys. The handle owns nothing the caller
 * gave it and is immutable, hence safe to read from several threads.
 * ========================================================================= */

/* Heads-up range-vs-range equity.
 *
 * Both ranges must carry the same game tag; a mismatch is
 * XIAPL_ERR_INVALID_ARGUMENT naming both. board_mask has 0..5 bits set.
 * `mode` is an xiapl_range_equity_mode_t:
 *
 *   PER_COMBO (0)       per-combo breakdown plus aggregates, common-random-
 *                       number board set. Bit-identical at every `threads`.
 *   AGGREGATE_ONLY (1)  aggregates only; both breakdowns come back empty.
 *                       With iterations > 0 this is a sampled-pair Monte Carlo
 *                       estimator that does NOT share a random stream with
 *                       PER_COMBO (the two modes agree statistically, not
 *                       bit-exactly) and never auto-falls back to exact. With
 *                       iterations == 0 the two modes run the same enumeration
 *                       and agree bit-exactly.
 *
 * XIAPL_ERR_RUNTIME when exact mode would exceed the ~512 MB evaluation-cache
 * cap (wide preflop ranges, and PLO far sooner than Hold'em; the message names
 * AGGREGATE_ONLY + a seeded Monte Carlo as the remedy), and when
 * AGGREGATE_ONLY Monte Carlo cannot find a card-compatible hero/villain pair.
 * PER_COMBO instead returns zeroed aggregates for mutually blocking ranges.
 *
 * On success the caller owns `*out_result`. */
XIAPL_API int32_t xiapl_calculate_range_equity(
    const xiapl_range_t* hero_range,
    const xiapl_range_t* villain_range,
    uint64_t board_mask,
    const xiapl_sim_options_t* options,
    int32_t mode,
    xiapl_range_equity_t** out_result);

/* Release a range equity result. NULL is a no-op. */
XIAPL_API void xiapl_range_equity_destroy(xiapl_range_equity_t* result);

/* Scalar part of the result. */
XIAPL_API int32_t xiapl_range_equity_summary(
    const xiapl_range_equity_t* result,
    xiapl_range_equity_summary_t* out_summary);

/* Per-combo breakdown, three parallel columns; any column pointer may be NULL.
 * Query-then-fill per convention 4, and free of charge here: the result is
 * already materialized inside the handle. Both are empty in AGGREGATE_ONLY
 * mode. */
XIAPL_API int32_t xiapl_range_equity_hero(const xiapl_range_equity_t* result,
                                          uint64_t* out_masks,
                                          double* out_equities,
                                          double* out_weights,
                                          int32_t entries_capacity,
                                          int32_t* out_total);

XIAPL_API int32_t xiapl_range_equity_villain(const xiapl_range_equity_t* result,
                                             uint64_t* out_masks,
                                             double* out_equities,
                                             double* out_weights,
                                             int32_t entries_capacity,
                                             int32_t* out_total);

/* ===========================================================================
 * Deck  (mirrors <xiapl/deck.h>)
 *
 * A deck is an ORDERED, mutable stack of remaining cards, so it is exposed as
 * ordered id arrays rather than masks: a mask cannot express draw order, which
 * is exactly what a deck is for. Dealing takes from the END of the array.
 *
 * The exposed vocabulary is the frozen minimal set; investigational methods
 * are not added here any more than they are added to the C++ or Python
 * surface. One Python-visible convenience is deliberately absent because it is
 * an exact composition of what IS here:
 *   deal_one()  == xiapl_deck_deal(d, 1, ...), yielding XIAPL_CARD_INVALID_ID
 *                  when nothing was written (the C++ behaviour verbatim).
 * A deck handle is mutable and is NOT internally synchronized.
 * ========================================================================= */

/* A full 52-card deck in ascending id order, NOT shuffled: deterministic state
 * by default. Call xiapl_deck_shuffle / xiapl_deck_shuffle_seeded to
 * randomize. */
XIAPL_API int32_t xiapl_deck_create(xiapl_deck_t** out_deck);

/* A deck holding exactly these cards, in this order. Every id must be in
 * [0, 51] and the list must be duplicate-free; either violation is
 * XIAPL_ERR_INVALID_ARGUMENT. `ids` may be NULL only when `count` is 0. */
XIAPL_API int32_t xiapl_deck_create_from_cards(const uint8_t* ids,
                                               int32_t count,
                                               xiapl_deck_t** out_deck);

/* Release a deck handle. NULL is a no-op. */
XIAPL_API void xiapl_deck_destroy(xiapl_deck_t* deck);

/* Shuffle from std::random_device: NOT reproducible. */
XIAPL_API int32_t xiapl_deck_shuffle(xiapl_deck_t* deck);

/* Reproducible shuffle: the same seed always produces the same permutation.
 * Seed 0 is a valid, ordinary seed here, not a request for randomness. This
 * single implementation is the reason Deck is on the waist at all -- a
 * binding that reimplemented the shuffle would have to reproduce the RNG bit
 * for bit to keep the determinism contract. */
XIAPL_API int32_t xiapl_deck_shuffle_seeded(xiapl_deck_t* deck, uint64_t seed);

/* Deal up to `n` cards off the end, writing their ids in the order dealt.
 *
 * MUTATING, so there is no query pass -- a query would consume the cards.
 * `out_ids` must not be NULL and `ids_capacity` must be >= n; otherwise the
 * call fails with XIAPL_ERR_INVALID_ARGUMENT and the deck is UNTOUCHED.
 * `*out_count` receives the number actually dealt, which is less than `n`
 * when the deck runs out (that is not an error). `n <= 0` deals nothing. */
XIAPL_API int32_t xiapl_deck_deal(xiapl_deck_t* deck,
                                  int32_t n,
                                  uint8_t* out_ids,
                                  int32_t ids_capacity,
                                  int32_t* out_count);

/* Deal up to `n` cards and return them as a 52-bit mask (draw order is
 * discarded -- this is the Tier-2 boundary form). 0 if the deck is empty. */
XIAPL_API int32_t xiapl_deck_deal_mask(xiapl_deck_t* deck,
                                       int32_t n,
                                       uint64_t* out_mask);

/* Discard up to `n` cards off the end. */
XIAPL_API int32_t xiapl_deck_burn(xiapl_deck_t* deck, int32_t n);

/* Restore all 52 cards. `shuffle` is a boolean: 0 (the deterministic default)
 * leaves ascending id order, non-zero shuffles with std::random_device. */
XIAPL_API int32_t xiapl_deck_reset(xiapl_deck_t* deck, int32_t shuffle);

/* Remove the listed cards wherever they sit. Ids that are not present are
 * skipped silently (the C++ behaviour). */
XIAPL_API int32_t xiapl_deck_remove_cards(xiapl_deck_t* deck,
                                          const uint8_t* ids,
                                          int32_t count);

/* Replace the contents with exactly these cards, in this order. Validated
 * before anything is mutated: every id in [0, 51], no duplicates. */
XIAPL_API int32_t xiapl_deck_set_cards(xiapl_deck_t* deck,
                                       const uint8_t* ids,
                                       int32_t count);

/* Remaining card ids, front to back (the next card dealt is the LAST entry).
 * Query-then-fill per convention 4; non-mutating, so the query pass is safe.
 * This is the waist form of both `cards` and `card_ids` on higher surfaces --
 * they differ only in the wrapper type the binding builds. */
XIAPL_API int32_t xiapl_deck_get_cards(const xiapl_deck_t* deck,
                                       uint8_t* out_ids,
                                       int32_t ids_capacity,
                                       int32_t* out_total);

/* Remaining card count / emptiness / "are there at least n cards left". */
XIAPL_API int32_t xiapl_deck_size(const xiapl_deck_t* deck, int32_t* out_size);
XIAPL_API int32_t xiapl_deck_empty(const xiapl_deck_t* deck, int32_t* out_empty);
XIAPL_API int32_t xiapl_deck_has_cards(const xiapl_deck_t* deck,
                                       int32_t n,
                                       int32_t* out_has);

/* Display form: the remaining cards concatenated front to back, e.g.
 * "2c3c4c...". Mirrors xiapl::Deck::repr. Buffer protocol per convention 7.
 * (52 cards -> 104 bytes + NUL.) */
XIAPL_API int32_t xiapl_deck_repr(const xiapl_deck_t* deck,
                                  char* out_buf,
                                  int32_t buf_capacity,
                                  int32_t* out_length);

/* ===========================================================================
 * Canonicalization  (mirrors <xiapl/canonicalize.h>)
 *
 * All of these only ever RELABEL SUITS; ranks are never touched.
 *
 * The Card-taking twins of the C++ API are absent here on purpose: each of
 * them is literally cards -> mask -> canonicalize -> mask -> cards, so a
 * binding reproduces them bit-for-bit by composing xiapl_cards_to_mask, the
 * mask function, and xiapl_mask_to_ids. The ONE exception is
 * xiapl_canonicalize_hand_ids, below, whose card form is observably different
 * from its mask form.
 * ========================================================================= */

/* Canonicalize a (hero, board) pair under the frozen pre-v2 suit ordering
 * (suits ranked by their top board rank, hero rank then suit index as
 * tie-breaks).
 *
 * FROZEN: its behaviour can never change. It is NOT an abstraction generation
 * and it is NOT XIAPL_CANON_LEGACY -- version 1 was retired as a selectable
 * generation while this exact relabelling stayed, because a handful of callers
 * depend on it and on nothing else (the public card-level API, and artefacts
 * that were keyed by it before the retirement). For an abstraction-keyed
 * lookup use xiapl_canonicalize_hero_and_board_v with XIAPL_CANON_STRICT,
 * which is a different representative and will disagree with this function on
 * the same input. */
XIAPL_API int32_t xiapl_canonicalize_hero_and_board(uint64_t hero_mask,
                                                    uint64_t board_mask,
                                                    uint64_t* out_hero_mask,
                                                    uint64_t* out_board_mask);

/* Canonicalize a (hero, board) pair under an explicit xiapl_canon_version_t.
 * XIAPL_CANON_STRICT is the only accepted value; XIAPL_CANON_LEGACY and every
 * other code are XIAPL_ERR_INVALID_ARGUMENT rather than a silent fallback,
 * because guessing here would run a different card abstraction than the
 * artefact was built for. The version is validated by the C++ core, not by
 * this boundary, so the status and the message are the C++ ones. */
XIAPL_API int32_t xiapl_canonicalize_hero_and_board_v(uint64_t hero_mask,
                                                      uint64_t board_mask,
                                                      int32_t version,
                                                      uint64_t* out_hero_mask,
                                                      uint64_t* out_board_mask);

/* Canonicalize a board alone (a true canonical form: two boards share a
 * representative iff they are suit-isomorphic). Any card count is accepted. */
XIAPL_API int32_t xiapl_canonicalize_board(uint64_t board_mask,
                                           uint64_t* out_board_mask);

/* Two-card hand -> "AKs" / "AKo" / "TT" notation, from a 52-bit mask.
 * XIAPL_ERR_INVALID_ARGUMENT unless the mask has exactly 2 bits set.
 * Buffer protocol per convention 7. */
XIAPL_API int32_t xiapl_canonicalize_hand_mask(uint64_t hero_mask,
                                               char* out_buf,
                                               int32_t buf_capacity,
                                               int32_t* out_length);

/* The same from an id ARRAY. Not redundant with the mask form: a mask cannot
 * represent a duplicated card, so "exactly two cards" and "exactly two
 * DISTINCT cards" are different questions and the C++ API answers them
 * differently. This form mirrors xiapl::canonicalize_hand(vector<Card>)
 * exactly, including its arity check. */
XIAPL_API int32_t xiapl_canonicalize_hand_ids(const uint8_t* ids,
                                              int32_t count,
                                              char* out_buf,
                                              int32_t buf_capacity,
                                              int32_t* out_length);

/* ---------------------------------------------------------------------------
 * Canonical situation enumeration
 *
 * Every distinct (hero_mask, board_mask) representative for a board of
 * `board_size` cards: 3 = flop, 4 = turn, 5 = river. `version` must be
 * XIAPL_CANON_STRICT -- the only generation this library still enumerates --
 * and the populations are then the exact Burnside orbit counts for S4 acting
 * on the suits: flop 1,286,792, turn 13,960,050, river 123,156,254.
 * XIAPL_CANON_LEGACY used to enumerate 1,420,796 flop representatives and is
 * XIAPL_ERR_INVALID_ARGUMENT as of ABI v4. The version stays an explicit
 * parameter with no default so that what a caller generated is recorded rather
 * than inferred.
 *
 * `board_size` is validated BEFORE `version`, matching the C++ routine: a call
 * that gets both wrong reports the board size.
 *
 * This returns a handle for the same reason range equity does -- the
 * cardinality is not knowable without doing the work -- and additionally
 * because the work is very large: the river population is ~123 million pairs,
 * about 2 GB as two uint64 columns. The fill function therefore takes an
 * `offset` so a caller can stream it in bounded chunks instead of allocating
 * the whole thing. Counts and offsets here are int64_t (they are the only
 * quantities in this ABI not bounded by a small constant or by an input array
 * length).
 * ------------------------------------------------------------------------ */
XIAPL_API int32_t xiapl_generate_canonical_situations(
    int32_t board_size,
    int32_t version,
    xiapl_canonical_situations_t** out_situations);

/* Release an enumeration. NULL is a no-op. */
XIAPL_API void xiapl_canonical_situations_destroy(
    xiapl_canonical_situations_t* situations);

/* Total number of (hero, board) pairs held. */
XIAPL_API int32_t xiapl_canonical_situations_count(
    const xiapl_canonical_situations_t* situations,
    int64_t* out_total);

/* Copy `count` pairs starting at `offset` into two parallel columns; either
 * column pointer may be NULL. `*out_written` receives the number actually
 * copied, which is less than `count` at the end of the enumeration and 0 when
 * `offset` is past it. A negative `offset` or `count` is
 * XIAPL_ERR_INVALID_ARGUMENT; an `offset` past the end is not (it is how a
 * streaming loop terminates). */
XIAPL_API int32_t xiapl_canonical_situations_fill(
    const xiapl_canonical_situations_t* situations,
    int64_t offset,
    int64_t count,
    uint64_t* out_hero_masks,
    uint64_t* out_board_masks,
    int64_t* out_written);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XIAPL_C_API_H */
