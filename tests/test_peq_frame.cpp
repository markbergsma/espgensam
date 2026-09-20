/// @file test_peq_frame.cpp
/// @brief Host-side test for the DSP frame builders against real GLM bus captures.
///
/// ===================================================================================
/// RUNNING
/// ===================================================================================
///   c++ -std=c++17 -Wall -o /tmp/test_peq_frame \
///       tests/test_peq_frame.cpp components/gensam/commands.cpp \
///       components/gensam/frame.cpp components/gensam/crc.cpp \
///       components/gensam/biquad.cpp && /tmp/test_peq_frame
///
/// No framework, no build system. frame.h depends on uart9bit_char.h rather than the RMT
/// transceiver, so the framing layer compiles on a host compiler unmodified.
///
/// ===================================================================================
/// WHAT THIS PINS DOWN
/// ===================================================================================
/// Every expected frame below is a verbatim capture of the OEM GLM adapter configuring this
/// project's own monitors, reassembled from RAW RX lines in captures/glm_source_select_capture.log
/// and cited by index. They are complete frames -- address byte, payload, CRC-16/GSM and
/// delimiter -- so a passing check means our bytes are indistinguishable from Genelec's on the
/// wire, not merely plausible.
///
/// Three things can go wrong here and none of them are visible from a log:
///
/// 1. Float endianness. The five coefficients are IEEE-754 little-endian, the sole exception
///    to this protocol's big-endian rule, sitting inside a payload whose CRC is big-endian.
///    A byte-swapped float is still a valid float, so the monitor would accept the frame and
///    install a nonsense filter.
///
/// 2. CRC and escape ordering. The CRC is computed over the raw payload and only then is the
///    stream escaped. Coefficient bytes collide with 0x7E and 0x7D often enough that getting
///    this backwards would work for most bands and fail for a few -- the worst failure mode
///    available. test_escaped_coefficient_roundtrip() forces the collision deliberately.
///
/// 3. Slot addressing. The index byte selects which of the 20 sections is overwritten. Index
///    0x00 and index 0x13 are both covered, as are three different bus addresses, because an
///    off-by-one here silently retunes the wrong band on the wrong speaker.
///
/// The captured system was uncalibrated, so every captured PEQ frame carries the structural
/// bypass vector. That is exactly what makes these good fixtures for the framing: the
/// coefficients are known constants, so any mismatch is the frame builder's fault and not the
/// designer's. Coefficient values themselves are covered by tests/test_biquad.cpp, and will be
/// checked against real calibrated frames once a capture of a group apply exists.
/// ===================================================================================

#include "../components/gensam/commands.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using esphome::gensam::BiquadCoeffs;
using esphome::gensam::design_biquad;
using esphome::gensam::Frame;
using esphome::gensam::make_peq_band;
using esphome::gensam::make_prepare_config;
using esphome::gensam::PeqType;
using esphome::gensam::Uart9BitChar;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  std::printf("  %s %s\n", ok ? "[ok]  " : "[FAIL]", what);
  if (!ok) {
    failures++;
  }
}

/// @brief Render a serialized frame the way the capture log prints it: address bytes get a
/// trailing apostrophe, everything else is a plain hex pair.
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

/// @brief Assert that @p frame serializes to exactly @p expected, as printed by render().
void check_wire(const char *what, const Frame &frame, const char *expected) {
  const std::string got = render(frame.to_9bit());
  const bool ok = got == expected;
  check(ok, what);
  if (!ok) {
    std::printf("         want %s\n", expected);
    std::printf("         got  %s\n", got.c_str());
  }
}

/// The identity section every captured PEQ frame carries, since the captured system had never
/// been calibrated. b0 = 1.0 is 00 00 80 3F little-endian.
constexpr BiquadCoeffs BYPASS{1.0f, 0.0f, 0.0f, 0.0f, 0.0f};

// --- Verbatim OEM frames ---------------------------------------------------------------

void test_captured_peq_frames() {
  // Monitor at 0x02, slot 0. The most-repeated PEQ frame in the capture (11 occurrences).
  check_wire("0x02 slot 0x00 bypass matches capture", make_peq_band(0x02, 0x00, BYPASS),
             "02' 10 0E 00 00 00 80 3F 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 F6 B7 7E");

  // Same monitor, next slot: only the index byte differs, so the CRC must move with it.
  check_wire("0x02 slot 0x01 bypass matches capture", make_peq_band(0x02, 0x01, BYPASS),
             "02' 10 0E 01 00 00 80 3F 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 9B 6F 7E");

  // Slot 0x13 is the last of the 20, and the boundary an off-by-one would cross.
  check_wire("0x02 slot 0x13 bypass matches capture", make_peq_band(0x02, 0x13, BYPASS),
             "02' 10 0E 13 00 00 80 3F 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 FD 99 7E");

  // Different bus addresses: the address byte participates in the CRC, so these are not the
  // same frame with a different first byte.
  check_wire("0x03 slot 0x06 bypass matches capture", make_peq_band(0x03, 0x06, BYPASS),
             "03' 10 0E 06 00 00 80 3F 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 CF F2 7E");

  check_wire("0x04 slot 0x05 bypass matches capture", make_peq_band(0x04, 0x05, BYPASS),
             "04' 10 0E 05 00 00 80 3F 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 B1 B7 7E");
}

