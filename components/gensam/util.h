#pragma once

/// @file util.h
/// @brief Volume scaling, 24-bit fixed-point arithmetic, and decibel conversion utilities.
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// Genelec Smart Active Monitors (SAM) encode audio volume as a 24-bit signed/unsigned
/// fractional multiplier (represented as an integer ranging from 0 to 2^23 - 1 = 8,388,607):
///
/// 1. Mathematical Mapping:
///    - Fractional Ratio:
///        vol_ratio = sint24 / (2^23 - 1)
///    - Decibel Level:
///        dB = 20.0 * log10(vol_ratio)
///    - Inverse (dB to 24-bit fixed point):
///        vol_ratio = 10^(dB / 20.0)
///        sint24    = round(vol_ratio * (2^23 - 1))
///
/// 2. Wire Representation (Big-Endian):
///    Volume is transmitted over RS-485 via CMD_VOLUME (0x1F) as a 3-byte payload:
///      Byte 0: (sint24 >> 16) & 0xFF
///      Byte 1: (sint24 >> 8) & 0xFF
///      Byte 2: sint24 & 0xFF
///    For example, -30.0 dB produces sint24 = 265,271 -> bytes [0x04, 0x0C, 0x37].
///
/// 3. Home Assistant 0.0 - 1.0 Slider Mapping:
///    Home Assistant's MediaPlayer component represents volume as a linear float from 0.0
///    to 1.0. To provide an intuitive, natural perceptual response without abrupt jumps at
///    low levels, volume is mapped across configurable logarithmic bounds
///    [min_volume_db, max_volume_db] (e.g. -80.0 dB to 0.0 dB):
///      dB = min_volume_db + slider * (max_volume_db - min_volume_db)
///
/// 4. Per-Device Level Trim and Time-of-Flight Delay:
///    Both are DSP parameters a group preset carries per speaker and that Home Assistant can
///    also set by hand, so the conversions live here rather than in the entity layer: the
///    registry needs them to publish, the hub needs them to clamp, and the number entities
///    need them to interpret what the user typed. Two rules hold throughout:
///      - Level is attenuation only, and its floor is a safety bound rather than a taste
///        judgement; see MIN_LEVEL_DB in const.h.
///      - Delay is stored and transmitted as a sample count at 48 kHz. Milliseconds are a
///        presentation unit and are derived from the samples, never the other way round.
/// ===================================================================================

#include "const.h"

#include <cmath>
#include <cstdint>
#include <algorithm>

