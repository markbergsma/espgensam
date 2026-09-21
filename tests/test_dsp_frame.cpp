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
#include <limits>
#include <string>
#include <vector>

using esphome::gensam::clamp_level_db;
using esphome::gensam::delay_ms_to_samples;
using esphome::gensam::delay_samples_to_ms;
using esphome::gensam::Frame;
using esphome::gensam::make_delay;
using esphome::gensam::make_level;
using esphome::gensam::parse_dsp_delay;
using esphome::gensam::parse_dsp_level;
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

// --- Presentation conversions and safety bounds ------------------------------------------
//
// These guard the two paths by which a value reaches make_level() / make_delay() from outside
// a group preset: a Home Assistant number entity, and a YAML lambda calling the hub directly.

void test_delay_unit_round_trip() {
  // Every captured AutoPhase result survives the trip through milliseconds unchanged. This is
  // what lets the number entity display milliseconds while the binding stores samples.
  for (uint32_t samples : {0u, 67u, 267u, 289u, 4800u, esphome::gensam::MAX_DELAY_SAMPLES}) {
    char label[96];
    std::snprintf(label, sizeof(label), "%u samples -> ms -> %u samples", samples, samples);
    check(delay_ms_to_samples(delay_samples_to_ms(samples)) == samples, label);
  }

  check(std::fabs(delay_samples_to_ms(289) - 6.0208333f) < 1e-5f,
        "289 samples is 6.0208333 ms, not a round number");
  check(delay_samples_to_ms(4800) == 100.0f, "4800 samples is exactly 100 ms");
  check(delay_ms_to_samples(esphome::gensam::MAX_DELAY_MS) == esphome::gensam::MAX_DELAY_SAMPLES,
        "192 ms is the 9216-sample ceiling");
}

void test_delay_clamping() {
  check(delay_ms_to_samples(-1.0f) == 0, "a negative delay is 0 samples");
  check(delay_ms_to_samples(-0.0f) == 0, "negative zero is 0 samples");
  check(delay_ms_to_samples(1000.0f) == esphome::gensam::MAX_DELAY_SAMPLES,
        "a delay past the ceiling clamps rather than wrapping");
  check(delay_ms_to_samples(std::numeric_limits<float>::infinity()) ==
            esphome::gensam::MAX_DELAY_SAMPLES,
        "an infinite delay clamps to the ceiling");
  check(delay_ms_to_samples(std::numeric_limits<float>::quiet_NaN()) == 0,
        "a NaN delay is 0 samples, not an enormous one");
}

void test_conversions_are_projections() {
  // f(f(x)) == f(x). Without this, nudging a number entity repeatedly could walk the value:
  // each adjustment starts from what was published last, so publishing anything other than
  // the canonical form would compound.
  for (float ms = 0.0f; ms <= 200.0f; ms += 0.05f) {
    const uint32_t once = delay_ms_to_samples(ms);
    const uint32_t twice = delay_ms_to_samples(delay_samples_to_ms(once));
    if (once != twice) {
      char label[96];
      std::snprintf(label, sizeof(label), "delay is stable at %.2f ms (%u vs %u)", ms, once, twice);
      check(false, label);
      return;
    }
  }
  check(true, "delay conversion is stable across 0..200 ms in 0.05 ms steps");

  // The off-grid group value specifically: it must not creep towards a step boundary.
  check(delay_ms_to_samples(6.0208333f) == 289, "the off-grid 6.0208333 ms lands back on 289");

  for (float db = -70.0f; db <= 10.0f; db += 0.05f) {
    if (clamp_level_db(clamp_level_db(db)) != clamp_level_db(db)) {
      check(false, "level clamp is stable");
      return;
    }
  }
  check(true, "level clamp is stable across -70..+10 dB");
}

void test_level_floor_keeps_the_speaker_audible() {
  // A GLM setup file writes Calibration_Level: -999 for "not calibrated". Unclamped, that
  // encodes as 0x000000 - digital silence - and would mute the speaker instead of trimming it.
  check(clamp_level_db(-999.0f) == esphome::gensam::MIN_LEVEL_DB,
        "the -999 dB 'not calibrated' sentinel clamps to the floor");
  check(clamp_level_db(-std::numeric_limits<float>::infinity()) == esphome::gensam::MIN_LEVEL_DB,
        "negative infinity clamps to the floor");
  check(clamp_level_db(std::numeric_limits<float>::quiet_NaN()) == esphome::gensam::MIN_LEVEL_DB,
        "NaN clamps to the floor rather than reaching the encoder");
  check(clamp_level_db(+10.0f) == esphome::gensam::MAX_LEVEL_DB, "positive gain clamps to unity");
  check(clamp_level_db(std::numeric_limits<float>::infinity()) == esphome::gensam::MAX_LEVEL_DB,
        "positive infinity clamps to unity");

  // Sweep the whole offered range: no step of it may encode as silence, and the encoding must
  // stay monotonic, so a smaller trim is never a louder one.
  uint32_t previous = 0;
  bool all_audible = true;
  bool monotonic = true;
  for (int i = 0; i <= 600; i++) {
    const float db = esphome::gensam::MIN_LEVEL_DB + static_cast<float>(i) * 0.1f;
    const uint32_t word = esphome::gensam::volume_db_to_int24(clamp_level_db(db));
    if (word == 0) {
      all_audible = false;
    }
    if (i > 0 && word <= previous) {
      monotonic = false;
    }
    previous = word;
  }
  check(all_audible, "no 0.1 dB step of the offered range encodes as digital silence");
  check(monotonic, "the offered range encodes monotonically");
}

