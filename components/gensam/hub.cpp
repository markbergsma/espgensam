/// @file hub.cpp
/// @brief ESPHome hub component for native Genelec SAM RS-485 communication.
/// See hub.h for architectural design rationale and complete API documentation.

#include "hub.h"
#include "commands.h"

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "crc.h"
#include "esphome/core/log.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "driver/gpio.h"

static const char *const TAG = "gensam";

namespace esphome {
namespace gensam {

namespace {

/// Slew time for the monitors' internal DSP volume to ramp down to digital silence.
constexpr uint32_t VOLUME_RAMP_DOWN_MS = 30;

/// Time for monitor input circuitry, the AES3 PLL, and the SRC to relock after an input switch.
constexpr uint32_t INPUT_SETTLE_MS = 100;

/// Spacing between per-monitor source selection frames during a global source change.
constexpr uint32_t SOURCE_FRAME_GAP_MS = 5;

/// How often to emit the RX / framing statistics line.
constexpr uint32_t STAT_LOG_INTERVAL_MS = 15000;

/// How often to sweep the registry for monitors that have gone silent.
constexpr uint32_t TIMEOUT_CHECK_INTERVAL_MS = 500;

/// Polls a monitor may miss before it is considered offline.
constexpr uint32_t STALE_POLL_CYCLES = 4;

/// Floor on the staleness window, so a short poll interval cannot declare a monitor
/// offline over a single missed reply.
constexpr uint32_t MIN_STALE_TIMEOUT_MS = 5000;

/// Floor on the staleness window while snooping.  The hub is not driving the queries then,
/// so how often a monitor is heard from depends on the external master's polling cadence.
constexpr uint32_t PASSIVE_STALE_TIMEOUT_MS = 15000;

/// How long a commanded power change is trusted over contradicting telemetry.
///
/// Monitors keep reporting their previous amplifier state until the transition physically
/// completes: send_standby() alone spends 160 ms on the wire before the DSP begins powering
/// down, and a wakeup needs a DSP boot plus a full RACE cycle before the first poll.  Polling
/// continues throughout, so without this window the first reply would revert the state the
/// user just commanded.  A command that never took effect still self-corrects once the window
/// expires and telemetry keeps disagreeing.
constexpr uint32_t STANDBY_SETTLE_MS = 5000;

}  // namespace

void GenSAMHub::assert_output_pin_(int pin, bool active_high, bool pull_up,
                                   const char *description) {
  if (pin < 0) {
    return;
  }
  rs485_configure_output(pin, active_high, true, pull_up);
  ESP_LOGI(TAG, "%s (GPIO%d) asserted %s", description, pin, active_high ? "HIGH" : "LOW");
}

void GenSAMHub::setup_transceiver_pins_() {
  // 1. Assert RS485 DC-DC Power Enable (T-CAN485: GPIO16 = 1 powers 5V boost converter)
  if (rs485_.power_pin >= 0) {
    this->assert_output_pin_(rs485_.power_pin, rs485_.power_active_high, false,
                             "RS485 5V Power Enable pin");
    delay(50);  // Allow 5V DC-DC booster to stabilize
  }

  // 2. Assert Transceiver Enable (T-CAN485: GPIO19 = 1 turns ON NPN shifter to enable MAX13487)
  this->assert_output_pin_(rs485_.se_pin, rs485_.se_active_high, false,
                           "RS485 Transceiver Enable pin");

  // 3. Assert Receiver Enable / AutoDirection (T-CAN485: GPIO17 = 1 turns ON NPN shifter -> Receiver & AutoDirection ON)
  this->assert_output_pin_(rs485_.re_pin, rs485_.re_active_high, false,
                           "RS485 Receiver Enable pin");
}

void GenSAMHub::setup_uart_() {
  // Initialize 9-bit driver (RMT RX + RMT TX continuous zero-gap bitstream)
  Rs485Profile profile = rs485_;
  if (listen_only_) {
    // In listen-only mode, drive TX pin HIGH (DE deasserted on RS-485 transceiver)
    // and do not initialize the RMT TX transmitter channel.
    this->assert_output_pin_(rs485_.tx_pin, !rs485_.tx_inverted, true,
                             "RS485 TX pin (listen-only, idle)");
    profile.tx_pin = -1;
  }
  uart9_.setup(profile, baud_rate_, rx_buffer_size_);
}

void GenSAMHub::setup() {
  ESP_LOGI(TAG, "Initializing GenSAM Hub on RS485 bus...");
  boot_time_ = millis();

  if (rs485_.tx_pin < 0 || rs485_.rx_pin < 0) {
    ESP_LOGE(TAG, "Invalid TX (%d) or RX (%d) pin configuration", rs485_.tx_pin, rs485_.rx_pin);
    this->mark_failed();
    return;
  }

  this->setup_transceiver_pins_();
  this->setup_uart_();

  if (!uart9_.is_initialized()) {
    ESP_LOGE(TAG, "Failed to initialize 9-bit UART/RMT driver");
    this->mark_failed();
    return;
  }

  current_volume_db_ = startup_volume_db_;
  if (volume_number_ != nullptr) {
    volume_number_->publish_state(current_volume_db_);
  }

  for (auto &b : registry_.bindings()) {
    if (b.input_select != nullptr && b.input_configured) {
      b.input_select->publish_state(input_to_str(b.source, b.aes3_channel));
    }
  }

  // Show the group that discovery is about to apply, rather than leaving the entity unknown
  // for the ten seconds or so until it lands. Suppressed in listen-only mode, where nothing
  // will be transmitted and naming a group would assert something untrue about the bus.
  if (!listen_only_ && default_group_ >= 0 && group_select_ != nullptr) {
    const GroupPreset *group = this->get_group(static_cast<uint8_t>(default_group_));
    if (group != nullptr) {
      group_select_->publish_state(group->name);
    }
  }

  this->update_bus_status_();

  ESP_LOGI(TAG, "GenSAM Hub initialized successfully (baud=%lu, TX=%s, RX=GPIO%d, yield_to_glm=%s, cooldown=%u ms)",
           (unsigned long)baud_rate_,
           listen_only_ ? "DISABLED (listen_only)" : ("GPIO" + std::to_string(rs485_.tx_pin)).c_str(),
           rs485_.rx_pin, YESNO(yield_to_glm_), (unsigned)glm_inactivity_cooldown_ms_);
}

void GenSAMHub::set_active_group(uint8_t index) {
  const GroupPreset *group = this->get_group(index);
  if (group == nullptr) {
    ESP_LOGW(TAG, "Ignoring group preset %u: only %u are configured", (unsigned) index,
             (unsigned) group_count_);
    return;
  }

  // Record the request rather than transmitting here. The push is hundreds of frames and has
  // to be paced, and this is called from a Home Assistant callback which must return promptly.
  // Overwriting any earlier request is deliberate: flicking through the select in the user
  // interface should settle on the last choice, not play every group in turn.
  pending_group_ = index;
  ESP_LOGD(TAG, "Group preset '%s' queued for application", group->name);

  // Reflect the choice immediately so the entity does not sit on the old value for the
  // second or so the push takes; finish_group_apply_() publishes it again on completion.
  if (group_select_ != nullptr) {
    group_select_->publish_state(group->name);
  }
}

void GenSAMHub::set_active_group_by_name(const std::string &name) {
  const int index = this->find_group_index(name);
  if (index < 0) {
    ESP_LOGW(TAG, "Ignoring unknown group preset '%s'", name.c_str());
    return;
  }
  this->set_active_group(static_cast<uint8_t>(index));
}

int GenSAMHub::find_group_index(const std::string &name) const {
  for (uint8_t i = 0; i < group_count_; i++) {
    if (groups_[i].name != nullptr && name == groups_[i].name) {
      return i;
    }
  }
  return -1;
}

const GroupDevice *GenSAMHub::find_group_device(const GroupPreset &group, uint32_t unique_id) {
  for (uint8_t i = 0; i < group.device_count; i++) {
    if (group.devices[i].unique_id == unique_id) {
      return &group.devices[i];
    }
  }
  return nullptr;
}

void GenSAMHub::dump_config() {
  ESP_LOGCONFIG(TAG, "GenSAM Hub:");
  ESP_LOGCONFIG(TAG, "  TX Pin: GPIO%d", rs485_.tx_pin);
  ESP_LOGCONFIG(TAG, "  RX Pin: GPIO%d", rs485_.rx_pin);
  ESP_LOGCONFIG(TAG, "  Volume Bounds: [%.1f dB, %.1f dB] (Startup: %.1f dB)",
                min_volume_db_, max_volume_db_, startup_volume_db_);
  if (volume_number_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Volume dB Number Entity: configured");
  }
  if (group_modified_sensor_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Group Modified Binary Sensor: configured");
  }
  if (bus_status_sensor_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Bus Status Text Sensor: configured");
  }
  ESP_LOGCONFIG(TAG, "  Configured Monitor Bindings: %u", (unsigned)registry_.bindings().size());
  for (const auto &b : registry_.bindings()) {
    ESP_LOGCONFIG(TAG, "    - Name: '%s' (SN: '%s')", b.name.c_str(), b.serial_number.c_str());
  }
  ESP_LOGCONFIG(TAG, "  Group Presets: %u", (unsigned)group_count_);
  for (uint8_t g = 0; g < group_count_; g++) {
    const GroupPreset &grp = groups_[g];
    ESP_LOGCONFIG(TAG, "    - '%s' (%u devices)", grp.name, (unsigned)grp.device_count);
    for (uint8_t d = 0; d < grp.device_count; d++) {
      const GroupDevice &dev = grp.devices[d];
      // Report the bands that will actually be transmitted as filters rather than as the
      // bypass vector, since a generated table is mostly empty slots and the count is the
      // quickest way to see that a group carries the calibration it should.
      unsigned active = 0;
      for (uint8_t b = 0; b < dev.band_count; b++) {
        if (dev.bands[b].type != PeqType::BYPASS && dev.bands[b].gain_db != 0.0f) {
          active++;
        }
      }
      char xo[CROSSOVER_STR_SIZE];
      ESP_LOGCONFIG(TAG, "        id %lu: %s, %s, %s, %.2f dB, %lu samples, %u/%u bands",
                    (unsigned long)dev.unique_id, dev.enabled ? "on" : "off",
                    input_to_str(dev.source, dev.aes3_channel),
                    crossover_to_str(dev.crossover_hz, xo, sizeof(xo)), dev.level_db, (unsigned long)dev.delay_samples, active,
                    (unsigned)dev.band_count);
    }
  }
  if (rs485_.de_pin >= 0) {
    ESP_LOGCONFIG(TAG, "  Hardware DE (Direction) Pin: GPIO%d (active %s)", rs485_.de_pin,
                  rs485_.de_active_high ? "high" : "low");
  }
  if (rs485_.re_pin >= 0) {
    ESP_LOGCONFIG(TAG, "  Receiver Enable Pin: GPIO%d", rs485_.re_pin);
  }
  if (rs485_.power_pin >= 0) {
    ESP_LOGCONFIG(TAG, "  5V Power Pin: GPIO%d", rs485_.power_pin);
  }
  if (rs485_.se_pin >= 0) {
    ESP_LOGCONFIG(TAG, "  Transceiver Pin: GPIO%d", rs485_.se_pin);
  }
  ESP_LOGCONFIG(TAG, "  TX Echoes into RX: %s", YESNO(rs485_.tx_echoes_rx));
  ESP_LOGCONFIG(TAG, "  Baud Rate: %lu bps (fixed GLM standard)", (unsigned long)baud_rate_);
  ESP_LOGCONFIG(TAG, "  RX Buffer Size: %u characters", (unsigned)rx_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Listen Only: %s", YESNO(listen_only_));
  ESP_LOGCONFIG(TAG, "  Yield to GLM: %s", YESNO(yield_to_glm_));
  ESP_LOGCONFIG(TAG, "  GLM Inactivity Cooldown: %u ms", (unsigned)glm_inactivity_cooldown_ms_);
  ESP_LOGCONFIG(TAG, "  Discovered Monitors: %u", (unsigned)registry_.size());
  for (const auto &kv : registry_.monitors()) {
    ESP_LOGCONFIG(TAG, "    - %s", kv.second.to_string().c_str());
  }
}

bool GenSAMHub::can_transmit() const {
  if (listen_only_) {
    return false;
  }
  if (yield_to_glm_ && arbiter_.is_active()) {
    return false;
  }
  return uart9_.is_initialized();
}

void GenSAMHub::send_raw_frame(const std::vector<Uart9BitChar> &raw_chars) {
  if (!can_transmit() || raw_chars.empty()) {
    return;
  }
  arbiter_.note_tx(millis());
  uart9_.write(raw_chars.data(), raw_chars.size());
}

bool GenSAMHub::send_frame(const Frame &frame) {
  if (!uart9_.is_initialized()) {
    ESP_LOGW(TAG, "Cannot send frame: UART9 driver not initialized");
    return false;
  }

  if (listen_only_) {
    ESP_LOGW(TAG, "Cannot send frame: listen_only mode is enabled");
    return false;
  }

  if (yield_to_glm_ && arbiter_.is_active()) {
    uint32_t now = millis();
    if (now - last_tx_blocked_warning_ > 5000) {
      last_tx_blocked_warning_ = now;
      uint32_t elapsed = now - arbiter_.last_activity();
      uint32_t remaining = (elapsed < glm_inactivity_cooldown_ms_) ? (glm_inactivity_cooldown_ms_ - elapsed) : 0;
      ESP_LOGW(TAG, "Cannot send frame: External GLM master/adapter is active on bus (cooldown: %u s remaining)",
               (unsigned)(remaining / 1000));
    }
    return false;
  }

  std::vector<Uart9BitChar> wire_chars = frame.to_9bit();
  ESP_LOGD(TAG, "TX -> %s", frame.to_string().c_str());

  arbiter_.note_tx(millis());
  uart9_.write(wire_chars.data(), wire_chars.size());
  parser_.clear();
  return true;
}

bool GenSAMHub::send_frame_twice(const Frame &frame) {
  bool sent = this->send_frame(frame);
  if (sent) {
    delayMicroseconds(250);
  }
  return this->send_frame(frame) || sent;
}

void GenSAMHub::send_wakeup() {
  if (!can_transmit()) {
    return;
  }
  ESP_LOGI(TAG, "Sending GLM wakeup broadcast sequence to monitors...");

  for (int i = 0; i < 3; i++) {
    this->send_frame(make_wakeup_step(WAKEUP_OP_POWER, WAKEUP_VAL_ON_1));  // {0x03, 0x7F}
    delay(5);
    this->send_frame(make_wakeup_step(WAKEUP_OP_POWER, WAKEUP_VAL_ON_2));  // {0x03, 0x01}
    delay(10);
  }
}

void GenSAMHub::send_standby() {
  if (!can_transmit()) {
    return;
  }
  ESP_LOGI(TAG, "Sending GLM standby broadcast sequence to monitors...");

  for (int i = 0; i < 2; i++) {
    this->send_frame(make_wakeup_step(WAKEUP_OP_POWER, WAKEUP_VAL_STANDBY_1));  // {0x03, 0x02}
    delay(20);
  }

  delay(80);

  for (int i = 0; i < 2; i++) {
    this->send_frame(make_wakeup_step(WAKEUP_OP_POWER, WAKEUP_VAL_STANDBY_2));  // {0x03, 0x00}
    delay(20);
  }
}

void GenSAMHub::handle_incoming_frame_(const Frame &frame) {
  uint32_t now = millis();

  // 1. ACTIVE MASTER STATE MACHINE: a frame that answers our own outstanding request is
  //    consumed here and not snooped, since we already know what it means.
  if (!arbiter_.is_active() && !listen_only_ && this->handle_active_reply_(frame, now)) {
    return;
  }

  // 2. PASSIVE SNOOPING: everything else, including all traffic from an external GLM master.
  this->snoop_frame_(frame);
}

void GenSAMHub::process_rx_() {
  constexpr size_t BATCH_SIZE = 64;
  Uart9BitChar rx_chars[BATCH_SIZE];

  size_t count = uart9_.read(rx_chars, BATCH_SIZE, 0);
  if (count == 0) {
    return;
  }

  char raw_hex[256];
  size_t hpos = 0;
  for (size_t i = 0; i < count && hpos + 6 < sizeof(raw_hex); i++) {
    hpos += snprintf(raw_hex + hpos, sizeof(raw_hex) - hpos, "%02X%s ",
                     rx_chars[i].data, rx_chars[i].ninth_bit ? "'" : "");
  }
  ESP_LOGD(TAG, "RAW RX (%u chars): %s", (unsigned)count, raw_hex);

  // Feed characters to incremental stream parser
  parser_.feed(rx_chars, count);

  // Dispatch all decoded frames
  Frame frame;
  while (parser_.pop_frame(frame)) {
    uint32_t now = millis();
    ESP_LOGD(TAG, "RX <- %s", frame.to_string().c_str());

    if (!frame.crc_valid) {
      ESP_LOGW(TAG, "Dropping frame with invalid CRC: %s", frame.to_string().c_str());
      continue;
    }

    // Bus arbitration: attribute the frame, and yield the bus to any external GLM master
    BusArbiter::Verdict verdict = arbiter_.classify(frame, now);
    if (verdict == BusArbiter::Verdict::EXTERNAL_MASTER) {
      if (arbiter_.note_external_activity(now)) {
        ESP_LOGW(TAG, "External GLM master/adapter detected on bus (%s). Yielding bus control (listen-only mode)...",
                 frame.to_string().c_str());
        this->update_bus_status_();
      }
    } else if (verdict == BusArbiter::Verdict::LOOPBACK_ECHO && !listen_only_) {
      // Loopback echo of our own master transmission; ignore
      continue;
    }

    // Process frame through monitor discovery / telemetry state machine
    handle_incoming_frame_(frame);

    for (auto &cb : callbacks_) {
      cb(frame);
    }
  }
}

void GenSAMHub::check_glm_cooldown_() {
  if (!arbiter_.check_cooldown(millis(), glm_inactivity_cooldown_ms_)) {
    return;
  }

  ESP_LOGI(TAG, "No GLM master/adapter traffic observed for %u seconds. Resuming active bus control.",
           (unsigned)(glm_inactivity_cooldown_ms_ / 1000));
  // Trigger fresh wakeup and discovery when resuming active master control
  this->rediscover_monitors();

  // Refresh the status here too: rediscover_monitors() returns before publishing when the hub
  // cannot transmit (listen-only), which would otherwise latch the status at "GLM Active".
  this->update_bus_status_();
}

void GenSAMHub::mark_monitor_seen_(GenSAMMonitor &mon) {
  if (registry_.mark_seen(mon)) {
    this->evaluate_system_mute_();
  }
}

void GenSAMHub::check_monitor_timeouts_() {
  uint32_t now = millis();
  if (now - last_timeout_check_ < TIMEOUT_CHECK_INTERVAL_MS) {
    return;
  }
  last_timeout_check_ = now;

  uint32_t stale_timeout_ms = std::max<uint32_t>(MIN_STALE_TIMEOUT_MS, poll_interval_ms_ * STALE_POLL_CYCLES);
  if (listen_only_ || arbiter_.is_active()) {
    stale_timeout_ms = std::max<uint32_t>(stale_timeout_ms, PASSIVE_STALE_TIMEOUT_MS);
  }

  if (registry_.expire_stale(now, stale_timeout_ms)) {
    this->evaluate_system_mute_();
    this->evaluate_system_standby_();
  }
}

void GenSAMHub::evaluate_system_mute_() {
  if (registry_.empty()) {
    return;
  }
  bool any_online = false;
  bool all_muted = registry_.all_online_muted(any_online);

  bool new_mute = any_online ? all_muted : false;
  if (current_mute_ != new_mute) {
    current_mute_ = new_mute;
    ESP_LOGI(TAG, "System mute updated to %s", YESNO(current_mute_));
    this->notify_state_callbacks_();
  }
}

void GenSAMHub::evaluate_system_standby_() {
  if (registry_.empty()) {
    return;
  }
  if (restore_standby_after_discovery_) {
    return;  // Amplifiers are temporarily up for discovery; that is not the user's intent
  }
  if (millis() - last_standby_command_ < STANDBY_SETTLE_MS) {
    return;  // A commanded transition is still in flight; telemetry has not caught up yet
  }
  bool any_reported = false;
  bool all_standby = registry_.all_online_in_standby(any_reported);
  if (!any_reported) {
    return;  // Nothing on the bus reports a power state; leave the commanded state alone
  }

  if (current_standby_ != all_standby) {
    current_standby_ = all_standby;
    ESP_LOGI(TAG, "System power state updated to %s based on monitor telemetry",
             current_standby_ ? "STANDBY (OFF)" : "ACTIVE (ON)");
    this->notify_state_callbacks_();
  }
}

void GenSAMHub::notify_state_callbacks_() {
  if (volume_number_ != nullptr) {
    volume_number_->publish_state(current_volume_db_);
  }
  for (auto &cb : state_callbacks_) {
    cb(current_volume_db_, current_mute_, current_standby_);
  }
}

void GenSAMHub::update_bus_status_() {
  std::string status;
  if (arbiter_.is_active()) {
    status = "GLM Active";
  } else {
    switch (race_state_) {
      case RaceState::WAKEUP_SENT:
      case RaceState::RACE_PING_SENT:
      case RaceState::RACE_SET_RID_SENT:
      case RaceState::QUERYING_DEVICES:
        status = "Discovering";
        break;
      case RaceState::CONFIGURING_DEVICES:
        status = "Configuring";
        break;
      case RaceState::POLLING_MONITORS:
        status = "Active";
        break;
      case RaceState::IDLE:
      default:
        status = registry_.any_online() ? "Active" : "Offline";
        break;
    }
  }

  if (status != last_bus_status_) {
    last_bus_status_ = status;
    ESP_LOGI(TAG, "Bus operational status changed to: %s", status.c_str());
    if (bus_status_sensor_ != nullptr) {
      bus_status_sensor_->publish_state(status);
    }
    for (auto &cb : bus_status_callbacks_) {
      cb(status);
    }
  }
}

void GenSAMHub::set_volume_db(float db) {
  float target_db = std::clamp(db, min_volume_db_, max_volume_db_);
  if (current_standby_) {
    current_volume_db_ = target_db;
    this->notify_state_callbacks_();
    return;
  }
  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot send volume command: Bus is not available for TX");
    return;
  }
  // Broadcast master volume (0xFF) to all monitors on bus
  if (!this->send_frame(make_broadcast_volume_db(target_db))) {
    ESP_LOGW(TAG, "Volume command was not transmitted; keeping previous state");
    return;
  }

  current_volume_db_ = target_db;

  ESP_LOGI(TAG, "Set system volume: %.1f dB (int24=%u)", current_volume_db_,
           (unsigned)volume_db_to_int24(target_db));
  this->notify_state_callbacks_();
}

void GenSAMHub::set_group_mute(bool mute) {
  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot send mute command: Bus is not available for TX");
    return;
  }
  bool transmitted = false;

