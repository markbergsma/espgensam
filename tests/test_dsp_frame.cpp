/// @file test_dsp_frame.cpp
/// @brief Host-side test for the level and delay frame builders against real GLM captures.
///
/// ===================================================================================
/// RUNNING
/// ===================================================================================
///   c++ -std=c++17 -Wall -o /tmp/test_dsp_frame \
///       tests/test_dsp_frame.cpp components/gensam/commands.cpp \
///       components/gensam/frame.cpp components/gensam/crc.cpp \
///       components/gensam/biquad.cpp && /tmp/test_dsp_frame
///
/// ===================================================================================
/// WHAT THIS PINS DOWN
/// ===================================================================================
/// Every expected frame is a verbatim capture of GLM switching a calibrated 7350A + 2x 8330A
/// between the three groups of captures/glm-config/Home Cinema.sam, taken from
/// captures/glm_group_switch_capture.log. They are complete frames -- address, payload, CRC
/// and delimiter -- so a pass means our bytes are indistinguishable from Genelec's.
///
/// 1. The level scale. The protocol specification says the 24-bit level field uses a scale of
///    2^23, but 0 dB is transmitted as 0x7FFFFF, which is 2^23 - 1; the larger scale does not
///    even fit the field. The existing volume_db_to_int24() already uses 2^23 - 1 and
///    reproduces all three captured levels exactly, so make_level() reuses it rather than
///    introducing a second encoder that could drift from CMD_VOLUME's.
///
/// 2. That sub-command 0x00 is a per-device trim, not a limit. The three captured values are
///    the three groups' Level_Sensitivity settings for the same physical subwoofer, which is
///    the evidence behind that reading; the -8.3783 dB case is the one that makes it
///    unambiguous, being far too large for any plausible output ceiling.
///
/// 3. Escaping of the CRC itself. The 0 dB frame's CRC is 0x7E9E, whose high byte collides
///    with the frame delimiter, so GLM transmits it stuffed as 7D 5E 9E. This single captured
///    frame is the strongest available evidence that the CRC is computed over the raw payload
///    and escaped afterwards -- reverse those and this frame cannot be produced.
///
/// 4. The delay timebase. Subwoofer PEQ is designed at 12 kHz but the delay is in 48 kHz
///    samples, a 4x difference the specification flags as unresolved. The three captured
///    subwoofer delays are the AutoPhase results of the three groups, and the phase-to-sample
///    conversion is asserted here so a future change to DSP_DELAY_RATE_HZ has to confront it.
/// ===================================================================================

#include "../components/gensam/commands.h"
#include "../components/gensam/util.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using esphome::gensam::Frame;
using esphome::gensam::make_delay;
using esphome::gensam::make_level;
using esphome::gensam::Uart9BitChar;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  std::printf("  %s %s\n", ok ? "[ok]  " : "[FAIL]", what);
  if (!ok) {
    failures++;
  }
}

/// @brief Render a serialized frame the way the capture log prints it.
std::string render(const std::vector<Uart9BitChar> &chars) {
  std::string out;
  char buf[8];
  for (size_t i = 0; i < chars.size(); i++) {
    std::snprintf(buf, sizeof(buf), "%02X", chars[i].data);
    out += buf;
    if (chars[i].ninth_bit != 0) {
      out += '\'';
    }
    if (i + 1 < chars.size()) {
      out += ' ';
    }
  }
  return out;
}

void check_wire(const char *what, const Frame &frame, const char *expected) {
  const std::string got = render(frame.to_9bit());
  const bool ok = got == expected;
  check(ok, what);
  if (!ok) {
    std::printf("         want %s\n", expected);
    std::printf("         got  %s\n", got.c_str());
  }
}

// --- Level compensation -----------------------------------------------------------------
// The three values are the same subwoofer's Level_Sensitivity in the three groups, plus the
// two-way monitors' unity. Full .sam precision matters: truncating -8.37833 to -8.3783
// shifts the encoded word by 11 counts.

void test_captured_level_frames() {
  check_wire("level -1.9258 dB on 0x02 matches capture", make_level(0x02, -1.9258f),
             "02' 10 01 00 66 8B D9 9B 34 7E");

  check_wire("level -8.37833 dB on 0x02 matches capture", make_level(0x02, -8.37833f),
             "02' 10 01 00 30 C9 2A C3 88 7E");

  // 0 dB is 0x7FFFFF, which is 2^23 - 1 and not the 2^23 the specification states.
  // This frame's CRC is 0x7E9E and is transmitted escaped as 7D 5E 9E.
  check_wire("level 0 dB on 0x03 matches capture, with its CRC escaped",
             make_level(0x03, 0.0f), "03' 10 01 00 7F FF FF 7D 5E 9E 7E");
}

