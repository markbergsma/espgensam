#pragma once

/// @file gensam_hub.h
/// @brief ESPHome hub component for native Genelec SAM RS-485 communication.
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// The GenSAM Hub coordinates communication with Genelec Smart Active Monitors (SAM)
/// over half-duplex 9-bit RS-485 at 281,250 baud.
///
/// 1. Bus Arbitration & External Master Detection:
///    The RS-485 bus supports only one master transmitting at any instant - normally that 
///    would be the GLM USB adapter talking to the GLM application.
///    - When GenSAM detects bus traffic from an external controller (frames addressed to
///      or from addresses other than HOST_ADDRESS, or monitor replies arriving when GenSAM
///      did not transmit), it immediately yields the bus (`glm_active_ = true`).
///    - While yielded, GenSAM enters PASSIVE SNOOPING mode: it continues reading and parsing
///      all wire frames, learning monitor addresses and updating telemetry from GLM's queries,
///      without transmitting any pulses on the wire.
///    - If no external GLM traffic is observed for `glm_inactivity_cooldown_ms` (default 30s),
///      GenSAM automatically resumes active master control.
///
/// 2. RACE Discovery State Machine (Active Mode):
///    Genelec SAM monitors power up in unaddressed mode. The Hub assigns sequential logical
///    addresses (0x02, 0x03, ...) using the RACE protocol:
///    - WAKEUP: Hub broadcasts CMD_WAKEUP (0x3A) pulses to bring sleeping monitors online.
///    - RACE PING: Hub broadcasts CMD_DISCOVERY (0xFF 0xFE). Unassigned monitors compete
///      using a carrier-sense backoff; the winning monitor replies with its 3-byte hardware serial.
///    - SET RID: Hub multicasts CMD_SET_RID (0xF0 0x02) containing the winning serial and the
///      next assigned address (e.g. 0x02). The monitor confirms with an ACK.
///    - Loop: Hub repeats discovery pings until no unassigned monitors respond (350 ms timeout).
///    - STAY ONLINE: Hub broadcasts CMD_STAY_ONLINE (0xFF 0x04) to transition monitors into active mode.
///    - QUERY DEVICES: Hub queries each monitor with CMD_SOFTWARE_QUERY (0x39) to obtain model and
///      firmware metadata, and CMD_BAR_CODE (0x19) for the factory serial number.
///    - LIVE POLLING: Hub broadcasts CMD_STAY_ONLINE (0x04) to refresh address leases and polls
///      monitors round-robin with CMD_QUERY_STATUS (0x08) for telemetry.
///
/// 3. Hardware Transceiver Abstraction:
///    Supports both auto-direction transceivers (M5Stack Atomic RS-485 Base) and discrete
///    enable pins (LilyGO T-CAN485 with 5V booster `power_pin`, transceiver enable `se_pin`,
///    and receiver enable `re_pin`, or standard boards with hardware direction control `de_pin`).
/// ===================================================================================

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "const.h"
#include "frame.h"
#include "monitor.h"
#include "uart9bit.h"

#include <vector>
#include <map>
#include <functional>

namespace esphome {
namespace gensam {

/// @brief Phases of the active RACE monitor discovery and telemetry state machine.
enum class RaceState : uint8_t {
  IDLE,               ///< Waiting or idle between discovery retries.
  WAKEUP_SENT,        ///< Wakeup broadcast sequence sent; waiting for monitor power-on.
  RACE_PING_SENT,     ///< Discovery ping (0xFF 0xFE) broadcast; awaiting winning monitor serial.
  RACE_SET_RID_SENT,  ///< Address assignment (0xF0 0x02) sent; awaiting monitor ACK.
  QUERYING_DEVICES,   ///< Querying model and firmware metadata for all discovered monitors.
  POLLING_MONITORS,   ///< Periodic round-robin polling of monitor status and telemetry.
};

/// @brief Hub component managing the 9-bit RS-485 physical bus, framing,
/// GLM arbitration, monitor discovery (RACE), and device queries.
class GenSAMHub : public Component {
 public:
  /// @brief Set the GPIO pin used for UART TX (RMT pulse generator).
  void set_tx_pin(int pin) { tx_pin_ = pin; }

  /// @brief Set the GPIO pin used for UART RX (RMT pulse digitizer).
  void set_rx_pin(int pin) { rx_pin_ = pin; }

  /// @brief Set the optional GPIO pin for RS-485 dynamic Driver Enable (DE).
  void set_de_pin(int pin) { de_pin_ = pin; }

  /// @brief Set the optional GPIO pin for RS-485 Receiver Enable (RE / held HIGH).
  void set_re_pin(int pin) { re_pin_ = pin; }

  /// @brief Set the optional GPIO pin for RS-485 module DC-DC power booster.
  void set_power_pin(int pin) { power_pin_ = pin; }

  /// @brief Set the optional GPIO pin for RS-485 transceiver enable / shutdown.
  void set_se_pin(int pin) { se_pin_ = pin; }

  /// @brief Set the RX ring buffer capacity in 9-bit character units.
  void set_rx_buffer_size(size_t size) { rx_buffer_size_ = size; }

  /// @brief Enable listen-only mode (prevents all transmissions on the bus).
  void set_listen_only(bool listen_only) { listen_only_ = listen_only; }