  // 1. Unicast CMD_BYPASS to each discovered monitor individually (as per GLM protocol)
  for (const auto &kv : registry_.monitors()) {
    bool sent = this->send_frame(make_bypass(kv.first, mute));
    transmitted = transmitted || sent;
    if (sent) {
      delayMicroseconds(250);
    }
  }

  // 2. Also send to BROADCAST_ADDRESS (0xFF)
  transmitted = this->send_frame(make_bypass(BROADCAST_ADDRESS, mute)) || transmitted;

  if (!transmitted) {
    ESP_LOGW(TAG, "Mute command was not transmitted; keeping previous state");
    return;
  }

  current_mute_ = mute;
  for (auto &kv : registry_.monitors()) {
    registry_.set_mute(kv.second, mute);
  }

  ESP_LOGI(TAG, "Set system mute: %s across %zu monitors", YESNO(mute), registry_.size());
  this->notify_state_callbacks_();
}

void GenSAMHub::set_monitor_mute(uint8_t address, bool mute) {
  GenSAMMonitor *mon = registry_.find(address);
  if (mon == nullptr) {
    ESP_LOGW(TAG, "Cannot mute monitor 0x%02X: not found in registry", address);
    return;
  }

  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot send mute command to 0x%02X: bus not available for TX", address);
    return;
  }

