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
/// @param op Power sub-command, normally WAKEUP_OP_POWER.
/// @param val Power sub-command value, such as WAKEUP_VAL_ON_1 or WAKEUP_VAL_STANDBY_2.
/// @return Broadcast CMD_WAKEUP frame carrying [op, val].
Frame make_wakeup_step(uint8_t op, uint8_t val);

/// @brief Build an ISS monitor LED enable/disable frame (CMD_WAKEUP 0x3A, sub-command 0x06).
/// @param addr Target monitor address.
/// @param enabled True to enable the monitor LEDs, false to disable them.
/// @return CMD_WAKEUP frame carrying [0x06, 0x00] when enabled or [0x06, 0x01] when disabled.
Frame make_iss_led_state(uint8_t addr, bool enabled);

/// @brief Build an ISS sleep-delay frame (CMD_WAKEUP 0x3A, sub-command 0x01).
/// @param addr Target address; a monitor or BROADCAST_ADDRESS.
/// @param minutes Delay in minutes, clamped to ISS_SLEEP_DELAY_MAX_MIN, or ISS_SLEEP_DELAY_NEVER.
/// @return CMD_WAKEUP frame carrying [0x01, minutes as big-endian 32-bit].
Frame make_iss_sleep_delay(uint8_t addr, uint32_t minutes);

/// @brief Build an ISS threshold frame (CMD_WAKEUP 0x3A, sub-command 0x02).
/// @param addr Target address; a monitor or BROADCAST_ADDRESS.
/// @param dbu Threshold in dBu, e.g. ISS_THRESHOLD_MEDIUM_DBU.
/// @return CMD_WAKEUP frame carrying [0x02, dbu as big-endian 32-bit two's complement].
Frame make_iss_threshold(uint8_t addr, int32_t dbu);

/// @brief Build an ISS read (CMD_WAKEUP 0x3A with the read bit set on the sub-command).
///
/// The monitor answers with CMD_REPORT_STATUS to the host. The value is a big-endian 32-bit
/// word for WAKEUP_OP_ISS_SLEEP_DELAY, WAKEUP_OP_ISS_THRESHOLD and WAKEUP_OP_ISS_LEVEL, and a
/// single ISS_STATE_* byte for WAKEUP_OP_POWER. A read neither resets the ISS timer nor
/// wakes a sleeping monitor, which is what makes it usable as the sleep-state probe.
/// @param addr Target monitor address; it must hold a session (see make_set_rid()).
/// @param op The sub-command to read, e.g. WAKEUP_OP_ISS_LEVEL.
/// @return Unicast CMD_WAKEUP frame carrying [WAKEUP_READ_FLAG | op].
Frame make_iss_read(uint8_t addr, uint8_t op);

/// @brief Decode the 32-bit word of an ISS read reply.
/// @param payload The reply payload.
/// @param[out] value The big-endian word, as two's complement where the value is signed.
/// @return True if @p payload is exactly four bytes.
bool parse_iss_word(const std::vector<uint8_t> &payload, int32_t &value);

/// @brief Build a store-working-settings frame (CMD_SERVICE 0x15, SERVICE_STORE_USER_PARS).
///
/// Writes the speaker's working settings into its stored start-up profile. Never sent by this
/// component; see const.h.
/// @param addr Target monitor address.
/// @return CMD_SERVICE frame carrying [0x33, SERVICE_PROFILE_DEFAULT].
Frame make_store_user_params(uint8_t addr);

/// @brief Build a filter-bypass frame (CMD_BYPASS_FILTERS 0x53).
/// @param addr Target monitor address.
/// @param mask One FILTER_BYPASS_* value; the masks are not combinable.
/// @return CMD_BYPASS_FILTERS frame carrying [0x00, mask].
Frame make_filter_bypass(uint8_t addr, uint8_t mask);

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
/// @return Unicast query frame; CMD_BAR_CODE and CMD_DIP_SWITCH carry their selector payloads.
Frame make_query(uint8_t addr, uint8_t cmd);

/// @brief Check whether a frame is the 0x55 0x84 current DIP-status query.
/// @param frame The observed frame.
/// @return True only for CMD_DIP_SWITCH with the single DIPSW_READ_STATUS selector.
bool is_dip_switch_status_query(const Frame &frame);

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

/// @brief Build a live monitor LED enable/disable frame (CMD_BYPASS 0x2B).
/// @param addr Target monitor address.
/// @param mute Current mute state, preserved in bit 0.
/// @param enabled True to enable the LED, false to disable it.
/// @return CMD_BYPASS frame carrying 0x04 (enabled) or 0x08 (disabled), with the mute bit preserved.
Frame make_bypass_led_state(uint8_t addr, bool mute, bool enabled);

