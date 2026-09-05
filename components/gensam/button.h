#pragma once

/// @file button.h
/// @brief ESPHome Button entity implementations for Genelec SAM speaker identification and discovery.
///
/// ===================================================================================
/// ARCHITECTURAL DESIGN RATIONALE
/// ===================================================================================
/// 1. Speaker Identification (GenSAMIdentifyButton):
///    Genelec SAM monitors feature a front bi-color status LED. Pulsing this LED allows
///    users to visually match logical Home Assistant device entities with physical
///    hardware in the studio/room.
///    When pressed, the button invokes GenSAMHub::identify_monitor_by_serial(), which
///    transmits CMD_BYPASS (0x2B) with the LED pulsing bit asserted. The hub automatically
///    restores steady LED status after 5000 ms.
///
/// 2. Bus Rediscovery (GenSAMRediscoverButton):
///    Allows manually triggering a fresh RACE discovery and address assignment cycle
///    without rebooting the ESP32 node.
/// ===================================================================================

#include "esphome/core/component.h"
#include "esphome/components/button/button.h"
#include "hub.h"

#include <string>

namespace esphome {
namespace gensam {

/// @brief Button entity that pulses a specific monitor's LED for physical identification.
class GenSAMIdentifyButton : public button::Button {
 public:
  /// @brief Set the parent GenSAMHub instance.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

  /// @brief Set the target speaker's factory serial number or unique ID string.
  void set_serial_or_id(const std::string &id) { serial_or_id_ = id; }

 protected:
  /// @brief Action executed when user presses the button in Home Assistant.
  void press_action() override {
    if (hub_ != nullptr) {
      hub_->identify_monitor_by_serial(serial_or_id_);
    }
  }

  GenSAMHub *hub_{nullptr};
  std::string serial_or_id_{};
};

/// @brief Button entity to manually re-run RACE bus discovery.
class GenSAMRediscoverButton : public button::Button {
 public:
  /// @brief Set the parent GenSAMHub instance.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

 protected:
  /// @brief Action executed when user presses the button in Home Assistant.
  void press_action() override {
    if (hub_ != nullptr) {
      hub_->rediscover_monitors();
    }
  }

  GenSAMHub *hub_{nullptr};
};

}  // namespace gensam
}  // namespace esphome