  if (!this->send_frame_twice(make_bypass(address, mute))) {
    ESP_LOGW(TAG, "Mute command to 0x%02X was not transmitted; keeping previous state", address);
    return;
  }

  registry_.set_mute(*mon, mute);
  this->evaluate_system_mute_();

  ESP_LOGI(TAG, "Set monitor 0x%02X mute: %s", address, YESNO(mute));
}

void GenSAMHub::set_monitor_mute_by_serial(const std::string &serial_or_id, bool mute) {
  GenSAMMonitor *mon = registry_.find_by_serial_or_id(serial_or_id);
  if (mon == nullptr) {
    ESP_LOGW(TAG, "Cannot mute speaker '%s': monitor not currently discovered on bus", serial_or_id.c_str());
    return;
  }
  this->set_monitor_mute(mon->address, mute);
}

void GenSAMHub::set_monitor_crossover(uint8_t address, uint16_t freq_hz) {
  // A hand-picked crossover deviates from the active group, the same as an input override.
  this->set_group_modified(true);

  GenSAMMonitor *mon = registry_.find(address);
  if (mon == nullptr) {
    ESP_LOGW(TAG, "Cannot set crossover for unknown monitor 0x%02X", address);
    return;
  }

  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot send crossover command to 0x%02X: bus not available for TX", address);
    return;
  }

  if (!this->send_frame_twice(make_crossover(address, freq_hz))) {
    ESP_LOGW(TAG, "Crossover command to 0x%02X was not transmitted", address);
    return;
  }

  registry_.set_crossover(*mon, freq_hz);

  char buf[CROSSOVER_STR_SIZE];
  ESP_LOGI(TAG, "Set monitor 0x%02X crossover: %s", address, crossover_to_str(freq_hz, buf, sizeof(buf)));
}

