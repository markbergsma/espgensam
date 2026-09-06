/// @file status_led.cpp
/// @brief Implementation of GenSAMStatusLED component.
/// See status_led.h for architectural design rationale and complete API documentation.

#include "status_led.h"
#include "esphome/core/log.h"

static const char *const TAG = "gensam.status_led";

namespace esphome {
namespace gensam {

const char *GenSAMStatusLED::mode_to_string(StatusLEDMode mode) {
  switch (mode) {
    case StatusLEDMode::BUS_STATUS:
      return "bus_status";
    default:
      return "unknown";
  }
}

void GenSAMStatusLED::set_source_sensor(text_sensor::TextSensor *sensor) {
  source_sensor_ = sensor;
  if (source_sensor_ != nullptr) {
    source_sensor_->add_on_state_callback([this](const std::string &status) {
      this->on_status_changed_(status);
    });
  }
}

void GenSAMStatusLED::set_brightness(float brightness) {
  brightness_ = brightness;
  if (brightness_initialized_ && light_ != nullptr && light_->remote_values.is_on()) {
    auto call = light_->make_call();
    call.set_brightness(brightness_);
    call.set_save(false);
    call.set_transition_length_if_supported(150);
    call.perform();
  }
}

void GenSAMStatusLED::setup() {
  if (light_ == nullptr) {
    ESP_LOGW(TAG, "No light entity configured for GenSAMStatusLED");
    return;
  }

  if (mode_ == StatusLEDMode::BUS_STATUS) {
    // Apply initial state if source sensor already has a value
    if (source_sensor_ != nullptr && source_sensor_->has_state() && !source_sensor_->state.empty()) {
      this->on_status_changed_(source_sensor_->state);
    } else {
      // Initial default: standby / off until first status arrives
      this->set_color_(0.0f, 0.0f, 0.0f, false);
    }
  }
}

void GenSAMStatusLED::dump_config() {
  ESP_LOGCONFIG(TAG, "GenSAM Status LED:");
  ESP_LOGCONFIG(TAG, "  Mode: %s", mode_to_string(mode_));
  ESP_LOGCONFIG(TAG, "  Default Brightness: %.0f%%", brightness_ * 100.0f);
}

void GenSAMStatusLED::on_status_changed_(const std::string &status) {
  if (mode_ != StatusLEDMode::BUS_STATUS) {
    return;
  }

  if (status == last_status_) {
    return;
  }
  last_status_ = status;

  ESP_LOGD(TAG, "Bus status changed to '%s'; updating status LED", status.c_str());

  if (status == "Active") {
    // Solid Green: Normal healthy listening loop
    this->set_color_(0.0f, 1.0f, 0.0f, true);
  } else if (status == "GLM Active") {
    // Amber / Orange: External GLM master has bus control
    this->set_color_(1.0f, 0.5f, 0.0f, true);
  } else if (status == "Discovering") {
    // Solid Blue: Active RACE discovery / device interrogation
    this->set_color_(0.0f, 0.4f, 1.0f, true);
  } else if (status == "Configuring") {
    // Cyan: Device parameter configuration phase
    this->set_color_(0.0f, 0.8f, 1.0f, true);
  } else if (status == "Standby") {
    // Off: System in low-power standby
    this->set_color_(0.0f, 0.0f, 0.0f, false);
  } else if (status == "Offline" || status == "Idle") {
    // Dim White: Standalone idle, no monitors discovered.
    // Dimming must come from color_brightness, not a scaled RGB triple: LightCall::validate_
    // normalizes the triple by its largest channel, turning any grey into full white.
    this->set_color_(1.0f, 1.0f, 1.0f, true, 0.15f);
  } else {
    // Unknown: Off
    this->set_color_(0.0f, 0.0f, 0.0f, false);
  }
}

void GenSAMStatusLED::set_color_(float r, float g, float b, bool state, float color_brightness) {
  if (light_ == nullptr) {
    return;
  }

  // The light being off while we last drove it on means someone switched it off from Home
  // Assistant. Respect that: keep the color up to date so it shows the current status the
  // moment it is switched back on, but never switch it on ourselves.
  bool user_turned_off = last_commanded_state_ && !light_->remote_values.is_on();

  auto call = light_->make_call();
  if (state) {
    call.set_rgb(r, g, b);
    call.set_color_brightness(color_brightness);
    if (!brightness_initialized_) {
      call.set_brightness(brightness_);
      brightness_initialized_ = true;
    }
  }
  if (!user_turned_off) {
    call.set_state(state);
    last_commanded_state_ = state;
  }
  call.set_save(false);
  call.set_transition_length_if_supported(150);
  call.perform();
}

}  // namespace gensam
}  // namespace esphome
