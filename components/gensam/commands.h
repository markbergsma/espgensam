#pragma once

/// @file commands.h
/// @brief Builders for the Genelec SAM ("GLM") command frames the hub transmits.
///
/// ===================================================================================
/// PROTOCOL COMMAND VOCABULARY
/// ===================================================================================
/// Every frame the hub puts on the wire is constructed here, as a pure function of its
/// parameters.  Centralising construction serves three purposes:
///
/// 1. Single Definition Per Command:
///    Payload layouts (bit packing of CMD_BYPASS, the big-endian 24-bit CMD_VOLUME word,
///    the four-byte CMD_SELECT_AUDIO_SOURCE record) are expressed exactly once, so a
///    protocol correction cannot be applied to some call sites and missed at others.
///
/// 2. Separation From Transport:
///    These functions know nothing about bus arbitration, transceiver timing, or hub
///    state.  They neither transmit nor consult `can_transmit()`; the caller decides
///    whether and when a frame reaches the wire.  This keeps the protocol layer free of
///    ESPHome and ESP-IDF dependencies.
///
/// 3. Documented Magic Numbers:
///    Payload constants that are not covered by a named symbol in const.h (for instance
///    the analog input-pair selector byte) are explained at their single point of use.
/// ===================================================================================

#include <cstdint>
#include <vector>

#include "biquad.h"
#include "const.h"
#include "frame.h"

