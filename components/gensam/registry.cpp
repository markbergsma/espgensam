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

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>
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
  GenSAMMonitorBinding *matched = nullptr;
  for (auto &b : bindings_) {
    if (!mon.matches(b)) {
      continue;
    }
    matched = &b;
    break;
  }

  if (matched == nullptr) {
    return;
  }

  mon.binding = matched;
  if ((mon.serial_number.empty() || mon.serial_number == "(none)") && !matched->serial_number.empty()) {
    mon.serial_number = matched->serial_number;
  }
  ESP_LOGI(TAG, "Bound monitor 0x%02X (%s, SN:%s) to HA entity '%s'",
           mon.address, model_or(mon, "(querying)"), mon.serial_number.c_str(), matched->name.c_str());
  this->publish_metadata(mon);
  this->publish_online(mon);
  if (matched->crossover_configured) {
    publish_crossover(*matched);
  }
  if (matched->input_select != nullptr && matched->input_configured) {
    matched->input_select->publish_state(input_to_str(matched->source, matched->aes3_channel));
  }
  if (matched->level_number != nullptr && matched->level_configured) {
    matched->level_number->publish_state(matched->level_db);
  }
  if (matched->delay_number != nullptr && matched->delay_configured) {
    matched->delay_number->publish_state(delay_samples_to_ms(matched->delay_samples));
  }
}

void MonitorRegistry::assign_discovery_slots() {
  const bool has_discovery_slots = std::any_of(
      bindings_.begin(), bindings_.end(), [](const GenSAMMonitorBinding &binding) {
        return binding.discovery_slot;
      });
  if (!has_discovery_slots) {
    return;
  }

  std::vector<GenSAMMonitor *> unassigned;
  for (auto &entry : monitors_) {
    GenSAMMonitor &mon = entry.second;
    if (mon.binding == nullptr && mon.unique_id != 0) {
      unassigned.push_back(&mon);
    }
  }
  std::sort(unassigned.begin(), unassigned.end(), [](const GenSAMMonitor *left, const GenSAMMonitor *right) {
    return left->unique_id < right->unique_id;
  });

  std::size_t overflow = 0;
  for (GenSAMMonitor *mon : unassigned) {
    for (auto &binding : bindings_) {
      if (binding.discovery_slot && binding.unique_id == mon->unique_id) {
        this->bind_if_matched(*mon);
        break;
      }
    }
    if (mon->binding != nullptr) {
      continue;
    }
    auto slot = std::find_if(bindings_.begin(), bindings_.end(), [](const GenSAMMonitorBinding &binding) {
      return binding.discovery_slot && binding.unique_id == 0;
    });
    if (slot == bindings_.end()) {
      overflow++;
      continue;
    }
    slot->unique_id = mon->unique_id;
    this->bind_if_matched(*mon);
  }

  if (overflow != 0) {
    ESP_LOGW(TAG, "%u discovered monitor(s) have no fixed entity slot; uploaded group settings still apply",
             static_cast<unsigned>(overflow));
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
  if (mon.binding->sam_name_sensor != nullptr) {
    const auto name = sam_monitor_names_.find(mon.unique_id);
    mon.binding->sam_name_sensor->publish_state(
        name != sam_monitor_names_.end() ? name->second : "");
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
  // The decoding depends on the model, which may arrive after a sniffed DIP reply.
  this->publish_dip_switch_status(mon);
}

void MonitorRegistry::set_sam_monitor_names(const std::map<uint32_t, std::string> &names) {
  sam_monitor_names_ = names;
  for (const auto &entry : monitors_) {
    this->publish_metadata(entry.second);
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
  // NAN until the speaker reports the record; a model that never does stays "unknown".
  const std::pair<sensor::Sensor *, float> extended[] = {
      {mon.binding->tw_coil_temperature_sensor, mon.tw_coil_temperature},
      {mon.binding->md_coil_temperature_sensor, mon.md_coil_temperature},
      {mon.binding->bs_coil_temperature_sensor, mon.bs_coil_temperature},
      {mon.binding->psu_main_voltage_sensor, mon.psu_main_voltage},
      {mon.binding->psu_low_voltage_sensor, mon.psu_low_voltage},
      {mon.binding->processor_load_sensor, mon.processor_load},
  };
  for (const auto &entry : extended) {
    if (entry.first != nullptr && !std::isnan(entry.second)) {
      entry.first->publish_state(entry.second);
    }
  }
  this->publish_online(mon);
}

void MonitorRegistry::publish_dip_switch_status(const GenSAMMonitor &mon) {
  if (mon.binding == nullptr || mon.binding->dip_switch_status_sensor == nullptr ||
      !mon.dip_switches_known) {
    return;
  }
  mon.binding->dip_switch_status_sensor->publish_state(describe_dip_switches(mon));
}

void MonitorRegistry::publish_online(const GenSAMMonitor &mon) {
  if (mon.binding != nullptr && mon.binding->online_sensor != nullptr) {
    mon.binding->online_sensor->publish_state(mon.online);
  }
}

void MonitorRegistry::publish_stale_telemetry() {
  for (auto &binding : bindings_) {
    if (binding.temperature_sensor != nullptr) binding.temperature_sensor->publish_state(NAN);
    if (binding.input_level_sensor != nullptr) binding.input_level_sensor->publish_state(NAN);
    if (binding.output_level_sensor != nullptr) binding.output_level_sensor->publish_state(NAN);
    for (sensor::Sensor *s : {binding.tw_coil_temperature_sensor, binding.md_coil_temperature_sensor,
                              binding.bs_coil_temperature_sensor, binding.psu_main_voltage_sensor,
                              binding.psu_low_voltage_sensor, binding.processor_load_sensor}) {
      if (s != nullptr) s->publish_state(NAN);
    }
    if (binding.online_sensor != nullptr) binding.online_sensor->invalidate_state();
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

void MonitorRegistry::set_binding_calibration_bypass(GenSAMMonitorBinding &b, bool bypass) {
  b.calibration_bypass = bypass;
  if (b.calibration_bypass_switch != nullptr) {
    b.calibration_bypass_switch->publish_state(bypass);
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
