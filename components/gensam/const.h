#pragma once

/// @file const.h
/// @brief Protocol and hardware constants for the Genelec SAM RS485 ("GLM") bus.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace gensam {

// --- Physical / serial parameters -----------------------------------------
static constexpr uint32_t BAUDRATE = 281250;

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
static constexpr uint8_t CMD_DISCOVERY = 0xFE;        ///< Monitor discovery ping

// --- ACK status values ----------------------------------------------------
static constexpr uint8_t ACK_OK = 0x2D;               ///< Positive ACK
static constexpr uint8_t ACK_ERROR = 0x2E;            ///< Negative ACK

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
static constexpr uint8_t WAKEUP_VAL_ON = 0x7F;             ///< Power ON / Wakeup
static constexpr uint8_t WAKEUP_VAL_STANDBY = 0x01;        ///< Standby / Sleep

}  // namespace gensam
}  // namespace esphome
