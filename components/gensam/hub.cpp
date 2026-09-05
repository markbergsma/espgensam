/// @file hub.cpp
/// @brief ESPHome hub component for native Genelec SAM RS-485 communication.
/// See hub.h for architectural design rationale and complete API documentation.

#include "hub.h"
#include "commands.h"
#include "crc.h"
#include "esphome/core/log.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
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
  if (volume_number_ != nullptr) {
    volume_number_->publish_state(current_volume_db_);
  }

  for (auto &b : registry_.bindings()) {
    if (b.aes3_channel_select != nullptr) {
      b.aes3_channel_select->publish_state(aes3_channel_to_str(b.aes3_channel));
    }
  }

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
  if (volume_number_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Volume dB Number Entity: configured");
  }
  if (audio_source_select_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Audio Source Select Entity: configured");
  }
  ESP_LOGCONFIG(TAG, "  Configured Monitor Bindings: %u", (unsigned)registry_.bindings().size());
  for (const auto &b : registry_.bindings()) {
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
  ESP_LOGCONFIG(TAG, "  Discovered Monitors: %u", (unsigned)registry_.size());
  for (const auto &kv : registry_.monitors()) {
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
  this->send_frame(make_discovery_ping());
}

void GenSAMHub::complete_rid_assignment_(uint8_t address) {
  uint32_t now = millis();
  GenSAMMonitor &mon = registry_.get_or_create(address);
  mon.unique_id = current_racing_id_;
  this->mark_monitor_seen_(mon);

  ESP_LOGI(TAG, "Assigned monitor at address 0x%02X (ID: %u)",
           address, (unsigned)mon.unique_id);
  registry_.bind_if_matched(mon);

  next_assign_addr_++;
  current_racing_bytes_.clear();
  current_racing_id_ = 0;
  rid_retries_ = 0;

  // Allow 300us turnaround before next ping
  delayMicroseconds(300);

  // Send next RACE ping
  this->send_frame(make_discovery_ping());
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
        this->send_frame(make_set_rid(current_racing_bytes_, next_assign_addr_));
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
        GenSAMMonitor &mon = registry_.get_or_create(current_query_addr_);
        if (current_query_cmd_ == CMD_BAR_CODE) {
          parse_barcode(frame.payload.data(), frame.payload.size(), mon);
          ESP_LOGI(TAG, "Discovered serial for 0x%02X: %s", current_query_addr_, mon.serial_number.c_str());
        } else {
          parse_device_info(frame.payload.data(), frame.payload.size(), mon);
          ESP_LOGI(TAG, "Discovered: %s", mon.to_string().c_str());
        }
        this->mark_monitor_seen_(mon);
        registry_.bind_if_matched(mon);
        registry_.publish_metadata(mon);

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
    } else if (race_state_ == RaceState::CONFIGURING_DEVICES) {
      if (current_query_addr_ != 0 && frame.address == HOST_ADDRESS &&
          (frame.command == CMD_REPORT_STATUS || frame.command == CMD_ACK)) {
        ESP_LOGD(TAG, "Monitor 0x%02X acknowledged configuration", current_query_addr_);
        current_poll_index_++;
        current_query_addr_ = 0;
        race_step_time_ = now;
        return;
      }
    } else if (race_state_ == RaceState::POLLING_MONITORS) {
      if (current_query_addr_ != 0 && frame.address == HOST_ADDRESS &&
          (frame.command == CMD_REPORT_STATUS || frame.command == CMD_QUERY_STATUS)) {
        GenSAMMonitor &mon = registry_.get_or_create(current_query_addr_);
        parse_telemetry(frame.payload.data(), frame.payload.size(), mon);
        this->mark_monitor_seen_(mon);
        registry_.bind_if_matched(mon);
        registry_.publish_telemetry(mon);
        if (frame.payload.size() == 1 && frame.payload[0] == STATUS_STANDBY) {
          ESP_LOGD(TAG, "[0x%02X %s] Telemetry: Monitor in standby (0x%02X)", current_query_addr_, mon.model.c_str(), STATUS_STANDBY);
        } else {
          ESP_LOGI(TAG, "[0x%02X %s] Telemetry: Temp=%d°C In=%d dBFS Out=%d dBFS",
                   current_query_addr_, mon.model.c_str(),
                   (int)mon.temperature, (int)mon.input_db, (int)mon.output_db);
        }
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
  } else if (frame.address == MULTICAST_ADDRESS && frame.command == CMD_SET_RID &&
             frame.payload.size() == 4) {
    uint8_t addr = frame.payload[3];
    GenSAMMonitor &mon = registry_.get_or_create(addr);
    mon.unique_id = (static_cast<uint32_t>(frame.payload[0]) << 16) |
                    (static_cast<uint32_t>(frame.payload[1]) << 8) |
                    static_cast<uint32_t>(frame.payload[2]);
    this->mark_monitor_seen_(mon);
    ESP_LOGI(TAG, "[Sniffed] Assigned monitor 0x%02X (ID: %u)", addr, (unsigned)mon.unique_id);
    registry_.bind_if_matched(mon);
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
      bool is_standby = (frame.payload[1] == WAKEUP_VAL_STANDBY_1 || frame.payload[1] == WAKEUP_VAL_STANDBY_2);
      bool is_on = (frame.payload[1] == WAKEUP_VAL_ON_1 || frame.payload[1] == WAKEUP_VAL_ON_2);
      if (is_standby && !current_standby_) {
        current_standby_ = true;
        ESP_LOGI(TAG, "[Sniffed] System power state updated to STANDBY (val 0x%02X)", frame.payload[1]);
        this->notify_state_callbacks_();
      } else if (is_on && current_standby_) {
        current_standby_ = false;
        ESP_LOGI(TAG, "[Sniffed] System power state updated to ON (val 0x%02X)", frame.payload[1]);
        this->notify_state_callbacks_();
      }
    }
  }

  // Sniff bypass / mute commands (broadcast, multicast, or unicast to individual monitors)
  if (frame.command == CMD_BYPASS && !frame.payload.empty()) {
    bool is_muted = (frame.payload[0] & BYPASS_MUTE_MASK) != 0;

    GenSAMMonitor *known = registry_.find(frame.address);
    if (known != nullptr) {
      registry_.set_mute(*known, is_muted);
      ESP_LOGD(TAG, "[Sniffed] Monitor 0x%02X mute updated to %s (raw 0x%02X)", frame.address, YESNO(is_muted), frame.payload[0]);
    } else if (frame.address >= MONITOR_START_ADDR && frame.address < 0x80) {
      GenSAMMonitor &mon = registry_.get_or_create(frame.address);
      // Record mute before marking seen, so the system mute re-evaluation triggered by the
      // offline->online transition already accounts for this monitor's new state.
      mon.mute = is_muted;
      this->mark_monitor_seen_(mon);
      registry_.bind_if_matched(mon);
      registry_.publish_mute(mon);
      ESP_LOGD(TAG, "[Sniffed] Discovered monitor 0x%02X mute set to %s (raw 0x%02X)", frame.address, YESNO(is_muted), frame.payload[0]);
    } else if (frame.address == MULTICAST_ADDRESS || frame.address == BROADCAST_ADDRESS) {
      for (auto &kv : registry_.monitors()) {
        registry_.set_mute(kv.second, is_muted);
      }
    }

    this->evaluate_system_mute_();
  }

  // Sniff bass management crossover commands (broadcast, multicast, or unicast to individual monitors)
  if (frame.command == CMD_BASS_MANAGE_XO && frame.payload.size() >= 2) {
    uint16_t freq = (static_cast<uint16_t>(frame.payload[0]) << 8) | frame.payload[1];
    if (frame.address == MULTICAST_ADDRESS || frame.address == BROADCAST_ADDRESS) {
      for (auto &kv : registry_.monitors()) {
        registry_.set_crossover(kv.second, freq);
      }
      ESP_LOGI(TAG, "[Sniffed] Global bass management crossover frequency set to %u Hz", freq);
    } else {
      GenSAMMonitor *known = registry_.find(frame.address);
      if (known != nullptr) {
        registry_.set_crossover(*known, freq);
        ESP_LOGI(TAG, "[Sniffed] Monitor 0x%02X bass management crossover frequency set to %u Hz", frame.address, freq);
      } else if (frame.address >= MONITOR_START_ADDR && frame.address < 0x80) {
        GenSAMMonitor &mon = registry_.get_or_create(frame.address);
        this->mark_monitor_seen_(mon);
        registry_.bind_if_matched(mon);
        registry_.set_crossover(mon, freq);
        ESP_LOGI(TAG, "[Sniffed] Discovered monitor 0x%02X bass management crossover frequency set to %u Hz", frame.address, freq);
      }
    }
  }

  // Sniff audio source selection and AES3 channel configuration
  if (frame.command == CMD_SELECT_AUDIO_SOURCE && frame.payload.size() >= 4) {
    uint8_t input_idx = frame.payload[0];
    uint8_t src = frame.payload[1];
    uint8_t ch = frame.payload[3];

    if (input_idx == 0x00) {
      if (src == SOURCE_ANALOG || src == SOURCE_DIGITAL_AES3) {
        if (!audio_source_configured_ || current_audio_source_ != src) {
          current_audio_source_ = src;
          audio_source_configured_ = true;
          if (audio_source_select_ != nullptr) {
            audio_source_select_->publish_state(
                (src == SOURCE_ANALOG) ? SOURCE_STR_ANALOG : SOURCE_STR_DIGITAL_AES3);
          }
          ESP_LOGI(TAG, "[Sniffed] System audio source set to %s",
                   (src == SOURCE_ANALOG) ? SOURCE_STR_ANALOG : SOURCE_STR_DIGITAL_AES3);
        }
      }
    }

    if (src == SOURCE_DIGITAL_AES3 && (ch >= AES3_CHANNEL_A && ch <= AES3_CHANNEL_SUM) && input_idx == 0x00) {
      GenSAMMonitor *known = registry_.find(frame.address);
      if (known != nullptr) {
        registry_.set_aes3_channel(*known, ch);
        ESP_LOGI(TAG, "[Sniffed] Monitor 0x%02X AES3 channel set to 0x%02X", frame.address, ch);
      } else if (frame.address >= MONITOR_START_ADDR && frame.address < 0x80) {
        GenSAMMonitor &mon = registry_.get_or_create(frame.address);
        this->mark_monitor_seen_(mon);
        registry_.bind_if_matched(mon);
        registry_.set_aes3_channel(mon, ch);
        ESP_LOGI(TAG, "[Sniffed] Discovered monitor 0x%02X AES3 channel set to 0x%02X", frame.address, ch);
      }
    }
  }

  // Track replies sent to host
  if (frame.address == HOST_ADDRESS && frame.command == CMD_REPORT_STATUS) {
    if (last_queried_cmd_ == CMD_SOFTWARE_QUERY && last_queried_addr_ != 0) {
      GenSAMMonitor &mon = registry_.get_or_create(last_queried_addr_);
      parse_device_info(frame.payload.data(), frame.payload.size(), mon);
      this->mark_monitor_seen_(mon);
      registry_.bind_if_matched(mon);
      ESP_LOGI(TAG, "[Sniffed] Discovered: %s", mon.to_string().c_str());
      last_queried_cmd_ = 0;
    } else if (last_queried_cmd_ == CMD_BAR_CODE && last_queried_addr_ != 0) {
      GenSAMMonitor &mon = registry_.get_or_create(last_queried_addr_);
      parse_barcode(frame.payload.data(), frame.payload.size(), mon);
      this->mark_monitor_seen_(mon);
      registry_.bind_if_matched(mon);
      ESP_LOGI(TAG, "[Sniffed] Serial for 0x%02X: %s", last_queried_addr_, mon.serial_number.c_str());
      last_queried_cmd_ = 0;
    } else if (last_queried_cmd_ == CMD_QUERY_STATUS && last_queried_addr_ != 0) {
      GenSAMMonitor &mon = registry_.get_or_create(last_queried_addr_);
      parse_telemetry(frame.payload.data(), frame.payload.size(), mon);
      this->mark_monitor_seen_(mon);
      registry_.bind_if_matched(mon);
      registry_.publish_telemetry(mon);
      last_queried_cmd_ = 0;
    }
  }
}