namespace esphome {
namespace gensam {

/// @brief Build the RACE discovery ping broadcast (0xFF 0xFE).
///
/// Unassigned monitors compete with a carrier-sense backoff; the winner replies with its
/// 3-byte hardware serial.
/// @return Broadcast CMD_DISCOVERY frame with no payload.
Frame make_discovery_ping();

/// @brief Build the RACE address assignment multicast (0xF0 0x02).
/// @param racing_bytes The 3-byte hardware serial reported by the winning monitor.
/// @param assign_addr Logical bus address to assign (0x02..0x7F).
/// @return Multicast CMD_SET_RID frame carrying [serial(3), address].
Frame make_set_rid(const std::vector<uint8_t> &racing_bytes, uint8_t assign_addr);

/// @brief Build the keep-alive broadcast (0xFF 0x04) that refreshes monitor address leases.
/// @return Broadcast CMD_STAY_ONLINE frame with no payload.
Frame make_stay_online();

/// @brief Build one step of a CMD_WAKEUP (0x3A) power sequence.
/// @param op Sub-command byte (e.g. WAKEUP_OP_POWER).
/// @param val Sub-command value (e.g. WAKEUP_VAL_ON_1, WAKEUP_VAL_STANDBY_2).
/// @return Broadcast CMD_WAKEUP frame carrying [op, val].
Frame make_wakeup_step(uint8_t op, uint8_t val);

/// @brief Build the master volume broadcast (0xFF 0x1F) for a decibel level.
/// @param db Target volume in dBFS; converted to the 24-bit fixed-point wire value.
/// @return Broadcast CMD_VOLUME frame carrying the big-endian 24-bit multiplier.
Frame make_broadcast_volume_db(float db);

/// @brief Build the transient digital-silence volume broadcast used before input switching.
/// @return Broadcast CMD_VOLUME frame carrying VOLUME_PAYLOAD_SILENCE (-130 dBFS).
Frame make_broadcast_volume_silence();

/// @brief Build a unicast device query.
/// @param addr Target monitor bus address.
/// @param cmd Query opcode (CMD_SOFTWARE_QUERY, CMD_BAR_CODE, CMD_QUERY_STATUS, ...).
/// @return Unicast query frame; CMD_BAR_CODE additionally carries its {0x01} selector payload.
Frame make_query(uint8_t addr, uint8_t cmd);

/// @brief Build a CMD_BYPASS (0x2B) mute and front-LED control frame.
///
/// Two distinct LED presentations are encoded, matching GLM behaviour:
/// - Steady (@p pulsing false): muted shows LED_RED, unmuted shows LED_OFF.
/// - Identify (@p pulsing true): LED_OFF with the pulsing bit set, mute state preserved.
/// @param addr Target address; may be a monitor, MULTICAST_ADDRESS, or BROADCAST_ADDRESS.
/// @param mute True to mute the channel.
/// @param pulsing True to pulse the front LED for physical identification.
/// @return CMD_BYPASS frame carrying the packed control byte.
Frame make_bypass(uint8_t addr, bool mute, bool pulsing = false);

/// @brief Build a bass management crossover frequency frame (CMD_BASS_MANAGE_XO 0x3B).
/// @param addr Target address (monitor, multicast, or broadcast).
/// @param freq_hz Crossover filter frequency in Hz, sent big-endian.
/// @return CMD_BASS_MANAGE_XO frame carrying the 16-bit frequency.
Frame make_crossover(uint8_t addr, uint16_t freq_hz);

/// @brief Build an audio source selection frame (CMD_SELECT_AUDIO_SOURCE 0x40).
///
/// Standard monitors are configured with input 0 only; 7xxx subwoofers additionally
/// require input 1.  The third payload byte selects the physical analog input pair and
/// differs per input index; it is unused (0x00) for AES3.  The AES3 sub-channel applies
/// to input 0 only.
/// @param addr Target monitor bus address.
/// @param input_idx Input index (0x00 primary, 0x01 secondary / subwoofer).
/// @param source SOURCE_ANALOG (0x01) or SOURCE_DIGITAL_AES3 (0x02).
/// @param channel AES3_CHANNEL_A / _B / _SUM; ignored for analog and for input 1.
/// @return CMD_SELECT_AUDIO_SOURCE frame carrying the 4-byte selection record.
Frame make_audio_source(uint8_t addr, uint8_t input_idx, uint8_t source, uint8_t channel);

/// @brief Build a "prepare for configuration" frame (CMD_PREPARE_CONFIG 0x17).
///
/// GLM sends this once per speaker immediately before a block of DSP settings, both during
/// enumeration and when re-applying a group.  What the monitor does with it is not known -
/// the protocol specification marks its meaning unresolved - but the exact frame
/// (`02' 17 01 1B 5A 7E`) appears in this project's own bus captures, so emitting it
/// reproduces observed OEM behaviour rather than inventing one.
/// @param addr Target monitor bus address.
/// @return CMD_PREPARE_CONFIG frame carrying its single {0x01} payload byte.
Frame make_prepare_config(uint8_t addr);

/// @brief Build one parametric EQ band frame (CMD_DSP 0x10, sub-command DSP_SUB_PEQ 0x0E).
///
/// Monitors carry a 20-slot cascade of second-order sections and accept only finished
/// coefficients; design them with design_biquad() in biquad.h, which also applies the
/// normalisation and feedback sign inversion the DSP expects.
///
/// The five floats go on the wire as IEEE-754 float32 in **little-endian** byte order, which
/// is the one place in this protocol that is not big-endian.  Coefficient bytes routinely
/// collide with the framing tokens 0x7E and 0x7D; that is handled correctly because
/// Frame::to_9bit() computes the CRC over the raw payload and escapes only afterwards.
///
/// @param addr Target monitor bus address.
/// @param index Filter slot, 0x00..0x13.  Frames for an out-of-range index are not built.
/// @param c The five normalised, sign-inverted coefficients, in transmission order.
/// @return CMD_DSP frame carrying [0x0E, index, b0, b1, b2, a1, a2, type], or an empty
///         default-constructed Frame if @p index exceeds PEQ_MAX_INDEX.
Frame make_peq_band(uint8_t addr, uint8_t index, const BiquadCoeffs &c);

/// @brief Build a per-device level compensation frame (CMD_DSP 0x10, DSP_SUB_LEVEL 0x01).
///
/// This is the trim AutoCal derives to match a speaker's output to the rest of the group -
/// the setup file's Level_Sensitivity - and it changes between groups, so it has to be
/// re-sent whenever the active group changes.  Despite the name the protocol specification
/// gives sub-command 0x00, it is not a maximum-level limit; see const.h.
///
/// The level shares CMD_VOLUME's encoding, so volume_db_to_int24() applies unchanged: its
/// scale of 2^23-1 reproduces every captured value exactly, including 0 dB as 0x7FFFFF.
///
/// @param addr Target monitor bus address.
/// @param db Attenuation in decibels; 0.0 is unity and positive values clamp to it.  Levels
///           at or below -130 dB encode as digital silence, so callers must filter sentinel
///           values - a GLM setup file writes Calibration_Level: -999 for "not calibrated",
///           and passing that through would mute the speaker.
/// @return CMD_DSP frame carrying [0x01, 0x00, level(3, big-endian)].
Frame make_level(uint8_t addr, float db);

/// @brief Build a time-of-flight delay frame (CMD_DSP 0x10, DSP_SUB_DELAY 0x02).
/// @param addr Target monitor bus address.
/// @param samples Delay in samples at 48 kHz (DSP_DELAY_RATE_HZ) on every device class,
///                including subwoofers, whose PEQ is designed at 12 kHz but whose delay is
///                not.  0 means no added delay.
/// @return CMD_DSP frame carrying [0x02, samples(4, big-endian)].
Frame make_delay(uint8_t addr, uint32_t samples);

}  // namespace gensam
}  // namespace esphome