void test_captured_prepare_config() {
  check_wire("0x02 prepare-for-config matches capture", make_prepare_config(0x02), "02' 17 01 1B 5A 7E");
}

// --- Structure -------------------------------------------------------------------------

void test_designed_band_is_little_endian() {
  // A real band from captures/glm-config/Home Cinema.sam: the 8330A's first notch. The point
  // is the byte order, so the expected payload is written out as bytes rather than floats.
  // b0 = 0.99804014f has the bit pattern 0x3F7F7F8F, so little-endian puts the low mantissa
  // byte first and the exponent last: 8F 7F 7F 3F.
  const BiquadCoeffs c = design_biquad(PeqType::PEAKING, 198.371f, -4.55673f, 3.49809f, 48000);
  const Frame f = make_peq_band(0x02, 0x04, c);

  check(f.payload.size() == 23, "PEQ payload is 23 bytes: sub-command, index, 5 floats, type flag");
  check(f.payload[0] == 0x0E, "payload starts with the PEQ sub-command");
  check(f.payload[1] == 0x04, "index byte follows the sub-command");
  check(f.payload[22] == 0x00, "type flag trails the coefficients");

  // Low byte of the mantissa first, exponent last: that is little-endian.
  const bool le = f.payload[2] == 0x8F && f.payload[3] == 0x7F && f.payload[4] == 0x7F && f.payload[5] == 0x3F;
  check(le, "b0 is packed little-endian (8F 7F 7F 3F, not 3F 7F 7F 8F)");
  if (!le) {
    std::printf("         got  %02X %02X %02X %02X\n", f.payload[2], f.payload[3], f.payload[4], f.payload[5]);
  }
}

void test_escaped_coefficient_roundtrip() {
  // Force a coefficient whose bytes contain both framing tokens, then confirm the serialized
  // stream escapes them and that a parser recovers the original payload. This is the case
  // that distinguishes "CRC before escaping" from "CRC after escaping": only the former
  // survives a round trip.
  BiquadCoeffs c{};
  // 0x7D7E7D7E as a float; every byte of b0 is a token that must be stuffed.
  const uint8_t pattern[4] = {0x7E, 0x7D, 0x7E, 0x7D};
  std::memcpy(&c.b0, pattern, 4);
  c.b1 = 0.0f;
  c.b2 = 0.0f;
  c.a1 = 0.0f;
  c.a2 = 0.0f;

  const Frame sent = make_peq_band(0x02, 0x07, c);
  const std::vector<Uart9BitChar> wire = sent.to_9bit();

  // Each of the four token bytes expands to two, so the stream is longer than the payload.
  check(wire.size() > sent.payload.size() + 4, "token bytes in a coefficient are escaped on the wire");

  esphome::gensam::FrameParser parser;
  std::vector<Frame> got;
  parser.feed(wire.data(), wire.size());
  Frame f;
  while (parser.pop_frame(f)) {
    got.push_back(f);
  }

  check(got.size() == 1, "escaped frame parses back as exactly one frame");
  if (got.size() == 1) {
    check(got[0].crc_valid, "CRC validates after unescaping");
    check(got[0].address == 0x02 && got[0].command == esphome::gensam::CMD_DSP,
          "address and command survive the round trip");
    check(got[0].payload == sent.payload, "payload bytes survive the round trip unchanged");
  }
}

void test_index_out_of_range_builds_nothing() {
  // 20 slots exist; 0x14 is the first that does not. Returning an empty frame keeps a bad
  // index from being silently truncated into a valid one and retuning the wrong band.
  const Frame f = make_peq_band(0x02, 0x14, BYPASS);
  check(f.payload.empty() && f.command == 0x00, "index 0x14 yields no frame");
}

}  // namespace

int main() {
  std::printf("DSP frame builders\n");
  test_captured_peq_frames();
  test_captured_prepare_config();
  test_designed_band_is_little_endian();
  test_escaped_coefficient_roundtrip();
  test_index_out_of_range_builds_nothing();

  if (failures != 0) {
    std::printf("\n%d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
