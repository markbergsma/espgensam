#!/usr/bin/env python3
"""Run production window/persistence methods with a fake clock, wire and preferences."""

from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    end = source.index("\n}\n", start) + 3
    return source[start:end]


def main():
    source = (ROOT / "components/gensam/hub.cpp").read_text()
    header = (ROOT / "components/gensam/hub.h").read_text()
    race = (ROOT / "components/gensam/race.cpp").read_text()
    registry = (ROOT / "components/gensam/registry.cpp").read_text()
    if "--baseline" in sys.argv:
        source = subprocess.check_output(
            ["git", "show", "HEAD:components/gensam/hub.cpp"], cwd=ROOT, text=True
        )
        source = source.replace(
            "bool GenSAMHub::open_control_window_()",
            "bool GenSAMHub::open_control_window_(bool)",
        )
    assert "Control window expired; all TX suspended" in source, "No bounded bus-silence policy"
    window_default = re.search(r"uint32_t control_window_ms_\{(\d+)\}", header)[1]
    stored = source[source.index("struct StoredControlState"):source.index("\n};", source.index("struct StoredControlState")) + 3]
    methods = "\n".join(method(source, signature) for signature in (
        "void GenSAMHub::restore_control_state_()",
        "void GenSAMHub::save_control_state_()",
        "bool GenSAMHub::open_control_window_(",
        "bool GenSAMHub::defer_user_command_(",
        "void GenSAMHub::service_control_window_(",
        "void GenSAMHub::set_volume_db(",
        "void GenSAMHub::set_group_mute(",
        "void GenSAMHub::service_pending_writes_(",
        "void GenSAMHub::check_monitor_timeouts_()",
    ))
    methods += method(race, "void GenSAMHub::update_race_state_machine_()")
    methods += method(registry, "void MonitorRegistry::publish_stale_telemetry()")
    reply = method(race, "bool GenSAMHub::handle_active_reply_(")
    reply = reply[:reply.index("\n  switch")] + "\n  return false;\n}\n"
    cpp = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
uint32_t clock_ms = 0;
uint32_t millis() { return clock_ms; }
int warnings = 0;
#define ESP_LOGW(...) (++warnings)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define YESNO(value) ((value) ? "YES" : "NO")
const char *TAG = "test";
enum class RaceState {
  IDLE, POLLING_MONITORS, APPLYING_GROUP, WAKEUP_SENT, RACE_PING_SENT,
  RACE_SET_RID_SENT, QUERYING_DEVICES, CONFIGURING_DEVICES
};
constexpr uint32_t BOOT_SETTLE_MS = 10000;
constexpr uint32_t TIMEOUT_CHECK_INTERVAL_MS = 500;
constexpr uint32_t MIN_STALE_TIMEOUT_MS = 5000;
constexpr uint32_t STALE_POLL_CYCLES = 4;
constexpr uint32_t PASSIVE_STALE_TIMEOUT_MS = 15000;
constexpr int CMD_QUERY_STATUS = 8, CMD_REPORT_STATUS = 9, HOST_ADDRESS = 1;
namespace sensor {
struct Sensor {
  float state = 0;
  void publish_state(float value) { state = value; }
};
}
namespace binary_sensor {
struct BinarySensor {
  bool state = false, has_state = false;
  int invalidations = 0;
  void publish_state(bool value) { state = value; has_state = true; }
  void invalidate_state() { has_state = false; ++invalidations; }
};
}
struct GenSAMMonitorBinding {
  sensor::Sensor *temperature_sensor = nullptr, *input_level_sensor = nullptr,
                 *output_level_sensor = nullptr, *tw_coil_temperature_sensor = nullptr,
                 *md_coil_temperature_sensor = nullptr, *bs_coil_temperature_sensor = nullptr,
                 *psu_main_voltage_sensor = nullptr, *psu_low_voltage_sensor = nullptr,
                 *processor_load_sensor = nullptr;
  binary_sensor::BinarySensor *online_sensor = nullptr;
};
class MonitorRegistry {
 public:
  std::vector<GenSAMMonitorBinding> bindings_;
  void publish_stale_telemetry();
};
struct Frame {
  int address = HOST_ADDRESS, command = CMD_REPORT_STATUS;
  std::vector<uint8_t> payload{0};
};
Frame make_query(uint8_t, int) { return {}; }
struct Monitor {
  bool standby_known = true, standby = false;
  uint32_t identify_end_ms = 0, last_seen_ms = 0;
};
void parse_telemetry(const uint8_t *data, size_t size, Monitor &mon) {
  mon.standby_known = size != 0;
  if (size) mon.standby = data[0] != 0;
}
struct Registry {
  std::map<uint8_t, Monitor> items{{12, {}}, {13, {}}};
  int stale = 0, published = 0, timeout_checks = 0;
  bool empty() const { return items.empty(); }
  std::vector<uint8_t> addresses() const {
    std::vector<uint8_t> result;
    for (const auto &item : items) result.push_back(item.first);
    return result;
  }
  Monitor *find(uint8_t address) {
    const auto it = items.find(address);
    return it == items.end() ? nullptr : &it->second;
  }
  auto &monitors() { return items; }
  void publish_stale_telemetry() { ++stale; }
  void publish_telemetry(const Monitor &) { ++published; }
  bool expire_stale(uint32_t, uint32_t) { ++timeout_checks; return true; }
};
struct Arbiter {
  bool active = false;
  bool is_active() const { return active; }
};
struct ESPPreferenceObject {
  bool valid = false, fail = false;
  std::vector<uint8_t> bytes;
  template<typename T> bool load(T *out) {
    if (!valid) return false;
    memcpy(out, bytes.data(), sizeof(T));
    return true;
  }
  template<typename T> bool save(const T *value) {
    if (fail) return false;
    bytes.resize(sizeof(T));
    memcpy(bytes.data(), value, sizeof(T));
    valid = true;
    return true;
  }
};
struct Preferences {
  ESPPreferenceObject stored;
  template<typename T> ESPPreferenceObject make_preference(uint32_t) { return stored; }
} preferences, *global_preferences = &preferences;
'''
    cpp += stored + r'''
