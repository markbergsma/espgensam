/// @file test_input.cpp
/// @brief Host-side test for the input routing name <-> wire byte conversion in input.h.
///
/// ===================================================================================
/// RUNNING
/// ===================================================================================
///   c++ -std=c++17 -Wall -o /tmp/test_input tests/test_input.cpp && /tmp/test_input
///
/// Header-only: input.h pulls in nothing but const.h and <cstdint>, so there is no
/// translation unit to compile alongside.
///
/// ===================================================================================
/// WHAT THIS PINS DOWN
/// ===================================================================================
/// These strings are the option list of a Home Assistant select, which means they are also
/// what the select hands back to control(). If input_to_str() and str_to_input() ever disagree
/// about one of them, that option silently stops working: selecting it logs "unknown input
/// option" and nothing reaches the bus. test_round_trip() is the assertion that they cannot
/// drift apart, and it is the reason the conversion lives in one file rather than as two
/// switch statements in different places.
///
/// The asymmetry in the pair is deliberate and worth fixing in a test. An analog device has no
/// sub-channel, so every analog combination has to collapse to the same single option -- but
/// the channel byte still has to come back as something definite, because it is transmitted
/// either way as the fourth byte of CMD_SELECT_AUDIO_SOURCE.
/// ===================================================================================

#include "../components/gensam/input.h"

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

void check_str(const char *what, const char *got, const char *want) {
  const bool ok = std::strcmp(got, want) == 0;
  check(ok, what);
  if (!ok) {
    std::printf("         want \"%s\", got \"%s\"\n", want, got);
  }
}

void check_parse(const char *name, bool want_ok, uint8_t want_src, uint8_t want_ch) {
  uint8_t src = 0xEE, ch = 0xEE;
  const bool ok = str_to_input(name, src, ch);
  if (!want_ok) {
    check(!ok && src == 0xEE && ch == 0xEE, "rejects an unrecognised option without writing outputs");
    return;
  }
  const bool good = ok && src == want_src && ch == want_ch;
  check(good, name);
  if (!good) {
    std::printf("         want ok=1 src=0x%02X ch=0x%02X, got ok=%d src=0x%02X ch=0x%02X\n",
                want_src, want_ch, (int) ok, src, ch);
  }
}

// --- Naming -------------------------------------------------------------------------------

void test_names() {
  check_str("analog", input_to_str(SOURCE_ANALOG, AES3_CHANNEL_A), INPUT_STR_ANALOG);
  check_str("aes3 channel A", input_to_str(SOURCE_DIGITAL_AES3, AES3_CHANNEL_A), INPUT_STR_AES3_A);
  check_str("aes3 channel B", input_to_str(SOURCE_DIGITAL_AES3, AES3_CHANNEL_B), INPUT_STR_AES3_B);
  check_str("aes3 channel A+B", input_to_str(SOURCE_DIGITAL_AES3, AES3_CHANNEL_SUM), INPUT_STR_AES3_SUM);
}

void test_analog_ignores_the_channel() {
  // An analog device has no sub-channel, so whatever the channel byte happens to hold - a
  // leftover from a previous AES3 routing, most likely - must not change what is displayed.
  check_str("analog with a stale channel B still reads analog",
            input_to_str(SOURCE_ANALOG, AES3_CHANNEL_B), INPUT_STR_ANALOG);
  check_str("analog with a stale channel sum still reads analog",
            input_to_str(SOURCE_ANALOG, AES3_CHANNEL_SUM), INPUT_STR_ANALOG);
}

void test_unknown_source_reads_as_analog() {
  // Source 0x03 ("Automatic") and anything undocumented. Analog is the safe display because
  // it is the one input every SAM device has; the snoop path separately stops asserting the
  // routing so nothing is transmitted on the strength of it.
  check_str("unknown source reads as analog", input_to_str(0x03, AES3_CHANNEL_A), INPUT_STR_ANALOG);
  check_str("zero source reads as analog", input_to_str(0x00, AES3_CHANNEL_A), INPUT_STR_ANALOG);
}

// --- Parsing ------------------------------------------------------------------------------

void test_parsing() {
  check_parse(INPUT_STR_ANALOG, true, SOURCE_ANALOG, AES3_CHANNEL_A);
  check_parse(INPUT_STR_AES3_A, true, SOURCE_DIGITAL_AES3, AES3_CHANNEL_A);
  check_parse(INPUT_STR_AES3_B, true, SOURCE_DIGITAL_AES3, AES3_CHANNEL_B);
  check_parse(INPUT_STR_AES3_SUM, true, SOURCE_DIGITAL_AES3, AES3_CHANNEL_SUM);
}

void test_rejects_unknown() {
  check_parse("Digital (AES3)", false, 0, 0);  // the removed global select's option
  check_parse("Channel A (Left)", false, 0, 0);  // the removed per-monitor channel option
  check_parse("", false, 0, 0);
  check_parse("Analogue", false, 0, 0);  // near miss: a longer string sharing a prefix
  check_parse("Analo", false, 0, 0);     // near miss: a prefix of a valid option
}

// --- The property that matters --------------------------------------------------------------

void test_round_trip() {
  // Every option the select offers must parse back to a pair that names it again. This is what
  // guarantees that selecting any option in Home Assistant reaches the bus.
  const char *options[] = {INPUT_STR_ANALOG, INPUT_STR_AES3_A, INPUT_STR_AES3_B, INPUT_STR_AES3_SUM};
  for (const char *opt : options) {
    uint8_t src = 0, ch = 0;
    const bool parsed = str_to_input(opt, src, ch);
    const bool ok = parsed && std::strcmp(input_to_str(src, ch), opt) == 0;
    check(ok, ok ? opt : "round trip");
    if (!ok) {
      std::printf("         \"%s\" did not survive the round trip\n", opt);
    }
  }
}

}  // namespace

int main() {
  std::printf("input routing conversion\n");
  test_names();
  test_analog_ignores_the_channel();
  test_unknown_source_reads_as_analog();
  test_parsing();
  test_rejects_unknown();
  test_round_trip();

  if (failures != 0) {
    std::printf("\n%d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
