#pragma once

/// @file iss.h
/// @brief How the ISS sleep delay and sensitivity are named for Home Assistant.
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// A SAM speaker's Intelligent Signal Sensing (ISS) puts the amplifiers to sleep after a
/// period without input above a threshold. CMD_WAKEUP carries both settings as a 32-bit
/// big-endian word: sub-command 0x01 the delay in minutes (ISS_SLEEP_DELAY_NEVER disables
/// sleep), 0x02 the threshold in dBu as a two's-complement integer (GLM bus decoding guide
/// v3.0, section 11.2). Setting the 0x80 flag on the sub-command reads a value back instead
/// (make_iss_read); 0x04 is the read-only measured input level that hub-managed sleep samples.
///
/// 1. Runtime only. GLM follows each write with CMD_SERVICE 0x33 (store working settings).
///    This component never sends that. Doing so would write the speaker's whole working state
///    into its start-up profile, including whatever calibration a group push left there. A
///    speaker restarts on every wake from ISS sleep and returns to its stored values, so the
///    hub re-sends an override after each discovery and group push. The consequence is that
///    an override governs the *next* sleep after each wake, which is the one that matters.
///    A 3A 01 delay write takes effect without the store (verified on 8351B firmware 1.24).
///
/// 2. Select, not number. GLM offers a fixed menu - seven delays plus Never, three named
///    sensitivities - and the options here are that menu. Each select also has
///    ISS_STR_SPEAKER_DEFAULT, which stops overriding. Choosing it sends nothing; the speaker
///    returns to its own setting at its next restart, and hub-managed sleep reads that setting
///    back from each speaker instead of assuming one. A value of 0 means "no override" in both conversions below: no menu
///    entry uses a zero delay, and 0 dBu is far outside the usable −80…−35 dBu range.
///
/// 3. Kept apart from const.h and select.h for the same reason as crossover.h: option strings
///    are presentation, and the hub needs them without referencing an entity type.
/// ===================================================================================

#include "const.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace gensam {

/// Option string shared by both selects: do not override the speaker's own setting.
static constexpr const char *ISS_STR_SPEAKER_DEFAULT = "Speaker default";

/// Sleep delay value meaning "no override".
static constexpr uint32_t ISS_SLEEP_DELAY_DEFAULT = 0;

/// Threshold value meaning "no override".
static constexpr int32_t ISS_THRESHOLD_DEFAULT = 0;

/// @brief One select option and the wire value it stands for.
template<typename T> struct IssOption {
  const char *name;  ///< Option string, exactly as Home Assistant shows it.
  T value;           ///< Wire value, or the *_DEFAULT sentinel.
};

/// Sleep delay options in display order; __init__.py must list the same strings.
static constexpr IssOption<uint32_t> ISS_SLEEP_DELAY_OPTIONS[] = {
    {ISS_STR_SPEAKER_DEFAULT, ISS_SLEEP_DELAY_DEFAULT},
    {"2 min", 2},
    {"10 min", 10},
    {"30 min", 30},
    {"60 min", 60},
    {"120 min", 120},
    {"180 min", 180},
    {"240 min", 240},
    {"Never", ISS_SLEEP_DELAY_NEVER},
};

/// Sensitivity options in display order; __init__.py must list the same strings.
static constexpr IssOption<int32_t> ISS_THRESHOLD_OPTIONS[] = {
    {ISS_STR_SPEAKER_DEFAULT, ISS_THRESHOLD_DEFAULT},
    {"Low (-61 dBu)", ISS_THRESHOLD_LOW_DBU},
    {"Medium (-68 dBu)", ISS_THRESHOLD_MEDIUM_DBU},
    {"High (-75 dBu)", ISS_THRESHOLD_HIGH_DBU},
};

/// @brief Find the option string for a value.
/// @param options Option table.
/// @param value Wire value or sentinel.
/// @return The option string, or nullptr when the value is not on the menu.
template<typename T, size_t N> const char *iss_option_name(const IssOption<T> (&options)[N], T value) {
  for (const auto &option : options) {
    if (option.value == value) {
      return option.name;
    }
  }
  return nullptr;
}

/// @brief Parse an option string back into its value.
/// @param options Option table.
/// @param name Option string; must match exactly.
/// @param[out] value Receives the wire value or sentinel.
/// @return False for an unrecognised string, leaving @p value untouched.
template<typename T, size_t N>
bool iss_option_value(const IssOption<T> (&options)[N], const char *name, T &value) {
  for (const auto &option : options) {
    if (std::strcmp(option.name, name) == 0) {
      value = option.value;
      return true;
    }
  }
  return false;
}

}  // namespace gensam
}  // namespace esphome
