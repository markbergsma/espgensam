/// @file gensam_hub.cpp
/// @brief ESPHome hub component for native Genelec SAM RS-485 communication.
/// See gensam_hub.h for architectural design rationale and complete API documentation.

#include "gensam_hub.h"
#include "crc.h"
#include "esphome/core/log.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "driver/gpio.h"

static const char *const TAG = "gensam";

namespace esphome {
namespace gensam {

void GenSAMHub::setup() {
  ESP_LOGI(TAG, "Initializing GenSAM Hub on RS485 bus...");
  boot_time_ = millis();

  if (tx_pin_ < 0 || rx_pin_ < 0) {
    ESP_LOGE(TAG, "Invalid TX (%d) or RX (%d) pin configuration", tx_pin_, rx_pin_);
    this->mark_failed();
    return;
  }

  // 1. Assert RS485 DC-DC Power Enable (T-CAN485: GPIO16 = 1 powers 5V boost converter)
  if (power_pin_ >= 0) {
    gpio_config_t pwr_cfg = {};
    pwr_cfg.pin_bit_mask = (1ULL << power_pin_);
    pwr_cfg.mode = GPIO_MODE_OUTPUT;
    pwr_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    pwr_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pwr_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&pwr_cfg);
    gpio_set_level(static_cast<gpio_num_t>(power_pin_), 1);  // High = 5V ON
    ESP_LOGI(TAG, "RS485 5V Power Enable pin (GPIO%d) asserted HIGH (5V Booster ON)", power_pin_);
    delay(50);  // Allow 5V DC-DC booster to stabilize
  }

  // 2. Assert Transceiver Enable (T-CAN485: GPIO19 = 1 turns ON NPN shifter to enable MAX13487)
  if (se_pin_ >= 0) {
    gpio_config_t se_cfg = {};
    se_cfg.pin_bit_mask = (1ULL << se_pin_);
    se_cfg.mode = GPIO_MODE_OUTPUT;
    se_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    se_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    se_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&se_cfg);
    gpio_set_level(static_cast<gpio_num_t>(se_pin_), 1);  // High = NPN ON -> Transceiver ENABLED
    ESP_LOGI(TAG, "RS485 Transceiver Enable pin (GPIO%d) asserted HIGH", se_pin_);
  }

  // 3. Assert Receiver Enable / AutoDirection (T-CAN485: GPIO17 = 1 turns ON NPN shifter -> Receiver & AutoDirection ON)
  if (re_pin_ >= 0) {
    gpio_config_t re_cfg = {};
    re_cfg.pin_bit_mask = (1ULL << re_pin_);
    re_cfg.mode = GPIO_MODE_OUTPUT;
    re_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    re_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    re_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&re_cfg);
    gpio_set_level(static_cast<gpio_num_t>(re_pin_), 1);  // High = NPN ON -> RX & AutoDirection ON
    ESP_LOGI(TAG, "RS485 Receiver Enable pin (GPIO%d) asserted HIGH", re_pin_);
  }

  // 4. Initialize 9-bit driver (RMT RX + RMT TX continuous zero-gap bitstream)
  if (listen_only_) {
    // In listen-only mode, drive TX pin HIGH (DE deasserted on RS-485 transceiver)
    // and do not initialize the RMT TX transmitter channel.
    gpio_config_t tx_cfg = {};
    tx_cfg.pin_bit_mask = (1ULL << tx_pin_);
    tx_cfg.mode = GPIO_MODE_OUTPUT;
    tx_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    tx_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    tx_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&tx_cfg);
    gpio_set_level(static_cast<gpio_num_t>(tx_pin_), 1);
    uart9_.setup(1, -1, rx_pin_, de_pin_, baud_rate_, rx_buffer_size_);
  } else {
    uart9_.setup(1, tx_pin_, rx_pin_, de_pin_, baud_rate_, rx_buffer_size_);
  }
  if (!uart9_.is_initialized()) {
    ESP_LOGE(TAG, "Failed to initialize 9-bit UART/RMT driver");
    this->mark_failed();
    return;
  }

  if (glm_usb_adapter_active_sensor_ != nullptr) {
    glm_usb_adapter_active_sensor_->publish_state(false);
  }
  current_volume_db_ = startup_volume_db_;

  ESP_LOGI(TAG, "GenSAM Hub initialized successfully (baud=%lu, TX=%s, RX=GPIO%d, yield_to_glm=%s, cooldown=%u ms)",
           (unsigned long)baud_rate_, listen_only_ ? "DISABLED (listen_only)" : ("GPIO" + std::to_string(tx_pin_)).c_str(),
           rx_pin_, YESNO(yield_to_glm_), (unsigned)glm_inactivity_cooldown_ms_);
}

