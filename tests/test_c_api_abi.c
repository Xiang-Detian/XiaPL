/* Pure-C conformance test for the waist v1 ABI.
 *
 * Two jobs, both of which a C++ test cannot do:
 *   1. It is the single source of truth for the numbers an FFI layer (Dart
 *      ffi, Swift, bindgen) must transcribe: one static assertion per struct
 *      field offset, plus the sizes and alignments.
 *   2. It proves the header is valid C and that the archive links from a C
 *      translation unit with no C++ entry point of its own.
 *
 * The behavioural depth lives in tests/test_c_api_waist.cpp; what runs here is
 * a smoke pass over each convention (status codes, the error slot, the buffer
 * protocol, zero-initialized options).
 */

#include <xiapl/c_api.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(cond)                                                  \
  do {                                                                 \
    if (!(cond)) {                                                     \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      exit(1);                                                         \
    }                                                                  \
  } while (0)

/* ---- Frozen constants --------------------------------------------------- */

_Static_assert(XIAPL_C_ABI_VERSION == 4u, "abi revision");
_Static_assert(XIAPL_FULL_DECK_MASK == ((UINT64_C(1) << 52) - 1), "deck mask");
_Static_assert(XIAPL_CARD_INVALID_ID == 255, "invalid card id");
_Static_assert(XIAPL_HAND_VALUE_MAX_KICKERS == 5, "kicker slots");
_Static_assert(XIAPL_STARTING_HAND_LABEL_SIZE == 4, "label stride");

_Static_assert(XIAPL_OK == 0, "status");
_Static_assert(XIAPL_ERR_INVALID_ARGUMENT == -1, "status");
_Static_assert(XIAPL_ERR_OUT_OF_RANGE == -2, "status");
_Static_assert(XIAPL_ERR_BAD_ALLOC == -3, "status");
_Static_assert(XIAPL_ERR_RUNTIME == -4, "status");

/* ---- POD layout: one line per field ------------------------------------- */

_Static_assert(sizeof(xiapl_hand_value_t) == 28, "abi");
_Static_assert(_Alignof(xiapl_hand_value_t) == 4, "abi");
_Static_assert(offsetof(xiapl_hand_value_t, category) == 0, "abi");
_Static_assert(offsetof(xiapl_hand_value_t, kicker_count) == 4, "abi");
_Static_assert(offsetof(xiapl_hand_value_t, kickers) == 8, "abi");

_Static_assert(sizeof(xiapl_sim_options_t) == 24, "abi");
_Static_assert(_Alignof(xiapl_sim_options_t) == 8, "abi");
_Static_assert(offsetof(xiapl_sim_options_t, seed) == 0, "abi");
_Static_assert(offsetof(xiapl_sim_options_t, iterations) == 8, "abi");
_Static_assert(offsetof(xiapl_sim_options_t, deterministic) == 12, "abi");
_Static_assert(offsetof(xiapl_sim_options_t, threads) == 16, "abi");
_Static_assert(offsetof(xiapl_sim_options_t, reserved) == 20, "abi");

_Static_assert(sizeof(xiapl_equity_summary_t) == 24, "abi");
_Static_assert(_Alignof(xiapl_equity_summary_t) == 8, "abi");
_Static_assert(offsetof(xiapl_equity_summary_t, chop_rate) == 0, "abi");
_Static_assert(offsetof(xiapl_equity_summary_t, trials) == 8, "abi");
_Static_assert(offsetof(xiapl_equity_summary_t, exact) == 16, "abi");
_Static_assert(offsetof(xiapl_equity_summary_t, reserved) == 20, "abi");

_Static_assert(sizeof(xiapl_range_equity_summary_t) == 40, "abi");
_Static_assert(_Alignof(xiapl_range_equity_summary_t) == 8, "abi");
_Static_assert(offsetof(xiapl_range_equity_summary_t, hero_aggregate_equity) == 0, "abi");
_Static_assert(offsetof(xiapl_range_equity_summary_t, villain_aggregate_equity) == 8, "abi");
_Static_assert(offsetof(xiapl_range_equity_summary_t, aggregate_std_error) == 16, "abi");
_Static_assert(offsetof(xiapl_range_equity_summary_t, trials) == 24, "abi");
_Static_assert(offsetof(xiapl_range_equity_summary_t, exact) == 32, "abi");
_Static_assert(offsetof(xiapl_range_equity_summary_t, reserved) == 36, "abi");

/* Card ids used below: id = suit * 13 + (rank - 2), suit 0=c,1=d,2=h,3=s. */
#define CARD_AS 51
#define CARD_KS 50
#define CARD_QS 49
#define CARD_JS 48
#define CARD_TS 47
#define CARD_AH 38
#define CARD_KD 24
#define CARD_KC 11
#define CARD_2C 0
#define CARD_3D 14

