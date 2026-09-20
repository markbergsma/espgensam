/// @file snoop.cpp
/// @brief Passive snooping of Genelec GLM bus traffic.
/// See hub.h for the snooping rationale and complete API documentation.

#include "hub.h"

#include "esphome/core/log.h"
#include "esphome/components/select/select.h"

static const char *const TAG = "gensam";

namespace esphome {
namespace gensam {

void GenSAMHub::snoop_frame_(const Frame &frame) {
  this->snoop_addressing_(frame);
  this->snoop_volume_(frame);
  this->snoop_power_(frame);
  this->snoop_mute_(frame);
  this->snoop_crossover_(frame);
  this->snoop_audio_source_(frame);
  this->snoop_host_reply_(frame);
}

void GenSAMHub::snoop_addressing_(const Frame &frame) {
  if (frame.address >= MONITOR_START_ADDR && frame.address < 0x80) {
    // Remember the outstanding query so snoop_host_reply_ can attribute the answer.
    last_queried_addr_ = frame.address;
    last_queried_cmd_ = frame.command;
    return;
  }

  if (frame.address == MULTICAST_ADDRESS && frame.command == CMD_SET_RID && frame.payload.size() == 4) {
    uint8_t addr = frame.payload[3];
    GenSAMMonitor &mon = registry_.get_or_create(addr);
    mon.unique_id = (static_cast<uint32_t>(frame.payload[0]) << 16) |
                    (static_cast<uint32_t>(frame.payload[1]) << 8) |
                    static_cast<uint32_t>(frame.payload[2]);
    this->mark_monitor_seen_(mon);
    ESP_LOGI(TAG, "[Sniffed] Assigned monitor 0x%02X (ID: %u)", addr, (unsigned)mon.unique_id);
    registry_.bind_if_matched(mon);
  }
}

void GenSAMHub::snoop_volume_(const Frame &frame) {
  // GLM broadcasts master volume to 0xFF; 0xF0 carries auxiliary pot data and is ignored.
  if (frame.address != BROADCAST_ADDRESS || frame.command != CMD_VOLUME || frame.payload.size() < 3) {
    return;
  }
  current_volume_db_ = volume_int24_to_db(decode_int24(frame.payload.data()));
  ESP_LOGI(TAG, "[Sniffed] System volume updated to %.1f dB", current_volume_db_);
  this->notify_state_callbacks_();
}

void GenSAMHub::snoop_power_(const Frame &frame) {
  if ((frame.address != MULTICAST_ADDRESS && frame.address != BROADCAST_ADDRESS) ||
      frame.command != CMD_WAKEUP || frame.payload.size() < 2 || frame.payload[0] != WAKEUP_OP_POWER) {
    return;
  }

  bool is_standby = (frame.payload[1] == WAKEUP_VAL_STANDBY_1 || frame.payload[1] == WAKEUP_VAL_STANDBY_2);
  bool is_on = (frame.payload[1] == WAKEUP_VAL_ON_1 || frame.payload[1] == WAKEUP_VAL_ON_2);
  if (is_standby && !current_standby_) {
    current_standby_ = true;
    last_standby_command_ = millis();
    ESP_LOGI(TAG, "[Sniffed] System power state updated to STANDBY (val 0x%02X)", frame.payload[1]);
    this->notify_state_callbacks_();
    this->update_bus_status_();
  } else if (is_on && current_standby_) {
    current_standby_ = false;
    last_standby_command_ = millis();
    ESP_LOGI(TAG, "[Sniffed] System power state updated to ON (val 0x%02X)", frame.payload[1]);
    this->notify_state_callbacks_();
    this->update_bus_status_();
  }
}

void GenSAMHub::snoop_mute_(const Frame &frame) {
  if (frame.command != CMD_BYPASS || frame.payload.empty()) {
    return;
  }
  bool is_muted = (frame.payload[0] & BYPASS_MUTE_MASK) != 0;

  GenSAMMonitor *known = registry_.find(frame.address);
  if (known != nullptr) {
    registry_.set_mute(*known, is_muted);
    ESP_LOGD(TAG, "[Sniffed] Monitor 0x%02X mute updated to %s (raw 0x%02X)",
             frame.address, YESNO(is_muted), frame.payload[0]);
  } else if (frame.address >= MONITOR_START_ADDR && frame.address < 0x80) {
    GenSAMMonitor &mon = registry_.get_or_create(frame.address);
    // Record mute before marking seen, so the system mute re-evaluation triggered by the
    // offline->online transition already accounts for this monitor's new state.
    mon.mute = is_muted;
    this->mark_monitor_seen_(mon);
    registry_.bind_if_matched(mon);
    registry_.publish_mute(mon);
    ESP_LOGD(TAG, "[Sniffed] Discovered monitor 0x%02X mute set to %s (raw 0x%02X)",
             frame.address, YESNO(is_muted), frame.payload[0]);
  } else if (frame.address == MULTICAST_ADDRESS || frame.address == BROADCAST_ADDRESS) {
    for (auto &kv : registry_.monitors()) {
      registry_.set_mute(kv.second, is_muted);
    }
  }

  this->evaluate_system_mute_();
}

void GenSAMHub::snoop_crossover_(const Frame &frame) {
  if (frame.command != CMD_BASS_MANAGE_XO || frame.payload.size() < 2) {
    return;
  }
  uint16_t freq = (static_cast<uint16_t>(frame.payload[0]) << 8) | frame.payload[1];

  // An external controller has moved the crossover off what the active group specified; the
  // same deviation a hand override creates. See set_group_modified().
  this->set_group_modified(true);

  if (frame.address == MULTICAST_ADDRESS || frame.address == BROADCAST_ADDRESS) {
    for (auto &kv : registry_.monitors()) {
      registry_.set_crossover(kv.second, freq);
    }
    ESP_LOGI(TAG, "[Sniffed] Global bass management crossover frequency set to %u Hz", freq);
    return;
  }

  GenSAMMonitor *known = registry_.find(frame.address);
  if (known != nullptr) {
    registry_.set_crossover(*known, freq);
    ESP_LOGI(TAG, "[Sniffed] Monitor 0x%02X bass management crossover frequency set to %u Hz",
             frame.address, freq);
  } else if (frame.address >= MONITOR_START_ADDR && frame.address < 0x80) {
    GenSAMMonitor &mon = registry_.get_or_create(frame.address);
    this->mark_monitor_seen_(mon);
    registry_.bind_if_matched(mon);
    registry_.set_crossover(mon, freq);
    ESP_LOGI(TAG, "[Sniffed] Discovered monitor 0x%02X bass management crossover frequency set to %u Hz",
             frame.address, freq);
  }
}

void GenSAMHub::snoop_audio_source_(const Frame &frame) {
  if (frame.command != CMD_SELECT_AUDIO_SOURCE || frame.payload.size() < 4) {
    return;
  }
  const uint8_t input_idx = frame.payload[0];
  const uint8_t src = frame.payload[1];
  const uint8_t ch = frame.payload[3];

  // Only the primary input carries the routing this component models. A subwoofer's secondary
  // frame repeats the same source with a zero channel and would otherwise overwrite it.
  if (input_idx != 0x00) {
    return;
  }

  if (src != SOURCE_ANALOG && src != SOURCE_DIGITAL_AES3) {
    // An external controller selected something this component cannot name -- 0x03
    // (Automatic, a standalone setting we do not implement) or an undocumented value. Stop
    // asserting this monitor's input: configure_monitor_() re-transmits it after a standby
    // cycle, so leaving the flag set would later revert the bus to the last routing we did
    // understand, silently undoing a change we watched go past.
    //
    // The entity keeps displaying its last value; an ESPHome select cannot be returned to
    // Unknown once published. Only transmission stops.
    GenSAMMonitor *known = registry_.find(frame.address);
    if (known != nullptr && known->binding != nullptr && known->binding->input_configured) {
      known->binding->input_configured = false;
      ESP_LOGW(TAG, "[Sniffed] Monitor 0x%02X input set to unrecognised source 0x%02X; no longer "
                    "asserting its routing (entity still shows its last known value)",
               frame.address, src);
    }
    return;
  }

  if (src == SOURCE_DIGITAL_AES3 && (ch < AES3_CHANNEL_A || ch > AES3_CHANNEL_SUM)) {
    ESP_LOGW(TAG, "[Sniffed] Monitor 0x%02X AES3 sub-channel 0x%02X is not one this component "
                  "recognises; routing left as it was", frame.address, ch);
    return;
  }

  GenSAMMonitor *mon = registry_.find(frame.address);
  if (mon == nullptr) {
    if (frame.address < MONITOR_START_ADDR || frame.address >= 0x80) {
      return;
    }
    mon = &registry_.get_or_create(frame.address);
    this->mark_monitor_seen_(*mon);
    registry_.bind_if_matched(*mon);
  }

  const bool changed = mon->binding == nullptr || !mon->binding->input_configured ||
                       mon->binding->source != src ||
                       (src == SOURCE_DIGITAL_AES3 && mon->binding->aes3_channel != ch);
  registry_.set_input(*mon, src, ch);
  if (changed) {
    // Someone else moved a speaker off what the active group specified, the same deviation a
    // hand override creates; see set_group_modified().
    this->set_group_modified(true);
    ESP_LOGI(TAG, "[Sniffed] Monitor 0x%02X input set to %s", frame.address,
             input_to_str(src, ch));
  }
}


void GenSAMHub::snoop_host_reply_(const Frame &frame) {
  if (frame.address != HOST_ADDRESS || frame.command != CMD_REPORT_STATUS || last_queried_addr_ == 0) {
    return;
  }

  // Attribute the reply to the query snoop_addressing_ last saw go out on the bus.
  uint8_t queried_cmd = last_queried_cmd_;
  if (queried_cmd != CMD_SOFTWARE_QUERY && queried_cmd != CMD_BAR_CODE && queried_cmd != CMD_QUERY_STATUS) {
    return;
  }

  GenSAMMonitor &mon = registry_.get_or_create(last_queried_addr_);
  switch (queried_cmd) {
    case CMD_SOFTWARE_QUERY:
      parse_device_info(frame.payload.data(), frame.payload.size(), mon);
      break;
    case CMD_BAR_CODE:
      parse_barcode(frame.payload.data(), frame.payload.size(), mon);
      break;
    default:
      parse_telemetry(frame.payload.data(), frame.payload.size(), mon);
      break;
  }
  this->mark_monitor_seen_(mon);
  registry_.bind_if_matched(mon);

  switch (queried_cmd) {
    case CMD_SOFTWARE_QUERY:
      ESP_LOGI(TAG, "[Sniffed] Discovered: %s", mon.to_string().c_str());
      break;
    case CMD_BAR_CODE:
      ESP_LOGI(TAG, "[Sniffed] Serial for 0x%02X: %s", last_queried_addr_, mon.serial_number.c_str());
      break;
    default:
      registry_.publish_telemetry(mon);
      this->evaluate_system_standby_();
      break;
  }
  last_queried_cmd_ = 0;
}

}  // namespace gensam
}  // namespace esphome
