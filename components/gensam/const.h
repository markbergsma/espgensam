#pragma once

/// @file const.h
/// @brief Protocol and hardware constants for the Genelec SAM RS485 ("GLM") bus.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace gensam {

// --- Physical / serial parameters -----------------------------------------
/// Genelec never published the GLM line rate. This figure was measured on a live bus by
/// fitting the bit period to the raw RMT edge intervals: 34.74 ticks at 10 MHz, i.e. 287,773
/// bps, which is 0.079% from 288,000 = 6 x 48 kHz (the monitor DSP sample rate). See
/// docs/glm-protocol-comparison.md section 2 for the measurement and its caveats.
///
/// Earlier releases used 281,250 and worked, as does the independent HLM project at 296,296:
/// the receiver re-synchronizes on every character's start edge and accepts the stop bit at
/// either of two positions, so it tolerates the whole 5.35% span between those two figures.
/// That tolerance is why decode success cannot be used to identify the rate.
static constexpr uint32_t BAUDRATE = 288000;

// --- Wire framing ---------------------------------------------------------
static constexpr uint8_t FRAME_DELIMITER = 0x7E;  ///< End-of-frame delimiter (ASCII '~')
static constexpr uint8_t FRAME_ESCAPE = 0x7D;     ///< Escape prefix byte (ASCII '}')
static constexpr uint8_t ESCAPE_XOR = 0x20;       ///< XOR value for escaped bytes

static constexpr uint8_t NINTH_BIT_FLAG = 0x01;   ///< 9th bit = 1 for address
static constexpr size_t MAX_FRAME_LENGTH = 256;   ///< Safety bound for frame buffer

// --- Addressing -----------------------------------------------------------
static constexpr uint8_t HOST_ADDRESS = 0x01;
static constexpr uint8_t MONITOR_START_ADDR = 0x02;
static constexpr uint8_t MULTICAST_ADDRESS = 0xF0;  ///< Native RS485 multicast address (0xF0)
static constexpr uint8_t BROADCAST_ADDRESS = 0xFF;  ///< Native RS485 broadcast address (0xFF)

// --- Command opcodes ------------------------------------------------------
static constexpr uint8_t CMD_STATUS = 0x00;           ///< Status/telemetry polling
static constexpr uint8_t CMD_ACK = 0x01;              ///< ACK / response opcode
static constexpr uint8_t CMD_SET_RID = 0x02;          ///< Assign device RID
static constexpr uint8_t CMD_STAY_ONLINE = 0x04;      ///< Keep-alive / stay online
static constexpr uint8_t CMD_SIGNAL_GEN = 0x05;       ///< Signal generator; see the block below
static constexpr uint8_t CMD_QUERY_STATUS = 0x08;     ///< GLMv5 monitor telemetry query
static constexpr uint8_t CMD_REPORT_STATUS = 0x09;    ///< GLMv5 monitor telemetry report
static constexpr uint8_t CMD_VOLUME = 0x1F;           ///< Volume control (24-bit)
static constexpr uint8_t CMD_BAR_CODE = 0x19;         ///< Serial number / barcode query
static constexpr uint8_t CMD_HARDWARE_QUERY = 0x22;   ///< Hardware ID query
static constexpr uint8_t CMD_BYPASS = 0x2B;           ///< Bypass / Mute / LED control
static constexpr uint8_t CMD_SESSION_PREAMBLE = 0x2D; ///< GLM app connect only; see the block below
static constexpr uint8_t CMD_DSP = 0x10;              ///< DSP parameter update (PEQ, delay, level)
static constexpr uint8_t CMD_PREPARE_CONFIG = 0x17;   ///< Prepare for a block of DSP settings
static constexpr uint8_t CMD_SOFTWARE_QUERY = 0x39;   ///< Firmware version query
static constexpr uint8_t CMD_WAKEUP = 0x3A;           ///< Wakeup / standby control (0x3A)
static constexpr uint8_t CMD_BASS_MANAGE_XO = 0x3B;   ///< Bass management crossover frequency configuration
static constexpr uint8_t CMD_SUB_LFE_XO = 0x3C;       ///< LFE crossover? See the block below
static constexpr uint8_t CMD_INPUT_SYNC = 0x3D;       ///< Input-select sync; see the block below
static constexpr uint8_t CMD_SUB_LFE_CHANNEL = 0x3E;  ///< LFE channel? See the block below
static constexpr uint8_t CMD_SELECT_AUDIO_SOURCE = 0x40; ///< Audio source selection & AES3 channel assignment
static constexpr uint8_t CMD_SUB_LFE_LEVEL = 0x42;    ///< LFE level? See the block below
static constexpr uint8_t CMD_DISCOVERY = 0xFE;        ///< Monitor discovery ping

