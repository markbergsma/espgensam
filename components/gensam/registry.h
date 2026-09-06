#pragma once

/// @file registry.h
/// @brief Registry of discovered Genelec SAM monitors and their Home Assistant entity bindings.
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// MonitorRegistry owns everything the hub knows about the monitors on the bus, and is the
/// only place that touches their Home Assistant entities.  Isolating it from the hub keeps
/// the bus protocol logic free of presentation concerns: the RACE state machine and the
/// passive GLM snooper both describe *what they learned*, and the registry decides what to
/// store and which entity to publish.
///
/// 1. Two-Layer Identity:
///    Monitors are keyed by their volatile RACE bus address (0x02..0x7F), which is reassigned
///    on every discovery cycle and reset whenever monitors pass through <0.5W standby.  The
///    stable identity is the factory serial number or the decimal hardware ID, which is what
///    a GenSAMMonitorBinding matches against.  Lookups by user-facing name, serial, or ID
///    therefore search both layers: the monitor's own fields first, then its bound binding's.
///
/// 2. Binding Pointer Stability:
///    GenSAMMonitor::binding is a raw pointer into the bindings vector held here.  Bindings
///    are appended exclusively during ESPHome code generation, before setup() runs, and are
///    never added, removed, or reordered afterwards.  Any future runtime mutation of the
///    bindings vector would reallocate it and dangle every bound monitor's pointer.
///
/// 3. Online State & Staleness:
///    A monitor is "online" from the moment any valid frame is attributed to it until it has
///    failed to respond for the staleness window.  Both transitions publish the online binary
///    sensor.  Transitions are reported back to the caller rather than acted on here, because
///    the derived system-wide mute state belongs to the hub, and evaluating it midway through
///    a batch of offline transitions would publish spurious intermediate states.
/// ===================================================================================

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "monitor.h"

namespace esphome {
namespace gensam {

/// @brief Owns the discovered monitor table, the configured bindings, and all entity publishing.
class MonitorRegistry {
 public:
  /// @brief Register a configured monitor binding. Call only before setup(); see rationale §2.
  /// @param b The binding to append.
  void add_binding(const GenSAMMonitorBinding &b) { bindings_.push_back(b); }

  /// @brief Access the configured bindings (read-only).
  const std::vector<GenSAMMonitorBinding> &bindings() const { return bindings_; }

  /// @brief Look up a monitor by logical bus address, creating an entry if none exists.
  ///
  /// A newly created entry has its address field populated; callers typically follow with
  /// mark_seen() and bind_if_matched().
  /// @param address Logical bus address (0x02..0x7F).
  /// @return Reference to the existing or newly created monitor descriptor.
  GenSAMMonitor &get_or_create(uint8_t address);

  /// @brief Look up a monitor by logical bus address.
  /// @param address Logical bus address (0x02..0x7F).
  /// @return Pointer to the monitor, or nullptr if not registered.
  GenSAMMonitor *find(uint8_t address);

  /// @brief Look up a monitor by logical bus address (read-only).
  /// @param address Logical bus address (0x02..0x7F).
  /// @return Const pointer to the monitor, or nullptr if not registered.
  const GenSAMMonitor *find(uint8_t address) const;

  /// @brief Look up a discovered monitor by serial number, decimal hardware ID, or binding name.
  /// @param serial_or_id Serial number string, decimal unique ID string, or configured speaker name.
  /// @return Pointer to the matching monitor, or nullptr if none is currently discovered.
  GenSAMMonitor *find_by_serial_or_id(const std::string &serial_or_id);

  /// @brief Look up a configured binding by serial number, decimal hardware ID, or name.
  ///
  /// Unlike find_by_serial_or_id(), this also succeeds for speakers that are configured but
  /// not currently present on the bus, so settings can be stored ahead of discovery.
  /// @param serial_or_id Serial number string, decimal unique ID string, or configured speaker name.
  /// @return Pointer to the matching binding, or nullptr if no binding matches.
  GenSAMMonitorBinding *find_binding_by_serial_or_id(const std::string &serial_or_id);

  /// @brief Match a monitor against the configured bindings and attach the first match.
  ///
  /// On a successful match the monitor adopts the binding's serial number if it has none yet,
  /// and its metadata, online, crossover, and AES3 entities are published.  No-op if already bound.
  /// @param mon The monitor to bind.
  void bind_if_matched(GenSAMMonitor &mon);

  /// @brief Record that a monitor responded, refreshing its staleness timer.
  /// @param mon The monitor that was seen.
  /// @return True if the monitor transitioned from offline to online.
  bool mark_seen(GenSAMMonitor &mon);

