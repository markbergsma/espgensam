/// @file race.cpp
/// @brief RACE monitor discovery, device interrogation, and telemetry polling state machine.
/// See hub.h for the RACE protocol rationale and complete API documentation.

#include "hub.h"
#include "commands.h"

#include "esphome/core/log.h"
#include "esphome/components/select/select.h"

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

/// Retries of the same device query before giving up on it.
///
/// Monitors answer these in 30-60 ms, so a silent 250 ms window means the request or the reply
/// was lost rather than delayed. Device metadata is only ever asked for once per discovery, so
/// without a retry a single corrupted frame leaves that monitor's model or serial blank until
/// the next rediscovery - which on a bus with a percent or so of frame errors happens readily.
constexpr uint8_t DEVICE_QUERY_MAX_RETRIES = 2;

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

/// Spacing between consecutive frames of a group's DSP block.
///
/// Matches the OEM's own pacing. Monitors acknowledge a PEQ frame in under a millisecond, and
/// GLM does not wait for the acknowledgement before sending the next; it simply spaces them.
/// Doing the same keeps a group push to a few hundred milliseconds while leaving the bus idle
/// between frames for a monitor's reply.
constexpr uint32_t GROUP_FRAME_GAP_MS = 3;

/// Abandon a group push that has not finished in this long.
///
/// A push holds the system at silence, so it must not be able to stall there indefinitely --
/// which it otherwise could, since the state machine stops advancing whenever an external GLM
/// adapter takes the bus, and that hold has its own multi-second cooldown.
constexpr uint32_t GROUP_APPLY_TIMEOUT_MS = 15000;

/// Frame sequence positions within one device's DSP block, following the order GLM uses.
enum : uint8_t {
  GROUP_STEP_PREPARE = 0,                                  ///< 0x17 0x01
  GROUP_STEP_DELAY,                                        ///< 0x10 0x02
  GROUP_STEP_LEVEL,                                        ///< 0x10 0x01 0x00
  GROUP_STEP_PEQ_FIRST,                                    ///< 0x10 0x0E, 20 of them
  GROUP_STEP_CROSSOVER = GROUP_STEP_PEQ_FIRST + PEQ_BAND_COUNT,  ///< 0x3B
  GROUP_STEP_SOURCE,                                       ///< 0x40, primary input
  GROUP_STEP_SOURCE_AUX,                                   ///< 0x40, subwoofer secondary input
  GROUP_STEP_DONE,
};

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
  this->update_bus_status_();
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

  ESP_LOGI(TAG, "Resetting monitor table and initiating RACE discovery...");

  // Mark all currently known monitors offline before clearing cache so HA state does not remain stale.
  registry_.invalidate_all();
  this->evaluate_system_mute_();

  registry_.clear();
  poll_addrs_.clear();
  current_poll_index_ = 0;
  current_query_addr_ = 0;
  current_query_cmd_ = 0;
  query_retries_ = 0;
  current_racing_bytes_.clear();
  current_racing_id_ = 0;
  rid_retries_ = 0;
  last_queried_addr_ = 0;
  last_queried_cmd_ = 0;
  next_assign_addr_ = MONITOR_START_ADDR;

  // A sleeping monitor cannot answer a RACE ping: its DSP is down and it is not listening on the
  // bus at all. Discovery therefore always begins by waking, exactly as GLM does. When the system
  // is meant to remain off, the amplifiers are silenced right behind the wakeup so nothing is
  // audible, and returned to standby by finish_temporary_wake_() once discovery completes.
  this->send_wakeup();
  if (current_standby_) {
    this->silence_system_volume_();
    restore_standby_after_discovery_ = true;
    ESP_LOGI(TAG, "Waking monitors silently for discovery; will return them to standby");
  }
  race_state_ = RaceState::WAKEUP_SENT;
  this->update_bus_status_();
  race_step_time_ = millis();
}