class GenSAMHub {
 public:
  bool listen_only_ = false, yield_to_glm_ = true, bus_silent_ = false;
  bool resuming_ = false, executing_user_command_ = false, power_off_pending_ = false;
  bool controls_only_session_ = false, current_mute_ = false;
  bool recovery_required_ = false;
  bool current_standby_ = false, initial_discovery_done_ = true;
  bool volume_tx_pending_ = false, mute_tx_pending_ = false, system_led_tx_pending_ = false;
  bool iss_tx_pending_ = false, filter_bypass_tx_pending_ = false;
  float current_volume_db_ = -30, min_volume_db_ = -80, max_volume_db_ = 0;
  uint32_t control_window_ms_ = WINDOW_DEFAULT, last_user_command_ms_ = 0;
  uint32_t race_step_time_ = 0, last_poll_cycle_time_ = 0;
  uint32_t boot_time_ = 0, last_poll_step_time_ = 0, last_volume_tx_ms_ = 0;
  uint32_t volume_tx_interval_ms_ = 50;
  uint32_t last_timeout_check_ = 0, poll_interval_ms_ = 1000;
  uint32_t group_generation_ = 5;
  uint8_t current_query_addr_ = 0, group_count_ = 2;
  size_t current_poll_index_ = 0;
  int selected_group_ = 0, default_group_ = 0, pending_group_ = -1;
  struct { bool active = false; } apply_;
  RaceState race_state_ = RaceState::POLLING_MONITORS;
  Registry registry_;
  Arbiter arbiter_;
  ESPPreferenceObject control_preference_;
  std::vector<uint8_t> poll_addrs_;
  std::vector<std::function<void()>> user_commands_;
  int discoveries = 0, transmissions = 0, notifications = 0, status_changes = 0, race_steps = 0;
  int volume_frames = 0, mute_frames = 0;
  int mute_evaluations = 0, standby_evaluations = 0;
  bool can_transmit() const { return !listen_only_ && !bus_silent_ && !arbiter_.active; }
  void discover_monitors_() {
    ++discoveries;
    resuming_ = false;
    controls_only_session_ = false;
    current_query_addr_ = 0;
    race_state_ = RaceState::WAKEUP_SENT;
  }
  void send_frame(const Frame &) { ++transmissions; }
  void update_bus_status_() { ++status_changes; }
  void notify_state_callbacks_() { ++notifications; }
  void evaluate_system_mute_() { ++mute_evaluations; }
  void evaluate_system_standby_() { ++standby_evaluations; }
  void mark_monitor_seen_(Monitor &) {}
  void transmit_volume_() {
    volume_tx_pending_ = false; ++volume_frames; ++transmissions;
  }
  void transmit_group_mute_() {
    mute_tx_pending_ = false; ++mute_frames; ++transmissions;
  }
  void transmit_system_leds_() { system_led_tx_pending_ = false; ++transmissions; }
  void transmit_filter_bypass_() { filter_bypass_tx_pending_ = false; ++transmissions; }
  void transmit_iss_settings_() { iss_tx_pending_ = false; ++transmissions; }
  void race_step_wakeup_(uint32_t) { ++race_steps; }
  void race_step_ping_(uint32_t) { ++race_steps; }
  void race_step_set_rid_(uint32_t) { ++race_steps; }
  void race_step_querying_(uint32_t) { ++race_steps; }
  void race_step_configuring_(uint32_t) { ++race_steps; }
  void race_step_applying_group_(uint32_t) { ++race_steps; }
  void race_step_polling_(uint32_t) { ++race_steps; }
  void race_step_idle_(uint32_t) { ++race_steps; }
  void set_standby(bool value) {
    assert(executing_user_command_);
    current_standby_ = value;
    bus_silent_ = value;
  }
  void restore_control_state_();
  void save_control_state_();
  bool open_control_window_(bool controls_only = false);
  bool defer_user_command_(std::function<void()> command);
  void service_control_window_(uint32_t now);
  void set_volume_db(float db);
  void set_group_mute(bool mute);
  void service_pending_writes_(uint32_t now);
  void update_race_state_machine_();
  void check_monitor_timeouts_();
  bool handle_active_reply_(const Frame &frame, uint32_t now);
};
'''
    cpp = cpp.replace("WINDOW_DEFAULT", window_default)
    cpp += methods + reply + r'''