// --- Opcodes GLM sends that this component does not -----------------------
/// @name Observed but unsent opcodes
///
/// These are named so that a reader of a bus capture is not left staring at raw bytes. Each is
/// emitted by an OEM GLM adapter, each has a payload that never varies across the five captures
/// in captures/, and none is transmitted here. docs/glm-group-apply.md has the full argument and
/// the frame-by-frame position of each within a group push.
///
///  - CMD_SIGNAL_GEN (0x05): the signal generator. GLM sends exactly one frame of it, per
///    speaker, at the head of every group push: the 13-byte "stop generator / restore" payload
///    04 00 00 00 FF FF EA 00 01 90 00 02 DA, identical in all 35 occurrences. **When a signal
///    generator feature is added here, the group push must send this frame the same way**, so
///    that a generator left running by an earlier operation cannot survive a group switch.
///  - CMD_SESSION_PREAMBLE (0x2D): always FF 2D 00, and only when the GLM app connects, paired
///    with a broadcast CMD_INPUT_SYNC. It is not part of a group push.
///  - CMD_INPUT_SYNC (0x3D): always 00 00. In a full single-device configuration it sits
///    immediately before CMD_SELECT_AUDIO_SOURCE, so it reads as an input-select preamble
///    rather than a latch applied afterwards; in a group push GLM instead sweeps it across
///    every speaker at the close.
///  - CMD_SUB_LFE_XO / _CHANNEL / _LEVEL (0x3C, 0x3E, 0x42): always 00 00, and in 48 observed
///    frames **only ever addressed to the subwoofer**. Their positions are fixed: 0x3C follows
///    the crossover, 0x3E follows the subwoofer's second CMD_SELECT_AUDIO_SOURCE frame, and
///    0x42 follows 0x3E. The names encode a **hypothesis, not a finding**: a .sam group node
///    has exactly four subwoofer-only fields left unaccounted for once Phase(degrees) is mapped
///    to DSP_SUB_DELAY -- LFE_+10, LFE_Channel, LFE_CrossoverFrequency(Hz) and LFE_Level -- and
///    0x3C sits next to CMD_BASS_MANAGE_XO with the same 2-byte big-endian shape. The captured
///    setup runs with LFE inactive, so every one of these frames is 00 00 and the correlation
///    has no variance to test against; LFE_CrossoverFrequency(Hz) is 120 there, which would be
///    00 78 rather than 00 00. Sending a hardcoded 00 00 would therefore be byte-correct for
///    that setup and would silently disable LFE on one that uses it.
///@}

// --- Audio source parameters (CMD_SELECT_AUDIO_SOURCE) --------------------
static constexpr uint8_t SOURCE_ANALOG = 0x01;        ///< Analog input
static constexpr uint8_t SOURCE_DIGITAL_AES3 = 0x02;  ///< Digital AES3 (AES/EBU) input

static constexpr uint8_t AES3_CHANNEL_A = 0x01;       ///< Sub-channel A (Left)
static constexpr uint8_t AES3_CHANNEL_B = 0x02;       ///< Sub-channel B (Right)
static constexpr uint8_t AES3_CHANNEL_SUM = 0x03;     ///< Sub-channel A+B summed mono (Subwoofer)

// --- Audio source switching transient volume parameters -------------------
/// GLM minimum volume payload bytes (digital silence / -130 dBFS during source switching).
static constexpr uint8_t VOLUME_PAYLOAD_SILENCE[3] = {0x00, 0x00, 0x02};

// --- CMD_DSP (0x10) sub-commands -------------------------------------------
/// @name DSP parameter sub-commands
///
/// 0x01 and 0x02 were both settled by capturing GLM switching a calibrated system between
/// three groups; see docs/glm-protocol-comparison.md.
///
///  - 0x01 carries [sub-command, 3-byte level] in the same linear-gain encoding as
///    CMD_VOLUME. Sub-command 0x00 is the **per-device level compensation**, not the "max
///    level restriction" the protocol specification labels it: across the three captured
///    groups its value tracked the setup file's Level_Sensitivity exactly, at -1.9258 dB,
///    -8.3783 dB and 0.0 dB, nowhere near that setup's global -20 dB volume limit.
///    Sub-command 0x09 is emitted by GLM on every device with a constant 0x000000 payload;
///    its meaning is unknown and this component does not send it. It is not a stray: all 28
///    occurrences across the captures follow a 0x00 frame immediately, one for one, so the
///    pair travels together.
///  - 0x02 carries a 4-byte big-endian time-of-flight delay as a sample count **at 48 kHz on
///    every device class**, which resolves the 4x ambiguity the specification flags for
///    subwoofers. On a subwoofer the value is the AutoPhase result expressed as a delay:
///    round(((phase_degrees mod 360) / 360) / crossover_hz * 48000) reproduced all three
///    captured groups exactly (289, 267 and 67 samples). The setup file's
///    Time-of-flight_Compensation field was zero throughout and is not the source.
///@{
static constexpr uint8_t DSP_SUB_LEVEL = 0x01;  ///< Level compensation / boundaries.
static constexpr uint8_t DSP_SUB_DELAY = 0x02;  ///< Time-of-flight delay, samples at 48 kHz.
static constexpr uint8_t DSP_SUB_PEQ = 0x0E;    ///< Parametric EQ band coefficients.
///@}