void GenSAMHub::dump_config() {
  ESP_LOGCONFIG(TAG, "GenSAM Hub:");
  ESP_LOGCONFIG(TAG, "  TX Pin: GPIO%d", tx_pin_);
  ESP_LOGCONFIG(TAG, "  RX Pin: GPIO%d", rx_pin_);
  ESP_LOGCONFIG(TAG, "  Volume Bounds: [%.1f dB, %.1f dB] (Startup: %.1f dB)",
                min_volume_db_, max_volume_db_, startup_volume_db_);
  ESP_LOGCONFIG(TAG, "  Configured Monitor Bindings: %u", (unsigned)bindings_.size());
  for (const auto &b : bindings_) {
    ESP_LOGCONFIG(TAG, "    - Name: '%s' (SN: '%s')", b.name.c_str(), b.serial_number.c_str());
  }
  if (de_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  Hardware DE (Direction) Pin: GPIO%d", de_pin_);
  }
  if (re_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  Receiver Enable Pin: GPIO%d", re_pin_);
  }
  if (power_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  5V Power Pin: GPIO%d", power_pin_);
  }
  if (se_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  Transceiver Pin: GPIO%d", se_pin_);
  }
  ESP_LOGCONFIG(TAG, "  Baud Rate: %lu bps (fixed GLM standard)", (unsigned long)baud_rate_);
  ESP_LOGCONFIG(TAG, "  RX Buffer Size: %u characters", (unsigned)rx_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Listen Only: %s", YESNO(listen_only_));
  ESP_LOGCONFIG(TAG, "  Yield to GLM: %s", YESNO(yield_to_glm_));
  ESP_LOGCONFIG(TAG, "  GLM Inactivity Cooldown: %u ms", (unsigned)glm_inactivity_cooldown_ms_);
  ESP_LOGCONFIG(TAG, "  Discovered Monitors: %u", (unsigned)monitors_.size());
  for (const auto &kv : monitors_) {
    ESP_LOGCONFIG(TAG, "    - %s", kv.second.to_string().c_str());
  }
}

bool GenSAMHub::can_transmit() const {
  if (listen_only_) {
    return false;
  }
  if (yield_to_glm_ && glm_active_) {
    return false;
  }
  return uart9_.is_initialized();
}

void GenSAMHub::send_raw_frame(const std::vector<Uart9BitChar> &raw_chars) {
  if (!can_transmit() || raw_chars.empty()) {
    return;
  }
  last_our_tx_time_ = millis();
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

  if (yield_to_glm_ && glm_active_) {
    uint32_t now = millis();
    if (now - last_tx_blocked_warning_ > 5000) {
      last_tx_blocked_warning_ = now;
      uint32_t elapsed = now - last_glm_activity_;
      uint32_t remaining = (elapsed < glm_inactivity_cooldown_ms_) ? (glm_inactivity_cooldown_ms_ - elapsed) : 0;
      ESP_LOGW(TAG, "Cannot send frame: External GLM master/adapter is active on bus (cooldown: %u s remaining)",
               (unsigned)(remaining / 1000));
    }
    return false;
  }

  std::vector<Uart9BitChar> wire_chars = frame.to_9bit();
  ESP_LOGD(TAG, "TX -> %s", frame.to_string().c_str());

  last_our_tx_time_ = millis();
  uart9_.write(wire_chars.data(), wire_chars.size());
  parser_.clear();
  return true;
}

GenSAMMonitor *GenSAMHub::get_monitor(uint8_t address) {
  auto it = monitors_.find(address);
  if (it != monitors_.end()) {
    return &it->second;
  }
  return nullptr;
}

const GenSAMMonitor *GenSAMHub::get_monitor(uint8_t address) const {
  auto it = monitors_.find(address);
  if (it != monitors_.end()) {
    return &it->second;
  }
  return nullptr;
}

void GenSAMHub::send_wakeup() {
  if (!can_transmit()) {
    return;
  }
  ESP_LOGI(TAG, "Sending GLM wakeup broadcast sequence to monitors...");

  for (int i = 0; i < 3; i++) {
    Frame w1;
    w1.address = BROADCAST_ADDRESS;
    w1.command = CMD_WAKEUP;  // 0x3A
    w1.payload = {0x03, 0x7F};
    this->send_frame(w1);
    delay(5);

    Frame w2;
    w2.address = BROADCAST_ADDRESS;
    w2.command = CMD_WAKEUP;  // 0x3A
    w2.payload = {0x03, 0x01};
    this->send_frame(w2);
    delay(10);
  }
}

void GenSAMHub::start_race_discovery() {
  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot start RACE discovery: Hub is not in transmitting state");
    return;
  }

  if (race_state_ == RaceState::RACE_PING_SENT || race_state_ == RaceState::RACE_SET_RID_SENT) {
    return;
  }

  initial_discovery_done_ = true;
  ESP_LOGI(TAG, "Starting GLM RACE monitor discovery...");
  race_state_ = RaceState::RACE_PING_SENT;
  next_assign_addr_ = MONITOR_START_ADDR;
  current_racing_bytes_.clear();
  current_racing_id_ = 0;
  rid_retries_ = 0;
  race_step_time_ = millis();

  // Broadcast initial RACE discovery ping (0xFF 0xFE)
  Frame ping;
  ping.address = BROADCAST_ADDRESS;
  ping.command = CMD_DISCOVERY;
  this->send_frame(ping);
}

void GenSAMHub::complete_rid_assignment_(uint8_t address) {
  uint32_t now = millis();
  GenSAMMonitor &mon = monitors_[address];
  mon.address = address;
  mon.unique_id = current_racing_id_;
  this->mark_monitor_seen_(mon);

  ESP_LOGI(TAG, "Assigned monitor at address 0x%02X (ID: %u)",
           address, (unsigned)mon.unique_id);
  this->bind_monitor_if_matched_(mon);

  next_assign_addr_++;
  current_racing_bytes_.clear();
  current_racing_id_ = 0;
  rid_retries_ = 0;

  // Allow 300us turnaround before next ping
  delayMicroseconds(300);

  // Send next RACE ping
  Frame ping;
  ping.address = BROADCAST_ADDRESS;
  ping.command = CMD_DISCOVERY;
  this->send_frame(ping);
  race_state_ = RaceState::RACE_PING_SENT;
  race_step_time_ = now;
}

