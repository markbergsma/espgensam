#pragma once

/// @file hub.h
/// @brief ESPHome hub component for native Genelec SAM RS-485 communication.
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// The GenSAM Hub coordinates communication with Genelec Smart Active Monitors (SAM)
/// over half-duplex 9-bit RS-485 at 281,250 baud.
///
/// 1. Bus Arbitration & External Master Detection:
///    The hub yields the bus whenever an external GLM controller is transmitting, and resumes
///    active master control once that controller has been silent for `glm_inactivity_cooldown_ms`
///    (default 30 s).  While yielded it keeps parsing all wire traffic (passive snooping) without
///    transmitting.  The frame-origin classification rules and their timing windows live in
///    BusArbiter; see arbiter.h.
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
/// 3. Power Control (Wakeup & Standby):
///    Monitors enter and exit ultra-low-power sleep (<0.5W) using multi-step broadcast commands
///    via CMD_WAKEUP (0x3A) with sub-command 0x03:
///    - WAKEUP (Power ON): Alternates {0x03, 0x7F} and {0x03, 0x01} repeated 3 times.
///    - STANDBY (Power OFF): Transmits {0x03, 0x02} twice (20 ms spacing), waits 80 ms, then
///      transmits {0x03, 0x00} twice (20 ms spacing).
///    - While monitors are in standby, the Hub enters silent idle: CMD_STAY_ONLINE (0x04) heartbeats
///      and status queries are suppressed so monitors stay in low-power sleep.
///    - Volatile Address Reset across Standby: In <0.5W standby, Genelec monitors shut down their
///      DSP and reset their volatile RACE address leases. When waking monitors from standby, the Hub
///      automatically initiates a fresh RACE rediscovery cycle: waking monitors, waiting 400 ms for
///      DSP boot, assigning dynamic addresses (0x02..), refreshing CMD_STAY_ONLINE keep-alives,
///      and restoring active volume.
///
/// 4. Hardware Transceiver Abstraction:
///    Supports both auto-direction transceivers (M5Stack Atomic RS-485 Base) and discrete
///    enable pins (LilyGO T-CAN485 with 5V booster `power_pin`, transceiver enable `se_pin`,
///    and receiver enable `re_pin`, or standard boards with hardware direction control `de_pin`).
///
/// 5. Audio Source Selection & AES3 Routing:
///    SAM monitors support Analog vs Digital (AES3) routing via CMD_SELECT_AUDIO_SOURCE (0x40).
///    Standard monitors receive 1 frame (input 0), while 7xxx subwoofers receive 2 frames (input 0 and 1).
///    Non-destructive boot ensures initial boot and discovery preserve monitor presets until changed
///    or snooped from GLM.
///    - Transient Volume Silencing: To prevent audible pops, clicks, or transient distortion during
///      analog multiplexer switching, AES3 PLL clock relock, or Sample Rate Converter (SRC) relocking,
///      audio is temporarily silenced by broadcasting CMD_VOLUME (0x1F) with minimum volume (-130.0 dB /
///      {0x00, 0x00, 0x02}) before transmitting source selection frames. After monitors settle on the
///      new input stream (~100 ms), the previous listening volume is automatically restored. System
///      volume state callbacks are not triggered during this transient silence so the Home Assistant
///      volume slider remains steady.
/// ===================================================================================

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "arbiter.h"
#include "const.h"
#include "frame.h"
#include "monitor.h"
#include "registry.h"
#include "uart9bit.h"
#include "util.h"

#include <vector>
#include <map>
#include <functional>