/// DSP_SUB_LEVEL sub-command selecting the per-device level compensation.
static constexpr uint8_t DSP_LEVEL_COMPENSATION = 0x00;

/// DSP_SUB_LEVEL sub-command GLM pairs 1:1 with DSP_LEVEL_COMPENSATION, payload always 0x000000.
/// Named only so the pairing is visible; never sent, and deliberately not decoded, since its
/// constant payload would read as -130 dB and mute the speaker.
static constexpr uint8_t DSP_LEVEL_UNKNOWN_09 = 0x09;

/// Timebase of a DSP_SUB_DELAY sample count, independent of the device's PEQ design rate.
static constexpr uint32_t DSP_DELAY_RATE_HZ = 48000;

/// Number of parametric EQ slots per device; wire indices run 0x00..PEQ_MAX_INDEX.
static constexpr uint8_t PEQ_BAND_COUNT = 20;
static constexpr uint8_t PEQ_MAX_INDEX = PEQ_BAND_COUNT - 1;  ///< 0x13.

/// @name PEQ design sample rates
///
/// A biquad only means anything relative to the rate it was designed at, and Genelec does not
/// use one rate across the range. Both values are confirmed: from a setup file, where every
/// subwoofer band carries SR:12000 and every two-way band SR:48000, and from a bus capture in
/// which coefficients designed at those rates reproduce GLM's exactly. Designing at the wrong
/// one shifts every filter by the ratio, so a 40 Hz notch lands at 160 Hz.
///
/// The 83x1 series is reported to run a 96 kHz DSP path and has never been captured. Rather
/// than guess, monitors carry an optional override; see GenSAMMonitorBinding::peq_design_rate.
///@{
static constexpr uint32_t PEQ_RATE_SUBWOOFER_HZ = 12000;
static constexpr uint32_t PEQ_RATE_DEFAULT_HZ = 48000;
///@}

/// Trailing type/flag byte of a CMD_DSP PEQ frame. Only 0x00 has ever been observed.
static constexpr uint8_t PEQ_TYPE_FLAG_ACTIVE = 0x00;

/// Payload byte accompanying CMD_PREPARE_CONFIG, always 0x01 in captured OEM traffic.
static constexpr uint8_t PREPARE_CONFIG_PAYLOAD = 0x01;

// --- Bass management crossover parameters ----------------------------------
static constexpr uint16_t DEFAULT_CROSSOVER_HZ = 85;  ///< Factory default bass management crossover frequency (Hz)
static constexpr uint16_t MIN_CROSSOVER_HZ = 50;      ///< Minimum allowable crossover frequency (Hz)
static constexpr uint16_t MAX_CROSSOVER_HZ = 120;     ///< Maximum allowable crossover frequency (Hz)
static constexpr uint16_t CROSSOVER_STEP_HZ = 5;      ///< Supported Genelec crossover frequency step resolution (Hz)

// --- Per-device level trim and time-of-flight delay ------------------------
/// @name Level trim bounds
///
/// The floor is what keeps the digital-silence sentinel out of reach. make_level() encodes
/// anything at or below GENELEC_VOLUME_MIN_DB (-130 dB) as 0x000000, and a GLM setup file
/// writes Calibration_Level: -999 for "not calibrated", so a value that leaks through would
/// mute the speaker rather than trim it. -60 dB is far enough below any real AutoCal trim to
/// be useful and far enough above the sentinel to be safe; the group schema picks it too.
///
/// The ceiling is unity. Level compensation is attenuation only: make_level() clamps positive
/// decibels to 0 dB, so there is nothing above it to offer.
///@{
static constexpr float MIN_LEVEL_DB = -60.0f;   ///< Minimum per-device level trim (dB).
static constexpr float MAX_LEVEL_DB = 0.0f;     ///< Unity; attenuation only.
static constexpr float LEVEL_STEP_DB = 0.1f;    ///< Adjustment resolution offered in Home Assistant (dB).
///@}

/// Longest time-of-flight delay accepted, in samples at DSP_DELAY_RATE_HZ: 192 ms, which is
/// the limit the GLM v5 user interface enforces. Captured AutoPhase results are far smaller
/// (289, 267 and 67 samples), so this is a guard rail rather than a working range.
static constexpr uint32_t MAX_DELAY_SAMPLES = 9216;