static uint64_t mask_of(const uint8_t *ids, int32_t n) {
  uint64_t mask = 0;
  REQUIRE(xiapl_cards_to_mask(ids, n, &mask) == XIAPL_OK);
  return mask;
}

static void check_version(void) {
  REQUIRE(xiapl_c_abi_version() == XIAPL_C_ABI_VERSION);

  int32_t major = -1, minor = -1, patch = -1;
  REQUIRE(xiapl_version(&major, &minor, &patch) == XIAPL_OK);
  REQUIRE(major >= 0 && minor >= 0 && patch >= 0);
  /* Every out pointer is optional on this one. */
  REQUIRE(xiapl_version(NULL, NULL, NULL) == XIAPL_OK);

  const char *text = xiapl_version_string();
  REQUIRE(text != NULL && strlen(text) > 0);
}

static void check_card(void) {
  uint8_t id = 0;
  REQUIRE(xiapl_card_from_string("As", &id) == XIAPL_OK);
  REQUIRE(id == CARD_AS);
  REQUIRE(xiapl_card_from_string("aS", &id) == XIAPL_OK && id == CARD_AS);

  REQUIRE(xiapl_card_from_rank_suit(14, 3, &id) == XIAPL_OK && id == CARD_AS);
  REQUIRE(xiapl_card_from_rank_suit(1, 3, &id) == XIAPL_ERR_INVALID_ARGUMENT);

  int32_t rank = 0, suit = 0;
  REQUIRE(xiapl_card_rank(CARD_AS, &rank) == XIAPL_OK && rank == 14);
  REQUIRE(xiapl_card_suit(CARD_AS, &suit) == XIAPL_OK && suit == 3);
  REQUIRE(xiapl_card_rank(XIAPL_CARD_INVALID_ID, &rank) == XIAPL_ERR_INVALID_ARGUMENT);

  /* Sentinel contract: an out-of-range id is an answer, not a failure. */
  REQUIRE(xiapl_card_from_id(200, &id) == XIAPL_OK);
  REQUIRE(id == XIAPL_CARD_INVALID_ID);
  REQUIRE(xiapl_card_from_id(CARD_AS, &id) == XIAPL_OK && id == CARD_AS);

  /* Buffer protocol (convention 7): query, exact fit, truncation. */
  int32_t length = -1;
  REQUIRE(xiapl_card_to_string(CARD_AS, NULL, 0, &length) == XIAPL_OK);
  REQUIRE(length == 2);

  char buf[8];
  memset(buf, 'x', sizeof buf);
  REQUIRE(xiapl_card_to_string(CARD_AS, buf, (int32_t)sizeof buf, &length) == XIAPL_OK);
  REQUIRE(length == 2 && strcmp(buf, "As") == 0);

  memset(buf, 'x', sizeof buf);
  REQUIRE(xiapl_card_to_string(CARD_AS, buf, 2, &length) == XIAPL_OK);
  REQUIRE(length == 2 && strcmp(buf, "A") == 0); /* truncated, still terminated */

  /* "<Card Invalid>" is 14 bytes and does not fit in an 8-byte buffer:
   * *out_length reports the full length, the buffer holds 7 bytes + NUL. */
  memset(buf, 'x', sizeof buf);
  REQUIRE(xiapl_card_repr(XIAPL_CARD_INVALID_ID, buf, (int32_t)sizeof buf, &length) == XIAPL_OK);
  REQUIRE(length == 14 && strcmp(buf, "<Card I") == 0);

  int32_t found = -1;
  REQUIRE(xiapl_try_rank_from_char('a', &rank, &found) == XIAPL_OK);
  REQUIRE(found == 1 && rank == 14);
  REQUIRE(xiapl_try_rank_from_char('z', &rank, &found) == XIAPL_OK);
  REQUIRE(found == 0 && rank == 0);
  REQUIRE(xiapl_try_suit_from_char('H', &suit, &found) == XIAPL_OK);
  REQUIRE(found == 1 && suit == 2);

  char c = 0;
  REQUIRE(xiapl_rank_to_char(10, &c) == XIAPL_OK && c == 'T');
  REQUIRE(xiapl_rank_to_char(1, &c) == XIAPL_ERR_INVALID_ARGUMENT);
}