void GenSAMHub::handle_incoming_frame_(const Frame &frame) {
  uint32_t now = millis();

  // 1. ACTIVE MASTER STATE MACHINE
  if (!glm_active_ && !listen_only_) {
    if (race_state_ == RaceState::RACE_PING_SENT) {
      // Expecting winning unassigned monitor response with 3-byte serial
      if (frame.payload.size() == 3) {
        current_racing_bytes_ = frame.payload;
        current_racing_id_ = (static_cast<uint32_t>(frame.payload[0]) << 16) |
                             (static_cast<uint32_t>(frame.payload[1]) << 8) |
                             static_cast<uint32_t>(frame.payload[2]);
        race_state_ = RaceState::RACE_SET_RID_SENT;
        race_step_time_ = now;
        rid_retries_ = 0;

        // Allow 300us transceiver turnaround before transmitting CMD_SET_RID
        delayMicroseconds(300);

        // Assign address to this monitor via CMD_SET_RID to multicast (0xF0)
        Frame set_rid;
        set_rid.address = MULTICAST_ADDRESS;
        set_rid.command = CMD_SET_RID;
        set_rid.payload = current_racing_bytes_;
        set_rid.payload.push_back(next_assign_addr_);
        this->send_frame(set_rid);
        return;
      }
    } else if (race_state_ == RaceState::RACE_SET_RID_SENT) {
      // Expecting ACK confirming address assignment:
      // Frame addressed to HOST_ADDRESS (0x01) with CMD_REPORT_STATUS (0x09) or CMD_ACK (0x01),
      // containing exactly 1 payload byte matching the assigned address.
      if (frame.address == HOST_ADDRESS &&
          (frame.command == CMD_REPORT_STATUS || frame.command == CMD_ACK) &&
          frame.payload.size() == 1 && frame.payload[0] == next_assign_addr_) {
        this->complete_rid_assignment_(next_assign_addr_);
        return;
      }
    } else if (race_state_ == RaceState::QUERYING_DEVICES) {
      if (current_query_addr_ != 0 && frame.address == HOST_ADDRESS && !frame.payload.empty() &&
          (frame.command == CMD_REPORT_STATUS || frame.command == CMD_HARDWARE_QUERY ||
           frame.command == CMD_SOFTWARE_QUERY || frame.command == CMD_BAR_CODE)) {
        GenSAMMonitor &mon = monitors_[current_query_addr_];
        if (current_query_cmd_ == CMD_BAR_CODE) {
          parse_barcode(frame.payload.data(), frame.payload.size(), mon);
          ESP_LOGI(TAG, "Discovered serial for 0x%02X: %s", current_query_addr_, mon.serial_number.c_str());
        } else {
          parse_device_info(frame.payload.data(), frame.payload.size(), mon);
          ESP_LOGI(TAG, "Discovered: %s", mon.to_string().c_str());
        }
        this->mark_monitor_seen_(mon);
        bind_monitor_if_matched_(mon);
        publish_monitor_metadata_(mon);

        // Advance to next query (info -> barcode -> next monitor)
        if (current_query_cmd_ == CMD_SOFTWARE_QUERY) {
          current_query_cmd_ = CMD_BAR_CODE;
        } else {
          current_query_cmd_ = CMD_SOFTWARE_QUERY;
          current_poll_index_++;
        }
        current_query_addr_ = 0;
        race_step_time_ = now;
        return;
      }
    } else if (race_state_ == RaceState::POLLING_MONITORS) {
      if (current_query_addr_ != 0 && frame.address == HOST_ADDRESS &&
          (frame.command == CMD_REPORT_STATUS || frame.command == CMD_QUERY_STATUS)) {
        GenSAMMonitor &mon = monitors_[current_query_addr_];
        parse_telemetry(frame.payload.data(), frame.payload.size(), mon);
        this->mark_monitor_seen_(mon);
        bind_monitor_if_matched_(mon);
        publish_monitor_telemetry_(mon);
        ESP_LOGI(TAG, "[0x%02X %s] Telemetry: Temp=%d°C In=%d dBFS Out=%d dBFS",
                 current_query_addr_, mon.model.c_str(),
                 (int)mon.temperature, (int)mon.input_db, (int)mon.output_db);
        current_query_addr_ = 0;
        last_poll_step_time_ = now;
        return;
      }
    }
  }

  // 2. PASSIVE SNOOPING (Listen-Only or External GLM Master Active)
  // Track commands sent on bus
  if (frame.address >= MONITOR_START_ADDR && frame.address < 0x80) {
    last_queried_addr_ = frame.address;
    last_queried_cmd_ = frame.command;
  } else if ((frame.address == MULTICAST_ADDRESS || frame.address == 0xF0) &&
             frame.command == CMD_SET_RID && frame.payload.size() == 4) {
    uint8_t addr = frame.payload[3];
    GenSAMMonitor &mon = monitors_[addr];
    mon.address = addr;
    mon.unique_id = (static_cast<uint32_t>(frame.payload[0]) << 16) |
                    (static_cast<uint32_t>(frame.payload[1]) << 8) |
                    static_cast<uint32_t>(frame.payload[2]);
    this->mark_monitor_seen_(mon);
    ESP_LOGI(TAG, "[Sniffed] Assigned monitor 0x%02X (ID: %u)", addr, (unsigned)mon.unique_id);
    bind_monitor_if_matched_(mon);
  }

  // Sniff volume broadcast (GLM broadcasts master volume to 0xFF; 0xF0 carries auxiliary pot data)
  if (frame.address == BROADCAST_ADDRESS && frame.command == CMD_VOLUME && frame.payload.size() >= 3) {
    uint32_t int24 = decode_int24(frame.payload.data());
    current_volume_db_ = volume_int24_to_db(int24);
    ESP_LOGI(TAG, "[Sniffed] System volume updated to %.1f dB", current_volume_db_);
    this->notify_state_callbacks_();
  }

  // Sniff wakeup / standby broadcast or multicast commands
  if ((frame.address == MULTICAST_ADDRESS || frame.address == BROADCAST_ADDRESS) &&
      frame.command == CMD_WAKEUP && frame.payload.size() >= 2) {
    if (frame.payload[0] == WAKEUP_OP_POWER) {
      current_standby_ = (frame.payload[1] == WAKEUP_VAL_STANDBY);
      ESP_LOGI(TAG, "[Sniffed] System power state updated to %s", current_standby_ ? "STANDBY" : "ON");
      this->notify_state_callbacks_();
    }
  }

  // Sniff bypass / mute commands (broadcast, multicast, or unicast to individual monitors)
  if (frame.command == CMD_BYPASS && !frame.payload.empty()) {
    bool is_muted = (frame.payload[0] & BYPASS_MUTE_MASK) != 0;

    auto it = monitors_.find(frame.address);
    if (it != monitors_.end()) {
      it->second.mute = is_muted;
      if (it->second.binding != nullptr && it->second.binding->mute_switch != nullptr) {
        it->second.binding->mute_switch->publish_state(is_muted);
      }
      ESP_LOGD(TAG, "[Sniffed] Monitor 0x%02X mute updated to %s (raw 0x%02X)", frame.address, YESNO(is_muted), frame.payload[0]);
    } else if (frame.address >= MONITOR_START_ADDR && frame.address < 0x80) {
      GenSAMMonitor &mon = monitors_[frame.address];
      mon.address = frame.address;
      mon.mute = is_muted;
      this->mark_monitor_seen_(mon);
      bind_monitor_if_matched_(mon);
      if (mon.binding != nullptr && mon.binding->mute_switch != nullptr) {
        mon.binding->mute_switch->publish_state(is_muted);
      }
      ESP_LOGD(TAG, "[Sniffed] Discovered monitor 0x%02X mute set to %s (raw 0x%02X)", frame.address, YESNO(is_muted), frame.payload[0]);
    } else if (frame.address == MULTICAST_ADDRESS || frame.address == BROADCAST_ADDRESS) {
      for (auto &kv : monitors_) {
        kv.second.mute = is_muted;
        if (kv.second.binding != nullptr && kv.second.binding->mute_switch != nullptr) {
          kv.second.binding->mute_switch->publish_state(is_muted);
        }
      }
    }

    this->evaluate_system_mute_();
  }

  // Track replies sent to host
  if (frame.address == HOST_ADDRESS && frame.command == CMD_REPORT_STATUS) {
    if (last_queried_cmd_ == CMD_SOFTWARE_QUERY && last_queried_addr_ != 0) {
      GenSAMMonitor &mon = monitors_[last_queried_addr_];
      mon.address = last_queried_addr_;
      parse_device_info(frame.payload.data(), frame.payload.size(), mon);
      this->mark_monitor_seen_(mon);
      bind_monitor_if_matched_(mon);
      ESP_LOGI(TAG, "[Sniffed] Discovered: %s", mon.to_string().c_str());
      last_queried_cmd_ = 0;
    } else if (last_queried_cmd_ == CMD_BAR_CODE && last_queried_addr_ != 0) {
      GenSAMMonitor &mon = monitors_[last_queried_addr_];
      mon.address = last_queried_addr_;
      parse_barcode(frame.payload.data(), frame.payload.size(), mon);
      this->mark_monitor_seen_(mon);
      bind_monitor_if_matched_(mon);
      ESP_LOGI(TAG, "[Sniffed] Serial for 0x%02X: %s", last_queried_addr_, mon.serial_number.c_str());
      last_queried_cmd_ = 0;
    } else if (last_queried_cmd_ == CMD_QUERY_STATUS && last_queried_addr_ != 0) {
      GenSAMMonitor &mon = monitors_[last_queried_addr_];
      mon.address = last_queried_addr_;
      parse_telemetry(frame.payload.data(), frame.payload.size(), mon);
      this->mark_monitor_seen_(mon);
      bind_monitor_if_matched_(mon);
      publish_monitor_telemetry_(mon);
      last_queried_cmd_ = 0;
    }
  }
}

