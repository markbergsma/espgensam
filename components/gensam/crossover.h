#pragma once

/// @file crossover.h
/// @brief How a bass management crossover setting is named for Home Assistant, and validated.
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// CMD_BASS_MANAGE_XO carries one big-endian 16-bit word, and that word is not always a
/// frequency.  50..120 on a 5 Hz grid is the crossover in Hz; CROSSOVER_FULL_BAND (0x0001)
/// switches bass management off, so the speaker reproduces the full band on its own.  A GLM
/// setup file stores exactly the same values in `CrossoverFrequency(Hz)`, 1 included.
///
/// That makes the setting an enum rather than a quantity, which is why Home Assistant is
/// offered a select instead of a number: a slider has no honest place to put "off".  This
/// file is the join between the option strings and the wire word, kept apart from const.h and
/// select.h for the same reasons input.h is - presentation is not protocol vocabulary, and the
/// registry, the snoop decoder and the group push all need it without referencing an entity
/// type (AGENTS.md section 2).
/// ===================================================================================

#include "const.h"

#include <cstddef>
#include <cstdio>

namespace esphome {
namespace gensam {

/// Option string for CROSSOVER_FULL_BAND.  Every other option is "<n> Hz".
static constexpr const char *CROSSOVER_STR_FULL_BAND = "Full band";

/// Buffer size that holds any string crossover_to_str() produces, terminator included.
static constexpr size_t CROSSOVER_STR_SIZE = 12;

/// @brief Whether a wire word is one of the settings the select offers.
/// @param value CMD_BASS_MANAGE_XO payload word.
/// @return True for CROSSOVER_FULL_BAND or a 5 Hz step within [MIN_CROSSOVER_HZ, MAX_CROSSOVER_HZ].
inline bool is_valid_crossover(uint16_t value) {
  if (value == CROSSOVER_FULL_BAND) {
    return true;
  }
  return value >= MIN_CROSSOVER_HZ && value <= MAX_CROSSOVER_HZ &&
         (value - MIN_CROSSOVER_HZ) % CROSSOVER_STEP_HZ == 0;
}

/// @brief Format a wire word for display, or as a select option.
///
/// Values the select does not offer are still formatted - a snooped 87 reads "87 Hz" in a log -
/// so callers publishing to the entity must check is_valid_crossover() first.
/// @param value CMD_BASS_MANAGE_XO payload word.
/// @param buf Output buffer, at least CROSSOVER_STR_SIZE bytes.
/// @param size Size of @p buf.
/// @return @p buf.
inline const char *crossover_to_str(uint16_t value, char *buf, size_t size) {
  if (value == CROSSOVER_FULL_BAND) {
    snprintf(buf, size, "%s", CROSSOVER_STR_FULL_BAND);
  } else {
    snprintf(buf, size, "%u Hz", static_cast<unsigned>(value));
  }
  return buf;
}

/// @brief Parse an option string back into the wire word.
///
/// Accepts exactly the strings crossover_to_str() produces for valid settings, and nothing
/// else: no whitespace variants, no bare numbers, nothing off the 5 Hz grid.
/// @param name Option string.
/// @param[out] value Receives the wire word.
/// @return False for an unrecognised string, leaving @p value untouched.
inline bool str_to_crossover(const char *name, uint16_t &value) {
  for (unsigned v = CROSSOVER_FULL_BAND; v <= MAX_CROSSOVER_HZ; v++) {
    if (!is_valid_crossover(static_cast<uint16_t>(v))) {
      continue;
    }
    char buf[CROSSOVER_STR_SIZE];
    crossover_to_str(static_cast<uint16_t>(v), buf, sizeof(buf));
    const char *a = name;
    const char *b = buf;
    while (*a != '\0' && *a == *b) {
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0') {
      value = static_cast<uint16_t>(v);
      return true;
    }
  }
  return false;
}

}  // namespace gensam
}  // namespace esphome