// --- Decoding what GLM puts on the bus ---------------------------------------------------

void test_level_decode_matches_the_captures() {
  // The same three payloads the builders are checked against above, read back.
  struct Case {
    std::vector<uint8_t> payload;
    float expect;
  };
  const Case cases[] = {
      {{0x01, 0x00, 0x66, 0x8B, 0xD9}, -1.9258f},
      {{0x01, 0x00, 0x30, 0xC9, 0x2A}, -8.37833f},
      {{0x01, 0x00, 0x7F, 0xFF, 0xFF}, 0.0f},
  };
  for (const Case &c : cases) {
    float db = 99.0f;
    char label[96];
    std::snprintf(label, sizeof(label), "captured level payload decodes to %.5g dB", c.expect);
    check(parse_dsp_level(c.payload, db) && std::fabs(db - c.expect) < 0.001f, label);
  }

  // Round trip against the builder, which is the property that keeps the two in step.
  for (float db : {0.0f, -1.9258f, -8.37833f, -20.0f, -59.9f}) {
    float back = 99.0f;
    char label[96];
    std::snprintf(label, sizeof(label), "make_level(%.5g dB) decodes back to itself", db);
    check(parse_dsp_level(make_level(0x02, db).payload, back) && std::fabs(back - db) < 0.001f,
          label);
  }
}

void test_level_decode_rejects_the_unknown_sub_command() {
  // GLM emits this on every device immediately after the real level, with a constantly zero
  // payload. Decoded as a level it is 0x000000 = -130 dB = digital silence, so a decoder that
  // matched only the sub-command would store a mute for every speaker GLM touched.
  float db = 99.0f;
  check(!parse_dsp_level({0x01, 0x09, 0x00, 0x00, 0x00}, db),
        "the constant `01 09 00 00 00` frame is not decoded as a level");
  check(db == 99.0f, "a rejected level frame leaves the output untouched");

  // Everything else that shares the 0x10 opcode or is simply malformed.
  check(!parse_dsp_level({0x02, 0x00, 0x00, 0x01, 0x21}, db), "a delay payload is not a level");
  check(!parse_dsp_level({0x0E, 0x00, 0x00, 0x00, 0x80}, db), "a PEQ payload is not a level");
  check(!parse_dsp_level({}, db), "an empty payload is not a level");
  check(!parse_dsp_level({0x01, 0x00, 0x7F}, db), "a truncated level payload is rejected");
  check(!parse_dsp_level({0x01, 0x00, 0x7F, 0xFF, 0xFF, 0x00}, db),
        "an over-long level payload is rejected");

  // The floor is not the decoder's job, but callers rely on seeing the real value to decide.
  check(parse_dsp_level({0x01, 0x00, 0x00, 0x00, 0x00}, db) && db <= -130.0f,
        "a genuine zero level decodes as digital silence for the caller to reject");
}

void test_delay_decode() {
  uint32_t samples = 99;
  check(parse_dsp_delay({0x02, 0x00, 0x00, 0x01, 0x21}, samples) && samples == 289,
        "captured delay payload decodes to 289 samples");
  check(parse_dsp_delay({0x02, 0x00, 0x00, 0x01, 0x0B}, samples) && samples == 267,
        "captured delay payload decodes to 267 samples");
  check(parse_dsp_delay({0x02, 0x00, 0x00, 0x00, 0x43}, samples) && samples == 67,
        "captured delay payload decodes to 67 samples");
  check(parse_dsp_delay({0x02, 0x00, 0x00, 0x00, 0x00}, samples) && samples == 0,
        "captured zero delay decodes to 0 samples");

  for (uint32_t s : {0u, 67u, 289u, 9216u, 0x12345678u}) {
    char label[96];
    std::snprintf(label, sizeof(label), "make_delay(%u) decodes back to itself", s);
    uint32_t back = 99;
    check(parse_dsp_delay(make_delay(0x02, s).payload, back) && back == s, label);
  }

  samples = 99;
  check(!parse_dsp_delay({0x01, 0x00, 0x7F, 0xFF, 0xFF}, samples), "a level payload is not a delay");
  check(!parse_dsp_delay({0x0E, 0x00, 0x00, 0x00, 0x80}, samples), "a PEQ payload is not a delay");
  check(!parse_dsp_delay({}, samples), "an empty payload is not a delay");
  check(!parse_dsp_delay({0x02, 0x00, 0x01}, samples), "a truncated delay payload is rejected");
  check(samples == 99, "a rejected delay frame leaves the output untouched");
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
  test_delay_unit_round_trip();
  test_delay_clamping();
  test_conversions_are_projections();
  test_level_floor_keeps_the_speaker_audible();
  test_level_decode_matches_the_captures();
  test_level_decode_rejects_the_unknown_sub_command();
  test_delay_decode();

  if (failures != 0) {
    std::printf("\n%d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
