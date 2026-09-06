#pragma once

/// @file status_led.h
/// @brief ESPHome component driving an onboard RGB status LED from bus status text sensor updates.
///
/// ===================================================================================
/// ARCHITECTURAL DESIGN RATIONALE
/// ===================================================================================
/// 1. Single Responsibility & Decoupling:
///    The core RS-485 protocol components (GenSAMHub, BusArbiter, MonitorRegistry, race)
///    maintain zero awareness of RGB LEDs, color math, or ESPHome light components.
///    Instead, the hub publishes its operational state to a standard ESPHome
///    text_sensor::TextSensor entity (e.g. "bus_status").
///
/// 2. Configurable Operating Modes (StatusLEDMode):
///    GenSAMStatusLED provides a modular architecture with a configurable `mode` option
///    (default: `bus_status`). This encapsulates the translation logic from domain events
///    into physical LED color/brightness states, allowing future modes (such as volume level
///    or mute state visualization) to be added without modifying core bus protocol code:
///      - BUS_STATUS: Subscribes to the bus status text sensor and displays operational states:
///        * "Active"      -> Solid Green (#00FF00): Normal healthy listening loop
///        * "GLM Active"  -> Amber / Orange (#FF8000): External GLM master has bus control
///        * "Discovering" -> Blue (#0066FF): Active RACE discovery / device interrogation
///        * "Configuring" -> Cyan (#00CCFF): Device parameter setup (source/crossover)
///        * "Standby"     -> Off: Monitors in low-power sleep (<0.5W)
///        * "Offline"     -> Dim White: Standalone idle, no monitors discovered
///
/// 3. Non-Volatile Hardware Protection & Brightness Preservation:
///    The configured brightness (default 50%) is applied only as the initial default upon boot.
///    If the user adjusts the light brightness via Home Assistant (or an automation), that chosen
///    brightness level is preserved across all subsequent bus status transitions until the next
///    reboot, when the configured default applies again.
///    Switching the light off from Home Assistant is likewise respected: the LED is left off and
///    only its color keeps tracking the bus status, so it shows the current state the moment it is
///    switched back on. Status changes never switch it back on by themselves.
///    All light state calls use `call.set_save(false)` so rapid bus status changes never wear out
///    the ESP32 flash memory; brightness is deliberately not persisted across power cycles.
///    Smooth 150 ms transitions are requested to provide a polished physical visual indicator on
///    boards like M5Stack AtomS3 Lite.
/// ===================================================================================

#include "esphome/core/component.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <string>

namespace esphome {
namespace gensam {

/// @brief Operating modes supported by the GenSAM status LED indicator.
enum class StatusLEDMode : uint8_t {
  BUS_STATUS = 0,  ///< Display real-time RS-485 bus and controller operational state.
};

/// @brief Standalone component that translates system state updates into RGB LED states.
class GenSAMStatusLED : public Component {
 public:
  /// @brief Set the target RGB light entity (e.g. onboard WS2812 on AtomS3 Lite).
  /// @param light Pointer to the LightState entity.
  void set_light(light::LightState *light) { light_ = light; }

  /// @brief Set the operating mode of the status LED.
  /// @param mode Mode enum (StatusLEDMode::BUS_STATUS, etc.).
  void set_mode(StatusLEDMode mode) { mode_ = mode; }

  /// @brief Get the configured operating mode.
  /// @return Currently configured StatusLEDMode.
  StatusLEDMode get_mode() const { return mode_; }

  /// @brief Get the string representation of an operating mode.
  /// @param mode Mode enum value.
  /// @return Human-readable mode string name.
  static const char *mode_to_string(StatusLEDMode mode);

  /// @brief Set the source text sensor to subscribe to (e.g. Bus Status diagnostic entity).
  /// @param sensor Pointer to the TextSensor entity.
  void set_source_sensor(text_sensor::TextSensor *sensor);

  /// @brief Set the default output brightness (0.0 to 1.0).
  /// If the light is currently active, immediately applies the new brightness level.
  /// @param brightness Float brightness value (default 0.50).
  void set_brightness(float brightness);

  /// @brief Setup priority - LATE ensures light and hub components are ready.
  float get_setup_priority() const override { return setup_priority::LATE; }

  /// @brief Component setup: initial visual state update.
  void setup() override;

  /// @brief Log component configuration.
  void dump_config() override;

 protected:
  /// @brief Callback invoked when the observed text sensor publishes a state update.
  /// @param status The newly published status string.
  void on_status_changed_(const std::string &status);

  /// @brief Dispatch an RGB color update to the controlled light entity.
  /// Preserves the user's current light brightness level after initial startup.
  /// @param r Red channel (0.0 to 1.0).
  /// @param g Green channel (0.0 to 1.0).
  /// @param b Blue channel (0.0 to 1.0).
  /// @param state True to turn LED on, false to turn off.
  /// @param color_brightness Intensity of the color itself (0.0 to 1.0), independent of the
  ///        user's master brightness. Always sent, since the light retains its previous value.
  void set_color_(float r, float g, float b, bool state = true, float color_brightness = 1.0f);

  light::LightState *light_{nullptr};
  text_sensor::TextSensor *source_sensor_{nullptr};
  StatusLEDMode mode_{StatusLEDMode::BUS_STATUS};
  float brightness_{0.50f};
  bool brightness_initialized_{false};
  bool last_commanded_state_{false};
  std::string last_status_{};
};

}  // namespace gensam
}  // namespace esphome
