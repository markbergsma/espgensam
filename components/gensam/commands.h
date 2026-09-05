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

}  // namespace gensam
}  // namespace esphome