  /// @brief Transition monitors offline that have not responded within the staleness window.
  /// @param now Current millis() timestamp.
  /// @param stale_timeout_ms Silence duration after which a monitor is considered offline.
  /// @return True if any monitor changed state.
  bool expire_stale(uint32_t now, uint32_t stale_timeout_ms);

  /// @brief Mark every monitor offline and clear its staleness timestamp.
  ///
  /// Used before a rediscovery cycle wipes the table, so Home Assistant does not keep
  /// reporting monitors under bus addresses that are about to be reassigned.
  void invalidate_all();

  /// @brief Transition all currently online monitors offline (used when entering standby).
  void mark_all_offline();

  /// @brief Discard all discovered monitors. Bindings are retained.
  void clear() { monitors_.clear(); }

  /// @brief Publish model, serial, firmware, and hardware ID to the bound text sensors.
  /// @param mon The monitor whose metadata to publish.
  void publish_metadata(const GenSAMMonitor &mon);

  /// @brief Publish temperature, input level, output level, and online state to bound sensors.
  /// @param mon The monitor whose telemetry to publish.
  void publish_telemetry(const GenSAMMonitor &mon);

  /// @brief Publish a monitor's online state to its bound binary sensor.
  /// @param mon The monitor whose online state to publish.
  void publish_online(const GenSAMMonitor &mon);

  /// @brief Publish a monitor's current mute state to its bound mute switch.
  /// @param mon The monitor whose mute state to publish.
  void publish_mute(const GenSAMMonitor &mon);

  /// @brief Update a monitor's mute state and publish it to the bound mute switch.
  /// @param mon The monitor to update.
  /// @param mute New mute state.
  void set_mute(GenSAMMonitor &mon, bool mute);

  /// @brief Store a crossover frequency on a binding and publish it to the bound number entity.
  /// @param b The binding to update.
  /// @param freq_hz Crossover filter frequency in Hz.
  void set_binding_crossover(GenSAMMonitorBinding &b, uint16_t freq_hz);

  /// @brief Store an AES3 sub-channel on a binding and publish it to the bound select entity.
  /// @param b The binding to update.
  /// @param channel AES3_CHANNEL_A, _B, or _SUM.
  void set_binding_aes3_channel(GenSAMMonitorBinding &b, uint8_t channel);

  /// @brief Store a monitor's crossover frequency on its binding and publish it.
  /// @param mon The monitor to update; no-op if unbound.
  /// @param freq_hz Crossover filter frequency in Hz.
  void set_crossover(GenSAMMonitor &mon, uint16_t freq_hz);

  /// @brief Store a monitor's AES3 sub-channel on its binding and publish it.
  /// @param mon The monitor to update; no-op if unbound.
  /// @param channel AES3_CHANNEL_A, _B, or _SUM.
  void set_aes3_channel(GenSAMMonitor &mon, uint8_t channel);

  /// @brief Determine whether every online monitor is muted.
  /// @param[out] any_online Set to true if at least one monitor is online.
  /// @return True if all online monitors are muted (meaningless when @p any_online is false).
  bool all_online_muted(bool &any_online) const;

  /// @brief Whether any registered monitor is currently responding.
  ///
  /// Distinct from empty(): monitors that go stale are marked offline but kept, so they retain
  /// their identity and bindings across a silence. Liveness, not registry population, is what
  /// tells the state machine whether the bus still has anyone on it.
  bool any_online() const;

  /// @brief Determine whether every online monitor that reports a power state is in standby.
  ///
  /// Monitors that have never reported one are ignored: Format A telemetry carries no power
  /// field, and not every TLV reply includes the 'G' tag.
  /// @param[out] any_reported Set to true if at least one online monitor has reported its power state.
  /// @return True if all such monitors are in standby (meaningless when @p any_reported is false).
  bool all_online_in_standby(bool &any_reported) const;

  /// @brief Snapshot of all registered logical bus addresses, in ascending order.
  std::vector<uint8_t> addresses() const;

  /// @brief Access the discovered monitor table.
  std::map<uint8_t, GenSAMMonitor> &monitors() { return monitors_; }

  /// @brief Access the discovered monitor table (read-only).
  const std::map<uint8_t, GenSAMMonitor> &monitors() const { return monitors_; }

  /// @brief Number of discovered monitors.
  size_t size() const { return monitors_.size(); }

  /// @brief Whether no monitors have been discovered.
  bool empty() const { return monitors_.empty(); }

 protected:
  /// Discovered monitors, keyed by volatile RACE bus address.
  std::map<uint8_t, GenSAMMonitor> monitors_;

  /// Configured bindings; pointer-stable for the component's lifetime (see rationale §2).
  std::vector<GenSAMMonitorBinding> bindings_;
};

}  // namespace gensam
}  // namespace esphome
