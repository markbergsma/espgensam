#pragma once

/// @file gensam_media_player.h
/// @brief Master media_player component for Genelec SAM speaker system in Home Assistant.
///
/// ===================================================================================
/// ARCHITECTURAL DESIGN RATIONALE
/// ===================================================================================
/// 1. Whole-System Representation:
///    In a multi-speaker Genelec SAM setup (e.g. stereo pair + subwoofer), daily listening
///    and volume control operates across the entire group synchronously. The master
///    GenSAMMediaPlayer entity exposes the system to Home Assistant as a standard media
///    player with:
///      - Volume slider (0.0 to 1.0, mapped logarithmically to native dBFS via CMD_VOLUME 0xFF broadcast)
///      - Group mute toggle (transmitting CMD_BYPASS unicast to each monitor)
///      - System power / standby (transmitting CMD_WAKEUP broadcast 0xFF)
///
/// 2. Bidirectional & Passive Synchronization:
///    The media player registers a callback with GenSAMHub. When an external Genelec GLM
///    hardware adapter or USB controller transmits volume (0x1F) or bypass (0x2B) frames,
///    the hub decodes them and updates the media player state in real time without
///    re-transmitting, keeping the Home Assistant interface perfectly aligned with
///    physical GLM knobs or software.
///
/// 3. Safety Lockout:
///    When an external GLM adapter is actively broadcasting on the bus, outgoing control
///    commands from Home Assistant are suppressed by the hub to prevent bus collisions.
/// ===================================================================================

#include "esphome/core/component.h"
#include "esphome/components/media_player/media_player.h"
#include "gensam_hub.h"

namespace esphome {
namespace gensam {

/// @brief Master media player component representing the collective Genelec SAM system.
class GenSAMMediaPlayer : public media_player::MediaPlayer, public Component {
 public:
  /// @brief Set the parent GenSAMHub instance.
  /// @param hub Pointer to the active hub component.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

  /// @brief Initialize callbacks and set initial state.
  void setup() override;

  /// @brief Log media player configuration details.
  void dump_config() override;

  /// @brief Declare supported media player capabilities to Home Assistant.
  /// @return Traits struct with volume, mute, and power enabled.
  media_player::MediaPlayerTraits get_traits() override;

  /// @brief Handle incoming commands from Home Assistant (volume, mute, power).
  /// @param call The command payload received from Home Assistant.
  void control(const media_player::MediaPlayerCall &call) override;

  /// @brief Report whether media player is currently muted.
  /// @return True if muted, false otherwise.
  bool is_muted() const override { return this->muted_; }

 protected:
  GenSAMHub *hub_{nullptr};
  bool muted_{false};
};

}  // namespace gensam
}  // namespace esphome