void GenSAMHub::update_race_state_machine_() {
  if (!can_transmit()) {
    return;
  }

  uint32_t now = millis();

  // Wait 10 seconds after boot for WiFi association to settle before active bus probing
  if (now - boot_time_ < 10000) {
    return;
  }

  // Trigger initial wakeup + discovery cycle
  if (!initial_discovery_done_) {
    this->send_wakeup();
    race_state_ = RaceState::WAKEUP_SENT;
    race_step_time_ = now;
    initial_discovery_done_ = true;
    return;
  }

  switch (race_state_) {
    case RaceState::WAKEUP_SENT: {
      // Wait 300ms after wakeup before beginning RACE
      if (now - race_step_time_ >= 300) {
        this->start_race_discovery();
      }
      break;
    }

    case RaceState::RACE_PING_SENT: {
      // Timeout waiting for unassigned monitors -> RACE discovery complete
      if (now - race_step_time_ > 350) {
        if (monitors_.empty()) {
          ESP_LOGI(TAG, "RACE discovery complete: No monitors responded (will retry in 10s)");
          race_state_ = RaceState::IDLE;
          last_discovery_retry_time_ = now;
        } else {
          ESP_LOGI(TAG, "RACE discovery complete. Total monitors found: %u", (unsigned)monitors_.size());

          // Transition all monitors from discovery to online mode
          Frame stay_online;
          stay_online.address = BROADCAST_ADDRESS;
          stay_online.command = CMD_STAY_ONLINE;
          this->send_frame(stay_online);

          // Populate poll_addrs_ with discovered monitor addresses
          poll_addrs_.clear();
          poll_addrs_.reserve(monitors_.size());
          for (const auto &kv : monitors_) {
            poll_addrs_.push_back(kv.first);
          }

          race_state_ = RaceState::QUERYING_DEVICES;
          race_step_time_ = now;
          current_poll_index_ = 0;
          current_query_addr_ = 0;
          current_query_cmd_ = CMD_SOFTWARE_QUERY;
        }
      }
      break;
    }

    case RaceState::RACE_SET_RID_SENT: {
      // Timeout waiting for RID ACK -> retry CMD_SET_RID up to 3 times
      if (now - race_step_time_ > 300) {
        rid_retries_++;
        if (rid_retries_ <= 3) {
          ESP_LOGW(TAG, "Timeout waiting for RID ACK for address 0x%02X (attempt %u/3). Retrying CMD_SET_RID...",
                   next_assign_addr_, (unsigned)rid_retries_);
          // Re-send CMD_SET_RID
          Frame set_rid;
          set_rid.address = MULTICAST_ADDRESS;
          set_rid.command = CMD_SET_RID;
          set_rid.payload = current_racing_bytes_;
          set_rid.payload.push_back(next_assign_addr_);
          this->send_frame(set_rid);
          race_step_time_ = now;
        } else {
          // Retries exhausted. Monitor may have adopted the address despite lost ACK.
          // Register monitor and probe device during query phase rather than abandoning it.
          ESP_LOGW(TAG, "Retries exhausted for RID ACK at address 0x%02X. Registering monitor and resuming discovery...",
                   next_assign_addr_);
          this->complete_rid_assignment_(next_assign_addr_);
        }
      }
      break;
    }

    case RaceState::QUERYING_DEVICES: {
      if (current_query_addr_ != 0 && now - race_step_time_ > 250) {
        // Query timed out; advance to next query (info -> barcode -> next monitor)
        if (current_query_cmd_ == CMD_SOFTWARE_QUERY) {
          current_query_cmd_ = CMD_BAR_CODE;
        } else {
          current_query_cmd_ = CMD_SOFTWARE_QUERY;
          current_poll_index_++;
        }
        current_query_addr_ = 0;
      }

      if (current_query_addr_ == 0) {
        if (current_poll_index_ < poll_addrs_.size()) {
          current_query_addr_ = poll_addrs_[current_poll_index_];
          race_step_time_ = now;
          Frame q;
          q.address = current_query_addr_;
          q.command = current_query_cmd_;
          if (current_query_cmd_ == CMD_BAR_CODE) {
            q.payload = {0x01};
          }
          this->send_frame(q);
          return;
        }

        // All monitors queried! Advance to polling
        ESP_LOGI(TAG, "All discovered monitors queried. Entering live telemetry polling loop.");

        // Broadcast active volume and stay_online heartbeat to establish monitor gain
        uint32_t int24 = volume_db_to_int24(current_volume_db_);
        uint8_t pld[3];
        encode_int24(int24, pld);
        Frame vol_frame;
        vol_frame.address = BROADCAST_ADDRESS;
        vol_frame.command = CMD_VOLUME;
        vol_frame.payload = {pld[0], pld[1], pld[2]};
        this->send_frame(vol_frame);
        delayMicroseconds(250);

        Frame stay_online;
        stay_online.address = BROADCAST_ADDRESS;
        stay_online.command = CMD_STAY_ONLINE;
        this->send_frame(stay_online);

        race_state_ = RaceState::POLLING_MONITORS;
        last_poll_cycle_time_ = now - poll_interval_ms_;  // Start polling immediately
        last_poll_step_time_ = now - 20;
        current_poll_index_ = 0;
        current_query_addr_ = 0;
        current_query_cmd_ = 0;
      }
      break;
    }

    case RaceState::POLLING_MONITORS: {
      if (monitors_.empty()) {
        race_state_ = RaceState::IDLE;
        return;
      }

      if (current_query_addr_ != 0 && now - race_step_time_ > 300) {
        current_query_addr_ = 0;
        last_poll_step_time_ = now;
      }

      if (current_query_addr_ == 0 && (now - last_poll_step_time_ >= 20) &&
          (now - last_poll_cycle_time_ >= poll_interval_ms_)) {
        // At the start of each polling cycle, broadcast active volume and stay_online heartbeat
        if (current_poll_index_ == 0) {
          uint32_t int24 = volume_db_to_int24(current_volume_db_);
          uint8_t pld[3];
          encode_int24(int24, pld);
          Frame vf;
          vf.address = BROADCAST_ADDRESS;
          vf.command = CMD_VOLUME;
          vf.payload = {pld[0], pld[1], pld[2]};
          this->send_frame(vf);
          delayMicroseconds(250);

          Frame stay_online;
          stay_online.address = BROADCAST_ADDRESS;
          stay_online.command = CMD_STAY_ONLINE;
          this->send_frame(stay_online);
          delayMicroseconds(300);
        }

        // Refresh address cache only if monitor registry size changed
        if (poll_addrs_.size() != monitors_.size()) {
          poll_addrs_.clear();
          poll_addrs_.reserve(monitors_.size());
          for (const auto &kv : monitors_) {
            poll_addrs_.push_back(kv.first);
          }
        }

        if (current_poll_index_ < poll_addrs_.size()) {
          current_query_addr_ = poll_addrs_[current_poll_index_++];
          race_step_time_ = now;
          last_poll_step_time_ = now;
          Frame poll_frame;
          poll_frame.address = current_query_addr_;
          poll_frame.command = CMD_QUERY_STATUS;
          this->send_frame(poll_frame);
        } else {
          current_poll_index_ = 0;
          last_poll_cycle_time_ = now;
          last_poll_step_time_ = now;
        }
      }
      break;
    }

    case RaceState::IDLE: {
      // Periodically retry discovery if no monitors are registered
      if (monitors_.empty() && now - last_discovery_retry_time_ >= 10000) {
        last_discovery_retry_time_ = now;
        this->start_race_discovery();
      }
      break;
    }

    default:
      break;
  }
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

    // Bus arbitration: Detect any external GLM master/adapter activity
    bool external_master_frame = false;
    if (glm_active_) {
      // While yielded, any observed traffic on the bus originates from the external GLM ecosystem
      external_master_frame = true;
    } else if (frame.address != HOST_ADDRESS) {
      if (now - last_our_tx_time_ > 50) {
        external_master_frame = true;
      }
    } else {
      if (last_our_tx_time_ == 0 || now - last_our_tx_time_ > 300) {
        external_master_frame = true;
      }
    }

    if (external_master_frame) {
      last_glm_activity_ = now;
      if (!glm_active_) {
        glm_active_ = true;
        if (glm_usb_adapter_active_sensor_ != nullptr) {
          glm_usb_adapter_active_sensor_->publish_state(true);
        }
        ESP_LOGW(TAG, "External GLM master/adapter detected on bus (%s). Yielding bus control (listen-only mode)...",
                 frame.to_string().c_str());
      }
    } else if (!glm_active_ && !listen_only_ && frame.address != HOST_ADDRESS) {
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
  if (!glm_active_) {
    return;
  }

  uint32_t now = millis();
  if (now - last_glm_activity_ >= glm_inactivity_cooldown_ms_) {
    glm_active_ = false;
    if (glm_usb_adapter_active_sensor_ != nullptr) {
      glm_usb_adapter_active_sensor_->publish_state(false);
    }
    ESP_LOGI(TAG, "No GLM master/adapter traffic observed for %u seconds. Resuming active bus control.",
             (unsigned)(glm_inactivity_cooldown_ms_ / 1000));
    // Trigger fresh wakeup and discovery when resuming active master control
    this->send_wakeup();
    race_state_ = RaceState::WAKEUP_SENT;
    race_step_time_ = now;
  }
}

void GenSAMHub::bind_monitor_if_matched_(GenSAMMonitor &mon) {
  if (mon.binding != nullptr) {
    return;
  }
  for (auto &b : bindings_) {
    if (mon.matches(b)) {
      mon.binding = &b;
      if ((mon.serial_number.empty() || mon.serial_number == "(none)") && !b.serial_number.empty()) {
        mon.serial_number = b.serial_number;
      }
      ESP_LOGI(TAG, "Bound monitor 0x%02X (%s, SN:%s) to HA entity '%s'",
               mon.address, mon.model.empty() ? "(querying)" : mon.model.c_str(),
               mon.serial_number.c_str(), b.name.c_str());
      publish_monitor_metadata_(mon);
      if (mon.binding->online_sensor != nullptr) {
        mon.binding->online_sensor->publish_state(mon.online);
      }
      break;
    }
  }
}

void GenSAMHub::publish_monitor_metadata_(const GenSAMMonitor &mon) {
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

void GenSAMHub::publish_monitor_telemetry_(const GenSAMMonitor &mon) {
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
  if (mon.binding->online_sensor != nullptr) {
    mon.binding->online_sensor->publish_state(mon.online);
  }
}

void GenSAMHub::mark_monitor_seen_(GenSAMMonitor &mon) {
  uint32_t now = millis();
  mon.last_seen_ms = now;
  if (!mon.online) {
    mon.online = true;
    if (mon.binding != nullptr && mon.binding->online_sensor != nullptr) {
      mon.binding->online_sensor->publish_state(true);
    }
    ESP_LOGI(TAG, "[0x%02X %s] Monitor is online",
             mon.address, mon.model.empty() ? "(querying)" : mon.model.c_str());
    this->evaluate_system_mute_();
  }
}

void GenSAMHub::check_monitor_timeouts_() {
  uint32_t now = millis();
  if (now - last_timeout_check_ < 500) {
    return;
  }
  last_timeout_check_ = now;

  uint32_t stale_timeout_ms = std::max<uint32_t>(5000, poll_interval_ms_ * 4);
  if (listen_only_ || glm_active_) {
    stale_timeout_ms = std::max<uint32_t>(stale_timeout_ms, 15000);
  }

  bool state_changed = false;
  for (auto &kv : monitors_) {
    GenSAMMonitor &mon = kv.second;
    if (mon.online && (now - mon.last_seen_ms > stale_timeout_ms)) {
      mon.online = false;
      if (mon.binding != nullptr && mon.binding->online_sensor != nullptr) {
        mon.binding->online_sensor->publish_state(false);
      }
      ESP_LOGW(TAG, "[0x%02X %s] Monitor went offline (no response for %u ms)",
               mon.address, mon.model.empty() ? "(unknown)" : mon.model.c_str(),
               (unsigned)(now - mon.last_seen_ms));
      state_changed = true;
    }
  }

  if (state_changed) {
    this->evaluate_system_mute_();
  }
}

void GenSAMHub::evaluate_system_mute_() {
  if (monitors_.empty()) {
    return;
  }
  bool any_online = false;
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

  bool new_mute = any_online ? all_muted : false;
  if (current_mute_ != new_mute) {
    current_mute_ = new_mute;
    ESP_LOGI(TAG, "System mute updated to %s", YESNO(current_mute_));
    this->notify_state_callbacks_();
  }
}

void GenSAMHub::set_volume_db(float db) {
  float target_db = std::clamp(db, min_volume_db_, max_volume_db_);
  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot send volume command: Bus is not available for TX");
    return;
  }
  uint32_t int24 = volume_db_to_int24(target_db);
  uint8_t pld[3];
  encode_int24(int24, pld);

  // Broadcast master volume (0xFF) to all monitors on bus
  Frame f;
  f.address = BROADCAST_ADDRESS;
  f.command = CMD_VOLUME;
  f.payload = {pld[0], pld[1], pld[2]};
  if (!this->send_frame(f)) {
    ESP_LOGW(TAG, "Volume command was not transmitted; keeping previous state");
    return;
  }

  current_volume_db_ = target_db;

  ESP_LOGI(TAG, "Set system volume: %.1f dB (int24=%u)", current_volume_db_, (unsigned)int24);
  this->notify_state_callbacks_();
}

void GenSAMHub::set_group_mute(bool mute) {
  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot send mute command: Bus is not available for TX");
    return;
  }
  uint8_t val = mute ? (BYPASS_MUTE_MASK | (LED_RED << 1)) : (LED_OFF << 1);
  bool transmitted = false;

  // 1. Unicast CMD_BYPASS to each discovered monitor individually (as per GLM protocol)
  for (const auto &kv : monitors_) {
    Frame f;
    f.address = kv.first;
    f.command = CMD_BYPASS;
    f.payload = {val};
    bool sent = this->send_frame(f);
    transmitted = transmitted || sent;
    if (sent) {
      delayMicroseconds(250);
    }
  }

  // 2. Also send to BROADCAST_ADDRESS (0xFF)
  Frame fb;
  fb.address = BROADCAST_ADDRESS;
  fb.command = CMD_BYPASS;
  fb.payload = {val};
  transmitted = this->send_frame(fb) || transmitted;

  if (!transmitted) {
    ESP_LOGW(TAG, "Mute command was not transmitted; keeping previous state");
    return;
  }

  current_mute_ = mute;
  for (auto &kv : monitors_) {
    kv.second.mute = mute;
    if (kv.second.binding != nullptr && kv.second.binding->mute_switch != nullptr) {
      kv.second.binding->mute_switch->publish_state(mute);
    }
  }

  ESP_LOGI(TAG, "Set system mute: %s across %zu monitors", YESNO(mute), monitors_.size());
  this->notify_state_callbacks_();
}

