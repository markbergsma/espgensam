/// @file gensam_media_player.cpp
/// @brief Implementation of master Genelec SAM media_player component.
/// See gensam_media_player.h for architectural design rationale and complete API documentation.

#include "gensam_media_player.h"
#include "esphome/core/log.h"
#include "util.h"

static const char *const TAG = "gensam.media_player";

namespace esphome {
namespace gensam {

void GenSAMMediaPlayer::setup() {
  if (hub_ == nullptr) {
    ESP_LOGE(TAG, "Parent GenSAMHub is not set");
    this->mark_failed();
    return;
  }

  // Register callback to update media player state when volume or mute changes externally
  hub_->set_volume_state_callback([this](float db, bool mute, bool standby) {
    float slider = volume_db_to_slider(db, hub_->get_min_volume_db(), hub_->get_max_volume_db());
    this->volume = slider;
    this->muted_ = mute;
    this->state = standby ? media_player::MEDIA_PLAYER_STATE_OFF : media_player::MEDIA_PLAYER_STATE_ON;
    this->publish_state();
  });

  // Set initial state from hub configuration
  float initial_db = hub_->get_current_volume_db();
  this->volume = volume_db_to_slider(initial_db, hub_->get_min_volume_db(), hub_->get_max_volume_db());
  this->muted_ = hub_->is_muted();
  this->state = hub_->is_standby() ? media_player::MEDIA_PLAYER_STATE_OFF : media_player::MEDIA_PLAYER_STATE_ON;
  this->publish_state();

  ESP_LOGI(TAG, "Genelec SAM Media Player initialized (Startup: %.1f dB, Slider: %.2f)",
           initial_db, this->volume);
}

void GenSAMMediaPlayer::dump_config() {
  ESP_LOGCONFIG(TAG, "Genelec SAM Media Player:");
  if (hub_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Volume Bounds: [%.1f dB, %.1f dB]",
                  hub_->get_min_volume_db(), hub_->get_max_volume_db());
  }
}

media_player::MediaPlayerTraits GenSAMMediaPlayer::get_traits() {
  auto traits = media_player::MediaPlayerTraits();
  traits.add_feature_flags(media_player::MediaPlayerEntityFeature::VOLUME_SET |
                           media_player::MediaPlayerEntityFeature::VOLUME_MUTE |
                           media_player::MediaPlayerEntityFeature::TURN_ON |
                           media_player::MediaPlayerEntityFeature::TURN_OFF);
  return traits;
}

void GenSAMMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  if (hub_ == nullptr) {
    return;
  }

  // 1. Volume setting
  if (call.get_volume().has_value()) {
    float slider = *call.get_volume();
    float db = volume_slider_to_db(slider, hub_->get_min_volume_db(), hub_->get_max_volume_db());
    ESP_LOGI(TAG, "Home Assistant set volume slider: %.2f (%.1f dB)", slider, db);
    hub_->set_volume_db(db);
  }

  // 2. Power and mute commands
  if (call.get_command().has_value()) {
    auto cmd = *call.get_command();
    ESP_LOGI(TAG, "Home Assistant media player command: %s (%d)",
             media_player::media_player_command_to_string(cmd), static_cast<int>(cmd));
    switch (cmd) {
      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        hub_->set_group_mute(true);
        break;

      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        hub_->set_group_mute(false);
        break;

      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP: {
        float step = 0.05f;
        float new_vol = std::clamp(this->volume + step, 0.0f, 1.0f);
        float db = volume_slider_to_db(new_vol, hub_->get_min_volume_db(), hub_->get_max_volume_db());
        ESP_LOGI(TAG, "Volume UP: %.2f -> %.2f (%.1f dB)", this->volume, new_vol, db);
        hub_->set_volume_db(db);
        break;
      }

      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN: {
        float step = 0.05f;
        float new_vol = std::clamp(this->volume - step, 0.0f, 1.0f);
        float db = volume_slider_to_db(new_vol, hub_->get_min_volume_db(), hub_->get_max_volume_db());
        ESP_LOGI(TAG, "Volume DOWN: %.2f -> %.2f (%.1f dB)", this->volume, new_vol, db);
        hub_->set_volume_db(db);
        break;
      }

      case media_player::MEDIA_PLAYER_COMMAND_TURN_ON:
        hub_->set_standby(false);
        break;

      case media_player::MEDIA_PLAYER_COMMAND_TURN_OFF:
        hub_->set_standby(true);
        break;

      case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
        if (this->state == media_player::MEDIA_PLAYER_STATE_OFF) {
          hub_->set_standby(false);
        } else {
          hub_->set_standby(true);
        }
        break;

      default:
        break;
    }
  }

}

}  // namespace gensam
}  // namespace esphome
