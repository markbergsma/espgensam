/// @file race.cpp
/// @brief RACE monitor discovery, device interrogation, and telemetry polling state machine.
/// See hub.h for the RACE protocol rationale and complete API documentation.

#include "hub.h"
#include "commands.h"

#include "esphome/core/log.h"

static const char *const TAG = "gensam";

namespace esphome {
namespace gensam {

namespace {

/// Settling delay after boot before probing the bus, so WiFi association completes first.
constexpr uint32_t BOOT_SETTLE_MS = 10000;

/// Time monitors need to boot their DSP after a wakeup broadcast before they can race.
constexpr uint32_t WAKEUP_BOOT_MS = 400;

/// Silence after a discovery ping that means no unassigned monitor is left to answer.
constexpr uint32_t RACE_PING_TIMEOUT_MS = 350;

/// Time to wait for a monitor to acknowledge its assigned address before resending.
constexpr uint32_t RID_ACK_TIMEOUT_MS = 300;

/// Address assignment attempts before assuming the monitor took the address but lost the ACK.
constexpr uint8_t RID_MAX_RETRIES = 3;

/// Time to wait for a model / firmware / barcode reply before moving on.
constexpr uint32_t DEVICE_QUERY_TIMEOUT_MS = 250;

/// Time to wait for a configuration acknowledgement before moving to the next monitor.
constexpr uint32_t CONFIG_ACK_TIMEOUT_MS = 200;

/// Time to wait for a telemetry reply before abandoning that monitor's turn in the cycle.
constexpr uint32_t POLL_REPLY_TIMEOUT_MS = 300;

/// Minimum spacing between consecutive monitor polls within one cycle.
constexpr uint32_t POLL_STEP_INTERVAL_MS = 20;

/// How often to retry discovery while no monitors are registered.
constexpr uint32_t DISCOVERY_RETRY_INTERVAL_MS = 10000;

/// RS-485 transceiver direction turnaround allowance between back-to-back frames.
constexpr uint32_t TURNAROUND_US = 300;

/// Spacing between the volume broadcast and the keep-alive that follows it.
constexpr uint32_t VOLUME_KEEPALIVE_GAP_US = 250;

/// Delay between the two configuration frames sent to the same monitor.
constexpr uint32_t CONFIG_FRAME_GAP_MS = 10;

}  // namespace

// --- Discovery entry points --------------------------------------------------------

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

void GenSAMHub::complete_rid_assignment_(uint8_t address) {
  uint32_t now = millis();
  GenSAMMonitor &mon = registry_.get_or_create(address);
  mon.unique_id = current_racing_id_;
  this->mark_monitor_seen_(mon);

  ESP_LOGI(TAG, "Assigned monitor at address 0x%02X (ID: %u)", address, (unsigned)mon.unique_id);
  registry_.bind_if_matched(mon);

  next_assign_addr_++;
  current_racing_bytes_.clear();
  current_racing_id_ = 0;
  rid_retries_ = 0;

  delayMicroseconds(TURNAROUND_US);

  // Send next RACE ping
  this->send_frame(make_discovery_ping());
  race_state_ = RaceState::RACE_PING_SENT;
  race_step_time_ = now;
}

void GenSAMHub::advance_device_query_() {
  if (current_query_cmd_ == CMD_SOFTWARE_QUERY) {
    current_query_cmd_ = CMD_BAR_CODE;
  } else {
    current_query_cmd_ = CMD_SOFTWARE_QUERY;
    current_poll_index_++;
  }
  current_query_addr_ = 0;
}

void GenSAMHub::broadcast_volume_and_keepalive_() {
  this->send_frame(make_broadcast_volume_db(current_volume_db_));
  delayMicroseconds(VOLUME_KEEPALIVE_GAP_US);
  this->send_frame(make_stay_online());
}

// --- Incoming replies to what the state machine sent --------------------------------

bool GenSAMHub::handle_active_reply_(const Frame &frame, uint32_t now) {
  switch (race_state_) {
    case RaceState::RACE_PING_SENT: {
      // Expecting winning unassigned monitor response with 3-byte serial
      if (frame.payload.size() != 3) {
        return false;
      }
      current_racing_bytes_ = frame.payload;
      current_racing_id_ = (static_cast<uint32_t>(frame.payload[0]) << 16) |
                           (static_cast<uint32_t>(frame.payload[1]) << 8) |
                           static_cast<uint32_t>(frame.payload[2]);
      race_state_ = RaceState::RACE_SET_RID_SENT;
      race_step_time_ = now;
      rid_retries_ = 0;

      delayMicroseconds(TURNAROUND_US);

      // Assign address to this monitor via CMD_SET_RID to multicast (0xF0)
      this->send_frame(make_set_rid(current_racing_bytes_, next_assign_addr_));
      return true;
    }

    case RaceState::RACE_SET_RID_SENT: {
      // Expecting ACK confirming address assignment: a frame addressed to HOST_ADDRESS with
      // CMD_REPORT_STATUS or CMD_ACK, carrying exactly the address we just assigned.
      if (frame.address != HOST_ADDRESS ||
          (frame.command != CMD_REPORT_STATUS && frame.command != CMD_ACK) ||
          frame.payload.size() != 1 || frame.payload[0] != next_assign_addr_) {
        return false;
      }
      this->complete_rid_assignment_(next_assign_addr_);
      return true;
    }

    case RaceState::QUERYING_DEVICES: {
      if (current_query_addr_ == 0 || frame.address != HOST_ADDRESS || frame.payload.empty() ||
          (frame.command != CMD_REPORT_STATUS && frame.command != CMD_HARDWARE_QUERY &&
           frame.command != CMD_SOFTWARE_QUERY && frame.command != CMD_BAR_CODE)) {
        return false;
      }
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

      this->advance_device_query_();
      race_step_time_ = now;
      return true;
    }

    case RaceState::CONFIGURING_DEVICES: {
      if (current_query_addr_ == 0 || frame.address != HOST_ADDRESS ||
          (frame.command != CMD_REPORT_STATUS && frame.command != CMD_ACK)) {
        return false;
      }
      ESP_LOGD(TAG, "Monitor 0x%02X acknowledged configuration", current_query_addr_);
      current_poll_index_++;
      current_query_addr_ = 0;
      race_step_time_ = now;
      return true;
    }

    case RaceState::POLLING_MONITORS: {
      if (current_query_addr_ == 0 || frame.address != HOST_ADDRESS ||
          (frame.command != CMD_REPORT_STATUS && frame.command != CMD_QUERY_STATUS)) {
        return false;
      }
      GenSAMMonitor &mon = registry_.get_or_create(current_query_addr_);
      parse_telemetry(frame.payload.data(), frame.payload.size(), mon);
      this->mark_monitor_seen_(mon);
      registry_.bind_if_matched(mon);
      registry_.publish_telemetry(mon);
      if (frame.payload.size() == 1 && frame.payload[0] == STATUS_STANDBY) {
        ESP_LOGD(TAG, "[0x%02X %s] Telemetry: Monitor in standby (0x%02X)",
                 current_query_addr_, mon.model.c_str(), STATUS_STANDBY);
      } else {
        ESP_LOGI(TAG, "[0x%02X %s] Telemetry: Temp=%d°C In=%d dBFS Out=%d dBFS",
                 current_query_addr_, mon.model.c_str(),
                 (int)mon.temperature, (int)mon.input_db, (int)mon.output_db);
      }
      current_query_addr_ = 0;
      last_poll_step_time_ = now;
      return true;
    }

    default:
      return false;
  }
}

// --- State machine ------------------------------------------------------------------

void GenSAMHub::update_race_state_machine_() {
  if (!can_transmit() || current_standby_) {
    return;
  }

  uint32_t now = millis();

  // Wait for WiFi association to settle before active bus probing
  if (now - boot_time_ < BOOT_SETTLE_MS) {
    return;
  }

  // Trigger initial wakeup + discovery cycle
  if (!initial_discovery_done_) {
    this->rediscover_monitors();
    return;
  }

  switch (race_state_) {
    case RaceState::WAKEUP_SENT:
      this->race_step_wakeup_(now);
      break;
    case RaceState::RACE_PING_SENT:
      this->race_step_ping_(now);
      break;
    case RaceState::RACE_SET_RID_SENT:
      this->race_step_set_rid_(now);
      break;
    case RaceState::QUERYING_DEVICES:
      this->race_step_querying_(now);
      break;
    case RaceState::CONFIGURING_DEVICES:
      this->race_step_configuring_(now);
      break;
    case RaceState::POLLING_MONITORS:
      this->race_step_polling_(now);
      break;
    case RaceState::IDLE:
      this->race_step_idle_(now);
      break;
    default:
      break;
  }
}

void GenSAMHub::race_step_wakeup_(uint32_t now) {
  // Wait for the monitors' DSPs to boot before beginning RACE
  if (now - race_step_time_ >= WAKEUP_BOOT_MS) {
    this->start_race_discovery();
  }
}

void GenSAMHub::race_step_ping_(uint32_t now) {
  // Silence following the discovery ping means no unassigned monitor is left
  if (now - race_step_time_ <= RACE_PING_TIMEOUT_MS) {
    return;
  }

  if (registry_.empty()) {
    ESP_LOGI(TAG, "RACE discovery complete: No monitors responded (will retry in %us)",
             (unsigned)(DISCOVERY_RETRY_INTERVAL_MS / 1000));
    race_state_ = RaceState::IDLE;
    last_discovery_retry_time_ = now;
    return;
  }

  ESP_LOGI(TAG, "RACE discovery complete. Total monitors found: %u", (unsigned)registry_.size());

  // Transition all monitors from discovery to online mode
  this->send_frame(make_stay_online());

  poll_addrs_ = registry_.addresses();
  race_state_ = RaceState::QUERYING_DEVICES;
  race_step_time_ = now;
  current_poll_index_ = 0;
  current_query_addr_ = 0;
  current_query_cmd_ = CMD_SOFTWARE_QUERY;
}

void GenSAMHub::race_step_set_rid_(uint32_t now) {
  if (now - race_step_time_ <= RID_ACK_TIMEOUT_MS) {
    return;
  }

  rid_retries_++;
  if (rid_retries_ <= RID_MAX_RETRIES) {
    ESP_LOGW(TAG, "Timeout waiting for RID ACK for address 0x%02X (attempt %u/%u). Retrying CMD_SET_RID...",
             next_assign_addr_, (unsigned)rid_retries_, (unsigned)RID_MAX_RETRIES);
    this->send_frame(make_set_rid(current_racing_bytes_, next_assign_addr_));
    race_step_time_ = now;
    return;
  }

  // Retries exhausted. The monitor may have adopted the address despite the lost ACK, so
  // register it and probe it during the query phase rather than abandoning it.
  ESP_LOGW(TAG, "Retries exhausted for RID ACK at address 0x%02X. Registering monitor and resuming discovery...",
           next_assign_addr_);
  this->complete_rid_assignment_(next_assign_addr_);
}

void GenSAMHub::race_step_querying_(uint32_t now) {
  if (current_query_addr_ != 0 && now - race_step_time_ > DEVICE_QUERY_TIMEOUT_MS) {
    // Query timed out; advance to next query (info -> barcode -> next monitor)
    this->advance_device_query_();
  }

  if (current_query_addr_ != 0) {
    return;  // Still waiting on the outstanding query
  }

  if (current_poll_index_ < poll_addrs_.size()) {
    current_query_addr_ = poll_addrs_[current_poll_index_];
    race_step_time_ = now;
    this->send_frame(make_query(current_query_addr_, current_query_cmd_));
    return;
  }

  ESP_LOGI(TAG, "All discovered monitors queried. Entering device configuration phase.");
  race_state_ = RaceState::CONFIGURING_DEVICES;
  race_step_time_ = now;
  current_poll_index_ = 0;
  current_query_addr_ = 0;
}

bool GenSAMHub::configure_monitor_(const GenSAMMonitor &mon) {
  uint8_t addr = mon.address;
  bool configured_anything = false;

  // 1. Audio source and AES3 channel configuration
  // Note: Transient volume silencing (silence_system_volume_ / restore_system_volume_) is
  // intentionally omitted here.  Monitors are waking from amplifier standby with internal
  // amplifiers already muted, so there is no listening signal to protect from switching
  // transients.  Volume is restored later by the normal post-wakeup volume command.
  if (audio_source_configured_) {
    uint8_t ch = AES3_CHANNEL_A;
    if (mon.binding != nullptr) {
      ch = mon.binding->aes3_channel;
    } else if (mon.is_subwoofer()) {
      ch = AES3_CHANNEL_SUM;
    }
    ESP_LOGI(TAG, "Configuring audio source for monitor 0x%02X: %s (ch 0x%02X)", addr,
             (current_audio_source_ == SOURCE_ANALOG) ? SOURCE_STR_ANALOG : SOURCE_STR_DIGITAL_AES3, ch);
    this->send_audio_source_frame(addr, current_audio_source_, ch, mon.is_subwoofer());
    configured_anything = true;
  }

  // 2. Bass management crossover frequency
  if (mon.binding != nullptr && mon.binding->crossover_number != nullptr &&
      mon.binding->crossover_configured) {
    if (configured_anything) {
      delay(CONFIG_FRAME_GAP_MS);
    }
    uint16_t freq = mon.binding->crossover_freq;
    ESP_LOGI(TAG, "Configuring bass management crossover frequency for monitor 0x%02X: %u Hz", addr, freq);
    this->send_frame(make_crossover(addr, freq));
    configured_anything = true;
  }

  return configured_anything;
}

void GenSAMHub::race_step_configuring_(uint32_t now) {
  if (current_query_addr_ != 0 && now - race_step_time_ > CONFIG_ACK_TIMEOUT_MS) {
    // Configuration acknowledgement timed out; advance to next monitor
    current_poll_index_++;
    current_query_addr_ = 0;
  }

  if (current_query_addr_ != 0) {
    return;  // Still waiting on the outstanding acknowledgement
  }

  // Walk forward until a monitor actually needs configuring; monitors with nothing to
  // configure are skipped without spending a bus turn on them.
  while (current_poll_index_ < poll_addrs_.size()) {
    GenSAMMonitor *mon = registry_.find(poll_addrs_[current_poll_index_]);
    if (mon != nullptr && this->configure_monitor_(*mon)) {
      current_query_addr_ = mon->address;
      race_step_time_ = now;
      return;
    }
    current_poll_index_++;
  }

  ESP_LOGI(TAG, "All discovered monitors configured. Entering live telemetry polling loop.");

  // Broadcast active volume and stay_online heartbeat to establish monitor gain
  this->broadcast_volume_and_keepalive_();

  race_state_ = RaceState::POLLING_MONITORS;
  last_poll_cycle_time_ = now - poll_interval_ms_;      // Start polling immediately
  last_poll_step_time_ = now - POLL_STEP_INTERVAL_MS;
  current_poll_index_ = 0;
  current_query_addr_ = 0;
  current_query_cmd_ = 0;
}

void GenSAMHub::race_step_polling_(uint32_t now) {
  if (registry_.empty()) {
    race_state_ = RaceState::IDLE;
    return;
  }

  if (current_query_addr_ != 0 && now - race_step_time_ > POLL_REPLY_TIMEOUT_MS) {
    current_query_addr_ = 0;
    last_poll_step_time_ = now;
  }

  if (current_query_addr_ != 0 || (now - last_poll_step_time_ < POLL_STEP_INTERVAL_MS) ||
      (now - last_poll_cycle_time_ < poll_interval_ms_)) {
    return;
  }

  // At the start of each polling cycle, refresh monitor gain and address leases
  if (current_poll_index_ == 0) {
    this->broadcast_volume_and_keepalive_();
    delayMicroseconds(TURNAROUND_US);
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

void GenSAMHub::race_step_idle_(uint32_t now) {
  // Periodically retry discovery if no monitors are registered and not in standby
  if (!current_standby_ && registry_.empty() &&
      now - last_discovery_retry_time_ >= DISCOVERY_RETRY_INTERVAL_MS) {
    last_discovery_retry_time_ = now;
    this->rediscover_monitors();
  }
}

}  // namespace gensam
}  // namespace esphome