void GenSAMHub::set_monitor_crossover_by_serial(const std::string &serial_or_id, uint16_t freq_hz) {
  // Store on the binding first, so the setting survives a monitor that is not (yet) on the bus.
  GenSAMMonitorBinding *binding = registry_.find_binding_by_serial_or_id(serial_or_id);
  if (binding != nullptr) {
    registry_.set_binding_crossover(*binding, freq_hz);
  }

  GenSAMMonitor *mon = registry_.find_by_serial_or_id(serial_or_id);
  if (mon == nullptr) {
    char buf[CROSSOVER_STR_SIZE];
    ESP_LOGW(TAG, "Crossover set for '%s' to %s (stored; monitor not currently discovered on bus)",
             serial_or_id.c_str(), crossover_to_str(freq_hz, buf, sizeof(buf)));
    return;
  }
  this->set_monitor_crossover(mon->address, freq_hz);
}

void GenSAMHub::set_monitor_crossover_by_name(const std::string &serial_or_id, const std::string &option) {
  uint16_t freq_hz = DEFAULT_CROSSOVER_HZ;
  if (!str_to_crossover(option.c_str(), freq_hz)) {
    ESP_LOGW(TAG, "Unknown crossover option '%s' for '%s'", option.c_str(), serial_or_id.c_str());
    return;
  }
  this->set_monitor_crossover_by_serial(serial_or_id, freq_hz);
}

