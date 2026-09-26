/// @file registry.cpp
/// @brief Discovered monitor registry and Home Assistant entity publishing.
/// See registry.h for architectural design rationale and complete API documentation.

#include "registry.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <strings.h>

static const char *const TAG = "gensam";

namespace esphome {
namespace gensam {

namespace {

/// Case-insensitive match of a user-supplied identifier against a binding's serial, ID, or name.
bool binding_matches_identifier(const GenSAMMonitorBinding &b, const std::string &serial_or_id) {
  return strcasecmp(b.serial_number.c_str(), serial_or_id.c_str()) == 0 ||
         std::to_string(b.unique_id) == serial_or_id ||
         strcasecmp(b.name.c_str(), serial_or_id.c_str()) == 0;
}

/// Match an identifier against a monitor's own serial or hardware ID, falling back to its binding.
bool monitor_matches_identifier(const GenSAMMonitor &mon, const std::string &serial_or_id) {
  if (strcasecmp(mon.serial_number.c_str(), serial_or_id.c_str()) == 0 ||
      std::to_string(mon.unique_id) == serial_or_id) {
    return true;
  }
  return mon.binding != nullptr && binding_matches_identifier(*mon.binding, serial_or_id);
}

/// Publish a binding's crossover to its select, if the value is one of the select's options.
/// A value that is not stays stored, and is still re-sent to the speaker, but publishing it
/// would give the select a state outside its option list.
void publish_crossover(const GenSAMMonitorBinding &b) {
  if (b.crossover_select == nullptr) {
    return;
  }
  char buf[CROSSOVER_STR_SIZE];
  crossover_to_str(b.crossover_freq, buf, sizeof(buf));
  if (!is_valid_crossover(b.crossover_freq)) {
    ESP_LOGW(TAG, "Crossover %s for '%s' is not a selectable option; not shown", buf, b.name.c_str());
    return;
  }
  b.crossover_select->publish_state(buf);
}

/// Log-friendly model name for a monitor whose device query may not have completed yet.
const char *model_or(const GenSAMMonitor &mon, const char *fallback) {
  return mon.model.empty() ? fallback : mon.model.c_str();
}

}  // namespace

GenSAMMonitor &MonitorRegistry::get_or_create(uint8_t address) {
  GenSAMMonitor &mon = monitors_[address];
  mon.address = address;
  return mon;
}

GenSAMMonitor *MonitorRegistry::find(uint8_t address) {
  auto it = monitors_.find(address);
  return (it != monitors_.end()) ? &it->second : nullptr;
}

const GenSAMMonitor *MonitorRegistry::find(uint8_t address) const {
  auto it = monitors_.find(address);
  return (it != monitors_.end()) ? &it->second : nullptr;
}

GenSAMMonitor *MonitorRegistry::find_by_serial_or_id(const std::string &serial_or_id) {
  for (auto &kv : monitors_) {
    if (monitor_matches_identifier(kv.second, serial_or_id)) {
      return &kv.second;
    }
  }
  return nullptr;
}

GenSAMMonitorBinding *MonitorRegistry::find_binding_by_serial_or_id(const std::string &serial_or_id) {
  for (auto &b : bindings_) {
    if (binding_matches_identifier(b, serial_or_id)) {
      return &b;
    }
  }
  return nullptr;
}

void MonitorRegistry::bind_if_matched(GenSAMMonitor &mon) {
  if (mon.binding != nullptr) {
    return;
  }
  for (auto &b : bindings_) {
    if (!mon.matches(b)) {
      continue;
    }
    mon.binding = &b;
    if ((mon.serial_number.empty() || mon.serial_number == "(none)") && !b.serial_number.empty()) {
      mon.serial_number = b.serial_number;
    }
    ESP_LOGI(TAG, "Bound monitor 0x%02X (%s, SN:%s) to HA entity '%s'",
             mon.address, model_or(mon, "(querying)"), mon.serial_number.c_str(), b.name.c_str());
    this->publish_metadata(mon);
    this->publish_online(mon);
    if (b.crossover_configured) {
      publish_crossover(b);
    }
    if (b.input_select != nullptr && b.input_configured) {
      b.input_select->publish_state(input_to_str(b.source, b.aes3_channel));
    }
    if (b.level_number != nullptr && b.level_configured) {
      b.level_number->publish_state(b.level_db);
    }
    if (b.delay_number != nullptr && b.delay_configured) {
      b.delay_number->publish_state(delay_samples_to_ms(b.delay_samples));
    }
    break;
  }
}

bool MonitorRegistry::mark_seen(GenSAMMonitor &mon) {
  mon.last_seen_ms = millis();
  if (mon.online) {
    return false;
  }
  mon.online = true;
  this->publish_online(mon);
  ESP_LOGI(TAG, "[0x%02X %s] Monitor is online", mon.address, model_or(mon, "(querying)"));
  return true;
}

bool MonitorRegistry::expire_stale(uint32_t now, uint32_t stale_timeout_ms) {
  bool state_changed = false;
  for (auto &kv : monitors_) {
    GenSAMMonitor &mon = kv.second;
    if (!mon.online || (now - mon.last_seen_ms <= stale_timeout_ms)) {
      continue;
    }
    mon.online = false;
    this->publish_online(mon);
    ESP_LOGW(TAG, "[0x%02X %s] Monitor went offline (no response for %u ms)",
             mon.address, model_or(mon, "(unknown)"), (unsigned)(now - mon.last_seen_ms));
    state_changed = true;
  }
  return state_changed;
}

void MonitorRegistry::invalidate_all() {
  for (auto &kv : monitors_) {
    kv.second.online = false;
    kv.second.last_seen_ms = 0;
    this->publish_online(kv.second);
  }
}

void MonitorRegistry::mark_all_offline() {
  for (auto &kv : monitors_) {
    if (kv.second.online) {
      kv.second.online = false;
      this->publish_online(kv.second);
    }
  }
}

void MonitorRegistry::publish_metadata(const GenSAMMonitor &mon) {
  if (mon.binding == nullptr) {
    return;
  }
  if (mon.binding->model_sensor != nullptr && !mon.model.empty()) {
    mon.binding->model_sensor->publish_state(mon.model);
  }
  if (mon.binding->serial_sensor != nullptr && !mon.serial_number.empty() && mon.serial_number != "(none)") {
    mon.binding->serial_sensor->publish_state(mon.serial_number);
  }
  if (mon.binding->firmware_sensor != nullptr && !mon.firmware_version.empty() && mon.firmware_version != "?") {
    mon.binding->firmware_sensor->publish_state(mon.firmware_version);
  }
  if (mon.binding->hardware_id_sensor != nullptr && mon.unique_id != 0) {
    mon.binding->hardware_id_sensor->publish_state(std::to_string(mon.unique_id));
  }
}

void MonitorRegistry::publish_telemetry(const GenSAMMonitor &mon) {
  if (mon.binding == nullptr) {
    return;
  }
  if (mon.binding->temperature_sensor != nullptr) {
    mon.binding->temperature_sensor->publish_state(mon.temperature);
  }
  if (mon.binding->input_level_sensor != nullptr) {
    mon.binding->input_level_sensor->publish_state(mon.input_db);
  }
  if (mon.binding->output_level_sensor != nullptr) {
    mon.binding->output_level_sensor->publish_state(mon.output_db);
  }
  this->publish_online(mon);
}

void MonitorRegistry::publish_online(const GenSAMMonitor &mon) {
  if (mon.binding != nullptr && mon.binding->online_sensor != nullptr) {
    mon.binding->online_sensor->publish_state(mon.online);
  }
}

void MonitorRegistry::publish_mute(const GenSAMMonitor &mon) {
  if (mon.binding != nullptr && mon.binding->mute_switch != nullptr) {
    mon.binding->mute_switch->publish_state(mon.mute);
  }
}

void MonitorRegistry::set_mute(GenSAMMonitor &mon, bool mute) {
  mon.mute = mute;
  this->publish_mute(mon);
}

void MonitorRegistry::set_binding_crossover(GenSAMMonitorBinding &b, uint16_t freq_hz) {
  b.crossover_freq = freq_hz;
  b.crossover_configured = true;
  publish_crossover(b);
}

void MonitorRegistry::set_binding_input(GenSAMMonitorBinding &b, uint8_t source, uint8_t channel) {
  b.source = source;
  b.aes3_channel = channel;
  b.input_configured = true;
  if (b.input_select != nullptr) {
    b.input_select->publish_state(input_to_str(source, channel));
  }
}

void MonitorRegistry::set_binding_level(GenSAMMonitorBinding &b, float db) {
  b.level_db = db;
  b.level_configured = true;
  if (b.level_number != nullptr) {
    b.level_number->publish_state(db);
  }
}

void MonitorRegistry::set_binding_delay(GenSAMMonitorBinding &b, uint32_t samples) {
  b.delay_samples = samples;
  b.delay_configured = true;
  if (b.delay_number != nullptr) {
    b.delay_number->publish_state(delay_samples_to_ms(samples));
  }
}

void MonitorRegistry::set_crossover(GenSAMMonitor &mon, uint16_t freq_hz) {
  if (mon.binding != nullptr) {
    this->set_binding_crossover(*mon.binding, freq_hz);
  }
}

void MonitorRegistry::set_input(GenSAMMonitor &mon, uint8_t source, uint8_t channel) {
  if (mon.binding != nullptr) {
    this->set_binding_input(*mon.binding, source, channel);
  }
}

void MonitorRegistry::set_level(GenSAMMonitor &mon, float db) {
  if (mon.binding != nullptr) {
    this->set_binding_level(*mon.binding, db);
  }
}

void MonitorRegistry::set_delay(GenSAMMonitor &mon, uint32_t samples) {
  if (mon.binding != nullptr) {
    this->set_binding_delay(*mon.binding, samples);
  }
}

bool MonitorRegistry::all_online_muted(bool &any_online) const {
  any_online = false;
  bool all_muted = true;
  for (const auto &kv : monitors_) {
    if (kv.second.online) {
      any_online = true;
      if (!kv.second.mute) {
        all_muted = false;
        break;
      }
    }
  }
  return all_muted;
}

bool MonitorRegistry::any_online() const {
  for (const auto &kv : monitors_) {
    if (kv.second.online) {
      return true;
    }
  }
  return false;
}

bool MonitorRegistry::all_online_in_standby(bool &any_reported) const {
  any_reported = false;
  bool all_standby = true;
  for (const auto &kv : monitors_) {
    // Monitors that never reported a power state say nothing about it; a Format A telemetry
    // reply carries no power field at all, and not every TLV reply includes the 'G' tag.
    if (!kv.second.online || !kv.second.standby_known) {
      continue;
    }
    any_reported = true;
    if (!kv.second.standby) {
      all_standby = false;
      break;
    }
  }
  return all_standby;
}

std::vector<uint8_t> MonitorRegistry::addresses() const {
  std::vector<uint8_t> addrs;
  addrs.reserve(monitors_.size());
  for (const auto &kv : monitors_) {
    addrs.push_back(kv.first);
  }
  return addrs;
}

}  // namespace gensam
}  // namespace esphome