void tick(GenSAMHub &hub, uint32_t now) {
  clock_ms = now;
  hub.service_control_window_(now);
}
void probe_active(GenSAMHub &hub) {
  for (int i = 0; i < 2; ++i) {
    tick(hub, clock_ms);
    assert(hub.current_query_addr_ != 0);
    assert(hub.handle_active_reply_(Frame{}, clock_ms));
  }
  tick(hub, clock_ms);
}
int main() {
  GenSAMHub hub;
  assert(hub.control_window_ms_ == 60000);
  tick(hub, 59999); assert(!hub.bus_silent_);
  tick(hub, 60000); assert(hub.bus_silent_ && hub.registry_.stale == 1);
  tick(hub, 70000); assert(hub.transmissions == 0);
  clock_ms = 70000;
  hub.set_volume_db(-42);
  assert(!hub.resuming_ && !hub.bus_silent_ && hub.volume_tx_pending_);
  tick(hub, 70301);
  assert(hub.transmissions == 0 && hub.discoveries == 0);
  assert(!hub.resuming_ && hub.discoveries == 0 && hub.current_volume_db_ == -42);
  hub.volume_tx_pending_ = false;
  tick(hub, 129999); assert(!hub.bus_silent_);
  tick(hub, 130000); assert(hub.bus_silent_);

  // No replies or valid cached addresses are needed, even across repeated windows.
  for (bool mute_first : {false, true}) {
    GenSAMHub controls;
    controls.bus_silent_ = true;
    controls.current_standby_ = true;
    controls.recovery_required_ = true;
    controls.pending_group_ = 1;  // A GLM takeover may have scheduled restoration.
    if (mute_first) controls.registry_.items.clear();
    clock_ms = 200000;
    if (mute_first) controls.set_group_mute(true);
    else controls.set_volume_db(-45);
    assert(controls.controls_only_session_ && !controls.resuming_);
    assert(controls.transmissions == 0);  // Setters still never transmit.
    controls.service_pending_writes_(clock_ms);
    if (mute_first) controls.set_volume_db(-45);
    else controls.set_group_mute(true);
    controls.service_pending_writes_(clock_ms);
    assert(controls.volume_frames == 1 && controls.mute_frames == 1);
    for (uint32_t now : {200001U, 200301U, 210001U, 260000U}) {
      tick(controls, now);
      controls.update_race_state_machine_();
    }
    assert(controls.discoveries == 0 && controls.race_steps == 0);
    assert(controls.transmissions == 2 && controls.bus_silent_);
    controls.set_group_mute(false);
    controls.service_pending_writes_(clock_ms);
    assert(controls.controls_only_session_ && controls.mute_frames == 2);
    controls.recovery_required_ = false;
    assert(controls.open_control_window_());  // Explicit Power On/group restoration.
    tick(controls, clock_ms);
    assert(controls.discoveries == 1 && !controls.controls_only_session_);
  }

  GenSAMHub active_controls;
  clock_ms = 100000;
  active_controls.set_group_mute(true);
  assert(!active_controls.controls_only_session_ && !active_controls.resuming_);
  active_controls.current_query_addr_ = 12;
  active_controls.service_pending_writes_(clock_ms);
  assert(active_controls.transmissions == 0);  // Still wait for an outstanding reply.
  active_controls.current_query_addr_ = 0;
  active_controls.service_pending_writes_(clock_ms);
  active_controls.update_race_state_machine_();
  assert(active_controls.mute_frames == 1 && active_controls.race_steps == 1);

  GenSAMHub paused_timeout;
  paused_timeout.controls_only_session_ = true;
  paused_timeout.registry_.items[12].last_seen_ms = 1;
  tick(paused_timeout, 5000);
  paused_timeout.check_monitor_timeouts_();
  assert(paused_timeout.registry_.timeout_checks == 0 &&
         paused_timeout.last_timeout_check_ == 0);
  paused_timeout.controls_only_session_ = false;
  paused_timeout.check_monitor_timeouts_();
  assert(paused_timeout.registry_.timeout_checks == 1 &&
         paused_timeout.last_timeout_check_ == 5000 &&
         paused_timeout.mute_evaluations == 1 &&
         paused_timeout.standby_evaluations == 1);

  sensor::Sensor temperature, input_level, output_level, coil;
  binary_sensor::BinarySensor online;
  temperature.publish_state(42);
  input_level.publish_state(-30);
  output_level.publish_state(-20);
  coil.publish_state(33);
  online.publish_state(true);
  MonitorRegistry registry;
  GenSAMMonitorBinding binding;
  binding.temperature_sensor = &temperature;
  binding.input_level_sensor = &input_level;
  binding.output_level_sensor = &output_level;
  binding.tw_coil_temperature_sensor = &coil;
  binding.online_sensor = &online;
  registry.bindings_.push_back(binding);
  registry.publish_stale_telemetry();
  assert(std::isnan(temperature.state) && std::isnan(input_level.state) &&
         std::isnan(output_level.state) && std::isnan(coil.state));
  assert(!online.has_state && online.invalidations == 1);

  GenSAMHub infinite;
  infinite.control_window_ms_ = 0;
  tick(infinite, 4000000000U); assert(!infinite.bus_silent_);
  GenSAMHub wrapped;
  wrapped.last_user_command_ms_ = UINT32_MAX - 10000;
  tick(wrapped, 49998); assert(!wrapped.bus_silent_);
  tick(wrapped, 49999); assert(wrapped.bus_silent_);

  GenSAMHub busy;
  busy.current_query_addr_ = 12;
  tick(busy, 60000); assert(!busy.bus_silent_);
  busy.current_query_addr_ = 0;
  busy.apply_.active = true;
  tick(busy, 60000); assert(!busy.bus_silent_);
  busy.apply_.active = false;
  busy.volume_tx_pending_ = true;
  tick(busy, 60000); assert(!busy.bus_silent_);
  busy.volume_tx_pending_ = false;
  busy.iss_tx_pending_ = true;
  tick(busy, 60000); assert(!busy.bus_silent_);
  busy.iss_tx_pending_ = false;
  busy.filter_bypass_tx_pending_ = true;
  tick(busy, 60000); assert(!busy.bus_silent_);
  busy.filter_bypass_tx_pending_ = false;
  busy.pending_group_ = 1;
  tick(busy, 60000); assert(!busy.bus_silent_);
  busy.pending_group_ = -1;
  busy.registry_.items[12].identify_end_ms = 90000;
  tick(busy, 60000); assert(!busy.bus_silent_);
  busy.registry_.items[12].identify_end_ms = 0;
  tick(busy, 60000); assert(busy.bus_silent_);

  GenSAMHub commands;
  commands.bus_silent_ = true;
  int applied = 0;
  clock_ms = 100;
  assert(commands.defer_user_command_([&]() { ++applied; }));
  assert(applied == 0 && commands.resuming_);
  probe_active(commands);
  assert(applied == 1 && commands.discoveries == 0);
  assert(commands.last_user_command_ms_ == 100);  // Probes/background do not extend it.
  tick(commands, 60100); assert(commands.bus_silent_);
  commands.current_standby_ = true;
  assert(commands.open_control_window_() && !commands.current_standby_);

  GenSAMHub asleep;
  asleep.bus_silent_ = true;
  assert(asleep.open_control_window_());
  tick(asleep, clock_ms);
  Frame sleeping; sleeping.payload = {1};
  assert(asleep.handle_active_reply_(sleeping, clock_ms));
  assert(asleep.discoveries == 1);
  GenSAMHub missing;
  missing.bus_silent_ = true;
  assert(missing.open_control_window_());
  tick(missing, 10);
  Frame incomplete; incomplete.payload.clear();
  assert(missing.handle_active_reply_(incomplete, 10));
  tick(missing, 309); assert(missing.discoveries == 0);
  tick(missing, 310); assert(missing.discoveries == 1);

  GenSAMHub off;
  off.power_off_pending_ = true;
  tick(off, 100);
  assert(off.current_standby_ && off.bus_silent_ && off.discoveries == 0);
  GenSAMHub off_during_probe;
  off_during_probe.power_off_pending_ = true;
  off_during_probe.resuming_ = true;
  off_during_probe.current_query_addr_ = 12;
  off_during_probe.race_step_time_ = 10;
  tick(off_during_probe, 359);
  assert(!off_during_probe.bus_silent_ && off_during_probe.transmissions == 0);
  tick(off_during_probe, 360);
  assert(off_during_probe.bus_silent_ && !off_during_probe.resuming_ &&
         off_during_probe.discoveries == 0);
  GenSAMHub blocked;
  blocked.arbiter_.active = true;
  const int before = warnings;
  assert(blocked.defer_user_command_([]() {}));
  blocked.set_volume_db(-20);
  blocked.set_group_mute(true);
  assert(blocked.user_commands_.empty() && blocked.current_volume_db_ == -30);
  assert(!blocked.current_mute_ && warnings >= before + 3);
  blocked.arbiter_.active = false;
  blocked.listen_only_ = true;
  assert(!blocked.open_control_window_());
  GenSAMHub full;
  for (int i = 0; i < 32; ++i) assert(full.defer_user_command_([]() {}));
  assert(full.defer_user_command_([]() {}));
  assert(full.user_commands_.size() == 32);
  GenSAMHub externally_changed;
  externally_changed.bus_silent_ = true;
  externally_changed.recovery_required_ = true;
  assert(externally_changed.open_control_window_());
  tick(externally_changed, 100);
  assert(externally_changed.discoveries == 1 && externally_changed.transmissions == 0);
  GenSAMHub empty;
  empty.registry_.items.clear();
  empty.bus_silent_ = true;
  assert(empty.open_control_window_());
  assert(empty.discoveries == 0);  // External caller never transmits.
  tick(empty, 100);
  assert(empty.discoveries == 1);

  GenSAMHub saved;
  saved.restore_control_state_();
  saved.current_volume_db_ = -42;
  saved.selected_group_ = 1;
  saved.save_control_state_();
  preferences.stored = saved.control_preference_;
  GenSAMHub reboot;
  reboot.max_volume_db_ = -50;
  reboot.restore_control_state_();
  assert(reboot.current_volume_db_ == -50 && reboot.default_group_ == 1);
  GenSAMHub new_upload;
  new_upload.group_generation_ = 6;
  new_upload.restore_control_state_();
  assert(new_upload.default_group_ == 0 && new_upload.selected_group_ == 0);
  saved.control_preference_.fail = true;
  const int save_warning = warnings;
  saved.save_control_state_();
  assert(warnings == save_warning + 1);
}
'''
    with tempfile.TemporaryDirectory(prefix="gensam-window-") as directory:
        root = Path(directory)
        (root / "test.cpp").write_text(cpp)
        subprocess.run(
            ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", "-fsanitize=address,undefined",
             str(root / "test.cpp"), "-o", str(root / "test")], check=True
        )
        subprocess.run([str(root / "test")], check=True)
    print("Control-window, cached resume and preference regression checks passed")


if __name__ == "__main__":
    main()