namespace esphome {
namespace binary_sensor {
class BinarySensor;
}  // namespace binary_sensor

namespace number {
class Number;
}  // namespace number

namespace select {
class Select;
}  // namespace select

namespace text_sensor {
class TextSensor;
}  // namespace text_sensor

namespace gensam {

/// @brief Phases of the active RACE monitor discovery and telemetry state machine.
enum class RaceState : uint8_t {
  IDLE,               ///< Waiting or idle between discovery retries.
  WAKEUP_SENT,        ///< Wakeup broadcast sequence sent; waiting for monitor power-on.
  RACE_PING_SENT,     ///< Discovery ping (0xFF 0xFE) broadcast; awaiting winning monitor serial.
  RACE_SET_RID_SENT,  ///< Address assignment (0xF0 0x02) sent; awaiting monitor ACK.
  QUERYING_DEVICES,    ///< Querying model and firmware metadata for all discovered monitors.
  CONFIGURING_DEVICES, ///< Transmitting device configuration (e.g. crossover frequency) to discovered monitors.
  POLLING_MONITORS,    ///< Periodic round-robin polling of monitor status and telemetry.
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

  /// @brief Set minimum volume in decibels corresponding to slider 0.0 (e.g. -80.0 dB).
  void set_min_volume_db(float db) { min_volume_db_ = db; }

  /// @brief Set maximum volume in decibels corresponding to slider 1.0 (e.g. 0.0 dB).
  void set_max_volume_db(float db) { max_volume_db_ = db; }

  /// @brief Set initial volume in decibels at startup/boot (e.g. -30.0 dB).
  void set_startup_volume_db(float db) { startup_volume_db_ = db; current_volume_db_ = db; }

  /// @brief Register a configured monitor binding to match discovered hardware.
  void add_monitor_binding(const GenSAMMonitorBinding &binding) { registry_.add_binding(binding); }

  /// @brief Set optional binary sensor reflecting external GLM USB adapter bus occupancy.
  void set_glm_usb_adapter_active_sensor(binary_sensor::BinarySensor *sensor) {
    glm_usb_adapter_active_sensor_ = sensor;
  }

  /// @brief Register system volume in dB number entity.
  /// @param num Pointer to the GenSAMVolumeNumber entity.
  void set_volume_number(number::Number *num) { volume_number_ = num; }

  /// @brief Get system volume in dB number entity.
  /// @return Pointer to registered number entity or nullptr.
  number::Number *get_volume_number() const { return volume_number_; }

  /// @brief Set optional global audio source select entity.
  /// @param sel Pointer to the GenSAMSourceSelect entity.
  void set_audio_source_select(select::Select *sel) { audio_source_select_ = sel; }

  /// @brief Get optional global audio source select entity.
  /// @return Pointer to registered select entity or nullptr.
  select::Select *get_audio_source_select() const { return audio_source_select_; }

  /// @brief Set optional bus operational status diagnostic text sensor entity.
  /// @param sensor Pointer to the TextSensor entity.
  void set_bus_status_sensor(text_sensor::TextSensor *sensor) { bus_status_sensor_ = sensor; }

  /// @brief Get optional bus operational status diagnostic text sensor entity.
  /// @return Pointer to registered text sensor or nullptr.
  text_sensor::TextSensor *get_bus_status_sensor() const { return bus_status_sensor_; }

  /// @brief Current bus operational status string ("Active", "GLM Active", "Discovering", "Configuring", "Standby", "Offline").
  const std::string &get_bus_status() const { return last_bus_status_; }

  /// @brief Register a callback for when bus operational status changes.
  void add_bus_status_callback(std::function<void(const std::string &)> cb) {
    bus_status_callbacks_.push_back(std::move(cb));
  }

  /// @brief Current system audio source (SOURCE_ANALOG or SOURCE_DIGITAL_AES3).
  uint8_t get_current_audio_source() const { return current_audio_source_; }

  /// @brief Whether system audio source has been configured by user or snooped from GLM.
  bool is_audio_source_configured() const { return audio_source_configured_; }

  /// @brief Register a callback for when volume, mute, or power changes (from commands or passive snooping).
  void add_state_callback(std::function<void(float, bool, bool)> cb) {
    state_callbacks_.push_back(std::move(cb));
  }

  /// @brief Legacy setter for volume state callback.
  void set_volume_state_callback(std::function<void(float, bool, bool)> cb) {
    add_state_callback(std::move(cb));
  }

  /// @brief Minimum volume in dB corresponding to slider 0.0.
  float get_min_volume_db() const { return min_volume_db_; }

  /// @brief Maximum volume in dB corresponding to slider 1.0.
  float get_max_volume_db() const { return max_volume_db_; }

