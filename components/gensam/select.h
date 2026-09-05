#pragma once

/// @file select.h
/// @brief ESPHome Select entity implementations for Genelec SAM audio source and AES3 channel configuration.
///
/// ===================================================================================
/// ARCHITECTURAL DESIGN RATIONALE
/// ===================================================================================
/// 1. Audio Source Selection (GenSAMSourceSelect):
///    Genelec SAM monitors and subwoofers support dynamic selection between Analog audio
///    inputs and Digital AES3 (AES/EBU) stereo streams via RS-485 opcode CMD_SELECT_AUDIO_SOURCE
///    (0x40).
///    - The wire payload is a 4-byte descriptor: [input_index, source_type, mode, channel].
///    - For Analog: source_type = 0x01. Input 0 uses mode 0x02, channel 0x00.
///      Subwoofers (7xxx series) require a secondary frame for Input 1 with mode 0x01, channel 0x00.
///    - For Digital (AES3): source_type = 0x02, mode = 0x00.
///      Input 0 specifies the routed sub-channel (0x01 = Channel A/Left, 0x02 = Channel B/Right,
///      0x03 = Channel A+B Summed Mono).
///      Subwoofers (7xxx series) require a secondary frame for Input 1 with mode 0x00, channel 0x00.
///    - Non-Destructive Boot: Monitors store persistent input routing in internal flash memory.
///      To prevent overwriting stored presets at boot, the source select entity starts unconfigured
///      (Unknown state in Home Assistant) and does not transmit until explicitly commanded or
///      snooped from an external GLM controller.
///
/// 2. Per-Monitor AES3 Sub-Channel Assignment (GenSAMAES3ChannelSelect):
///    A digital AES3 stream carries two audio sub-channels (Channel A and Channel B).
///    Each SAM speaker must know which channel to decode and reproduce:
///    - "Channel A (Left)" (0x01)
///    - "Channel B (Right)" (0x02)
///    - "Channel A+B (Sum)" (0x03) - typically used by SAM subwoofers to sum stereo low frequencies.
///    - The per-monitor AES3 channel configuration entity allows setting each speaker's sub-channel
///      independently.
///
/// 3. Volatile Persistence & Standby Wakeup Retransmission:
///    When monitors enter amplifier sleep (<0.5W standby), volatile DSP state is powered down.
///    Upon wake from standby, the GenSAM hub automatically rediscovery-cycles and re-transmits the
///    configured audio source and AES3 channel assignments during the CONFIGURING_DEVICES state.
///
/// 4. Passive Bus Snooping:
///    External GLM software or GLM network adapters transmitting 0x40 frames are snooped in real
///    time, immediately updating the Home Assistant select states without bus contention.
/// ===================================================================================

#include "esphome/core/component.h"
#include "esphome/components/select/select.h"
#include "hub.h"
#include "const.h"

#include <string>

namespace esphome {
namespace gensam {

/// @brief Select entity that configures global audio input source (Analog vs Digital AES3).
class GenSAMSourceSelect : public select::Select {
 public:
  /// @brief Set the parent GenSAMHub instance.
  /// @param hub Pointer to the GenSAMHub.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

 protected:
  /// @brief Action executed when user selects an option in Home Assistant.
  /// @param value Selected option string ("Analog" or "Digital (AES3)").
  void control(const std::string &value) override {
    if (hub_ != nullptr) {
      hub_->set_global_source_by_name(value);
    }
  }

  GenSAMHub *hub_{nullptr};
};

/// @brief Select entity that configures the AES3 channel routing for a specific monitor.
class GenSAMAES3ChannelSelect : public select::Select {
 public:
  /// @brief Set the parent GenSAMHub instance.
  /// @param hub Pointer to the GenSAMHub.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

  /// @brief Set the target speaker's factory serial number or unique ID string.
  /// @param id Serial number string or unique ID string.
  void set_serial_or_id(const std::string &id) { serial_or_id_ = id; }

 protected:
  /// @brief Action executed when user selects an AES3 channel option in Home Assistant.
  /// @param value Selected option string ("Channel A (Left)", "Channel B (Right)", or "Channel A+B (Sum)").
  void control(const std::string &value) override {
    if (hub_ != nullptr) {
      hub_->set_monitor_aes3_channel_by_name(serial_or_id_, value);
    }
  }

  GenSAMHub *hub_{nullptr};
  std::string serial_or_id_{};
};

}  // namespace gensam
}  // namespace esphome

