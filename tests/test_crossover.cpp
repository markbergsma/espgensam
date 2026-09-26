/// @file test_crossover.cpp
/// @brief Host-side test for the crossover option name <-> wire word conversion in crossover.h.
///
/// ===================================================================================
/// RUNNING
/// ===================================================================================
///   c++ -std=c++17 -Wall -o /tmp/test_crossover tests/test_crossover.cpp && /tmp/test_crossover
///
/// Header-only: crossover.h pulls in nothing but const.h and the C library.
///
/// ===================================================================================
/// WHAT THIS PINS DOWN
/// ===================================================================================
/// The strings are a Home Assistant select's option list, and __init__.py builds that list
/// independently as CROSSOVER_OPTIONS. test_option_list() spells the list out so a change on
/// either side shows up here; test_round_trip() is the assertion that every option reaches
/// the bus.
///
/// Full band is the case this file exists for. Its wire word is 0x0001, which a Hz-only
/// reading would display as "1 Hz" and a 50..120 range check would clamp to 50 Hz -- turning
/// bass management back *on* for a system that has no subwoofer to take the low end.
/// ===================================================================================

#include "../components/gensam/crossover.h"

#include <cstdio>
#include <cstring>

using namespace esphome::gensam;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  std::printf("  %s %s\n", ok ? "[ok]  " : "[FAIL]", what);
  if (!ok) {
    failures++;
  }
}

void check_str(const char *what, uint16_t value, const char *want) {
  char buf[CROSSOVER_STR_SIZE];
  const char *got = crossover_to_str(value, buf, sizeof(buf));
  const bool ok = std::strcmp(got, want) == 0;
  check(ok, what);
  if (!ok) {
    std::printf("         want \"%s\", got \"%s\"\n", want, got);
  }
}

void check_parse(const char *name, bool want_ok, uint16_t want) {
  uint16_t value = 0xEEEE;
  const bool ok = str_to_crossover(name, value);
  if (!want_ok) {
    check(!ok && value == 0xEEEE, name[0] ? name : "(empty string) is rejected");
    return;
  }
  const bool good = ok && value == want;
  check(good, name);
  if (!good) {
    std::printf("         want ok=1 value=%u, got ok=%d value=%u\n", want, (int) ok, value);
  }
}

// --- Naming -------------------------------------------------------------------------------

void test_names() {
  check_str("full band is named, not shown as 1 Hz", CROSSOVER_FULL_BAND, CROSSOVER_STR_FULL_BAND);
  check_str("50 Hz", 50, "50 Hz");
  check_str("90 Hz", 90, "90 Hz");
  check_str("120 Hz", 120, "120 Hz");
  // Formatted for logs even though the select cannot offer it.
  check_str("an off-grid snooped value still formats", 87, "87 Hz");
}

// --- Validity -----------------------------------------------------------------------------

void test_validity() {
  check(is_valid_crossover(CROSSOVER_FULL_BAND), "full band is valid");
  check(is_valid_crossover(MIN_CROSSOVER_HZ), "the minimum is valid");
  check(is_valid_crossover(MAX_CROSSOVER_HZ), "the maximum is valid");
  check(is_valid_crossover(85), "the factory default is valid");
  check(!is_valid_crossover(0), "0 is not a known mode");
  check(!is_valid_crossover(2), "2 is not a known mode");
  check(!is_valid_crossover(45), "below the minimum");
  check(!is_valid_crossover(125), "above the maximum");
  check(!is_valid_crossover(87), "off the 5 Hz grid");
}

// --- Parsing ------------------------------------------------------------------------------

void test_parsing() {
  check_parse(CROSSOVER_STR_FULL_BAND, true, CROSSOVER_FULL_BAND);
  check_parse("50 Hz", true, 50);
  check_parse("85 Hz", true, 85);
  check_parse("120 Hz", true, 120);
}

void test_rejects_unknown() {
  check_parse("", false, 0);
  check_parse("1 Hz", false, 0);      // full band's wire word, spelled as a frequency
  check_parse("90", false, 0);        // bare number
  check_parse("87 Hz", false, 0);     // off grid
  check_parse("125 Hz", false, 0);    // out of range
  check_parse("Full", false, 0);      // prefix of a valid option
  check_parse("90 Hz ", false, 0);    // valid option with a trailing extra
}

// --- The properties that matter -------------------------------------------------------------

void test_option_list() {
  // Must match CROSSOVER_OPTIONS in __init__.py, in order.
  const char *want[] = {"Full band", "50 Hz", "55 Hz", "60 Hz", "65 Hz", "70 Hz", "75 Hz", "80 Hz",
                        "85 Hz", "90 Hz", "95 Hz", "100 Hz", "105 Hz", "110 Hz", "115 Hz", "120 Hz"};
  size_t n = 0;
  bool ok = true;
  for (unsigned v = 0; v <= 0xFFFF; v++) {
    if (!is_valid_crossover(static_cast<uint16_t>(v))) {
      continue;
    }
    char buf[CROSSOVER_STR_SIZE];
    crossover_to_str(static_cast<uint16_t>(v), buf, sizeof(buf));
    if (n >= sizeof(want) / sizeof(want[0]) || std::strcmp(buf, want[n]) != 0) {
      ok = false;
    }
    n++;
  }
  check(ok && n == sizeof(want) / sizeof(want[0]), "valid settings are exactly the select's 16 options");
}

void test_round_trip() {
  for (unsigned v = 0; v <= 0xFFFF; v++) {
    if (!is_valid_crossover(static_cast<uint16_t>(v))) {
      continue;
    }
    char buf[CROSSOVER_STR_SIZE];
    crossover_to_str(static_cast<uint16_t>(v), buf, sizeof(buf));
    uint16_t back = 0;
    const bool ok = str_to_crossover(buf, back) && back == v;
    check(ok, buf);
  }
}

}  // namespace

int main() {
  std::printf("crossover conversion\n");
  test_names();
  test_validity();
  test_parsing();
  test_rejects_unknown();
  test_option_list();
  test_round_trip();

  if (failures != 0) {
    std::printf("\n%d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