  /// @brief Current system volume in dB.
  float get_current_volume_db() const { return current_volume_db_; }

  /// @brief Current system mute state.
  bool is_muted() const { return current_mute_; }

  /// @brief Current system standby state.
  bool is_standby() const { return current_standby_; }

  /// @brief Set master speaker group volume in decibels.
  /// @param db Target volume level in dB (clamped between min_volume_db_ and max_volume_db_).
  void set_volume_db(float db);

  /// @brief Set master speaker group mute state.
  /// @param mute True to mute all speakers via CMD_BYPASS, false to unmute.
  void set_group_mute(bool mute);

  /// @brief Set mute state for an individual monitor by logical RS-485 bus address.
  /// @param address Logical bus address (0x02..0x7F).
  /// @param mute True to mute audio and set front LED red, false to unmute.
  void set_monitor_mute(uint8_t address, bool mute);

  /// @brief Set mute state for an individual monitor by serial number or unique ID string.
  /// @param serial_or_id Serial number string (e.g. "7350APM88123456") or decimal unique ID string.
  /// @param mute True to mute audio and set front LED red, false to unmute.
  void set_monitor_mute_by_serial(const std::string &serial_or_id, bool mute);

  /// @brief Set bass management crossover frequency for an individual monitor by logical address.
  /// @param address Logical bus address (0x02..0x7F).
  /// @param freq_hz Crossover filter frequency in Hz (typically 50..120 Hz, step 5 Hz).
  void set_monitor_crossover(uint8_t address, uint16_t freq_hz);

  /// @brief Set bass management crossover frequency for an individual monitor by serial number or unique ID string.
  /// @param serial_or_id Serial number string (e.g. "7350APM88123456") or decimal unique ID string.
  /// @param freq_hz Crossover filter frequency in Hz (typically 50..120 Hz, step 5 Hz).
  void set_monitor_crossover_by_serial(const std::string &serial_or_id, uint16_t freq_hz);

  /// @brief Set global audio source (Analog vs Digital AES3) across all monitors.
  /// @param source SOURCE_ANALOG (0x01) or SOURCE_DIGITAL_AES3 (0x02).
  void set_global_source(uint8_t source);

  /// @brief Set global audio source by option string ("Analog" or "Digital (AES3)").
  /// @param source_name Option name string.
  void set_global_source_by_name(const std::string &source_name);

  /// @brief Set AES3 channel routing for an individual monitor by logical RS-485 address.
  /// @param address Logical bus address (0x02..0x7F).
  /// @param channel AES3_CHANNEL_A (0x01), AES3_CHANNEL_B (0x02), or AES3_CHANNEL_SUM (0x03).
  void set_monitor_aes3_channel(uint8_t address, uint8_t channel);

  /// @brief Set AES3 channel routing for an individual monitor by serial number or unique ID string.
  /// @param serial_or_id Serial number string (e.g. "7350APM88123456") or decimal unique ID string.
  /// @param channel AES3_CHANNEL_A (0x01), AES3_CHANNEL_B (0x02), or AES3_CHANNEL_SUM (0x03).
  void set_monitor_aes3_channel_by_serial(const std::string &serial_or_id, uint8_t channel);

  /// @brief Set AES3 channel routing for an individual monitor by option name string.
  /// @param serial_or_id Serial number string or decimal unique ID string.
  /// @param channel_name Option string ("Channel A (Left)", "Channel B (Right)", or "Channel A+B (Sum)").
  void set_monitor_aes3_channel_by_name(const std::string &serial_or_id, const std::string &channel_name);

  /// @brief Send audio source frame(s) to a specific monitor.
  /// Standard monitors receive 1 frame (input 0); subwoofers (7xxx) receive 2 frames (inputs 0 and 1).
  /// @param address Target monitor RS-485 bus address.
  /// @param source SOURCE_ANALOG (0x01) or SOURCE_DIGITAL_AES3 (0x02).
  /// @param channel AES3_CHANNEL_A (0x01), AES3_CHANNEL_B (0x02), or AES3_CHANNEL_SUM (0x03).
  /// @param is_subwoofer True if monitor is a 7xxx series subwoofer.
  void send_audio_source_frame(uint8_t address, uint8_t source, uint8_t channel, bool is_subwoofer);