void GenSAMHub::set_monitor_mute(uint8_t address, bool mute) {
  auto it = monitors_.find(address);
  if (it == monitors_.end()) {
    ESP_LOGW(TAG, "Cannot mute monitor 0x%02X: not found in registry", address);
    return;
  }

  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot send mute command to 0x%02X: bus not available for TX", address);
    return;
  }

  uint8_t val = mute ? (BYPASS_MUTE_MASK | (LED_RED << 1)) : (LED_OFF << 1);
  Frame f;
  f.address = address;
  f.command = CMD_BYPASS;
  f.payload = {val};
  bool sent = this->send_frame(f);
  if (sent) {
    delayMicroseconds(250);
  }
  sent = this->send_frame(f) || sent;

  if (!sent) {
    ESP_LOGW(TAG, "Mute command to 0x%02X was not transmitted; keeping previous state", address);
    return;
  }

  it->second.mute = mute;
  if (it->second.binding != nullptr && it->second.binding->mute_switch != nullptr) {
    it->second.binding->mute_switch->publish_state(mute);
  }

  this->evaluate_system_mute_();

  ESP_LOGI(TAG, "Set monitor 0x%02X mute: %s", address, YESNO(mute));
}

void GenSAMHub::set_monitor_mute_by_serial(const std::string &serial_or_id, bool mute) {
  for (auto &kv : monitors_) {
    GenSAMMonitor &mon = kv.second;
    bool matches = (strcasecmp(mon.serial_number.c_str(), serial_or_id.c_str()) == 0 ||
                    std::to_string(mon.unique_id) == serial_or_id);
    if (!matches && mon.binding != nullptr) {
      matches = (strcasecmp(mon.binding->serial_number.c_str(), serial_or_id.c_str()) == 0 ||
                 std::to_string(mon.binding->unique_id) == serial_or_id ||
                 strcasecmp(mon.binding->name.c_str(), serial_or_id.c_str()) == 0);
    }
    if (matches) {
      this->set_monitor_mute(mon.address, mute);
      return;
    }
  }
  ESP_LOGW(TAG, "Cannot mute speaker '%s': monitor not currently discovered on bus", serial_or_id.c_str());
}

