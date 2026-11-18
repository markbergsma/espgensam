/// @file test_parse_dip_status.cpp
/// @brief Host-side regression test for the CMD_DIP_SWITCH 0x84 reply parser.
///
/// Run with:
///   c++ -std=c++17 -Wall -o /tmp/test_parse_dip_status \
///       tests/test_parse_dip_status.cpp components/gensam/monitor.cpp && /tmp/test_parse_dip_status

#include "../components/gensam/monitor.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

using esphome::gensam::GenSAMMonitor;
using esphome::gensam::describe_dip_switches;
using esphome::gensam::parse_dip_switch_status;

static std::string describe(const char *model, uint32_t word, uint32_t word3 = 0) {
  GenSAMMonitor monitor;
  monitor.model = model;
  monitor.dip_word = word;
  monitor.dip_auxiliary_word = word3;
  monitor.dip_switches_known = true;
  return describe_dip_switches(monitor);
}

int main() {
  const std::vector<uint8_t> reply = {
      0x84,
      0x00, 0x00, 0x20, 0x01,
      0x00, 0x00, 0x00, 0x02,
      0x00, 0x00, 0x00, 0x3B,
  };
  GenSAMMonitor monitor;
  assert(parse_dip_switch_status(reply.data(), reply.size(), monitor));
  assert(monitor.dip_switches_known);
  assert(monitor.dip_word == 0x00002001);
  assert(monitor.previous_dip_word == 0x00000002);
  assert(monitor.dip_auxiliary_word == 0x0000003B);

  const std::vector<uint8_t> unprefixed_reply = {
      0x00, 0x00, 0x20, 0x01,
      0x00, 0x00, 0x00, 0x02,
      0x00, 0x00, 0x00, 0x3B,
  };
  assert(parse_dip_switch_status(unprefixed_reply.data(), unprefixed_reply.size(), monitor));
  assert(monitor.dip_word == 0x00002001);
  assert(monitor.previous_dip_word == 0x00000002);
  assert(monitor.dip_auxiliary_word == 0x0000003B);

  const std::vector<uint8_t> truncated = {0x84, 0x00, 0x00, 0x00, 0x01};
  assert(!parse_dip_switch_status(truncated.data(), truncated.size(), monitor));
  assert(monitor.dip_word == 0x00002001);
  const std::vector<uint8_t> wrong_selector(13, 0);
  assert(!parse_dip_switch_status(wrong_selector.data(), wrong_selector.size(), monitor));
  assert(!parse_dip_switch_status(nullptr, 0, monitor));

  // No reply yet: nothing to publish.
  assert(describe_dip_switches(GenSAMMonitor{}).empty());

  // Monitor table (guide 6.5): the guide's own 8320A example and paired switches that add.
  assert(describe("8320A", 0x00000001) == "Stored");
  assert(describe("8330A", 0x00000000) == "All off");
  assert(describe("8340A", 0x00000006) == "Level -30 dB");
  assert(describe("8350A", 0x00000018) == "Digital A+B");
  assert(describe("8320A", 0x00000180) == "Treble tilt -2 dB");
  assert(describe("8320A", 0x00003000) == "Bass roll-off -6 dB");
  assert(describe("8320A", 0x00000C24) ==
         "Level -10 dB, ISS, Desktop 160 Hz -4 dB, Bass tilt -2 dB");
  assert(describe("8330A", 0x80000001) == "Stored, Switches not valid (bit 31)");
  assert(describe("8320A", 0x40000000) == "Stored override (bit 30)");
  assert(describe("8320A", 0x00004000) == "Unknown bits 0x4000");

  // Three-way level knob in word 3: 57 = -0.3 dB, 120 = +6 dB; out-of-range values are dropped.
  assert(describe("8341A", 0x00000000, 57) == "All off; Level knob -0.3 dB");
  assert(describe("8351B", 0x00000001, 120) == "Stored; Level knob +6.0 dB");
  assert(describe("8351B", 0x00000041, 0) == "Stored, LED disable; Level knob -6.0 dB");
  assert(describe("8341A", 0x00000000, 121) == "All off");
  // Two-way word 3 is a level variable, not the knob.
  assert(describe("8320A", 0x00000000, 59) == "All off");

  // Subwoofer table (guide 6.7.1, 7350A): stored is bit 2, not bit 0.
  assert(describe("7350A", 0x00000004) == "Stored");
  assert(describe("7350A", 0x00000001) == "Unknown bits 0x1");
  assert(describe("7350A", 0xC0000004) ==
         "Stored, Stored override (bit 30), Switches not valid (bit 31)");
  assert(describe("7360A", 0x00003C00) == "Bass roll-off -12 dB, Phase -270°");
  assert(describe("7380A", 0x000043A8) ==
         "Digital LFE, LFE channel A, LFE +10 dB, Level -20 dB, ISS, Bypass");

  // Undocumented models get the raw word only.
  assert(describe("1237A", 0x00000001) == "0x00000001");
  assert(describe("", 0x00000001) == "0x00000001");
}