// Level and delay follow the crossover pattern above, with one deliberate difference: no
// CMD_PREPARE_CONFIG frame precedes them. GLM emits 0x17 before a *block* of DSP settings but
// routinely omits it for an individual write - in captures/glm_group_switch_capture.log
// monitor 0x02 takes a delay and a level at 18:31:56.899 with its nearest 0x17 ninety seconds
// away - so a bare frame lands, and spending a bus turn on a frame whose meaning is still
// unresolved (see commands.h) buys nothing.

void GenSAMHub::set_monitor_level(uint8_t address, float db) {
  // A hand-picked trim deviates from the active group, the same as an input override.
  this->set_group_modified(true);

  const float level = clamp_level_db(db);

  GenSAMMonitor *mon = registry_.find(address);
  if (mon == nullptr) {
    ESP_LOGW(TAG, "Cannot set level for unknown monitor 0x%02X", address);
    return;
  }

  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot send level command to 0x%02X: bus not available for TX", address);
    return;
  }

  if (!this->send_frame_twice(make_level(address, level))) {
    ESP_LOGW(TAG, "Level command to 0x%02X was not transmitted", address);
    return;
  }

  registry_.set_level(*mon, level);

  ESP_LOGI(TAG, "Set monitor 0x%02X level trim: %.1f dB", address, level);
}