void GenSAMHub::set_standby(bool standby) {
  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot send standby command: Bus is not available for TX");
    return;
  }
  Frame f;
  f.address = BROADCAST_ADDRESS;
  f.command = CMD_WAKEUP;
  f.payload = {WAKEUP_OP_POWER, standby ? WAKEUP_VAL_STANDBY : WAKEUP_VAL_ON};
  bool sent = this->send_frame(f);
  if (sent) {
    delay(15);
  }
  sent = this->send_frame(f) || sent;
  if (!sent) {
    ESP_LOGW(TAG, "Standby command was not transmitted; keeping previous state");
    return;
  }

  current_standby_ = standby;
  ESP_LOGI(TAG, "Set system power: %s", standby ? "STANDBY" : "WAKEUP/ON");
  this->notify_state_callbacks_();
}

void GenSAMHub::identify_monitor_by_serial(const std::string &serial_or_id, uint32_t duration_ms) {
  for (auto &kv : monitors_) {
    GenSAMMonitor &mon = kv.second;
    bool matches = (strcasecmp(mon.serial_number.c_str(), serial_or_id.c_str()) == 0 ||
                    std::to_string(mon.unique_id) == serial_or_id);
    if (!matches && mon.binding != nullptr) {
      matches = (strcasecmp(mon.binding->serial_number.c_str(), serial_or_id.c_str()) == 0 ||
                 std::to_string(mon.binding->unique_id) == serial_or_id ||
                 strcasecmp(mon.binding->name.c_str(), serial_or_id.c_str()) == 0);
    }
    if (matches) {
      this->identify_monitor_by_address(mon.address, duration_ms);
      return;
    }
  }
  ESP_LOGW(TAG, "Cannot identify speaker '%s': monitor not currently discovered on bus", serial_or_id.c_str());
}

