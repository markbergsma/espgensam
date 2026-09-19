#pragma once

/// @file arbiter.h
/// @brief Half-duplex bus arbitration between GenSAM and an external Genelec GLM master.
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// The RS-485 bus supports only one master transmitting at any instant - normally that
/// would be the GLM USB adapter talking to the GLM application.  BusArbiter decides, for
/// every received frame, whether GenSAM is looking at its own traffic or somebody else's,
/// and holds the resulting yield state.
///
/// 1. Yielding & Passive Snooping:
///    When GenSAM detects bus traffic from an external controller it immediately yields the
///    bus.  While yielded, GenSAM enters PASSIVE SNOOPING mode: it continues reading and
///    parsing all wire frames, learning monitor addresses and updating telemetry from GLM's
///    queries, without transmitting any pulses on the wire.  If no external GLM traffic is
///    observed for the inactivity cooldown (default 30 s), GenSAM resumes active master control.
///
/// 2. Why Three Classification Rules:
///    A half-duplex RS-485 transceiver echoes everything GenSAM transmits back onto its own
///    RX line, so "a frame arrived" is not by itself evidence of another master.  The origin
///    of a frame is inferred from its destination address and its distance in time from our
///    last transmission:
///    - Already yielded: every frame on the bus belongs to the external GLM ecosystem.
///      GenSAM is transmitting nothing, so nothing can be its own echo.
///    - Not addressed to the host (a query going out to a monitor): ours only if we
///      transmitted within the echo window, otherwise another master is polling.
///    - Addressed to the host (a monitor's reply): ours only if we transmitted within the
///      reply window, which is longer because it must cover the monitor's turnaround and
///      response time.  A host-addressed frame before GenSAM has ever transmitted is
///      necessarily a reply to somebody else.
///
/// 3. Timing Windows:
///    The echo window (50 ms) bounds how long our own outbound frame can still be draining
///    back through the receiver.  The reply window (300 ms) bounds how long a monitor may
///    take to answer a query of ours.  Both are generous relative to the 288,000 baud wire
///    rate; they trade a slightly slower external-master detection for never mistaking our
///    own conversation for an intruder and needlessly abandoning the bus.
/// ===================================================================================

#include <cstdint>

#include "const.h"
#include "frame.h"

namespace esphome {
namespace gensam {

/// @brief Tracks external GLM master activity and holds the bus yield decision.
class BusArbiter {
 public:
  /// @brief Origin attributed to a received frame.
  enum class Verdict : uint8_t {
    EXTERNAL_MASTER,  ///< Another master is driving the bus; GenSAM must yield.
    OUR_REPLY,        ///< A monitor answering a query GenSAM sent.
    LOOPBACK_ECHO,    ///< GenSAM's own outbound frame echoed back by the transceiver.
  };

  /// @brief Record that GenSAM transmitted on the bus.
  /// @param now Current millis() timestamp.
  void note_tx(uint32_t now) { last_our_tx_ = now; }

  /// @brief Attribute an incoming frame to its likely origin. See rationale §2.
  /// @param frame The decoded frame received from the bus.
  /// @param now Current millis() timestamp.
  /// @return The origin verdict; does not mutate arbitration state.
  Verdict classify(const Frame &frame, uint32_t now) const;

  /// @brief Record observed external master activity, yielding the bus if not already yielded.
  /// @param now Current millis() timestamp.
  /// @return True if this observation caused the transition into the yielded state.
  bool note_external_activity(uint32_t now);

  /// @brief Reclaim the bus if the external master has been silent for the cooldown period.
  /// @param now Current millis() timestamp.
  /// @param cooldown_ms Silence duration required before resuming active master control.
  /// @return True if this call reclaimed the bus.
  bool check_cooldown(uint32_t now, uint32_t cooldown_ms);

  /// @brief Whether an external GLM master currently holds the bus.
  bool is_active() const { return active_; }

  /// @brief Timestamp (millis) of the most recently observed external master frame.
  uint32_t last_activity() const { return last_activity_; }

 protected:
  /// Maximum age of our own transmission for an inbound non-host frame to be our echo.
  static constexpr uint32_t ECHO_WINDOW_MS = 50;

  /// Maximum age of our own transmission for an inbound host-addressed frame to be our reply.
  static constexpr uint32_t REPLY_WINDOW_MS = 300;

  bool active_{false};            ///< True while the bus is yielded to an external master.
  uint32_t last_activity_{0};     ///< millis() of the last external master frame observed.
  uint32_t last_our_tx_{0};       ///< millis() of our last transmission; 0 if we never transmitted.
};

}  // namespace gensam
}  // namespace esphome
