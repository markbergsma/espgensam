/// @file commands.cpp
/// @brief Genelec SAM command frame builders.
/// See commands.h for the protocol command vocabulary and complete API documentation.

#include "commands.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "util.h"

namespace esphome {
namespace gensam {

Frame make_discovery_ping() { return Frame(BROADCAST_ADDRESS, CMD_DISCOVERY); }

Frame make_set_rid(const std::vector<uint8_t> &racing_bytes, uint8_t assign_addr) {
  std::vector<uint8_t> payload = racing_bytes;
  payload.push_back(assign_addr);
  return Frame(MULTICAST_ADDRESS, CMD_SET_RID, std::move(payload));
}

Frame make_stay_online() { return Frame(BROADCAST_ADDRESS, CMD_STAY_ONLINE); }

Frame make_wakeup_step(uint8_t op, uint8_t val) {
  return Frame(BROADCAST_ADDRESS, CMD_WAKEUP, {op, val});
}

Frame make_broadcast_volume_db(float db) {
  uint8_t pld[3];
  encode_int24(volume_db_to_int24(db), pld);
  return Frame(BROADCAST_ADDRESS, CMD_VOLUME, {pld[0], pld[1], pld[2]});
}

Frame make_broadcast_volume_silence() {
  return Frame(BROADCAST_ADDRESS, CMD_VOLUME,
               {VOLUME_PAYLOAD_SILENCE[0], VOLUME_PAYLOAD_SILENCE[1], VOLUME_PAYLOAD_SILENCE[2]});
}

Frame make_query(uint8_t addr, uint8_t cmd) {
  if (cmd == CMD_BAR_CODE) {
    return Frame(addr, cmd, {0x01});
  }
  return Frame(addr, cmd);
}

Frame make_bypass(uint8_t addr, bool mute, bool pulsing) {
  uint8_t val;
  if (pulsing) {
    // Identify: pulse the LED with the color driver off, preserving the current mute bit.
    val = (mute ? BYPASS_MUTE_MASK : 0x00) | (LED_OFF << 1) | BYPASS_LED_PULSING_MASK;
  } else {
    val = mute ? (BYPASS_MUTE_MASK | (LED_RED << 1)) : (LED_OFF << 1);
  }
  return Frame(addr, CMD_BYPASS, {val});
}

Frame make_crossover(uint8_t addr, uint16_t freq_hz) {
  return Frame(addr, CMD_BASS_MANAGE_XO,
               {static_cast<uint8_t>((freq_hz >> 8) & 0xFF), static_cast<uint8_t>(freq_hz & 0xFF)});
}

Frame make_prepare_config(uint8_t addr) {
  return Frame(addr, CMD_PREPARE_CONFIG, {PREPARE_CONFIG_PAYLOAD});
}

/// @brief Append one IEEE-754 float32 to @p out in little-endian byte order.
///
/// memcpy into an integer rather than a reinterpret_cast: type-punning a float through a
/// uint32_t pointer is undefined behaviour and real compilers do miscompile it under -O2.
/// Shifting out of the integer then makes the byte order explicit, so the result does not
/// depend on the host's endianness -- the ESP32 is little-endian and a raw memcpy of the
/// float would happen to be correct there, but silently wrong in a host test on a big-endian
/// machine, which is precisely where this would go unnoticed.
static void append_float_le(std::vector<uint8_t> &out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  out.push_back(static_cast<uint8_t>(bits & 0xFF));
  out.push_back(static_cast<uint8_t>((bits >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((bits >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((bits >> 24) & 0xFF));
}

Frame make_peq_band(uint8_t addr, uint8_t index, const BiquadCoeffs &c) {
  if (index > PEQ_MAX_INDEX) {
    return Frame();
  }

  std::vector<uint8_t> payload;
  payload.reserve(23);
  payload.push_back(DSP_SUB_PEQ);
  payload.push_back(index);
  append_float_le(payload, c.b0);
  append_float_le(payload, c.b1);
  append_float_le(payload, c.b2);
  append_float_le(payload, c.a1);
  append_float_le(payload, c.a2);
  payload.push_back(PEQ_TYPE_FLAG_ACTIVE);

  return Frame(addr, CMD_DSP, std::move(payload));
}

Frame make_level(uint8_t addr, float db) {
  // Deliberately the same conversion CMD_VOLUME uses: the two fields share an encoding, and
  // a private copy here could drift out of step with it after a correction to one of them.
  uint8_t level[3];
  encode_int24(volume_db_to_int24(db), level);
  return Frame(addr, CMD_DSP, {DSP_SUB_LEVEL, DSP_LEVEL_COMPENSATION, level[0], level[1], level[2]});
}

Frame make_delay(uint8_t addr, uint32_t samples) {
  return Frame(addr, CMD_DSP,
               {DSP_SUB_DELAY, static_cast<uint8_t>((samples >> 24) & 0xFF),
                static_cast<uint8_t>((samples >> 16) & 0xFF),
                static_cast<uint8_t>((samples >> 8) & 0xFF), static_cast<uint8_t>(samples & 0xFF)});
}

Frame make_lfe_level(uint8_t addr, float level_db) {
  // Whole decibels in a signed byte, and GLM does not sign-extend into the pad: -4 dB is
  // 00 FC, not FF FC. Clamping before the cast keeps an out-of-range level from wrapping
  // round to the opposite sign, which would be a large boost where an attenuation was meant.
  const float clamped = std::clamp(level_db, MIN_LFE_LEVEL_DB, MAX_LFE_LEVEL_DB);
  const auto level = static_cast<int8_t>(std::lroundf(clamped));
  return Frame(addr, CMD_SUB_LFE_LEVEL, {LFE_LEVEL_PAD, static_cast<uint8_t>(level)});
}

Frame make_audio_source(uint8_t addr, uint8_t input_idx, uint8_t source, uint8_t channel) {
  if (source == SOURCE_DIGITAL_AES3) {
    // Byte 2 is unused for AES3. Byte 3 is the sub-channel of whichever feed this frame
    // describes: the program for input 0, the LFE channel for input 1. Callers pass 0 for
    // input 1 when there is no LFE feed, which is what GLM sends then.
    return Frame(addr, CMD_SELECT_AUDIO_SOURCE,
                 {input_idx, SOURCE_DIGITAL_AES3, 0x00, channel});
  }
  // Analog: byte 2 selects the physical input pair, which differs between input 0 and input 1.
  return Frame(addr, CMD_SELECT_AUDIO_SOURCE,
               {input_idx, SOURCE_ANALOG, (input_idx == 0x00) ? uint8_t{0x02} : uint8_t{0x01}, 0x00});
}

// --- CMD_DSP payload decoding ---

bool parse_dsp_level(const std::vector<uint8_t> &payload, float &db) {
  // Exact length, not a minimum: a longer payload carrying this sub-command is a record this
  // component has not seen and does not understand, not a level with something appended.
  if (payload.size() != 5 || payload[0] != DSP_SUB_LEVEL ||
      payload[1] != DSP_LEVEL_COMPENSATION) {
    return false;
  }
  db = volume_int24_to_db(decode_int24(&payload[2]));
  return true;
}

bool parse_dsp_delay(const std::vector<uint8_t> &payload, uint32_t &samples) {
  if (payload.size() != 5 || payload[0] != DSP_SUB_DELAY) {
    return false;
  }
  samples = (static_cast<uint32_t>(payload[1]) << 24) | (static_cast<uint32_t>(payload[2]) << 16) |
            (static_cast<uint32_t>(payload[3]) << 8) | static_cast<uint32_t>(payload[4]);
  return true;
}

}  // namespace gensam
}  // namespace esphome