void GenSAMHub::set_monitor_level_by_serial(const std::string &serial_or_id, float db) {
  const float level = clamp_level_db(db);

  // Store on the binding first, so the setting survives a monitor that is not (yet) on the bus.
  GenSAMMonitorBinding *binding = registry_.find_binding_by_serial_or_id(serial_or_id);
  if (binding != nullptr) {
    registry_.set_binding_level(*binding, level);
  }

  GenSAMMonitor *mon = registry_.find_by_serial_or_id(serial_or_id);
  if (mon == nullptr) {
    this->set_group_modified(true);
    ESP_LOGW(TAG, "Level for '%s' set to %.1f dB (stored; monitor not currently discovered on bus)",
             serial_or_id.c_str(), level);
    return;
  }
  this->set_monitor_level(mon->address, level);
}

void GenSAMHub::set_monitor_delay(uint8_t address, uint32_t samples) {
  this->set_group_modified(true);

  const uint32_t delay = std::min(samples, MAX_DELAY_SAMPLES);

  GenSAMMonitor *mon = registry_.find(address);
  if (mon == nullptr) {
    ESP_LOGW(TAG, "Cannot set delay for unknown monitor 0x%02X", address);
    return;
  }

  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot send delay command to 0x%02X: bus not available for TX", address);
    return;
  }

  if (!this->send_frame_twice(make_delay(address, delay))) {
    ESP_LOGW(TAG, "Delay command to 0x%02X was not transmitted", address);
    return;
  }

  registry_.set_delay(*mon, delay);

  ESP_LOGI(TAG, "Set monitor 0x%02X delay: %u samples (%.2f ms)", address, (unsigned) delay,
           delay_samples_to_ms(delay));
}