  /// @brief Whether to yield the bus to external Genelec GLM adapters when traffic is detected.
  void set_yield_to_glm(bool yield) { yield_to_glm_ = yield; }

  /// @brief Inactivity cooldown before reclaiming active master control after GLM adapter goes silent.
  void set_glm_inactivity_cooldown(uint32_t cooldown_ms) { glm_inactivity_cooldown_ms_ = cooldown_ms; }

  /// @brief Telemetry round-robin polling interval in milliseconds.
  void set_poll_interval(uint32_t interval_ms) { poll_interval_ms_ = interval_ms; }

  /// @brief Initialize hardware pins, power rails, and the 9-bit RMT transceiver.
  void setup() override;

  /// @brief Main execution loop: handles incoming RX characters, GLM cooldown, and RACE state machine.
  void loop() override;

  /// @brief Log active hub configuration to ESPHome diagnostics.
  void dump_config() override;

  /// @brief Check if the hub is currently permitted to transmit on the bus.
  /// @return True if transmitter is initialized, listen-only is false, and no external GLM holds the bus.
  bool can_transmit() const;

  /// @brief Whether external GLM traffic is currently holding the bus.
  bool is_glm_active() const { return glm_active_; }

  /// @brief Send a high-level GenSAM Frame onto the bus.
  /// @param frame The frame to serialize and transmit.
  /// @return True if transmission was accepted, false if blocked by arbitration or uninitialized.
  bool send_frame(const Frame &frame);

  /// @brief Send raw 9-bit characters onto the bus (for testing and low-level diagnostics).
  /// @param raw_chars Vector of characters to transmit.
  void send_raw_frame(const std::vector<Uart9BitChar> &raw_chars);

  /// @brief Register a callback invoked for every complete decoded frame received from the bus.
  /// @param callback Callback function receiving the parsed frame.
  void register_frame_callback(std::function<void(const Frame &)> callback) {
    callbacks_.push_back(callback);
  }

  /// @brief Send broadcast wakeup sequence to wake SAM monitors from standby.
  void send_wakeup();

  /// @brief Trigger a fresh active RACE discovery cycle to detect and assign unaddressed monitors.
  void start_race_discovery();

  /// @brief Access the registry table of discovered monitors, keyed by logical address.
  /// @return Map of address to GenSAMMonitor descriptors.
  const std::map<uint8_t, GenSAMMonitor> &get_monitors() const { return monitors_; }

  /// @brief Look up a discovered monitor by its logical RS-485 address.
  /// @param address Target address (0x02..0x7F).
  /// @return Pointer to GenSAMMonitor descriptor, or nullptr if not registered.
  GenSAMMonitor *get_monitor(uint8_t address);

  /// @brief Look up a discovered monitor by its logical RS-485 address (read-only).
  /// @param address Target address (0x02..0x7F).
  /// @return Const pointer to GenSAMMonitor descriptor, or nullptr if not registered.
  const GenSAMMonitor *get_monitor(uint8_t address) const;

 protected:
  /// @brief Drain RX ring buffer, log raw bytes, feed the parser, and dispatch decoded frames.
  void process_rx_();

  /// @brief Check whether the external GLM master inactivity timer has expired to reclaim bus control.
  void check_glm_cooldown_();

  /// @brief Advance the active RACE discovery and telemetry polling state machine.
  void update_race_state_machine_();

  /// @brief Process an incoming frame through the active state machine or passive snooping registry.
  /// @param frame The decoded frame to handle.
  void handle_incoming_frame_(const Frame &frame);

  int tx_pin_{-1};
  int rx_pin_{-1};
  int de_pin_{-1};
  int re_pin_{-1};
  int power_pin_{-1};
  int se_pin_{-1};
  uint32_t baud_rate_{BAUDRATE};
  size_t rx_buffer_size_{512};
  bool listen_only_{false};
  bool yield_to_glm_{true};
  uint32_t glm_inactivity_cooldown_ms_{30000};
  uint32_t poll_interval_ms_{1000};

  Uart9Bit uart9_;
  FrameParser parser_;
  std::vector<std::function<void(const Frame &)>> callbacks_;

  // Bus arbitration state
  bool glm_active_{false};
  uint32_t last_glm_activity_{0};
  uint32_t last_our_tx_time_{0};
  uint32_t last_tx_blocked_warning_{0};
  uint32_t last_stat_log_{0};

  // Monitor registry
  std::map<uint8_t, GenSAMMonitor> monitors_;

  // Active RACE & query state machine
  RaceState race_state_{RaceState::IDLE};
  uint8_t next_assign_addr_{MONITOR_START_ADDR};
  std::vector<uint8_t> current_racing_serial_;
  uint32_t race_step_time_{0};
  uint8_t current_query_addr_{0};
  uint8_t current_query_cmd_{0};
  uint8_t query_retries_{0};
  uint32_t last_poll_cycle_time_{0};
  uint32_t last_discovery_retry_time_{0};
  size_t current_poll_index_{0};
  std::vector<uint8_t> poll_addrs_;
  bool initial_discovery_done_{false};
  uint32_t boot_time_{0};

  // Passive snooping tracker
  uint8_t last_queried_addr_{0};
  uint8_t last_queried_cmd_{0};
};

}  // namespace gensam
}  // namespace esphome