static void check_masks(void) {
  const uint8_t pair[2] = {CARD_AS, CARD_AH};
  uint64_t mask = mask_of(pair, 2);
  REQUIRE(mask == ((UINT64_C(1) << CARD_AS) | (UINT64_C(1) << CARD_AH)));

  /* Duplicates collapse, exactly as in C++. */
  const uint8_t dup[3] = {CARD_AS, CARD_AS, CARD_AH};
  REQUIRE(mask_of(dup, 3) == mask);

  /* Empty input is legal; a NULL array with a positive count is not. */
  uint64_t empty = UINT64_C(1);
  REQUIRE(xiapl_cards_to_mask(NULL, 0, &empty) == XIAPL_OK && empty == 0);
  REQUIRE(xiapl_cards_to_mask(NULL, 2, &empty) == XIAPL_ERR_INVALID_ARGUMENT);
  REQUIRE(xiapl_cards_to_mask(pair, -1, &empty) == XIAPL_ERR_INVALID_ARGUMENT);

  const uint8_t bad[1] = {XIAPL_CARD_INVALID_ID};
  REQUIRE(xiapl_cards_to_mask(bad, 1, &empty) == XIAPL_ERR_INVALID_ARGUMENT);
  REQUIRE(xiapl_card_to_mask(CARD_AS, &empty) == XIAPL_OK);
  REQUIRE(empty == (UINT64_C(1) << CARD_AS));
  REQUIRE(xiapl_card_to_mask(XIAPL_CARD_INVALID_ID, &empty) == XIAPL_ERR_INVALID_ARGUMENT);

  /* Query-then-fill (convention 4): the query pass never touches a buffer,
   * a short buffer is not an error, bits above 51 are ignored. */
  int32_t total = -1;
  REQUIRE(xiapl_mask_to_ids(mask, NULL, 0, &total) == XIAPL_OK && total == 2);

  uint8_t ids[4] = {9, 9, 9, 9};
  REQUIRE(xiapl_mask_to_ids(mask, ids, 1, &total) == XIAPL_OK);
  REQUIRE(total == 2 && ids[0] == CARD_AH && ids[1] == 9);

  REQUIRE(xiapl_mask_to_ids(mask, ids, 4, &total) == XIAPL_OK);
  REQUIRE(total == 2 && ids[0] == CARD_AH && ids[1] == CARD_AS);

  REQUIRE(xiapl_mask_to_ids(mask | (UINT64_C(1) << 60), ids, 4, &total) == XIAPL_OK);
  REQUIRE(total == 2);

  int32_t per_hand = 0;
  REQUIRE(xiapl_cards_per_hand(XIAPL_GAME_HOLDEM, &per_hand) == XIAPL_OK && per_hand == 2);
  REQUIRE(xiapl_cards_per_hand(XIAPL_GAME_PLO, &per_hand) == XIAPL_OK && per_hand == 4);
  REQUIRE(xiapl_cards_per_hand(7, &per_hand) == XIAPL_ERR_INVALID_ARGUMENT);
}

static void check_eval(void) {
  const uint8_t royal[5] = {CARD_TS, CARD_JS, CARD_QS, CARD_KS, CARD_AS};
  xiapl_hand_value_t value;
  memset(&value, 0, sizeof value);
  REQUIRE(xiapl_evaluate_cards(royal, 5, &value) == XIAPL_OK);
  REQUIRE(value.category == XIAPL_HAND_STRAIGHT_FLUSH);
  REQUIRE(value.kicker_count >= 1 && value.kicker_count <= 5);

  xiapl_hand_value_t from_mask;
  memset(&from_mask, 0, sizeof from_mask);
  REQUIRE(xiapl_evaluate_mask(mask_of(royal, 5), &from_mask) == XIAPL_OK);
  REQUIRE(memcmp(&value, &from_mask, sizeof value) == 0);

  /* Classification: a duplicate card is invalid_argument, a bad card count is
   * runtime_error -- the C++ exception types, projected. */
  const uint8_t dup[5] = {CARD_AS, CARD_AS, CARD_QS, CARD_JS, CARD_TS};
  REQUIRE(xiapl_evaluate_cards(dup, 5, &value) == XIAPL_ERR_INVALID_ARGUMENT);
  REQUIRE(xiapl_evaluate_cards(royal, 3, &value) == XIAPL_ERR_RUNTIME);
  REQUIRE(xiapl_evaluate_cards(NULL, 5, &value) == XIAPL_ERR_INVALID_ARGUMENT);

  const uint8_t board_ids[5] = {CARD_QS, CARD_JS, CARD_TS, CARD_2C, CARD_3D};
  const uint8_t hero_ids[2] = {CARD_AS, CARD_AH};
  const uint8_t villain_ids[2] = {CARD_KD, CARD_KC};
  const uint64_t board = mask_of(board_ids, 5);
  const uint64_t hero = mask_of(hero_ids, 2);
  const uint64_t villain = mask_of(villain_ids, 2);

  REQUIRE(xiapl_evaluate_hand(board, hero, XIAPL_GAME_HOLDEM, &value) == XIAPL_OK);
  REQUIRE(value.category == XIAPL_HAND_ONE_PAIR);
  REQUIRE(xiapl_evaluate_hand(board, hero, 9, &value) == XIAPL_ERR_INVALID_ARGUMENT);
  /* Hold'em wants exactly 2 hole cards; 5 is a runtime_error from C++. */
  REQUIRE(xiapl_evaluate_hand(board, mask_of(royal, 5), XIAPL_GAME_HOLDEM, &value)
          == XIAPL_ERR_RUNTIME);

  const uint64_t holes[2] = {hero, villain};
  int32_t winners[2] = {-1, -1};
  int32_t count = -1;
  REQUIRE(xiapl_judge(holes, 2, board, XIAPL_GAME_HOLDEM, winners, 2, &count) == XIAPL_OK);
  REQUIRE(count == 1 && winners[0] == 0);
  /* Bounded output: too small a buffer fails up front, it is not a query. */
  REQUIRE(xiapl_judge(holes, 2, board, XIAPL_GAME_HOLDEM, winners, 1, &count)
          == XIAPL_ERR_INVALID_ARGUMENT);
  /* No players is a runtime_error from C++, not a boundary rejection. */
  REQUIRE(xiapl_judge(holes, 0, board, XIAPL_GAME_HOLDEM, winners, 0, &count)
          == XIAPL_ERR_RUNTIME);

  int32_t length = -1;
  char buf[64];
  REQUIRE(xiapl_hand_category_name(XIAPL_HAND_FULL_HOUSE, buf, (int32_t)sizeof buf,
                                   &length) == XIAPL_OK);
  REQUIRE(strcmp(buf, "Full House") == 0 && length == 10);
  REQUIRE(xiapl_hand_category_name(0, buf, (int32_t)sizeof buf, &length)
          == XIAPL_ERR_INVALID_ARGUMENT);

  REQUIRE(xiapl_describe_hand(&from_mask, buf, (int32_t)sizeof buf, &length) == XIAPL_OK);
  REQUIRE(length > 0 && (int32_t)strlen(buf) == length);
}

