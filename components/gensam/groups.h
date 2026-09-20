#pragma once

/// @file groups.h
/// @brief Flash-resident description of the GLM group presets declared in YAML.
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// A GLM "group preset" is a named monitoring configuration: which speakers take part, how
/// each is fed, and the AutoCal result for each of them at one listening position.  Selecting
/// a group in GLM re-pushes that whole DSP block to every speaker, and this is the data that
/// makes the same thing possible here.
///
/// 1. Immutable, Flash-Resident, Generated:
///    These structures are never built at runtime.  The ESPHome code generator emits one
///    constant table from the `groups:` YAML and the hub only ever reads it, so the several
///    kilobytes of filter parameters live in flash rather than RAM.  A three-group setup for
///    four speakers is about 3 kB.  Nothing here owns memory or has a non-trivial
///    constructor, which is what keeps that true.
///
/// 2. Parameters, Not Coefficients:
///    Bands are stored the way the GLM user interface and the .sam setup file express them -
///    frequency, gain, Q - and converted to biquad coefficients by design_biquad() at the
///    moment they are transmitted.  Storing finished coefficients would be smaller and
///    faster, but it would make the YAML unreadable, unreviewable and impossible to edit by
///    hand, and it would rule out ever exposing live EQ controls.  The conversion costs a few
///    transcendentals per band on a core with an FPU, once per group change.
///
/// 3. Why The Design Rate Is Absent Here:
///    A band's coefficients depend on the rate it is designed at - 48 kHz for two-way
///    monitors, 12 kHz for subwoofers - but that is a property of the hardware, identical in
///    every group, and already derivable from the discovered model.  Storing it per band
///    would repeat one number several hundred times and create a second, conflicting source
///    of truth.  It is resolved at apply time instead.
///
/// 4. Joining To Hardware:
///    A device is identified by its GLM hardware id, the same decimal number a .sam file
///    calls `Serial:` and this component's `monitors:` block calls `unique_id`.  Bus
///    addresses cannot be used: they are leased by the RACE protocol and change across a
///    standby cycle.
///
/// 5. Input Routing Is Per Device, Not Per Group:
///    It would be natural to give a group one input mode, and GLM's own interface suggests
///    that, but the setup file does not work that way and neither does the hardware: one
///    captured group runs its subwoofer on AES3 sum while both main monitors are analog.
///    Source and sub-channel therefore live on the device.
/// ===================================================================================

#include <cstdint>

#include "biquad.h"
#include "const.h"

namespace esphome {
namespace gensam {

/// @brief One parametric EQ band, as the user interface expresses it.
///
/// Slot position is implied by the index in GroupDevice::bands, and is also the wire index in
/// the CMD_DSP PEQ frame.  Genelec assigns those slots by device class: a two-way monitor
/// uses 0-1 for low shelves, 2-3 for high shelves and 4-19 for peaking bands, while a
/// subwoofer uses all 20 as peaking bands and has no shelves.  That layout is the generator's
/// responsibility; nothing here enforces it.
struct PeqBand {
  PeqType type{PeqType::BYPASS};  ///< Filter shape; BYPASS leaves the slot inactive.
  float frequency_hz{0.0f};       ///< Centre frequency (peaking) or corner (shelving), Hz.
  float gain_db{0.0f};            ///< Gain in decibels. Exactly 0 transmits the bypass vector.
  float q{0.0f};                  ///< Quality factor; peaking only, ignored by the shelves.
};

/// @brief One speaker's configuration within a group.
struct GroupDevice {
  uint32_t unique_id{0};        ///< GLM hardware id; joins to a configured monitor binding.

  /// False for a speaker that is present in the setup but silent in this group - the setup
  /// file's `Group_ON: 0`, shown as a yellow front LED by GLM. Such a device is muted rather
  /// than reconfigured, so its DSP state is left alone.
  bool enabled{true};

  uint16_t crossover_hz{DEFAULT_CROSSOVER_HZ};  ///< Bass management crossover for this device.
  uint8_t source{SOURCE_ANALOG};                ///< SOURCE_ANALOG or SOURCE_DIGITAL_AES3.
  uint8_t aes3_channel{AES3_CHANNEL_A};         ///< AES3 sub-channel; ignored when analog.

  /// Per-device level trim from AutoCal, the setup file's Level_Sensitivity. Sent as
  /// CMD_DSP sub-command 0x01/0x00. Attenuation only: 0.0 is unity.
  float level_db{0.0f};

  /// Alignment delay in samples at DSP_DELAY_RATE_HZ, sent as CMD_DSP sub-command 0x02. On a
  /// subwoofer this is the AutoPhase result expressed as a delay; see const.h.
  uint32_t delay_samples{0};

  const PeqBand *bands{nullptr};  ///< PEQ_BAND_COUNT entries, in wire-index order.
  uint8_t band_count{0};          ///< Entries actually present; slots beyond it are bypassed.
};

/// @brief A named group preset.
struct GroupPreset {
  const char *name{nullptr};            ///< Display name; the Home Assistant select option.
  const GroupDevice *devices{nullptr};  ///< Devices taking part, in generation order.
  uint8_t device_count{0};
};

}  // namespace gensam
}  // namespace esphome
