#pragma once

/// @file input.h
/// @brief How a monitor's input routing is named for Home Assistant, and converted to the wire.
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// CMD_SELECT_AUDIO_SOURCE carries source and AES3 sub-channel as two separate payload bytes,
/// and const.h defines them that way because that is what the protocol says.  They are not,
/// however, two independent settings: a speaker is fed from exactly one place, and the
/// sub-channel means nothing unless that place is the AES3 receiver.  Genelec agrees - a GLM
/// setup file stores one per-device `Input:` enum whose four values are precisely the
/// combinations below.
///
/// This file is the join between those two views: one name per combination for the user
/// interface, and the byte pair for the wire.  It lives apart from const.h deliberately.
/// const.h is the protocol vocabulary and has no opinion about how anything is displayed;
/// these strings are presentation, and would be the first thing to change for a translation or
/// a renamed option.  It is also apart from select.h, because the conversion is needed by the
/// registry, the snoop decoder and the group push - none of which may reference an entity
/// type, per the layering rule in AGENTS.md section 2.
/// ===================================================================================

#include "const.h"

namespace esphome {
namespace gensam {

/// @name Input routing option strings
///
/// The four combinations a SAM device can be fed from, in the order a select should offer
/// them.  These are the entity's option list, so changing one changes what Home Assistant
/// stores and what str_to_input() must accept.
///@{
static constexpr const char *INPUT_STR_ANALOG = "Analog";
static constexpr const char *INPUT_STR_AES3_A = "AES3 Channel A (Left)";
static constexpr const char *INPUT_STR_AES3_B = "AES3 Channel B (Right)";
static constexpr const char *INPUT_STR_AES3_SUM = "AES3 Channel A+B (Sum)";
///@}

/// @brief Map a source / sub-channel pair to its option string.
/// @param source SOURCE_ANALOG or SOURCE_DIGITAL_AES3.
/// @param channel AES3 sub-channel; ignored when @p source is analog.
/// @return The matching option string.  Anything unrecognised reads as analog, which is the
///         safe default: it is the one input every SAM device has.
inline const char *input_to_str(uint8_t source, uint8_t channel) {
  if (source != SOURCE_DIGITAL_AES3) {
    return INPUT_STR_ANALOG;
  }
  return (channel == AES3_CHANNEL_B) ? INPUT_STR_AES3_B :
         (channel == AES3_CHANNEL_SUM) ? INPUT_STR_AES3_SUM : INPUT_STR_AES3_A;
}

/// @brief Parse an option string back into a source / sub-channel pair.
///
/// Compares character by character rather than through std::string, so that this header stays
/// usable from the protocol layer without dragging in <string>.
/// @param name Option string, as published by input_to_str().
/// @param[out] source Receives SOURCE_ANALOG or SOURCE_DIGITAL_AES3.
/// @param[out] channel Receives the AES3 sub-channel, or AES3_CHANNEL_A when analog.
/// @return False for an unrecognised string, leaving both outputs untouched.
inline bool str_to_input(const char *name, uint8_t &source, uint8_t &channel) {
  struct Entry {
    const char *name;
    uint8_t source;
    uint8_t channel;
  };
  static constexpr Entry TABLE[] = {
      {INPUT_STR_ANALOG, SOURCE_ANALOG, AES3_CHANNEL_A},
      {INPUT_STR_AES3_A, SOURCE_DIGITAL_AES3, AES3_CHANNEL_A},
      {INPUT_STR_AES3_B, SOURCE_DIGITAL_AES3, AES3_CHANNEL_B},
      {INPUT_STR_AES3_SUM, SOURCE_DIGITAL_AES3, AES3_CHANNEL_SUM},
  };
  for (const Entry &e : TABLE) {
    const char *a = name;
    const char *b = e.name;
    while (*a != '\0' && *a == *b) {
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0') {
      source = e.source;
      channel = e.channel;
      return true;
    }
  }
  return false;
}

}  // namespace gensam
}  // namespace esphome