  /// @brief Set system power / standby state.
  /// @param standby True to place monitors into amplifier standby (<0.5W), false to wake up.
  void set_standby(bool standby);

  /// @brief Pulse a monitor's front LED for a set duration to identify its physical position.
  /// @param serial_or_id Serial number string or space-separated hex unique ID.
  /// @param duration_ms Pulse duration in milliseconds (default 5000 ms).
  void identify_monitor_by_serial(const std::string &serial_or_id, uint32_t duration_ms = 5000);

  /// @brief Pulse a monitor's front LED by its current logical address.
  /// @param address Logical bus address (0x02..0x7F).
  /// @param duration_ms Pulse duration in milliseconds (default 5000 ms).
  void identify_monitor_by_address(uint8_t address, uint32_t duration_ms = 5000);

  /// @brief Manually trigger a fresh active RACE discovery cycle.
  void rediscover_monitors();

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
  bool is_glm_active() const { return arbiter_.is_active(); }

  /// @brief Send a high-level GenSAM Frame onto the bus.
  /// @param frame The frame to serialize and transmit.
  /// @return True if transmission was accepted, false if blocked by arbitration or uninitialized.
  bool send_frame(const Frame &frame);

  /// @brief Send a frame twice, separated by a 250 us transceiver turnaround gap.
  ///
  /// Genelec monitors occasionally miss a single unicast control frame during RS-485
  /// direction turnaround, so mute, crossover, and identify commands are sent twice.
  /// @param frame The frame to serialize and transmit.
  /// @return True if at least one of the two transmissions was accepted.
  bool send_frame_twice(const Frame &frame);

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

  /// @brief Send broadcast standby sequence to place SAM monitors into low-power sleep (<0.5W).
  void send_standby();

  /// @brief Trigger a fresh active RACE discovery cycle to detect and assign unaddressed monitors.
  void start_race_discovery();

  /// @brief Access the registry table of discovered monitors, keyed by logical address.
  /// @return Map of address to GenSAMMonitor descriptors.
  const std::map<uint8_t, GenSAMMonitor> &get_monitors() const { return registry_.monitors(); }

  /// @brief Look up a discovered monitor by its logical RS-485 address.
  /// @param address Target address (0x02..0x7F).
  /// @return Pointer to GenSAMMonitor descriptor, or nullptr if not registered.
  GenSAMMonitor *get_monitor(uint8_t address) { return registry_.find(address); }

  /// @brief Look up a discovered monitor by its logical RS-485 address (read-only).
  /// @param address Target address (0x02..0x7F).
  /// @return Const pointer to GenSAMMonitor descriptor, or nullptr if not registered.
  const GenSAMMonitor *get_monitor(uint8_t address) const { return registry_.find(address); }

 protected:
  /// @brief Configure a GPIO as a push-pull output and drive it HIGH.
  /// @param pin GPIO number.
  /// @param pull_up Whether to enable the internal pull-up (used for the idle TX line).
  /// @param description Human-readable pin role, for the confirmation log line.
  void drive_output_pin_(int pin, bool pull_up, const char *description);

  /// @brief Power and enable the RS-485 transceiver, in the order the hardware requires.
  void setup_transceiver_pins_();

  /// @brief Bring up the 9-bit RMT transceiver, RX-only in listen-only mode.
  void setup_uart_();

  /// @brief Drain RX ring buffer, log raw bytes, feed the parser, and dispatch decoded frames.
  void process_rx_();

  /// @brief Restore the steady front LED on monitors whose identify pulse has expired.
  void check_identify_timeouts_();

  /// @brief Periodically emit the RX / framing statistics diagnostic line.
  void log_stats_();

  /// @brief Check whether the external GLM master inactivity timer has expired to reclaim bus control.
  void check_glm_cooldown_();