/// @brief Build a bass management crossover frame (CMD_BASS_MANAGE_XO 0x3B).
/// @param addr Target address (monitor, multicast, or broadcast).
/// @param freq_hz Crossover filter frequency in Hz, or CROSSOVER_FULL_BAND; sent big-endian.
/// @return CMD_BASS_MANAGE_XO frame carrying the 16-bit word.
Frame make_crossover(uint8_t addr, uint16_t freq_hz);

/// @brief Build an audio source selection frame (CMD_SELECT_AUDIO_SOURCE 0x40).
///
/// Standard monitors are configured with input 0 only; 7xxx subwoofers additionally
/// require input 1.  The third payload byte selects the physical analog input pair and
/// differs per input index; it is unused (0x00) for AES3.
///
/// On a subwoofer the two inputs are two feeds, not one setting sent twice: input 0 is the
/// bass-managed program and **input 1 is the LFE channel**.  Its sub-channel byte is therefore
/// the setup file's `LFE_Channel`, which is 0x00 -- nothing -- whenever LFE is unused.  Pass 0
/// for @p channel on input 1 unless the device has an LFE feed.
/// @param addr Target monitor bus address.
/// @param input_idx Input index (0x00 primary, 0x01 secondary / subwoofer LFE).
/// @param source SOURCE_ANALOG (0x01) or SOURCE_DIGITAL_AES3 (0x02).
/// @param channel AES3_CHANNEL_A / _B / _SUM for input 0, the LFE channel for input 1;
///                ignored for analog.
/// @return CMD_SELECT_AUDIO_SOURCE frame carrying the 4-byte selection record.
Frame make_audio_source(uint8_t addr, uint8_t input_idx, uint8_t source, uint8_t channel);

/// @brief Build a subwoofer LFE level frame (CMD_SUB_LFE_LEVEL 0x3E).
///
/// Carries `LFE_Level` and `LFE_+10` summed, as whole decibels; see const.h for the encoding
/// and the captures that settled it.  Subwoofers only: no two-way monitor has ever been sent
/// this opcode.
/// @param addr Target monitor bus address.
/// @param level_db Effective LFE level in dB, rounded to the nearest whole decibel and clamped
///                 to the signed-byte range the wire field can express.
/// @return CMD_SUB_LFE_LEVEL frame carrying the 2-byte record.
Frame make_lfe_level(uint8_t addr, float level_db);

/// @brief Build the "use stored settings" frame (CMD_NET_SETTINGS 0x17, payload 0x01).
///
/// GLM sends this once per speaker immediately before a block of DSP settings, both during
/// enumeration and when re-applying a group.  The firmware takes it as "use the stored
/// settings and ignore the DIP switches" (GLM bus decoding guide v3.0, section 6.6), so the
/// values about to arrive are not overridden by the rear-panel switches.  The exact frame
/// (`02' 17 01 1B 5A 7E`) appears in this project's own bus captures.
/// @param addr Target monitor bus address.
/// @return CMD_NET_SETTINGS frame carrying NET_SETTINGS_USE_STORED.
Frame make_use_stored_settings(uint8_t addr);

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

// --- CMD_DSP payload decoding -------------------------------------------------------------
//
// The inverses of make_level() and make_delay(), used by the snoop decoder to follow changes
// an external GLM controller makes.  They live beside the builders deliberately: the two
// directions share one payload layout, and separating them is how a correction gets applied
// to the encoder and missed in the decoder.
//
// Both check structure only - length, sub-command, selector - and decode faithfully.  Whether
// a decoded value is one this component is willing to assert is a policy question, answered
// by the caller; see snoop_dsp_().

/// @brief Decode a per-device level compensation payload (CMD_DSP, DSP_SUB_LEVEL 0x01).
///
/// The selector check is not optional.  GLM emits `10 01 09 00 00 00` on every device
/// alongside the real level - a sub-command whose meaning is unknown and whose payload is
/// constantly zero (see const.h).  Decoding that as a level yields 0x000000, which is
/// -130 dB, which is digital silence: a decoder that matched the sub-command alone would
/// store a mute for every speaker GLM touched.
///
/// @param payload The frame payload.
/// @param[out] db Decoded level in decibels; may be as low as GENELEC_VOLUME_MIN_DB.
/// @return True if @p payload is a well-formed per-device level compensation record.
bool parse_dsp_level(const std::vector<uint8_t> &payload, float &db);

/// @brief Decode a time-of-flight delay payload (CMD_DSP, DSP_SUB_DELAY 0x02).
/// @param payload The frame payload.
/// @param[out] samples Decoded delay in samples at DSP_DELAY_RATE_HZ; not range-checked.
/// @return True if @p payload is a well-formed delay record.
bool parse_dsp_delay(const std::vector<uint8_t> &payload, uint32_t &samples);

}  // namespace gensam
}  // namespace esphome
