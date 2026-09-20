/// @file test_parse_telemetry.cpp
/// @brief Host-side regression test for parse_telemetry() against real GLM bus captures.
///
/// ===================================================================================
/// RUNNING
/// ===================================================================================
///   c++ -std=c++17 -Wall -o /tmp/test_parse_telemetry \
///       tests/test_parse_telemetry.cpp components/gensam/monitor.cpp && /tmp/test_parse_telemetry
///
/// No framework, no build system: parse_telemetry() is a pure function over a byte buffer, and
/// monitor.h includes only const.h and standard headers (it forward-declares every ESPHome type
/// it touches), so the translation unit compiles on a host compiler unmodified.
///
/// ===================================================================================
/// WHAT THIS PINS DOWN
/// ===================================================================================
/// Every payload below is a verbatim frame from captures/, cited by file and line. They exist
/// because an earlier release read a leading 0x07 as "monitor is in standby", set standby from
/// it, and then blocked tag 0x47 from correcting that. Every one of these frames reports tag
/// 0x47 = 0x01, ACTIVE, so that reading inverted the truth on real traffic -- and a false
/// standby is expensive: the volume slider keeps publishing to Home Assistant while transmitting
/// nothing, and the next rediscovery escalates it into a genuine send_standby() on the wire.
///
/// The marker cases are therefore the point of the file. The cases at the end cover the two
/// 0x47 paths the three capture files happen not to contain: a genuine standby report, and a
/// coincidental 0x47 data byte that must not be allowed to arm standby_known -- that flag is
/// sticky and never cleared.
/// ===================================================================================

#include "../components/gensam/monitor.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <vector>

using esphome::gensam::GenSAMMonitor;
using esphome::gensam::parse_telemetry;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  std::printf("  %s %s\n", ok ? "[ok]  " : "[FAIL]", what);
  if (!ok) {
    failures++;
  }
}

/// @brief Parse @p bytes into a fresh monitor so each case starts from the struct defaults.
GenSAMMonitor parse(std::initializer_list<uint8_t> bytes, bool *returned = nullptr) {
  std::vector<uint8_t> buf(bytes);
  GenSAMMonitor mon;
  bool r = parse_telemetry(buf.data(), buf.size(), mon);
  if (returned != nullptr) {
    *returned = r;
  }
  return mon;
}

// --- Real capture payloads -------------------------------------------------------------------

/// captures/glm_v5_no_wakeup_capture.log:296 -- 7350A, leading 0x07 marker, 1.57 s after a wake.
void test_leading_07_marker() {
  GenSAMMonitor m = parse({0x07, 0x41, 0x13, 0x83, 0x00, 0x25, 0x42, 0x00, 0x46, 0xFA,
                           0x43, 0x80, 0x45, 0x9D, 0x47, 0x01, 0x84, 0x01, 0x6C});
  check(m.standby_known, "leading 07: power state reported");
  check(!m.standby, "leading 07: ACTIVE, not standby (this is the regression)");
  check(m.temperature == 19, "leading 07: temperature 19 C");
}

/// captures/glm_startup_capture.log:363 -- 8330A, leading 0x06 marker, 1.57 s after a wake.
void test_leading_06_marker() {
  GenSAMMonitor m = parse({0x06, 0x41, 0x1F, 0x81, 0x00, 0x1F, 0x83, 0x00, 0x1F, 0x42, 0x90,
                           0x43, 0x8C, 0x45, 0x84, 0x47, 0x01, 0x84, 0x02, 0xC4});
  check(m.standby_known && !m.standby, "leading 06: ACTIVE");
  check(m.temperature == 31, "leading 06: temperature 31 C");
}

/// captures/glm_source_select_capture.log -- 7350A, marker as the LAST byte instead of the first.
void test_trailing_06_marker() {
  GenSAMMonitor m = parse({0x41, 0x29, 0x83, 0x00, 0x29, 0x42, 0x95, 0x46, 0x91, 0x43, 0x80,
                           0x45, 0x80, 0x47, 0x01, 0x84, 0x01, 0x6C, 0x06});
  check(m.standby_known && !m.standby, "trailing 06: ACTIVE");
  check(m.temperature == 41, "trailing 06: temperature 41 C");
}