void GenSAMHub::set_monitor_delay_by_serial(const std::string &serial_or_id, uint32_t samples) {
  const uint32_t delay = std::min(samples, MAX_DELAY_SAMPLES);

  GenSAMMonitorBinding *binding = registry_.find_binding_by_serial_or_id(serial_or_id);
  if (binding != nullptr) {
    registry_.set_binding_delay(*binding, delay);
  }

  GenSAMMonitor *mon = registry_.find_by_serial_or_id(serial_or_id);
  if (mon == nullptr) {
    this->set_group_modified(true);
    ESP_LOGW(TAG, "Delay for '%s' set to %u samples (stored; monitor not currently discovered on bus)",
             serial_or_id.c_str(), (unsigned) delay);
    return;
  }
  this->set_monitor_delay(mon->address, delay);
}

void GenSAMHub::send_audio_source_frame(uint8_t address, uint8_t source, uint8_t channel, bool is_subwoofer) {
  if (!can_transmit()) {
    return;
  }
  // Primary frame (Input 0)
  this->send_frame(make_audio_source(address, 0x00, source, channel));

  // Subwoofers (7xxx series) require a secondary frame for Input 1, which is the LFE feed.
  // Zero here rather than the device's LFE channel: this path is a hand-picked input change
  // with no group behind it, so there is nothing to say what the LFE routing should be, and
  // zero is what GLM sends for a subwoofer without one. A group push sets it properly.
  if (is_subwoofer) {
    delay(5);
    this->send_frame(make_audio_source(address, 0x01, source, 0x00));
  }
}

void GenSAMHub::silence_system_volume_() {
  this->send_frame(make_broadcast_volume_silence());
}

void GenSAMHub::restore_system_volume_() {
  this->send_frame(make_broadcast_volume_db(current_volume_db_));
}

void GenSAMHub::with_transient_silence_(const std::function<void()> &switch_inputs) {
  // Silencing is only worth doing (and only possible) when the monitors are awake and
  // reachable; otherwise there is no listening signal to protect and no bus to protect it on.
  bool active = (!current_standby_ && can_transmit());

  if (active) {
    this->silence_system_volume_();
    delay(VOLUME_RAMP_DOWN_MS);
  }

  switch_inputs();

  if (active) {
    delay(INPUT_SETTLE_MS);
    this->restore_system_volume_();
  }
}

void GenSAMHub::set_group_modified(bool modified) {
  if (group_modified_ == modified) {
    return;
  }
  group_modified_ = modified;
  if (group_modified_sensor_ != nullptr) {
    group_modified_sensor_->publish_state(modified);
  }
}

// TODO: This blocks the main loop for ~130 ms (silence ramp-down, PLL/SRC relock, volume
//       restore). Acceptable for a one-off manual change; a group push, which touches every
//       speaker, uses the non-blocking state machine in race.cpp instead.
void GenSAMHub::set_monitor_input(uint8_t address, uint8_t source, uint8_t channel) {
  GenSAMMonitor *mon = registry_.find(address);
  if (mon != nullptr) {
    registry_.set_input(*mon, source, channel);
  }

  // A hand-picked input is a deviation from whatever the active group specified. The group is
  // deliberately left alone, so the next push - a group switch, a standby cycle, a
  // rediscovery - puts its own routing back; this flag is how that shows until then.
  this->set_group_modified(true);

  const char *name = input_to_str(source, channel);
  if (!can_transmit()) {
    ESP_LOGW(TAG, "Monitor 0x%02X input set to %s (stored; bus not ready for TX)", address, name);
    return;
  }

  const bool is_sub = (mon != nullptr) && mon->is_subwoofer();
  ESP_LOGI(TAG, "Setting monitor 0x%02X input to %s", address, name);
  this->with_transient_silence_([this, address, source, channel, is_sub]() {
    this->send_audio_source_frame(address, source, channel, is_sub);
  });
}

void GenSAMHub::set_monitor_input_by_serial(const std::string &serial_or_id, uint8_t source,
                                            uint8_t channel) {
  // Store on the binding first, so the setting survives a monitor that is not (yet) on the bus.
  GenSAMMonitorBinding *binding = registry_.find_binding_by_serial_or_id(serial_or_id);
  if (binding != nullptr) {
    registry_.set_binding_input(*binding, source, channel);
  }

  GenSAMMonitor *mon = registry_.find_by_serial_or_id(serial_or_id);
  if (mon == nullptr) {
    this->set_group_modified(true);
    ESP_LOGW(TAG, "Input for '%s' set to %s (stored; monitor not currently discovered on bus)",
             serial_or_id.c_str(), input_to_str(source, channel));
    return;
  }
  this->set_monitor_input(mon->address, source, channel);
}

