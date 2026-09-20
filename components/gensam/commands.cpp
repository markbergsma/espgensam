/// @file commands.cpp
/// @brief Genelec SAM command frame builders.
/// See commands.h for the protocol command vocabulary and complete API documentation.

#include "commands.h"

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

Frame make_audio_source(uint8_t addr, uint8_t input_idx, uint8_t source, uint8_t channel) {
  if (source == SOURCE_DIGITAL_AES3) {
    // Byte 2 is unused for AES3; the sub-channel in byte 3 applies to the primary input only.
    return Frame(addr, CMD_SELECT_AUDIO_SOURCE,
                 {input_idx, SOURCE_DIGITAL_AES3, 0x00, (input_idx == 0x00) ? channel : uint8_t{0x00}});
  }
  // Analog: byte 2 selects the physical input pair, which differs between input 0 and input 1.
  return Frame(addr, CMD_SELECT_AUDIO_SOURCE,
               {input_idx, SOURCE_ANALOG, (input_idx == 0x00) ? uint8_t{0x02} : uint8_t{0x01}, 0x00});
}

}  // namespace gensam
}  // namespace esphome