void GenSAMHub::identify_monitor_by_address(uint8_t address, uint32_t duration_ms) {
  auto it = monitors_.find(address);
  if (it == monitors_.end()) {
    ESP_LOGW(TAG, "Cannot identify monitor 0x%02X: not in registry", address);
    return;
  }
  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot identify monitor: bus not available for TX");
    return;
  }
  it->second.identify_end_ms = millis() + duration_ms;
  uint8_t val = (it->second.mute ? BYPASS_MUTE_MASK : 0x00) | (LED_OFF << 1) | BYPASS_LED_PULSING_MASK;
  Frame f;
  f.address = address;
  f.command = CMD_BYPASS;
  f.payload = {val};
  this->send_frame(f);
  delayMicroseconds(250);
  this->send_frame(f);
  ESP_LOGI(TAG, "Identify activated on monitor 0x%02X (%s) for %u ms (val 0x%02X)",
           address, it->second.model.c_str(), (unsigned)duration_ms, val);
}

void GenSAMHub::rediscover_monitors() {
  ESP_LOGI(TAG, "Manual rediscovery requested. Clearing %u cached monitors and restarting discovery...",
           (unsigned)monitors_.size());

  // Mark all currently known monitors offline before clearing cache so HA state does not remain stale.
  for (auto &kv : monitors_) {
    kv.second.online = false;
    kv.second.last_seen_ms = 0;
    if (kv.second.binding != nullptr && kv.second.binding->online_sensor != nullptr) {
      kv.second.binding->online_sensor->publish_state(false);
    }
  }
  this->evaluate_system_mute_();

  monitors_.clear();
  poll_addrs_.clear();
  current_poll_index_ = 0;
  current_query_addr_ = 0;
  current_query_cmd_ = 0;
  current_racing_bytes_.clear();
  current_racing_id_ = 0;
  rid_retries_ = 0;
  last_queried_addr_ = 0;
  last_queried_cmd_ = 0;
  next_assign_addr_ = MONITOR_START_ADDR;
  race_state_ = RaceState::IDLE;

  this->start_race_discovery();
}