/// MAX_DELAY_SAMPLES expressed in milliseconds, which is the unit Home Assistant sees.
static constexpr float MAX_DELAY_MS = 192.0f;

// --- ACK status values ----------------------------------------------------
static constexpr uint8_t ACK_OK = 0x2D;               ///< Positive ACK
static constexpr uint8_t ACK_ERROR = 0x2E;            ///< Negative ACK

// --- CMD_REPORT_STATUS (0x09) telemetry markers ---------------------------
/// @name Telemetry reply markers
///
/// A 0x09 telemetry reply sometimes carries a lone 0x06 or 0x07 byte alongside its tagged
/// records: usually as the first byte, sometimes as the last, occasionally as the whole payload.
/// Its exact meaning is unknown. What the bus captures do establish is what it is *not*:
///
///  - Every marked frame observed so far also carries tag 0x47 = 0x01, i.e. amplifier ACTIVE.
///  - It arrives on the first telemetry poll after a wake burst (0x3A 03 7F / 03 01) or after a
///    configuration push (0x40 and friends). Both contexts are confirmed on live hardware. The
///    delay tracks the poller rather than the speaker: 1.6 s behind a GLM adapter's cadence,
///    3.5 s behind ours.
///  - Genuine standby replies look nothing like this: they carry 0x47 = 0x02, drop the audio
///    records entirely, and are unmarked (e.g. 41 14 47 02 84 03 8D, observed live).
///  - It is not positional: 10 frames carry it leading, 5 trailing, 2 consist of nothing else.
///  - The independent HLM project documents 0x06 as a settings-write / persistence marker and
///    does not handle 0x07.
///
/// So it reads as a transient "just woke" / "settings written" annotation on a monitor that is
/// already running. Earlier releases named 0x07 STATUS_STANDBY and forced monitor.standby true
/// from it, then let that override tag 0x47 — backwards, since the monitor has just *finished*
/// waking. Tag 0x47 is the only field in a 0x09 reply that reports power state; see
/// parse_telemetry().
///
/// The bounds below span exactly the two values that have been observed. Do not widen them
/// without capture evidence: whatever this predicate accepts is silently dropped from telemetry.
///@{
static constexpr uint8_t TELEMETRY_MARKER_FIRST = 0x06;  ///< Lowest observed marker value.
static constexpr uint8_t TELEMETRY_MARKER_LAST = 0x07;   ///< Highest observed marker value.

/// @brief Test whether a 0x09 payload byte is a reply marker rather than a telemetry record.
///
/// Scoped deliberately to the 0x09 telemetry payload: 0x06 and 0x07 are unremarkable bytes
/// elsewhere on the bus (in an ACK, a 0x1F volume triple, a serial string), so this must not be
/// reached for outside parse_telemetry().
/// @param b Candidate payload byte.
/// @return True for the observed marker values (0x06, 0x07), false otherwise.
inline bool is_telemetry_marker(uint8_t b) {
  return b >= TELEMETRY_MARKER_FIRST && b <= TELEMETRY_MARKER_LAST;
}
///@}

// --- LED colors (used with CMD_BYPASS) -----------------------------------
static constexpr uint8_t LED_GREEN = 0;
static constexpr uint8_t LED_RED = 1;
static constexpr uint8_t LED_OFF = 2;
static constexpr uint8_t LED_YELLOW = 3;

// --- CMD_BYPASS (0x2B) control bit flags -----------------------------------
static constexpr uint8_t BYPASS_MUTE_MASK = 0x01;          ///< Bit 0: Mute (1 = muted, 0 = unmuted)
static constexpr uint8_t BYPASS_LED_COLOR_MASK = 0x06;     ///< Bits 1-2: LED Color ((color << 1) & 0x06)
static constexpr uint8_t BYPASS_LED_PULSING_MASK = 0x08;   ///< Bit 3: LED Pulsing / Blink (1 = pulsing)
static constexpr uint8_t BYPASS_INVERT_LED_MASK = 0x10;    ///< Bit 4: Invert LED enable

// --- CMD_WAKEUP (0x3A) control parameters ---------------------------------
static constexpr uint8_t WAKEUP_OP_POWER = 0x03;           ///< Power sub-command byte
static constexpr uint8_t WAKEUP_VAL_ON_1 = 0x7F;           ///< Power ON / Wakeup phase 1
static constexpr uint8_t WAKEUP_VAL_ON_2 = 0x01;           ///< Power ON / Wakeup phase 2
static constexpr uint8_t WAKEUP_VAL_STANDBY_1 = 0x02;      ///< Standby / Sleep phase 1 (prepare)
static constexpr uint8_t WAKEUP_VAL_STANDBY_2 = 0x00;      ///< Standby / Sleep phase 2 (power off / sleep)

}  // namespace gensam
}  // namespace esphome