namespace esphome {
namespace gensam {

/// Maximum 24-bit multiplier value (2^23 - 1) corresponding to 0.0 dBFS unity gain.
static constexpr uint32_t GENELEC_VOLUME_MAX_INT24 = 8388607;

/// Minimum decibel floor considered above digital silence (-130.0 dB).
static constexpr float GENELEC_VOLUME_MIN_DB = -130.0f;

/// @brief Convert a Genelec 24-bit fixed-point integer to decibels (dBFS).
/// @param val 24-bit unsigned volume value (0 to 8,388,607).
/// @return Decibel level (0.0 dBFS at max, <= -130.0 dBFS at 0 or mute).
inline float volume_int24_to_db(uint32_t val) {
  if (val == 0) {
    return GENELEC_VOLUME_MIN_DB;
  }
  float ratio = static_cast<float>(val) / static_cast<float>(GENELEC_VOLUME_MAX_INT24);
  float db = 20.0f * std::log10(ratio);
  return std::max(db, GENELEC_VOLUME_MIN_DB);
}

/// @brief Convert decibels (dBFS) to a Genelec 24-bit fixed-point integer.
/// @param db Decibel level (<= 0.0 dBFS, or negative).
/// @return 24-bit unsigned integer clamped to [0, 8,388,607].
inline uint32_t volume_db_to_int24(float db) {
  if (db <= GENELEC_VOLUME_MIN_DB) {
    return 0;
  }
  if (db >= 0.0f) {
    return GENELEC_VOLUME_MAX_INT24;
  }
  float ratio = std::pow(10.0f, db / 20.0f);
  double val = std::round(static_cast<double>(ratio) * static_cast<double>(GENELEC_VOLUME_MAX_INT24));
  return static_cast<uint32_t>(std::clamp(val, 0.0, static_cast<double>(GENELEC_VOLUME_MAX_INT24)));
}

/// @brief Convert a Home Assistant volume slider (0.0 to 1.0) to decibels.
/// @param slider Home Assistant float value (0.0 to 1.0).
/// @param min_db Minimum decibel boundary corresponding to slider = 0.0 (e.g. -80.0 dB).
/// @param max_db Maximum decibel boundary corresponding to slider = 1.0 (e.g. 0.0 dB).
/// @return Mapped decibel value.
inline float volume_slider_to_db(float slider, float min_db = -80.0f, float max_db = 0.0f) {
  float clamped = std::clamp(slider, 0.0f, 1.0f);
  if (clamped <= 0.001f) {
    return GENELEC_VOLUME_MIN_DB;
  }
  return min_db + clamped * (max_db - min_db);
}

/// @brief Convert decibels to a Home Assistant volume slider position (0.0 to 1.0).
/// @param db Decibel value.
/// @param min_db Minimum decibel boundary corresponding to slider = 0.0 (e.g. -80.0 dB).
/// @param max_db Maximum decibel boundary corresponding to slider = 1.0 (e.g. 0.0 dB).
/// @return Slider position float clamped to [0.0, 1.0].
inline float volume_db_to_slider(float db, float min_db = -80.0f, float max_db = 0.0f) {
  if (db <= min_db) {
    return 0.0f;
  }
  if (db >= max_db) {
    return 1.0f;
  }
  return (db - min_db) / (max_db - min_db);
}

/// @brief Clamp a per-device level trim to the range the hardware may safely be given.
///
/// Every caller goes through this, not only the Home Assistant entity. A YAML lambda can
/// reach set_monitor_level_by_serial() with any float at all, and make_level() turns anything
/// at or below -130 dB into digital silence, so an unclamped -999 from a GLM setup file would
/// mute the speaker. NaN is treated as the floor rather than propagating: std::clamp with a
/// NaN argument is unspecified, and a silent NaN reaching the encoder is worse than a
/// conservative value.
/// @param db Requested level trim in decibels.
/// @return @p db clamped to [MIN_LEVEL_DB, MAX_LEVEL_DB].
inline float clamp_level_db(float db) {
  if (std::isnan(db)) {
    return MIN_LEVEL_DB;
  }
  return std::clamp(db, MIN_LEVEL_DB, MAX_LEVEL_DB);
}

/// @brief Convert a time-of-flight delay in milliseconds to a DSP sample count.
///
/// Samples are the authoritative representation: that is what goes on the wire, what a group
/// preset stores, and what a monitor binding holds. Milliseconds exist only because they are
/// what a person reasons about, so this direction rounds to the nearest whole sample and the
/// reverse direction reports exactly what was stored. Rounding here rather than in the entity
/// is what makes a write a projection - converting an already-converted value changes nothing
/// further - so repeated adjustment cannot accumulate drift.
/// @param ms Requested delay in milliseconds; NaN and negatives yield 0.
/// @return Sample count at DSP_DELAY_RATE_HZ, clamped to [0, MAX_DELAY_SAMPLES].
inline uint32_t delay_ms_to_samples(float ms) {
  if (std::isnan(ms) || ms <= 0.0f) {
    return 0;
  }
  if (ms >= MAX_DELAY_MS) {
    return MAX_DELAY_SAMPLES;
  }
  double samples = std::round(static_cast<double>(ms) * DSP_DELAY_RATE_HZ / 1000.0);
  return static_cast<uint32_t>(std::clamp(samples, 0.0, static_cast<double>(MAX_DELAY_SAMPLES)));
}

/// @brief Convert a DSP sample count to a delay in milliseconds.
///
/// The result is frequently not a round number - an AutoPhase result of 289 samples is
/// 6.0208333 ms - because the sample grid and any display step are unrelated. Reporting the
/// exact equivalent of what was transmitted is deliberate: it is the only value that is true.
/// @param samples Sample count at DSP_DELAY_RATE_HZ.
/// @return Delay in milliseconds.
inline float delay_samples_to_ms(uint32_t samples) {
  return static_cast<float>(static_cast<double>(samples) * 1000.0 / DSP_DELAY_RATE_HZ);
}

/// @brief Encode a 24-bit integer into 3 big-endian bytes.
/// @param val 24-bit value to encode.
/// @param[out] out 3-byte array to store [MSB, Mid, LSB].
inline void encode_int24(uint32_t val, uint8_t out[3]) {
  out[0] = static_cast<uint8_t>((val >> 16) & 0xFF);
  out[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>(val & 0xFF);
}

/// @brief Decode 3 big-endian bytes into a 24-bit unsigned integer.
/// @param in 3-byte array [MSB, Mid, LSB].
/// @return Decoded 24-bit integer.
inline uint32_t decode_int24(const uint8_t in[3]) {
  return (static_cast<uint32_t>(in[0]) << 16) |
         (static_cast<uint32_t>(in[1]) << 8) |
         static_cast<uint32_t>(in[2]);
}

}  // namespace gensam
}  // namespace esphome
