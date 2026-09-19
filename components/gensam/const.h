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
static constexpr uint8_t CMD_QUERY_STATUS = 0x08;     ///< GLMv5 monitor telemetry query
static constexpr uint8_t CMD_REPORT_STATUS = 0x09;    ///< GLMv5 monitor telemetry report
static constexpr uint8_t CMD_VOLUME = 0x1F;           ///< Volume control (24-bit)
static constexpr uint8_t CMD_BAR_CODE = 0x19;         ///< Serial number / barcode query
static constexpr uint8_t CMD_HARDWARE_QUERY = 0x22;   ///< Hardware ID query
static constexpr uint8_t CMD_BYPASS = 0x2B;           ///< Bypass / Mute / LED control
static constexpr uint8_t CMD_SOFTWARE_QUERY = 0x39;   ///< Firmware version query
static constexpr uint8_t CMD_WAKEUP = 0x3A;           ///< Wakeup / standby control (0x3A)
static constexpr uint8_t CMD_BASS_MANAGE_XO = 0x3B;   ///< Bass management crossover frequency configuration
static constexpr uint8_t CMD_SELECT_AUDIO_SOURCE = 0x40; ///< Audio source selection & AES3 channel assignment
static constexpr uint8_t CMD_DISCOVERY = 0xFE;        ///< Monitor discovery ping

// --- Audio source parameters (CMD_SELECT_AUDIO_SOURCE) --------------------
static constexpr uint8_t SOURCE_ANALOG = 0x01;        ///< Analog input
static constexpr uint8_t SOURCE_DIGITAL_AES3 = 0x02;  ///< Digital AES3 (AES/EBU) input

static constexpr uint8_t AES3_CHANNEL_A = 0x01;       ///< Sub-channel A (Left)
static constexpr uint8_t AES3_CHANNEL_B = 0x02;       ///< Sub-channel B (Right)
static constexpr uint8_t AES3_CHANNEL_SUM = 0x03;     ///< Sub-channel A+B summed mono (Subwoofer)

// Entity option strings
static constexpr const char *SOURCE_STR_ANALOG = "Analog";
static constexpr const char *SOURCE_STR_DIGITAL_AES3 = "Digital (AES3)";

static constexpr const char *AES3_CHANNEL_STR_A = "Channel A (Left)";
static constexpr const char *AES3_CHANNEL_STR_B = "Channel B (Right)";
static constexpr const char *AES3_CHANNEL_STR_SUM = "Channel A+B (Sum)";

/// @brief Map AES3 channel byte value to its display string.
/// @param ch AES3_CHANNEL_A (0x01), AES3_CHANNEL_B (0x02), or AES3_CHANNEL_SUM (0x03).
/// @return Corresponding entity option string; defaults to AES3_CHANNEL_STR_A for unknown values.
inline const char *aes3_channel_to_str(uint8_t ch) {
  return (ch == AES3_CHANNEL_B) ? AES3_CHANNEL_STR_B :
         (ch == AES3_CHANNEL_SUM) ? AES3_CHANNEL_STR_SUM : AES3_CHANNEL_STR_A;
}

// --- Audio source switching transient volume parameters -------------------
/// GLM minimum volume payload bytes (digital silence / -130 dBFS during source switching).
static constexpr uint8_t VOLUME_PAYLOAD_SILENCE[3] = {0x00, 0x00, 0x02};

// --- Bass management crossover parameters ----------------------------------
static constexpr uint16_t DEFAULT_CROSSOVER_HZ = 85;  ///< Factory default bass management crossover frequency (Hz)
static constexpr uint16_t MIN_CROSSOVER_HZ = 50;      ///< Minimum allowable crossover frequency (Hz)
static constexpr uint16_t MAX_CROSSOVER_HZ = 120;     ///< Maximum allowable crossover frequency (Hz)
static constexpr uint16_t CROSSOVER_STEP_HZ = 5;      ///< Supported Genelec crossover frequency step resolution (Hz)

// --- ACK status values ----------------------------------------------------
static constexpr uint8_t ACK_OK = 0x2D;               ///< Positive ACK
static constexpr uint8_t ACK_ERROR = 0x2E;            ///< Negative ACK

// --- Status report payload values -----------------------------------------
static constexpr uint8_t STATUS_STANDBY = 0x07;       ///< Monitor status payload: monitor in standby / sleep

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