void GenSAMHub::set_monitor_input_by_name(const std::string &serial_or_id,
                                          const std::string &input_name) {
  uint8_t source = SOURCE_ANALOG;
  uint8_t channel = AES3_CHANNEL_A;
  if (!str_to_input(input_name.c_str(), source, channel)) {
    ESP_LOGW(TAG, "Unknown input option '%s' for '%s'", input_name.c_str(), serial_or_id.c_str());
    return;
  }
  this->set_monitor_input_by_serial(serial_or_id, source, channel);
}

void GenSAMHub::set_standby(bool standby) {
  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot send standby command: Bus is not available for TX");
    return;
  }

  current_standby_ = standby;
  last_standby_command_ = millis();
  ESP_LOGI(TAG, "Set system power: %s", standby ? "STANDBY" : "WAKEUP/ON");
  for (auto &kv : registry_.monitors()) {
    kv.second.standby = standby;
  }
  this->notify_state_callbacks_();
  this->update_bus_status_();

  if (standby) {
    this->send_standby();
    this->evaluate_system_mute_();
  } else {
    // When waking from standby, monitors power on in an unaddressed state because their volatile
    // RACE addresses are reset during DSP reboot. Automatically initiate RACE rediscovery.
    this->rediscover_monitors();
  }
}

void GenSAMHub::identify_monitor_by_serial(const std::string &serial_or_id, uint32_t duration_ms) {
  GenSAMMonitor *mon = registry_.find_by_serial_or_id(serial_or_id);
  if (mon == nullptr) {
    ESP_LOGW(TAG, "Cannot identify speaker '%s': monitor not currently discovered on bus", serial_or_id.c_str());
    return;
  }
  this->identify_monitor_by_address(mon->address, duration_ms);
}

void GenSAMHub::identify_monitor_by_address(uint8_t address, uint32_t duration_ms) {
  GenSAMMonitor *mon = registry_.find(address);
  if (mon == nullptr) {
    ESP_LOGW(TAG, "Cannot identify monitor 0x%02X: not in registry", address);
    return;
  }
  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot identify monitor: bus not available for TX");
    return;
  }
  mon->identify_end_ms = millis() + duration_ms;
  Frame f = make_bypass(address, mon->mute, /*pulsing=*/true);
  this->send_frame_twice(f);
  ESP_LOGI(TAG, "Identify activated on monitor 0x%02X (%s) for %u ms (val 0x%02X)",
           address, mon->model.c_str(), (unsigned)duration_ms, f.payload[0]);
}

void GenSAMHub::check_identify_timeouts_() {
  uint32_t now = millis();
  for (auto &kv : registry_.monitors()) {
    GenSAMMonitor &mon = kv.second;
    if (mon.identify_end_ms == 0 || now < mon.identify_end_ms) {
      continue;
    }
    mon.identify_end_ms = 0;
    if (!can_transmit()) {
      continue;
    }
    Frame f = make_bypass(mon.address, mon.mute);
    this->send_frame_twice(f);
    ESP_LOGI(TAG, "Identify completed on monitor 0x%02X (%s); restored steady LED (val 0x%02X)",
             mon.address, mon.model.c_str(), f.payload[0]);
  }
}

void GenSAMHub::log_stats_() {
  uint32_t now = millis();
  if (now - last_stat_log_ <= STAT_LOG_INTERVAL_MS) {
    return;
  }
  last_stat_log_ = now;
  if (uart9_.rx_char_count() == 0) {
    return;  // Nothing has been received yet; stay quiet rather than log an empty tally
  }
  RxDecodeSnapshot rx = uart9_.rx_stats();
  ESP_LOGD(TAG,
           "Stats: %lu chars (%lu addr, %lu data), %lu bursts | rx: %lu start rej, %lu stop2, "
           "%lu framing errs | tx: %lu echo cancelled, %lu slow rel (max %lu us) | "
           "frames: %lu ok, %lu invalid, %lu crc errs, %lu C0 alias%s [Monitors: %u]",
           (unsigned long)rx.chars, (unsigned long)rx.addr_chars, (unsigned long)rx.data_chars,
           (unsigned long)uart9_.rx_burst_count(), (unsigned long)rx.start_rejects,
           (unsigned long)rx.stopbit2_rescues, (unsigned long)rx.framing_errs,
           (unsigned long)rx.echo_skip_cancelled, (unsigned long)uart9_.tx_overshoot_count(),
           (unsigned long)uart9_.tx_overshoot_max_us(),
           (unsigned long)parser_.valid_count(), (unsigned long)parser_.invalid_count(),
           (unsigned long)parser_.crc_mismatch_count(), (unsigned long)parser_.c0_alias_count(),
           arbiter_.is_active() ? " [GLM ACTIVE - YIELDING]" : "",
           (unsigned)registry_.size());
}

void GenSAMHub::loop() {
  this->process_rx_();
  this->check_glm_cooldown_();
  this->update_race_state_machine_();
  this->check_monitor_timeouts_();
  this->check_identify_timeouts_();
  this->log_stats_();
}

}  // namespace gensam
}  // namespace esphome