void GenSAMHub::update_race_state_machine_() {
  if (!can_transmit() || current_standby_) {
    return;
  }

  uint32_t now = millis();

  // Wait 10 seconds after boot for WiFi association to settle before active bus probing
  if (now - boot_time_ < 10000) {
    return;
  }

  // Trigger initial wakeup + discovery cycle
  if (!initial_discovery_done_) {
    this->rediscover_monitors();
    return;
  }

  switch (race_state_) {
    case RaceState::WAKEUP_SENT: {
      // Wait 400ms after wakeup before beginning RACE (~400ms for monitor DSP boot)
      if (now - race_step_time_ >= 400) {
        this->start_race_discovery();
      }
      break;
    }

    case RaceState::RACE_PING_SENT: {
      // Timeout waiting for unassigned monitors -> RACE discovery complete
      if (now - race_step_time_ > 350) {
        if (registry_.empty()) {
          ESP_LOGI(TAG, "RACE discovery complete: No monitors responded (will retry in 10s)");
          race_state_ = RaceState::IDLE;
          last_discovery_retry_time_ = now;
        } else {
          ESP_LOGI(TAG, "RACE discovery complete. Total monitors found: %u", (unsigned)registry_.size());

          // Transition all monitors from discovery to online mode
          this->send_frame(make_stay_online());

          // Populate poll_addrs_ with discovered monitor addresses
          poll_addrs_ = registry_.addresses();

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
          this->send_frame(make_set_rid(current_racing_bytes_, next_assign_addr_));
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
          this->send_frame(make_query(current_query_addr_, current_query_cmd_));
          return;
        }

        // All monitors queried! Advance to device configuration
        ESP_LOGI(TAG, "All discovered monitors queried. Entering device configuration phase.");
        race_state_ = RaceState::CONFIGURING_DEVICES;
        race_step_time_ = now;
        current_poll_index_ = 0;
        current_query_addr_ = 0;
      }
      break;
    }

    case RaceState::CONFIGURING_DEVICES: {
      if (current_query_addr_ != 0 && now - race_step_time_ > 200) {
        // Configuration query timed out; advance to next monitor
        current_poll_index_++;
        current_query_addr_ = 0;
      }

      if (current_query_addr_ == 0) {
        while (current_poll_index_ < poll_addrs_.size()) {
          uint8_t addr = poll_addrs_[current_poll_index_];
          GenSAMMonitor *mon = registry_.find(addr);
          if (mon != nullptr) {
            bool configured_anything = false;

            // 1. Audio source and AES3 channel configuration
            // Note: Transient volume silencing (silence_system_volume_ / restore_system_volume_) is
            // intentionally omitted here.  Monitors are waking from amplifier standby with internal
            // amplifiers already muted, so there is no listening signal to protect from switching
            // transients.  Volume is restored later by the normal post-wakeup volume command.
            if (audio_source_configured_) {
              uint8_t ch = AES3_CHANNEL_A;
              if (mon->binding != nullptr) {
                ch = mon->binding->aes3_channel;
              } else if (mon->is_subwoofer()) {
                ch = AES3_CHANNEL_SUM;
              }
              ESP_LOGI(TAG, "Configuring audio source for monitor 0x%02X: %s (ch 0x%02X)",
                       addr, (current_audio_source_ == SOURCE_ANALOG) ? SOURCE_STR_ANALOG : SOURCE_STR_DIGITAL_AES3, ch);
              this->send_audio_source_frame(addr, current_audio_source_, ch, mon->is_subwoofer());
              configured_anything = true;
            }

            // 2. Bass management crossover frequency
            if (mon->binding != nullptr && mon->binding->crossover_number != nullptr &&
                mon->binding->crossover_configured) {
              if (configured_anything) {
                delay(10);
              }
              uint16_t freq = mon->binding->crossover_freq;
              ESP_LOGI(TAG, "Configuring bass management crossover frequency for monitor 0x%02X: %u Hz", addr, freq);
              this->send_frame(make_crossover(addr, freq));
              configured_anything = true;
            }

            if (configured_anything) {
              current_query_addr_ = addr;
              race_step_time_ = now;
              return;
            }
          }
          current_poll_index_++;
        }

        // All discovered monitors configured! Advance to polling
        ESP_LOGI(TAG, "All discovered monitors configured. Entering live telemetry polling loop.");

        // Broadcast active volume and stay_online heartbeat to establish monitor gain
        this->send_frame(make_broadcast_volume_db(current_volume_db_));
        delayMicroseconds(250);
        this->send_frame(make_stay_online());

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
      if (registry_.empty()) {
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
          this->send_frame(make_broadcast_volume_db(current_volume_db_));
          delayMicroseconds(250);

          if (!current_standby_) {
            this->send_frame(make_stay_online());
            delayMicroseconds(300);
          }
        }

        // Refresh address cache only if monitor registry size changed
        if (poll_addrs_.size() != registry_.size()) {
          poll_addrs_ = registry_.addresses();
        }

        if (current_poll_index_ < poll_addrs_.size()) {
          current_query_addr_ = poll_addrs_[current_poll_index_++];
          race_step_time_ = now;
          last_poll_step_time_ = now;
          this->send_frame(make_query(current_query_addr_, CMD_QUERY_STATUS));
        } else {
          current_poll_index_ = 0;
          last_poll_cycle_time_ = now;
          last_poll_step_time_ = now;
        }
      }
      break;
    }

    case RaceState::IDLE: {
      // Periodically retry discovery if no monitors are registered and not in standby
      if (!current_standby_ && registry_.empty() && now - last_discovery_retry_time_ >= 10000) {
        last_discovery_retry_time_ = now;
        this->rediscover_monitors();
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
    this->rediscover_monitors();
  }
}

void GenSAMHub::mark_monitor_seen_(GenSAMMonitor &mon) {
  if (registry_.mark_seen(mon)) {
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

  if (registry_.expire_stale(now, stale_timeout_ms)) {
    this->evaluate_system_mute_();
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

void GenSAMHub::notify_state_callbacks_() {
  if (volume_number_ != nullptr) {
    volume_number_->publish_state(current_volume_db_);
  }
  for (auto &cb : state_callbacks_) {
    cb(current_volume_db_, current_mute_, current_standby_);
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

  ESP_LOGI(TAG, "Set monitor 0x%02X crossover frequency: %u Hz", address, freq_hz);
}

void GenSAMHub::set_monitor_crossover_by_serial(const std::string &serial_or_id, uint16_t freq_hz) {
  // Store on the binding first, so the setting survives a monitor that is not (yet) on the bus.
  GenSAMMonitorBinding *binding = registry_.find_binding_by_serial_or_id(serial_or_id);
  if (binding != nullptr) {
    binding->crossover_freq = freq_hz;
    binding->crossover_configured = true;
  }

  GenSAMMonitor *mon = registry_.find_by_serial_or_id(serial_or_id);
  if (mon == nullptr) {
    ESP_LOGW(TAG, "Crossover set for '%s' to %u Hz (stored; monitor not currently discovered on bus)",
             serial_or_id.c_str(), freq_hz);
    return;
  }
  this->set_monitor_crossover(mon->address, freq_hz);
}

void GenSAMHub::send_audio_source_frame(uint8_t address, uint8_t source, uint8_t channel, bool is_subwoofer) {
  if (!can_transmit()) {
    return;
  }
  // Primary frame (Input 0)
  this->send_frame(make_audio_source(address, 0x00, source, channel));

  // Subwoofers (7xxx series) require a secondary frame for Input 1
  if (is_subwoofer) {
    delay(5);
    this->send_frame(make_audio_source(address, 0x01, source, channel));
  }
}

void GenSAMHub::silence_system_volume_() {
  this->send_frame(make_broadcast_volume_silence());
}

void GenSAMHub::restore_system_volume_() {
  this->send_frame(make_broadcast_volume_db(current_volume_db_));
}

// TODO: This function blocks the main loop for ~130 ms + 5 ms per monitor (silence ramp-down,
//       per-monitor source frames, PLL/SRC settling, volume restore).  Consider converting to a
//       non-blocking state machine if the number of monitors grows significantly.
void GenSAMHub::set_global_source(uint8_t source) {
  if (source != SOURCE_ANALOG && source != SOURCE_DIGITAL_AES3) {
    ESP_LOGW(TAG, "Unknown global audio source value: 0x%02X", source);
    return;
  }

  current_audio_source_ = source;
  audio_source_configured_ = true;

  const char *name = (source == SOURCE_ANALOG) ? SOURCE_STR_ANALOG : SOURCE_STR_DIGITAL_AES3;
  if (audio_source_select_ != nullptr) {
    audio_source_select_->publish_state(name);
  }

  if (!can_transmit()) {
    ESP_LOGW(TAG, "Audio source set to %s (bus not ready for TX)", name);
    return;
  }

  // Pre-switch transient silencing: fade down to minimum volume before switching inputs
  bool active = (!current_standby_ && can_transmit());
  if (active) {
    this->silence_system_volume_();
    delay(30);  // Slew time for monitors' internal DSP volume ramp down
  }

  ESP_LOGI(TAG, "Setting global audio source to %s across %u monitor(s)...", name, (unsigned)registry_.size());
  for (const auto &kv : registry_.monitors()) {
    const GenSAMMonitor &mon = kv.second;
    uint8_t ch = AES3_CHANNEL_A;
    if (mon.binding != nullptr) {
      ch = mon.binding->aes3_channel;
    } else if (mon.is_subwoofer()) {
      ch = AES3_CHANNEL_SUM;
    }
    this->send_audio_source_frame(mon.address, source, ch, mon.is_subwoofer());
    delay(5);
  }

  // Post-switch volume restoration: wait for AES3 PLL clock relock and input stages to settle
  if (active) {
    delay(100);  // Allow monitor input circuitry and PLL/SRC to stabilize
    this->restore_system_volume_();
  }
}

void GenSAMHub::set_global_source_by_name(const std::string &source_name) {
  if (source_name == SOURCE_STR_ANALOG) {
    this->set_global_source(SOURCE_ANALOG);
  } else if (source_name == SOURCE_STR_DIGITAL_AES3) {
    this->set_global_source(SOURCE_DIGITAL_AES3);
  } else {
    ESP_LOGW(TAG, "Unknown audio source option: '%s'", source_name.c_str());
  }
}

// TODO: When the system is actively playing digital audio, this function blocks ~130 ms
//       (silence ramp-down + PLL settling + volume restore).  Same consideration as set_global_source().
void GenSAMHub::set_monitor_aes3_channel(uint8_t address, uint8_t channel) {
  GenSAMMonitor *mon = registry_.find(address);
  if (mon != nullptr) {
    registry_.set_aes3_channel(*mon, channel);
  }

  if (audio_source_configured_ && current_audio_source_ == SOURCE_DIGITAL_AES3) {
    bool is_sub = (mon != nullptr) ? mon->is_subwoofer() : false;
    bool active = (!current_standby_ && can_transmit());
    if (active) {
      this->silence_system_volume_();
      delay(30);
    }
    this->send_audio_source_frame(address, SOURCE_DIGITAL_AES3, channel, is_sub);
    if (active) {
      delay(100);
      this->restore_system_volume_();
    }
  }

  ESP_LOGI(TAG, "Set monitor 0x%02X AES3 channel: 0x%02X", address, channel);
}

void GenSAMHub::set_monitor_aes3_channel_by_serial(const std::string &serial_or_id, uint8_t channel) {
  // Store on the binding first, so the setting survives a monitor that is not (yet) on the bus.
  GenSAMMonitorBinding *binding = registry_.find_binding_by_serial_or_id(serial_or_id);
  if (binding != nullptr) {
    registry_.set_binding_aes3_channel(*binding, channel);
  }

  GenSAMMonitor *mon = registry_.find_by_serial_or_id(serial_or_id);
  if (mon == nullptr) {
    ESP_LOGW(TAG, "AES3 channel set for '%s' to 0x%02X (stored; monitor not currently discovered on bus)",
             serial_or_id.c_str(), channel);
    return;
  }
  this->set_monitor_aes3_channel(mon->address, channel);
}

void GenSAMHub::set_monitor_aes3_channel_by_name(const std::string &serial_or_id, const std::string &channel_name) {
  uint8_t ch = AES3_CHANNEL_A;
  if (channel_name == AES3_CHANNEL_STR_B) {
    ch = AES3_CHANNEL_B;
  } else if (channel_name == AES3_CHANNEL_STR_SUM) {
    ch = AES3_CHANNEL_SUM;
  } else if (channel_name == AES3_CHANNEL_STR_A) {
    ch = AES3_CHANNEL_A;
  } else {
    ESP_LOGW(TAG, "Unknown AES3 channel option '%s' for '%s'", channel_name.c_str(), serial_or_id.c_str());
    return;
  }
  this->set_monitor_aes3_channel_by_serial(serial_or_id, ch);
}

void GenSAMHub::set_standby(bool standby) {
  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot send standby command: Bus is not available for TX");
    return;
  }

  current_standby_ = standby;
  ESP_LOGI(TAG, "Set system power: %s", standby ? "STANDBY" : "WAKEUP/ON");
  this->notify_state_callbacks_();

  if (standby) {
    this->send_standby();
    // In standby, stop polling loop and mark monitors offline
    registry_.mark_all_offline();
    this->evaluate_system_mute_();
    race_state_ = RaceState::IDLE;
  } else {
    // When waking from standby, monitors power on in unaddressed state because their volatile
    // RACE addresses are reset during <0.5W sleep. Automatically initiate RACE rediscovery.
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

void GenSAMHub::rediscover_monitors() {
  if (!can_transmit()) {
    ESP_LOGW(TAG, "Cannot start RACE discovery: Hub is not in transmitting state");
    return;
  }

  initial_discovery_done_ = true;
  current_standby_ = false;
  this->notify_state_callbacks_();

  ESP_LOGI(TAG, "Resetting monitor table and initiating RACE discovery...");

  // Mark all currently known monitors offline before clearing cache so HA state does not remain stale.
  registry_.invalidate_all();
  this->evaluate_system_mute_();

  registry_.clear();
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

  // Send wakeup pulse sequence to ensure sleeping monitors boot up
  this->send_wakeup();
  race_state_ = RaceState::WAKEUP_SENT;
  race_step_time_ = millis();
}

void GenSAMHub::loop() {
  process_rx_();

  check_glm_cooldown_();
  update_race_state_machine_();
  check_monitor_timeouts_();

  // Check if any monitor identify pulse timer has expired
  uint32_t now = millis();
  for (auto &kv : registry_.monitors()) {
    if (kv.second.identify_end_ms != 0 && now >= kv.second.identify_end_ms) {
      kv.second.identify_end_ms = 0;
      if (can_transmit()) {
        Frame f = make_bypass(kv.first, kv.second.mute);
        this->send_frame_twice(f);
        ESP_LOGI(TAG, "Identify completed on monitor 0x%02X (%s); restored steady LED (val 0x%02X)",
                 kv.first, kv.second.model.c_str(), f.payload[0]);
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
               (unsigned)registry_.size());
    }
  }
}

}  // namespace gensam
}  // namespace esphome