static void check_options(void) {
  /* Zero-initialized is the library default: exact enumeration. */
  xiapl_sim_options_t zero;
  memset(&zero, 0, sizeof zero);
  int32_t mode = -1;
  REQUIRE(xiapl_sim_options_effective_mode(&zero, &mode) == XIAPL_OK);
  REQUIRE(mode == XIAPL_SIM_EXACT);

  xiapl_sim_options_t exact;
  memset(&exact, 0xAB, sizeof exact);
  REQUIRE(xiapl_sim_options_exact(&exact) == XIAPL_OK);
  REQUIRE(memcmp(&exact, &zero, sizeof zero) == 0);

  xiapl_sim_options_t mc;
  REQUIRE(xiapl_sim_options_mc_random(1000, &mc) == XIAPL_OK);
  REQUIRE(mc.iterations == 1000 && mc.deterministic == 0 && mc.reserved == 0);
  REQUIRE(xiapl_sim_options_effective_mode(&mc, &mode) == XIAPL_OK);
  REQUIRE(mode == XIAPL_SIM_MONTE_CARLO_RANDOM);

  REQUIRE(xiapl_sim_options_mc_seeded(1000, UINT64_C(42), &mc) == XIAPL_OK);
  REQUIRE(mc.iterations == 1000 && mc.deterministic == 1 && mc.seed == 42);
  REQUIRE(mc.threads == 0 && mc.reserved == 0);
  REQUIRE(xiapl_sim_options_effective_mode(&mc, &mode) == XIAPL_OK);
  REQUIRE(mode == XIAPL_SIM_MONTE_CARLO_SEEDED);

  REQUIRE(xiapl_sim_options_effective_mode(NULL, &mode) == XIAPL_ERR_INVALID_ARGUMENT);
  REQUIRE(xiapl_sim_options_exact(NULL) == XIAPL_ERR_INVALID_ARGUMENT);
}

static void check_error_slot(void) {
  /* Never NULL, "" with nothing recorded on this thread. */
  xiapl_clear_last_error();
  REQUIRE(xiapl_last_error_message() != NULL);
  REQUIRE(strcmp(xiapl_last_error_message(), "") == 0);

  /* A successful call records nothing. */
  uint8_t id = 0;
  REQUIRE(xiapl_card_from_string("As", &id) == XIAPL_OK);
  REQUIRE(strcmp(xiapl_last_error_message(), "") == 0);

  /* A failure fills the slot with the C++ message. */
  REQUIRE(xiapl_card_from_string("Zz", &id) == XIAPL_ERR_INVALID_ARGUMENT);
  REQUIRE(strlen(xiapl_last_error_message()) > 0);

  xiapl_clear_last_error();
  REQUIRE(strcmp(xiapl_last_error_message(), "") == 0);
}

int main(void) {
  check_version();
  check_card();
  check_masks();
  check_eval();
  check_options();
  check_error_slot();
  printf("test_c_api_abi: all checks passed\n");
  return 0;
}