/// captures/glm_source_select_capture.log:1853 -- the marker as the entire payload, mid wake burst.
void test_bare_marker_touches_nothing() {
  GenSAMMonitor before;
  bool returned = true;
  GenSAMMonitor m = parse({0x06}, &returned);
  check(!returned, "bare 06: reports nothing parsed");
  check(!m.standby_known, "bare 06: does not arm standby_known (it is sticky forever)");
  check(m.standby == before.standby, "bare 06: leaves standby untouched");
  check(m.temperature == before.temperature, "bare 06: leaves temperature untouched");
}

/// A marked frame and its unmarked sibling one poll apart must parse identically.
void test_marker_is_transparent() {
  std::initializer_list<uint8_t> unmarked = {0x41, 0x29, 0x83, 0x00, 0x29, 0x42, 0x95, 0x46,
                                             0x91, 0x43, 0x80, 0x45, 0x80, 0x47, 0x01, 0x84,
                                             0x01, 0x6C};
  GenSAMMonitor plain = parse(unmarked);
  GenSAMMonitor marked = parse({0x06, 0x41, 0x29, 0x83, 0x00, 0x29, 0x42, 0x95, 0x46, 0x91,
                                0x43, 0x80, 0x45, 0x80, 0x47, 0x01, 0x84, 0x01, 0x6C});
  check(plain.temperature == marked.temperature && plain.input_db == marked.input_db &&
            plain.output_db == marked.output_db && plain.standby == marked.standby,
        "marker is transparent: marked frame parses as its unmarked sibling");
}

// --- The 0x47 paths the three capture files happen not to contain ------------------------------

/// A genuine standby reply, observed live on 2026-09-20 with the system powered down: tag 47 02,
/// device-class/state 84 03, and the audio records dropped entirely. Note the contrast with the
/// marker frames above -- a real standby report carries no marker and no meters.
void test_tag_47_standby() {
  GenSAMMonitor m = parse({0x41, 0x14, 0x47, 0x02, 0x84, 0x03, 0x8D});
  check(m.standby_known && m.standby, "tag 47 02: STANDBY reported");
  check(m.temperature == 20, "tag 47 02: temperature still parsed");
}

/// Robustness, not a wire format: the same reply with its last byte missing. Frames like this do
/// occur on the bus -- the receiver drops a byte on half-duplex turnaround -- but they fail CRC
/// and never reach this function. Covered anyway because it is the boundary the 0x8x skip's
/// `i + 2 < tlv_len` bound guards, and a short record must not read past the buffer.
void test_tag_47_standby_truncated() {
  GenSAMMonitor m = parse({0x41, 0x14, 0x47, 0x02, 0x84, 0x03});
  check(m.standby_known && m.standby, "tag 47 02 truncated: STANDBY still reported");
}

/// A 0x47 carrying neither 0x01 nor 0x02 is a desynced data byte, not a power report, and must
/// not arm a permanent standby vote.
void test_tag_47_invalid_operand_ignored() {
  GenSAMMonitor m = parse({0x41, 0x1A, 0x47, 0x99});
  check(!m.standby_known, "stray 47: operand 0x99 does not arm standby_known");
  check(m.temperature == 0x1A, "stray 47: preceding records still parse");
}

/// Bare 0x07 is the case the old STATUS_STANDBY reading was built around. It never appears in
/// any capture -- only bare 0x06 does -- but it must not be a power report either.
void test_bare_07_is_not_standby() {
  bool returned = true;
  GenSAMMonitor m = parse({0x07}, &returned);
  check(!returned, "bare 07: reports nothing parsed");
  check(!m.standby_known, "bare 07: does not arm standby_known");
}

/// Format A has no power field and absolute offsets, so a marker-looking byte 0 must not shift it.
void test_format_a_unshifted() {
  GenSAMMonitor m = parse({0x07, 0x14, 0x00, 0x00, 0x00, 0x00, 0xB0, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0x9C});
  check(!m.standby_known, "format A: byte 0 is not a power state");
  check(m.temperature == 0x14, "format A: temperature read from absolute offset 1");
  check(m.output_db == static_cast<int8_t>(0x9C), "format A: output read from absolute offset 12");
}

}  // namespace

int main() {
  std::printf("parse_telemetry()\n");
  test_leading_07_marker();
  test_leading_06_marker();
  test_trailing_06_marker();
  test_bare_marker_touches_nothing();
  test_bare_07_is_not_standby();
  test_marker_is_transparent();
  test_tag_47_standby();
  test_tag_47_standby_truncated();
  test_tag_47_invalid_operand_ignored();
  test_format_a_unshifted();

  if (failures != 0) {
    std::printf("\n%d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
