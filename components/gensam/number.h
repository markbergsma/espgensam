#pragma once

/// @file number.h
/// @brief ESPHome Number entities for per-monitor level trim and delay, and system volume in dB.
///
/// ===================================================================================
/// ARCHITECTURAL DESIGN RATIONALE
/// ===================================================================================
/// 1. Per-Device Level Trim (GenSAMLevelNumber):
///    The gain offset AutoCal derives to match one speaker's output to the rest of a group,
///    carried as CMD_DSP (0x10) sub-command 0x01 with selector 0x00 and encoded exactly as
///    CMD_VOLUME is.
///    - Attenuation only. The encoder saturates at unity, so there is no positive half to
///      offer, and the entity's range stops at 0 dB.
///    - The floor matters more than the ceiling: anything at or below -130 dB encodes as
///      digital silence, and a GLM setup file writes -999 for "not calibrated". The range
///      stops at MIN_LEVEL_DB (-60 dB), well clear of both, and the hub clamps again for
///      callers that do not come through this entity.
///    - 0.1 dB steps. Captured AutoCal trims are values like -1.9258 dB, so the 0.5 dB of a
///      mixing fader would be too coarse to represent what a group already carries.
///
/// 2. Time-of-Flight Delay (GenSAMDelayNumber):
///    CMD_DSP (0x10) sub-command 0x02, a sample count at 48 kHz on every device class.
///    - Samples are authoritative and milliseconds are presentation. The binding stores the
///      sample count that went on the wire; this entity converts in both directions and
///      publishes the exact millisecond equivalent of what was transmitted, never the value
///      the user typed. That makes each write a projection, so repeated nudging cannot walk
///      the value away from the sample grid.
///    - A group's own delays are arbitrary - 289 samples is 6.0208333 ms - so a published
///      state frequently does not sit on the 0.1 ms display step. That is the honest value.
///    - The range stops at 192 ms, the limit the GLM v5 user interface enforces.
///
/// 3. Why Nothing is Pushed at Boot:
///    Neither of the two entities above, nor the crossover select, is registered with a
///    restored or initial value; there is simply no startup write path, which is what makes
///    boot non-destructive (see select.h section 5). A speaker keeps the trim, delay and
///    crossover its own DSP holds until a group is applied or somebody chooses a value.
///    Like the crossover, both are re-transmitted during CONFIGURING_DEVICES after a
///    rediscovery, and followed back when GLM changes them on the bus.
///
/// 4. System Volume in Decibels (GenSAMVolumeNumber):
///    While the GenSAM MediaPlayer entity provides standard 0.0..1.0 (0-100%) linear slider
///    control in Home Assistant, professional monitoring environments and studio workflows
///    often require explicit decibel (dB) attenuation readouts and control.
///    - The GenSAMVolumeNumber entity exposes the active system volume directly in decibels (dB),
///      bounded by the configured [min_volume_db, max_volume_db] limits.
///    - A step resolution of 0.5 dB is enforced to match standard audio mixing console fader
///      increments and provide precise acoustic adjustment without flooding the half-duplex
///      RS-485 bus during slider manipulation.
///    - The entity maintains strict bidirectional synchronization with the GenSAM MediaPlayer,
///      outgoing CMD_VOLUME (0x1F) broadcast frames, and passive bus sniffing of external GLM
///      controllers.
/// ===================================================================================

#include "esphome/core/component.h"
#include "esphome/components/number/number.h"
#include "hub.h"
#include "const.h"

#include <cmath>
#include <string>
#include <algorithm>

namespace esphome {
namespace gensam {

/// @brief Number entity that configures the per-device level trim (dB) for a SAM speaker.
class GenSAMLevelNumber : public number::Number {
 public:
  /// @brief Set the parent GenSAMHub instance.
  /// @param hub Pointer to the GenSAMHub.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

  /// @brief Set the target speaker's factory serial number or unique ID string.
  /// @param id Serial number string or unique ID string.
  void set_serial_or_id(const std::string &id) { serial_or_id_ = id; }

 protected:
  /// @brief Action executed when user adjusts the level in Home Assistant.
  /// @param value Requested level trim in decibels.
  void control(float value) override {
    float rounded = clamp_level_db(std::round(value / LEVEL_STEP_DB) * LEVEL_STEP_DB);

    if (hub_ != nullptr) {
      hub_->set_monitor_level_by_serial(serial_or_id_, rounded);
    }
    // Published optimistically, as the crossover select does: the hub stores the value on the
    // binding even when the bus is busy or the speaker is absent, so the entity should show
    // what was chosen rather than reverting.
    this->publish_state(rounded);
  }

  GenSAMHub *hub_{nullptr};        ///< Pointer to root GenSAM bus controller hub.
  std::string serial_or_id_{};    ///< Target monitor serial number or decimal ID.
};

/// @brief Number entity that configures the time-of-flight delay (ms) for a SAM speaker.
class GenSAMDelayNumber : public number::Number {
 public:
  /// @brief Set the parent GenSAMHub instance.
  /// @param hub Pointer to the GenSAMHub.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

  /// @brief Set the target speaker's factory serial number or unique ID string.
  /// @param id Serial number string or unique ID string.
  void set_serial_or_id(const std::string &id) { serial_or_id_ = id; }

 protected:
  /// @brief Action executed when user adjusts the delay in Home Assistant.
  /// @param value Requested delay in milliseconds.
  void control(float value) override {
    const uint32_t samples = delay_ms_to_samples(value);

    if (hub_ != nullptr) {
      hub_->set_monitor_delay_by_serial(serial_or_id_, samples);
    }
    // The exact equivalent of the sample count that was sent, not the requested value: the
    // sample grid is finer than the display step, so the two rarely coincide and only one of
    // them is what the speaker was actually told.
    this->publish_state(delay_samples_to_ms(samples));
  }

  GenSAMHub *hub_{nullptr};        ///< Pointer to root GenSAM bus controller hub.
  std::string serial_or_id_{};    ///< Target monitor serial number or decimal ID.
};

/// @brief Number entity that configures and displays the master system volume in decibels (dB).
class GenSAMVolumeNumber : public number::Number {
 public:
  /// @brief Set the parent GenSAMHub instance.
  /// @param hub Pointer to the GenSAMHub.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

 protected:
  /// @brief Action executed when user adjusts the volume dB number in Home Assistant.
  /// @param value Requested volume level in decibels.
  void control(float value) override {
    float step = 0.5f;
    float rounded = std::round(value / step) * step;
    if (hub_ != nullptr) {
      rounded = std::clamp(rounded, hub_->get_min_volume_db(), hub_->get_max_volume_db());
      hub_->set_volume_db(rounded);
      // If set_volume_db succeeded, notify_state_callbacks_() already published the state.
      // If transmission failed or was blocked, revert HA UI to actual current volume.
      this->publish_state(hub_->get_current_volume_db());
    } else {
      this->publish_state(rounded);
    }
  }

  GenSAMHub *hub_{nullptr};        ///< Pointer to root GenSAM bus controller hub.
};

}  // namespace gensam
}  // namespace esphome
