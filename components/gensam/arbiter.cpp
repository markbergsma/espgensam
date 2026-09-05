/// @file arbiter.cpp
/// @brief Bus arbitration between GenSAM and an external Genelec GLM master.
/// See arbiter.h for architectural design rationale and complete API documentation.

#include "arbiter.h"

namespace esphome {
namespace gensam {

BusArbiter::Verdict BusArbiter::classify(const Frame &frame, uint32_t now) const {
  // While yielded GenSAM transmits nothing, so no frame on the bus can be its own.
  if (active_) {
    return Verdict::EXTERNAL_MASTER;
  }

  if (frame.address != HOST_ADDRESS) {
    // A query heading out to a monitor: ours only while our own frame may still be echoing back.
    return (now - last_our_tx_ > ECHO_WINDOW_MS) ? Verdict::EXTERNAL_MASTER : Verdict::LOOPBACK_ECHO;
  }

  // A reply heading to the host: ours only if we asked recently enough to still be waiting.
  if (last_our_tx_ == 0 || now - last_our_tx_ > REPLY_WINDOW_MS) {
    return Verdict::EXTERNAL_MASTER;
  }
  return Verdict::OUR_REPLY;
}

bool BusArbiter::note_external_activity(uint32_t now) {
  last_activity_ = now;
  if (active_) {
    return false;
  }
  active_ = true;
  return true;
}

bool BusArbiter::check_cooldown(uint32_t now, uint32_t cooldown_ms) {
  if (!active_ || (now - last_activity_ < cooldown_ms)) {
    return false;
  }
  active_ = false;
  return true;
}

}  // namespace gensam
}  // namespace esphome