void test_level_encoding_details() {
  const Frame f = make_level(0x02, -1.9258f);
  const bool sized = f.payload.size() == 5;
  check(sized, "level payload is 5 bytes: sub-command, selector, 3-byte level");
  if (!sized) {
    return;  // Indexing a short payload would crash rather than report.
  }
  check(f.payload[0] == esphome::gensam::DSP_SUB_LEVEL, "payload starts with the level sub-command");
  check(f.payload[1] == esphome::gensam::DSP_LEVEL_COMPENSATION, "selector 0x00 is level compensation");

  // Big-endian, unlike the little-endian floats of a PEQ frame in the same 0x10 opcode.
  check(f.payload[2] == 0x66 && f.payload[3] == 0x8B && f.payload[4] == 0xD9,
        "level word is big-endian");

  // Positive gain is not representable; the encoder saturates at unity rather than wrapping.
  check(render(make_level(0x02, +6.0f).to_9bit()) == render(make_level(0x02, 0.0f).to_9bit()),
        "a positive level clamps to 0 dB");
}

void test_level_shares_the_volume_encoding() {
  // make_level() must not grow a second copy of the dB-to-int24 conversion: if it ever
  // diverges from CMD_VOLUME's, one of the two silently stops matching GLM.
  for (float db : {0.0f, -1.9258f, -8.37833f, -20.0f, -59.5f}) {
    uint8_t expect[3];
    esphome::gensam::encode_int24(esphome::gensam::volume_db_to_int24(db), expect);
    const Frame f = make_level(0x02, db);
    char label[96];
    std::snprintf(label, sizeof(label), "level %.5g dB encodes as volume_db_to_int24() does", db);
    check(f.payload.size() == 5 && f.payload[2] == expect[0] && f.payload[3] == expect[1] &&
              f.payload[4] == expect[2],
          label);
  }
}

// --- Time-of-flight delay ----------------------------------------------------------------

void test_captured_delay_frames() {
  // The subwoofer's three per-group AutoPhase delays.
  check_wire("delay 289 samples on 0x02 matches capture", make_delay(0x02, 289),
             "02' 10 02 00 00 01 21 C6 69 7E");
  check_wire("delay 267 samples on 0x02 matches capture", make_delay(0x02, 267),
             "02' 10 02 00 00 01 0B 43 41 7E");
  check_wire("delay 67 samples on 0x02 matches capture", make_delay(0x02, 67),
             "02' 10 02 00 00 00 43 B9 BC 7E");

  // Both two-way monitors, undelayed. Different addresses, so different CRCs.
  check_wire("delay 0 on 0x03 matches capture", make_delay(0x03, 0),
             "03' 10 02 00 00 00 00 79 7A 7E");
  check_wire("delay 0 on 0x04 matches capture", make_delay(0x04, 0),
             "04' 10 02 00 00 00 00 60 3E 7E");
}

void test_delay_encoding_details() {
  const Frame f = make_delay(0x02, 289);
  const bool sized = f.payload.size() == 5;
  check(sized, "delay payload is 5 bytes: sub-command and a 4-byte count");
  if (!sized) {
    return;
  }
  check(f.payload[0] == esphome::gensam::DSP_SUB_DELAY, "payload starts with the delay sub-command");
  check(f.payload[1] == 0x00 && f.payload[2] == 0x00 && f.payload[3] == 0x01 && f.payload[4] == 0x21,
        "sample count is big-endian (289 = 00 00 01 21)");

  const Frame big = make_delay(0x02, 0x12345678u);
  check(big.payload.size() == 5 && big.payload[1] == 0x12 && big.payload[2] == 0x34 &&
            big.payload[3] == 0x56 && big.payload[4] == 0x78,
        "a full 32-bit count is not truncated");
}

void test_phase_to_delay_conversion() {
  // The rule that produced the three captured values, asserted here because it is the only
  // place the 48 kHz timebase is observable: at 12 kHz these would be four times smaller.
  struct Case {
    float phase_deg;
    float crossover_hz;
    uint32_t expect;
  };
  const Case cases[] = {{-165.0f, 90.0f, 289}, {180.0f, 90.0f, 267}, {45.0f, 90.0f, 67}};
  for (const Case &c : cases) {
    float wrapped = std::fmod(c.phase_deg, 360.0f);
    if (wrapped < 0.0f) {
      wrapped += 360.0f;
    }
    const uint32_t samples = static_cast<uint32_t>(
        std::lround(wrapped / 360.0f / c.crossover_hz * esphome::gensam::DSP_DELAY_RATE_HZ));
    char label[96];
    std::snprintf(label, sizeof(label), "phase %+.0f deg at %.0f Hz -> %u samples", c.phase_deg,
                  c.crossover_hz, c.expect);
    check(samples == c.expect, label);
  }
}

}  // namespace

int main() {
  std::printf("level and delay frame builders\n");
  test_captured_level_frames();
  test_level_encoding_details();
  test_level_shares_the_volume_encoding();
  test_captured_delay_frames();
  test_delay_encoding_details();
  test_phase_to_delay_conversion();

  if (failures != 0) {
    std::printf("\n%d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