void GenSAMHub::finish_temporary_wake_() {
  if (!restore_standby_after_discovery_) {
    return;
  }
  restore_standby_after_discovery_ = false;
  ESP_LOGI(TAG, "Discovery complete; returning monitors to standby");
  this->send_standby();
  // Monitors keep reporting an active amplifier until the transition lands; hold the commanded
  // state over that telemetry the same way an explicit standby command does.
  last_standby_command_ = millis();
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
  query_retries_ = 0;
}

void GenSAMHub::broadcast_volume_and_keepalive_() {
  // The volume broadcast is what re-establishes amplifier gain, so it is skipped while in
  // standby. The keep-alive is not: it refreshes the volatile RACE address leases, and without
  // it monitors stop answering on their assigned addresses a minute or two later.
  if (!current_standby_) {
    this->send_frame(make_broadcast_volume_db(current_volume_db_));
    delayMicroseconds(VOLUME_KEEPALIVE_GAP_US);
  }
  this->send_frame(make_stay_online());
}

// --- Incoming replies to what the state machine sent --------------------------------

bool GenSAMHub::handle_active_reply_(const Frame &frame, uint32_t now) {
  switch (race_state_) {
    case RaceState::RACE_PING_SENT: {
      // Expecting winning unassigned monitor response with 3-byte serial
      if (frame.address != HOST_ADDRESS ||
          (frame.command != CMD_REPORT_STATUS && frame.command != CMD_ACK) ||
          frame.payload.size() != 3) {
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
      this->evaluate_system_standby_();
      if (frame.payload.size() == 1 && is_telemetry_marker(frame.payload[0])) {
        ESP_LOGD(TAG, "[0x%02X %s] Telemetry: marker-only reply (0x%02X); no fields reported",
                 current_query_addr_, mon.model.c_str(), frame.payload[0]);
      } else {
        ESP_LOGI(TAG, "[0x%02X %s] Telemetry: %s Temp=%d°C In=%d dBFS Out=%d dBFS",
                 current_query_addr_, mon.model.c_str(),
                 mon.standby_known ? (mon.standby ? "STANDBY" : "ACTIVE") : "UNKNOWN",
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
  if (!can_transmit()) {
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
    case RaceState::APPLYING_GROUP:
      this->race_step_applying_group_(now);
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
    // Nothing answered, but the wakeup may still have powered something up; put it back.
    this->finish_temporary_wake_();
    race_state_ = RaceState::IDLE;
    this->update_bus_status_();
    last_discovery_retry_time_ = now;
    return;
  }

  ESP_LOGI(TAG, "RACE discovery complete. Total monitors found: %u", (unsigned)registry_.size());

  // Transition all monitors from discovery to online mode
  this->send_frame(make_stay_online());

  poll_addrs_ = registry_.addresses();
  race_state_ = RaceState::QUERYING_DEVICES;
  this->update_bus_status_();
  race_step_time_ = now;
  current_poll_index_ = 0;
  current_query_addr_ = 0;
  current_query_cmd_ = CMD_SOFTWARE_QUERY;
  query_retries_ = 0;
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
    if (query_retries_ < DEVICE_QUERY_MAX_RETRIES) {
      // Re-ask the same monitor the same question: clearing the address alone leaves the poll
      // cursor and current_query_cmd_ untouched, so the block below resends it.
      query_retries_++;
      ESP_LOGW(TAG, "Timeout querying monitor 0x%02X (cmd 0x%02X), attempt %u/%u; retrying...",
               current_query_addr_, current_query_cmd_, (unsigned)query_retries_ + 1,
               (unsigned)DEVICE_QUERY_MAX_RETRIES + 1);
      current_query_addr_ = 0;
    } else {
      ESP_LOGW(TAG, "Monitor 0x%02X did not answer cmd 0x%02X after %u attempts; skipping",
               current_query_addr_, current_query_cmd_, (unsigned)DEVICE_QUERY_MAX_RETRIES + 1);
      // Give up on this question and advance (info -> barcode -> next monitor)
      this->advance_device_query_();
    }
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
  this->update_bus_status_();
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
  // Gated on crossover_configured alone, which already means "somebody set this deliberately"
  // (it defaults false, so an unconfigured monitor is never sent the default frequency).  The
  // presence of a Home Assistant number entity is not a precondition: a crossover snooped from
  // GLM sets crossover_configured on a binding that may have no entity at all, and that value
  // still has to be reapplied here, since monitors reset it when passing through standby.
  if (mon.binding != nullptr && mon.binding->crossover_configured) {
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

  this->finish_temporary_wake_();

  // Refresh the address leases, and monitor gain too when not in standby
  this->broadcast_volume_and_keepalive_();

  // A monitor loses its DSP state passing through standby, so whatever group was active has
  // to be pushed again now rather than only when the user next picks one. pending_group_ may
  // already hold a different choice made while the system was down; that one wins. Falling
  // back to the default covers the first discovery after boot, so the speakers are never
  // left in a state nothing here chose; see set_default_group().
  if (pending_group_ < 0) {
    pending_group_ = (active_group_ >= 0) ? active_group_ : default_group_;
  }
  if (pending_group_ >= 0 && this->start_group_apply_(now)) {
    return;
  }

  ESP_LOGI(TAG, "All discovered monitors configured. Entering live telemetry polling loop.");
  race_state_ = RaceState::POLLING_MONITORS;
  this->update_bus_status_();
  last_poll_cycle_time_ = now - poll_interval_ms_;      // Start polling immediately
  last_poll_step_time_ = now - POLL_STEP_INTERVAL_MS;
  current_poll_index_ = 0;
  current_query_addr_ = 0;
  current_query_cmd_ = 0;
  query_retries_ = 0;
}

// --- Group preset application ------------------------------------------------------
//
// One frame per loop() call, paced by GROUP_FRAME_GAP_MS. See GroupApplyState in hub.h for
// why this cannot be a loop.

uint32_t GenSAMHub::peq_design_rate_for(const GenSAMMonitor &mon) {
  if (mon.binding != nullptr && mon.binding->peq_design_rate != 0) {
    return mon.binding->peq_design_rate;
  }
  return mon.is_subwoofer() ? PEQ_RATE_SUBWOOFER_HZ : PEQ_RATE_DEFAULT_HZ;
}

bool GenSAMHub::start_group_apply_(uint32_t now) {
  if (pending_group_ < 0) {
    return false;
  }
  const uint8_t wanted = static_cast<uint8_t>(pending_group_);
  pending_group_ = -1;

  const GroupPreset *group = this->get_group(wanted);
  if (group == nullptr) {
    ESP_LOGW(TAG, "Group preset %u is out of range; nothing applied", (unsigned) wanted);
    return false;
  }

  ESP_LOGI(TAG, "Applying group preset '%s' (%u devices)", group->name,
           (unsigned) group->device_count);

  apply_ = GroupApplyState{};
  apply_.active = true;
  apply_.group_idx = wanted;
  apply_.started_ms = now;
  apply_.last_tx_ms = now - GROUP_FRAME_GAP_MS;  // let the first frame go immediately

  // Duck for the duration. The block retunes filters and levels underneath a playing signal,
  // and the OEM ducks for the same reason. Skipped in standby, where there is nothing to
  // protect and the volume broadcast would re-establish amplifier gain.
  if (!current_standby_ && this->can_transmit()) {
    this->silence_system_volume_();
    apply_.ducked = true;
  }

  race_state_ = RaceState::APPLYING_GROUP;
  this->update_bus_status_();
  return true;
}

void GenSAMHub::finish_group_apply_() {
  if (apply_.ducked) {
    this->restore_system_volume_();
  }

  // Only count the group as active if at least one speaker actually received it. A push that
  // reached nobody - every monitor offline, which happens when a wake from standby reboots
  // the DSPs and discovery runs before they answer - would otherwise leave Home Assistant
  // reporting a group the bus has never been told about. Leaving active_group_ alone instead
  // means the next discovery re-applies, and re-publishing keeps the entity honest until it
  // does.
  const uint8_t attempted = apply_.group_idx;
  const bool reached_anyone = apply_.configured > 0;
  apply_ = GroupApplyState{};

  if (reached_anyone) {
    active_group_ = attempted;
  } else {
    const GroupPreset *wanted = this->get_group(attempted);
    ESP_LOGW(TAG, "Group preset '%s' reached no monitors; leaving it unapplied",
             wanted != nullptr ? wanted->name : "?");
  }

  const GroupPreset *group =
      (active_group_ >= 0) ? this->get_group(static_cast<uint8_t>(active_group_)) : nullptr;
  if (group != nullptr && group_select_ != nullptr) {
    group_select_->publish_state(group->name);
  }

  race_state_ = RaceState::POLLING_MONITORS;
  this->update_bus_status_();
  last_poll_cycle_time_ = millis() - poll_interval_ms_;
  last_poll_step_time_ = millis() - POLL_STEP_INTERVAL_MS;
  current_poll_index_ = 0;
  current_query_addr_ = 0;
}

bool GenSAMHub::send_group_step_(const GenSAMMonitor &mon, const GroupDevice &dev, uint8_t step) {
  const uint8_t addr = mon.address;

  // A device switched off in this group plays nothing, so its DSP is left untouched and only
  // its mute is asserted. Reconfiguring a speaker that is about to be silent would spend bus
  // time to no effect, and would overwrite calibration another group still depends on.
  if (!dev.enabled) {
    if (step == GROUP_STEP_PREPARE) {
      this->send_frame(make_bypass(addr, true));
      return true;
    }
    return false;
  }

  switch (step) {
    case GROUP_STEP_PREPARE:
      this->send_frame(make_prepare_config(addr));
      return true;

    case GROUP_STEP_DELAY:
      this->send_frame(make_delay(addr, dev.delay_samples));
      return true;

    case GROUP_STEP_LEVEL:
      this->send_frame(make_level(addr, dev.level_db));
      return true;

    case GROUP_STEP_CROSSOVER:
      this->send_frame(make_crossover(addr, dev.crossover_hz));
      return true;

    // The two input frames are separate steps rather than one call to
    // send_audio_source_frame(), which sleeps 5 ms between them. Spacing them as ordinary
    // steps gets the same gap from the pacing that is already there, without blocking.
    case GROUP_STEP_SOURCE:
      this->send_frame(make_audio_source(addr, 0x00, dev.source, dev.aes3_channel));
      return true;

    case GROUP_STEP_SOURCE_AUX:
      // Only 7xxx subwoofers have a secondary input. Returning false for everything else
      // ends the device, which is correct because this is the last step in the sequence.
      if (!mon.is_subwoofer()) {
        return false;
      }
      this->send_frame(make_audio_source(addr, 0x01, dev.source, dev.aes3_channel));
      return true;

    default:
      break;
  }

  const uint8_t band = step - GROUP_STEP_PEQ_FIRST;
  if (band >= PEQ_BAND_COUNT) {
    return false;
  }
  // Slots past the configured bands are still transmitted, as the bypass vector: the monitor
  // holds whatever the previous group left in them otherwise, and a stale filter is worse
  // than an unnecessary frame.
  const PeqBand &spec = (band < dev.band_count) ? dev.bands[band] : PeqBand{};
  const BiquadCoeffs coeffs =
      design_biquad(spec.type, spec.frequency_hz, spec.gain_db, spec.q, peq_design_rate_for(mon));
  this->send_frame(make_peq_band(addr, band, coeffs));
  return true;
}

void GenSAMHub::race_step_applying_group_(uint32_t now) {
  const GroupPreset *group = this->get_group(apply_.group_idx);
  if (group == nullptr) {
    this->finish_group_apply_();
    return;
  }

  if (now - apply_.started_ms > GROUP_APPLY_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Group preset '%s' did not finish applying within %u ms; abandoning at "
                  "device %u step %u so the system does not stay silenced",
             group->name, (unsigned) GROUP_APPLY_TIMEOUT_MS, (unsigned) apply_.device_idx,
             (unsigned) apply_.step);
    this->finish_group_apply_();
    return;
  }

  if (now - apply_.last_tx_ms < GROUP_FRAME_GAP_MS) {
    return;
  }

  while (apply_.device_idx < group->device_count) {
    const GroupDevice &dev = group->devices[apply_.device_idx];
    GenSAMMonitor *mon = registry_.find_by_serial_or_id(std::to_string(dev.unique_id));

    if (mon == nullptr || !mon->online) {
      // Not discovered, or not answering. Skipping is right rather than retrying: the group
      // is re-applied after every rediscovery, which is when such a monitor comes back. It
      // is a warning rather than a debug line because until then that speaker is running
      // some other group's calibration while the rest of the system has moved on.
      ESP_LOGW(TAG, "Group '%s': monitor %lu is not online; it keeps its previous settings",
               group->name, (unsigned long) dev.unique_id);
      apply_.device_idx++;
      apply_.step = 0;
      continue;
    }

    if (apply_.step < GROUP_STEP_DONE && this->send_group_step_(*mon, dev, apply_.step)) {
      apply_.step++;
      apply_.last_tx_ms = now;
      return;
    }

    // Device finished. Mirror what was pushed onto the binding so the per-monitor entities
    // show the active group's values, and so a later rediscovery re-sends the same thing.
    if (mon->binding != nullptr && dev.enabled) {
      registry_.set_binding_crossover(*mon->binding, dev.crossover_hz);
      registry_.set_binding_aes3_channel(*mon->binding, dev.aes3_channel);
    }
    apply_.configured++;
    apply_.device_idx++;
    apply_.step = 0;
  }

  ESP_LOGI(TAG, "Group preset '%s' applied to %u of %u monitors", group->name,
           (unsigned) apply_.configured, (unsigned) group->device_count);
  this->finish_group_apply_();
}

void GenSAMHub::race_step_polling_(uint32_t now) {
  // A group switch requested while polling starts here, between telemetry cycles, so a push
  // never interleaves with an outstanding poll.
  if (pending_group_ >= 0 && current_query_addr_ == 0 && this->start_group_apply_(now)) {
    return;
  }

  // Fall back to IDLE once nothing answers any more, so discovery gets retried. Testing
  // registry_.empty() here would never fire: stale monitors are marked offline but kept.
  if (!registry_.any_online()) {
    race_state_ = RaceState::IDLE;
    this->update_bus_status_();
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

  // At the start of each polling cycle, refresh address leases and (when awake) monitor gain
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
  // Periodically retry discovery while nothing on the bus is responding.
  //
  // Not while the system is meant to be off. Discovery now has to wake monitors to enumerate
  // them, so retrying here would power amplifiers up and back down every DISCOVERY_RETRY_INTERVAL_MS
  // for as long as the system stays switched off. One attempt is made at boot (via the
  // initial_discovery_done_ path); if that finds nothing, the monitors are left alone until the
  // user switches the system on, which runs a full wake and discovery of its own.
  if (current_standby_) {
    return;
  }

  if (!registry_.any_online() &&
      now - last_discovery_retry_time_ >= DISCOVERY_RETRY_INTERVAL_MS) {
    last_discovery_retry_time_ = now;
    this->rediscover_monitors();
  }
}

}  // namespace gensam
}  // namespace esphome
