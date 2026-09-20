#pragma once

/// @file select.h
/// @brief ESPHome Select entities for per-monitor input routing and group preset selection.
///
/// ===================================================================================
/// ARCHITECTURAL DESIGN RATIONALE
/// ===================================================================================
/// 1. Per-Monitor Input Routing (GenSAMInputSelect):
///    SAM monitors and subwoofers choose between an analog line input and one sub-channel of
///    an AES3 (AES/EBU) stream, via CMD_SELECT_AUDIO_SOURCE (0x40).  The wire payload is a
///    4-byte descriptor, [input_index, source, pair_selector, aes3_channel]:
///    - Analog: source 0x01.  Input 0 uses pair selector 0x02; a 7xxx subwoofer needs a
///      second frame for input 1 with pair selector 0x01.
///    - AES3: source 0x02, pair selector 0x00, sub-channel in the last byte - 0x01 channel A
///      (left), 0x02 channel B (right), 0x03 A+B summed, which is what a subwoofer takes.
///      A subwoofer again needs a second frame for input 1.
///
///    Source and sub-channel are presented as *one* select per monitor rather than two,
///    because they are not independent settings: a speaker is fed from one place, and the
///    sub-channel means nothing unless that place is the AES3 receiver.  Genelec's own setup
///    file stores exactly this, as a single per-device `Input:` enum with four values.  Two
///    entities would also permit displaying contradictory pairs such as analog with channel B.
///
///    Routing is per monitor, not per system.  That is a requirement rather than a refinement:
///    a captured GLM group runs its subwoofer on AES3 sum while both main monitors are analog,
///    so no single system-wide source can describe it.  An earlier version of this component
///    had one global source select, which could not represent that setup; it was removed.
///
///    "Automatic" (source 0x03) is deliberately absent.  The HLM protocol specification lists
///    it, but it is a *standalone* setting - what a speaker does on its own, not live input
///    selection - neither project has observed it on the wire, so its pair selector and
///    channel bytes are unknown, and it would need a flash commit (0x15) to stick, which this
///    component does not implement.  A snooped 0x03 is handled defensively in
///    snoop_audio_source_().
///
/// 2. Group Preset Selection (GenSAMGroupSelect):
///    Selecting a group re-pushes a whole calibrated DSP block - filters, levels, delays,
///    crossover and input routing - to every speaker, which is what GLM does.  A group push
///    also drives the input selects above, so they show what the speakers were last told.
///
/// 3. Manual Overrides Are Temporary By Construction:
///    Changing a monitor's input by hand transmits immediately and updates its binding, but
///    nothing about the active group changes, so the next group push - a group switch, a
///    standby cycle, or a rediscovery - restores that group's routing.  The deviation is
///    visible meanwhile through the hub's "Group Modified" binary sensor.
///
/// 4. Non-Destructive Boot:
///    Monitors hold their input routing in their own flash.  A binding starts unconfigured and
///    transmits nothing until a group is applied, Home Assistant selects something, or a GLM
///    frame is snooped, so powering this component up never overwrites what the speakers had.
///
/// 5. Volatile Persistence & Standby Wakeup Retransmission:
///    Monitors lose volatile DSP state in amplifier sleep, so the configured routing is
///    re-transmitted during CONFIGURING_DEVICES after every rediscovery.
///
/// 6. Passive Bus Snooping:
///    0x40 frames from external GLM software are snooped, updating the owning monitor's select
///    without bus contention.
/// ===================================================================================

#include "esphome/core/component.h"
#include "esphome/components/select/select.h"
#include "hub.h"
#include "const.h"

#include <string>

namespace esphome {
namespace gensam {

/// @brief Select entity that chooses the active GLM group preset.
///
/// A group preset is a whole calibrated monitoring configuration - per-speaker room EQ, level
/// trim, alignment delay, crossover and input routing - so selecting one pushes several
/// hundred frames.  That happens asynchronously: control() only records the choice, and the
/// hub's state machine transmits it over the following few hundred milliseconds.
class GenSAMGroupSelect : public select::Select {
 public:
  /// @brief Set the parent GenSAMHub instance.
  /// @param hub Pointer to the GenSAMHub.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

 protected:
  /// @brief Action executed when the user picks a group in Home Assistant.
  /// @param value Selected group name, as declared in the `groups:` configuration.
  void control(const std::string &value) override {
    if (hub_ != nullptr) {
      hub_->set_active_group_by_name(value);
    }
  }

  GenSAMHub *hub_{nullptr};
};

/// @brief Select entity that routes one monitor's input: analog, or an AES3 sub-channel.
class GenSAMInputSelect : public select::Select {
 public:
  /// @brief Set the parent GenSAMHub instance.
  /// @param hub Pointer to the GenSAMHub.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

  /// @brief Set the target speaker's factory serial number or unique ID string.
  /// @param id Serial number string or unique ID string.
  void set_serial_or_id(const std::string &id) { serial_or_id_ = id; }

 protected:
  /// @brief Action executed when the user selects an input option in Home Assistant.
  /// @param value One of the INPUT_STR_* option strings.
  void control(const std::string &value) override {
    if (hub_ != nullptr) {
      hub_->set_monitor_input_by_name(serial_or_id_, value);
    }
  }

  GenSAMHub *hub_{nullptr};
  std::string serial_or_id_{};
};

}  // namespace gensam
}  // namespace esphome

