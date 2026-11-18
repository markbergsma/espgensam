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
#include <cmath>
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

/// A live 7350A frame with signal present. 0x43 sits at the floor because a subwoofer has no
/// tweeter; 0x45 carries the level. Pins that output_db follows whichever channel is active
/// rather than a particular tag, which is why the 0x43/0x45 assignment is a labelling question
/// and not a behavioural one.
void test_subwoofer_output_follows_active_channel() {
  GenSAMMonitor m = parse({0x07, 0x41, 0x18, 0x83, 0x00, 0x1A, 0x42, 0xF5, 0x46, 0x8F,
                           0x43, 0x80, 0x45, 0xE5, 0x47, 0x01, 0x84, 0x01, 0x66});
  check(m.output_db == static_cast<int8_t>(0xE5), "subwoofer: output_db takes the peak (0x45)");
  check(m.standby_known && !m.standby, "subwoofer: ACTIVE");
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

// --- Tag 0x49 (counter) -------------------------------------------------------------------------

/// Tag 0x49 is the speaker's 1-byte telemetry counter (guide v3.0 section 8.1). It wraps through
/// every value, so whenever it equals another tag the scan must still consume it as an operand.
/// Here the counter is 0x41: read as a tag, it would take 0x84 (the next tag) as the temperature.
void test_counter_operand_is_consumed() {
  GenSAMMonitor m = parse({0x41, 0x14, 0x47, 0x01, 0x49, 0x41, 0x84, 0x02, 0xB3});
  check(m.temperature == 20, "counter 0x41: temperature not overwritten by the counter operand");
  check(m.standby_known && !m.standby, "counter 0x41: ACTIVE");
}

/// A counter equal to an output-meter tag must not inject a phantom level either.
void test_counter_equal_to_meter_tag() {
  GenSAMMonitor m = parse({0x41, 0x14, 0x43, 0x80, 0x45, 0x90, 0x49, 0x45, 0x47, 0x01});
  check(m.output_db == static_cast<int8_t>(0x90), "counter 0x45: output_db is the real BS meter");
  check(m.standby_known && !m.standby, "counter 0x45: following power tag still parsed");
}

// --- 2-byte records: coil temperatures and supply rails -----------------------------------------

/// The worked example in the GLM bus decoding guide v3.0, section 8.2 (an 8320A): TW and BS coil
/// temperatures both 00 21 = 33 degC. A two-way has no midrange, so 0x82 stays unreported.
void test_coil_temperatures() {
  GenSAMMonitor m = parse({0x41, 0x21, 0x81, 0x00, 0x21, 0x83, 0x00, 0x21, 0x42, 0x90, 0x43,
                           0x80, 0x45, 0x80, 0x47, 0x01, 0x49, 0x24, 0x84, 0x02, 0xB3});
  check(m.tw_coil_temperature == 33.0f, "coil: TW 0x81 = 33 C");
  check(m.bs_coil_temperature == 33.0f, "coil: BS 0x83 = 33 C");
  check(std::isnan(m.md_coil_temperature), "coil: MD unreported on a two-way stays NAN");
  check(m.temperature == 33 && m.standby_known && !m.standby, "coil: 1-byte records still parse");
}

/// Processor load is the whole 16-bit word in per mille (guide sections 3.6 and 8): 02 B3 is the
/// 8320A's typical 691, and 02 EA is the two-way reading in captures/active-boot-20261116-0252.log.
void test_processor_load() {
  GenSAMMonitor m = parse({0x41, 0x21, 0x84, 0x02, 0xB3});
  check(std::fabs(m.processor_load - 69.1f) < 0.01f, "load: 0x84 02 B3 = 69.1 %");
  m = parse({0x41, 0x2E, 0x47, 0x01, 0x49, 0xC2, 0x84, 0x02, 0xEA});
  check(std::fabs(m.processor_load - 74.6f) < 0.01f, "load: 0x84 02 EA = 74.6 %");
  check(m.standby_known && !m.standby, "load: preceding records still parse");
  m = parse({0x41, 0x21, 0x81, 0x00, 0x21});
  check(std::isnan(m.processor_load), "load: unreported 0x84 stays NAN");
}

/// Supply rails at their typical three-way readings, 70 % and 62 % (guide section 8.1).
void test_supply_rails() {
  GenSAMMonitor m = parse({0x41, 0x21, 0x82, 0x00, 0x24, 0x88, 0x00, 0x46, 0x89, 0x00, 0x3E});
  check(m.md_coil_temperature == 36.0f, "rails: MD coil 0x82 = 36 C");
  check(std::fabs(m.psu_main_voltage - 51.1f) < 0.01f, "rails: main 70 % = 51.1 V");
  check(std::fabs(m.psu_low_voltage - 22.02f) < 0.01f, "rails: low 62 % = 22.02 V");
  check(std::isnan(m.tw_coil_temperature), "rails: unreported TW coil stays NAN");
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
  test_subwoofer_output_follows_active_channel();
  test_tag_47_standby();
  test_tag_47_standby_truncated();
  test_tag_47_invalid_operand_ignored();
  test_format_a_unshifted();
  test_counter_operand_is_consumed();
  test_counter_equal_to_meter_tag();
  test_coil_temperatures();
  test_processor_load();
  test_supply_rails();

  if (failures != 0) {
    std::printf("\n%d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