  // --- RACE discovery and telemetry polling (implemented in race.cpp) ---------------
  //
  // The phases and protocol are described in section 2 of this file's header comment.
  // Each phase is a non-blocking step function called once per loop() while its state is
  // current; it either waits, times out, or advances race_state_.  Every step is driven by
  // race_step_time_ against a named timeout, and the reply that ends a step arrives
  // separately through handle_active_reply_.

  /// @brief Advance the active RACE discovery and telemetry polling state machine.
  void update_race_state_machine_();

  /// @brief WAKEUP_SENT: wait for monitor DSPs to boot, then start the discovery race.
  void race_step_wakeup_(uint32_t now);

  /// @brief RACE_PING_SENT: on silence, conclude discovery and move to device interrogation.
  void race_step_ping_(uint32_t now);

  /// @brief RACE_SET_RID_SENT: retry the address assignment, or accept it unacknowledged.
  void race_step_set_rid_(uint32_t now);

  /// @brief QUERYING_DEVICES: walk the discovered monitors asking model, firmware, and serial.
  void race_step_querying_(uint32_t now);

  /// @brief CONFIGURING_DEVICES: walk the discovered monitors applying their configuration.
  void race_step_configuring_(uint32_t now);

  /// @brief POLLING_MONITORS: round-robin telemetry polling with a per-cycle keep-alive.
  void race_step_polling_(uint32_t now);

  /// @brief IDLE: periodically retry discovery while no monitors are registered.
  void race_step_idle_(uint32_t now);

  /// @brief Transmit one monitor's audio source and crossover configuration.
  /// @param mon The monitor to configure.
  /// @return True if any configuration frame was sent, false if the monitor needed nothing.
  bool configure_monitor_(const GenSAMMonitor &mon);

  /// @brief Broadcast the active volume followed by a STAY_ONLINE keep-alive.
  ///
  /// Sent when entering the polling loop and at the start of every polling cycle, to
  /// establish monitor gain and refresh the volatile RACE address leases.
  void broadcast_volume_and_keepalive_();

  /// @brief Process an incoming frame through the active state machine or passive snooping registry.
  /// @param frame The decoded frame to handle.
  void handle_incoming_frame_(const Frame &frame);

  /// @brief Consume a frame as the awaited reply to something the active state machine sent.
  /// @param frame The decoded frame to match against the current RaceState.
  /// @param now Current millis() timestamp.
  /// @return True if the frame was the expected reply and has been fully handled.
  bool handle_active_reply_(const Frame &frame, uint32_t now);

  // --- Passive snooping (implemented in snoop.cpp) ---------------------------------
  //
  // Every frame on the bus is snooped, whether GenSAM is the master or has yielded to an
  // external GLM controller.  This is how the hub learns state it did not command itself:
  // GLM's volume, mute, power, crossover, and source changes, and the monitors it addresses.
  //
  // The handlers below are deliberately independent and are all offered every frame, in the
  // order snoop_frame_ calls them.  That order matters in one place: snoop_addressing_ records
  // which monitor was last queried, and snoop_host_reply_ attributes a later host-addressed
  // reply to it.  Handlers must stay tolerant of frames from an unknown address, since GLM may
  // be addressing monitors this hub has not discovered yet.

  /// @brief Offer a frame to every passive snooping handler.
  /// @param frame The decoded frame to snoop.
  void snoop_frame_(const Frame &frame);

  /// @brief Track which monitor is being addressed, and adopt RACE assignments made by GLM.
  void snoop_addressing_(const Frame &frame);

  /// @brief Track master volume broadcasts.
  void snoop_volume_(const Frame &frame);

  /// @brief Track wakeup and standby power commands.
  void snoop_power_(const Frame &frame);

  /// @brief Track bypass / mute commands, per monitor or across the whole group.
  void snoop_mute_(const Frame &frame);

  /// @brief Track bass management crossover frequency commands.
  void snoop_crossover_(const Frame &frame);

  /// @brief Track audio source selection and AES3 sub-channel assignment.
  void snoop_audio_source_(const Frame &frame);

  /// @brief Attribute a monitor's reply to the host to whichever query was last observed.
  void snoop_host_reply_(const Frame &frame);

