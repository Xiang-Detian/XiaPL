// examples/evaluate_hand.cpp
//
// Show xiapl_core's hand evaluation API:
//   - parse cards via Card::from_string / try_parse_card
//   - evaluate 5-card / 7-card boards via evaluate_cards / evaluate_mask
//   - evaluate Hold'em and PLO hole+board via evaluate_hand(..., game)
//   - showdown winners for Hold'em and PLO via judge(..., game)
//   - compare HandValue results with operator<
//
// Build (from project root):
//   cmake -S . -B build -DXIAPL_BUILD_EXAMPLES=ON
//   cmake --build build --target evaluate_hand
//   ./build/examples/evaluate_hand
#include <xiapl/card.h>
#include <xiapl/eval.h>
#include <xiapl/hand_value.h>
#include <xiapl/utils.h>

#include <iostream>
#include <string>
#include <vector>

using namespace xiapl;

namespace {

std::vector<Card> parse_cards(const std::vector<std::string>& names) {
    std::vector<Card> out;
    out.reserve(names.size());
    for (const auto& n : names) {
        out.push_back(Card::from_string(n));
    }
    return out;
}

void show(const std::string& label, const HandValue& v) {
    std::cout << "  " << label << " : " << describe_hand(v) << "\n";
}

} // namespace

int main() {
    std::cout << "=== evaluate_cards (5 cards) ===\n";
    auto royal = parse_cards({"As", "Ks", "Qs", "Js", "Ts"});
    auto quads = parse_cards({"Ah", "Ad", "Ac", "As", "Kc"});
    auto two_pair = parse_cards({"Ah", "Ad", "Kh", "Kd", "2c"});
    show("Royal flush (AKQJT spades)", evaluate_cards(royal));
    show("Quad aces, K kicker      ", evaluate_cards(quads));
    show("Aces and kings, 2 kicker ", evaluate_cards(two_pair));

    std::cout << "\n=== evaluate_cards (7 cards: 2 hole + 5 board) ===\n";
    // Board makes the nut flush draw a 7-card flush.
    auto seven = parse_cards({"As", "Ks", "Qd", "Jd", "Th", "9s", "8s"});
    show("AKQJT9s8s -> straight flush?", evaluate_cards(seven));

    std::cout << "\n=== evaluate_hand, Hold'em (board + 2 hole, mask form) ===\n";
    auto hole = parse_cards({"As", "Ks"});
    auto board = parse_cards({"Qs", "Js", "Ts", "2c", "3d"});
    auto hv = evaluate_hand(cards_to_mask(board), cards_to_mask(hole));
    show("AsKs on Qs Js Ts 2c 3d   ", hv);

    std::cout << "\n=== evaluate_hand, PLO (board + 4 hole) ===\n";
    auto plo_hole = parse_cards({"As", "Ks", "Qd", "Jc"});
    auto plo_board = parse_cards({"Ts", "9s", "8c", "4h", "2d"});
    auto plo_hv = evaluate_hand(cards_to_mask(plo_board),
                                cards_to_mask(plo_hole), GameType::Plo);
    show("PLO AsKsQdJc / TsTs9s8c4h2d", plo_hv);

    std::cout << "\n=== judge, Hold'em (multiway showdown, mask form) ===\n";
    // Reuse the Hold'em hole/board fixture above, plus one more contender.
    auto villain_hole = parse_cards({"Ah", "Kh"});
    auto winners = judge({cards_to_mask(hole), cards_to_mask(villain_hole)},
                         cards_to_mask(board));
    std::cout << "  AsKs vs AhKh on Qs Js Ts 2c 3d -> winner index/indices:";
    for (int w : winners) std::cout << " " << w;
    std::cout << " (more than one index would mean a chop)\n";

    std::cout << "\n=== judge, PLO (multiway showdown, mask form) ===\n";
    // Reuse the PLO hole/board fixture above, plus one more contender.
    auto plo_villain_hole = parse_cards({"Ac", "Kd", "Qh", "Jh"});
    auto plo_winners = judge(
        {cards_to_mask(plo_hole), cards_to_mask(plo_villain_hole)},
        cards_to_mask(plo_board), GameType::Plo);
    std::cout << "  PLO AsKsQdJc vs AcKdQhJh on Ts 9s 8c 4h 2d -> winner index/indices:";
    for (int w : plo_winners) std::cout << " " << w;
    std::cout << "\n";

    std::cout << "\n=== HandValue ordering (operator<) ===\n";
    HandValue v_pair  = evaluate_cards(parse_cards({"Ah", "As", "5c", "3d", "2h"}));
    HandValue v_trip  = evaluate_cards(parse_cards({"Ah", "As", "Ac", "3d", "2h"}));
    HandValue v_flush = evaluate_cards(parse_cards({"Ah", "Kh", "Qh", "5h", "2h"}));
    std::cout << "  pair  < trips  : " << (v_pair < v_trip)   << "\n";
    std::cout << "  trips < flush  : " << (v_trip < v_flush)  << "\n";
    std::cout << "  flush > pair   : " << (v_flush > v_pair)  << "\n";

    std::cout << "\n=== try_parse_card vs Card::from_string ===\n";
    if (auto good = try_parse_card("Ah")) {
        std::cout << "  try_parse_card(\"Ah\") -> " << good->to_string() << "\n";
    }
    if (!try_parse_card("Xx").has_value()) {
        std::cout << "  try_parse_card(\"Xx\") -> nullopt (no throw)\n";
    }
    return 0;
}