void GenSAMHub::loop() {
  process_rx_();

  check_glm_cooldown_();
  update_race_state_machine_();
  check_monitor_timeouts_();

  // Check if any monitor identify pulse timer has expired
  uint32_t now = millis();
  for (auto &kv : monitors_) {
    if (kv.second.identify_end_ms != 0 && now >= kv.second.identify_end_ms) {
      kv.second.identify_end_ms = 0;
      if (can_transmit()) {
        uint8_t val = kv.second.mute ? (BYPASS_MUTE_MASK | (LED_RED << 1)) : (LED_OFF << 1);
        Frame f;
        f.address = kv.first;
        f.command = CMD_BYPASS;
        f.payload = {val};
        this->send_frame(f);
        delayMicroseconds(250);
        this->send_frame(f);
        ESP_LOGI(TAG, "Identify completed on monitor 0x%02X (%s); restored steady LED (val 0x%02X)",
                 kv.first, kv.second.model.c_str(), val);
      }
    }
  }

  uint32_t now_stat = millis();
  if (now_stat - last_stat_log_ > 15000) {
    last_stat_log_ = now_stat;
    if (uart9_.rx_char_count() > 0) {
      ESP_LOGD(TAG, "Stats: %lu chars (%lu addr, %lu data), %lu bursts, %lu framing errs, %lu invalid, %lu crc errs%s [Monitors: %u]",
               (unsigned long)uart9_.rx_char_count(), (unsigned long)uart9_.rx_addr_count(),
               (unsigned long)uart9_.rx_data_count(), (unsigned long)uart9_.rx_burst_count(),
               (unsigned long)uart9_.rx_framing_err_count(),
               (unsigned long)parser_.invalid_count(), (unsigned long)parser_.crc_mismatch_count(),
               glm_active_ ? " [GLM ACTIVE - YIELDING]" : "",
               (unsigned)monitors_.size());
    }
  }
}

}  // namespace gensam
}  // namespace esphome