  /// @brief Advance the device interrogation cursor: software query -> barcode -> next monitor.
  ///
  /// Each monitor is asked two questions in turn during QUERYING_DEVICES; this steps to the
  /// next question, or to the next monitor once both have been asked or timed out.
  void advance_device_query_();

  /// @brief Complete address assignment for a monitor after receiving RID ACK.
  /// @param address The assigned logical address.
  void complete_rid_assignment_(uint8_t address);

  /// @brief Mark a monitor as recently seen, re-evaluating system mute if it came back online.
  /// @param mon Reference to the active monitor.
  void mark_monitor_seen_(GenSAMMonitor &mon);

  /// @brief Check for monitors that have not responded within the stale timeout window and transition them offline.
  void check_monitor_timeouts_();

  /// @brief Re-evaluate whether all online monitors are muted and notify state callbacks if the state changed.
  void evaluate_system_mute_();

  /// @brief Broadcast transient digital silence (-130 dBFS) to all monitors prior to input switching.
  void silence_system_volume_();

  /// @brief Restore active listening volume across all monitors following input switching and settling.
  void restore_system_volume_();

  /// @brief Run an input-switching operation between a transient silence and a volume restore.
  ///
  /// Prevents audible pops during analog multiplexer switching, AES3 PLL relock, and SRC
  /// relock: audio is broadcast down to digital silence, @p switch_inputs transmits the source
  /// selection frames, and the previous listening volume is restored once monitors have settled.
  /// Silencing is skipped when the system is in standby or the bus is unavailable, since there
  /// is then no listening signal to protect.  Blocks the main loop for the settling duration.
  /// @param switch_inputs Operation transmitting the source selection frames.
  void with_transient_silence_(const std::function<void()> &switch_inputs);

  /// @brief Evaluate and publish operational bus status ("Active", "GLM Active", "Discovering", "Configuring", "Standby", "Offline").
  void update_bus_status_();

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

  // Bus arbitration state (external GLM master detection and yield decision)
  BusArbiter arbiter_;
  uint32_t last_tx_blocked_warning_{0};
  uint32_t last_stat_log_{0};

  // Monitor registry, configured bindings, and all Home Assistant entity publishing
  MonitorRegistry registry_;

  // Active RACE & query state machine
  RaceState race_state_{RaceState::IDLE};
  uint8_t next_assign_addr_{MONITOR_START_ADDR};
  std::vector<uint8_t> current_racing_bytes_;
  uint32_t current_racing_id_{0};
  uint32_t race_step_time_{0};
  uint8_t rid_retries_{0};
  uint8_t current_query_addr_{0};
  uint8_t current_query_cmd_{0};
  uint32_t last_poll_cycle_time_{0};
  uint32_t last_poll_step_time_{0};
  uint32_t last_timeout_check_{0};
  uint32_t last_discovery_retry_time_{0};
  size_t current_poll_index_{0};
  std::vector<uint8_t> poll_addrs_;
  bool initial_discovery_done_{false};
  uint32_t boot_time_{0};

  // Passive snooping tracker
  uint8_t last_queried_addr_{0};
  uint8_t last_queried_cmd_{0};

  // Volume, mute, standby, and binding state
  float min_volume_db_{-80.0f};
  float max_volume_db_{0.0f};
  float startup_volume_db_{-30.0f};
  float current_volume_db_{-30.0f};
  bool current_mute_{false};
  bool current_standby_{false};
  void notify_state_callbacks_();

  binary_sensor::BinarySensor *glm_usb_adapter_active_sensor_{nullptr};
  text_sensor::TextSensor *bus_status_sensor_{nullptr};
  std::string last_bus_status_{};
  number::Number *volume_number_{nullptr};
  select::Select *audio_source_select_{nullptr};
  uint8_t current_audio_source_{SOURCE_ANALOG};
  bool audio_source_configured_{false};
  std::vector<std::function<void(float, bool, bool)>> state_callbacks_;
  std::vector<std::function<void(const std::string &)>> bus_status_callbacks_;
};

}  // namespace gensam
}  // namespace esphome
