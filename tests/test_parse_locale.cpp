// Locale independence of the range-notation weight parsers (Hold'em and PLO).
//
// Both parsers used to reach the value through std::stod, i.e. strtod, i.e.
// the process-global C locale: with a comma-decimal locale installed (de_DE,
// fr_FR, ...) "0.5" stops at the '.' and EVERY weighted range item is
// rejected. A library cannot assume the embedding program left the global
// locale alone, so the notation pins '.' itself.
//
// This file is guarded, not skipped-by-default: it looks for a comma-decimal
// locale among a few common names and, if the machine has none installed
// (minimal CI images often ship only "C" and "C.UTF-8"), it reports that and
// passes without asserting. The guard is deliberate -- a missing locale is a
// property of the machine, not a property of the code under test.
#include "doctest.h"

#include <xiapl/game_type.h>
#include <xiapl/range.h>

#include <cstddef>
#include <locale>
#include <optional>
#include <stdexcept>
#include <string>

using namespace xiapl;

namespace {

// Restores the previous global locale on the way out, including when an
// assertion throws: leaving a comma-decimal locale installed would leak into
// every test that runs after this one.
class GlobalLocaleGuard {
public:
    explicit GlobalLocaleGuard(const std::locale& loc)
        : previous_(std::locale::global(loc)) {}
    ~GlobalLocaleGuard() { std::locale::global(previous_); }
    GlobalLocaleGuard(const GlobalLocaleGuard&) = delete;
    GlobalLocaleGuard& operator=(const GlobalLocaleGuard&) = delete;

private:
    std::locale previous_;
};

// First installed locale (of a few common names) whose decimal separator is
// ',' rather than '.'. nullopt means the machine has none.
std::optional<std::locale> find_comma_decimal_locale() {
    for (const char* name : {"de_DE.UTF-8", "fr_FR.UTF-8", "es_ES.UTF-8",
                             "it_IT.UTF-8", "de_DE", "fr_FR"}) {
        try {
            std::locale loc(name);
            if (std::use_facet<std::numpunct<char>>(loc).decimal_point() == ',') {
                return loc;
            }
        } catch (const std::runtime_error&) {
            // Not installed on this machine; try the next name.
        }
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("range weights parse under a comma-decimal global locale") {
    const std::optional<std::locale> comma_locale = find_comma_decimal_locale();
    if (!comma_locale) {
        MESSAGE("no comma-decimal locale installed on this machine "
                "(tried de_DE / fr_FR / es_ES / it_IT); locale check skipped");
        return;
    }

    const GlobalLocaleGuard guard(*comma_locale);

    // Canary: std::locale::global also re-points the C locale, which is what
    // made std::stod misread "0.5". If a platform ever leaves strtod alone,
    // the assertions below still pass -- they just stop being a regression
    // test, so say so in the log rather than pretending the check ran.
    std::size_t consumed = 0;
    (void)std::stod("0.5", &consumed);
    if (consumed == std::string("0.5").size()) {
        MESSAGE("this locale did not change strtod's decimal separator; "
                "the checks below are vacuous on this machine");
    }

    // Hold'em parser (src/core/range.cpp).
    const Range holdem = Range::from_string("AKs:0.5");
    REQUIRE(holdem.size() == 4);
    CHECK(holdem.combos()[0].weight == 0.5);
    CHECK(Range::from_string("AKs:2.5e-1").combos()[0].weight == 0.25);

    // PLO parser (src/core/range_plo.cpp).
    const Range plo = Range::from_string("AAKKds:0.5", GameType::Plo);
    REQUIRE(plo.size() == 6);
    CHECK(plo.combos()[0].weight == 0.5);
    CHECK(Range::from_string("AAKKds:2.5e-1", GameType::Plo).combos()[0].weight ==
          0.25);

    // The other half of the contract: the notation's decimal separator does
    // not follow the locale either. ',' is the ITEM separator in both
    // grammars, so "0,5" is never a weight, whatever the locale says.
    CHECK_THROWS_AS(Range::from_string("AKs:0,5"), std::invalid_argument);
    CHECK_THROWS_AS(Range::from_string("AAKKds:0,5", GameType::Plo),
                    std::invalid_argument);
}
