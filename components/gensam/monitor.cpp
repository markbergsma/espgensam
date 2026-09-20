/// @file monitor.cpp
/// @brief Implementations for Genelec SAM monitor tracking and parsers.
/// See monitor.h for architectural design rationale and complete API documentation.

#include "monitor.h"
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <strings.h>

namespace esphome {
namespace gensam {

std::string GenSAMMonitor::unique_id_str() const {
  if (unique_id == 0) {
    return "(none)";
  }
  return std::to_string(unique_id);
}

bool GenSAMMonitor::matches(const GenSAMMonitorBinding &b) const {
  if (!b.serial_number.empty() && !serial_number.empty()) {
    if (strcasecmp(b.serial_number.c_str(), serial_number.c_str()) == 0) {
      return true;
    }
    // Substring match: handles minor prefix differences such as optional 'M' (e.g. 7350APM88123456 vs 7350AP88123456)
    // or matching against purely the numeric serial digits (e.g. 88123456)
    if (serial_number.find(b.serial_number) != std::string::npos ||
        b.serial_number.find(serial_number) != std::string::npos) {
      return true;
    }
  }
  if (b.unique_id != 0 && unique_id != 0) {
    if (b.unique_id == unique_id) {
      return true;
    }
  }
  return false;
}

std::string GenSAMMonitor::to_string() const {
  char buf[160];
  if (!serial_number.empty()) {
    snprintf(buf, sizeof(buf), "Monitor 0x%02X [ID:%u, SN:%s] Model=%s FW=%s Temp=%d C %s",
             address, static_cast<unsigned>(unique_id), serial_number.c_str(),
             model.empty() ? "?" : model.c_str(),
             firmware_version.empty() ? "?" : firmware_version.c_str(),
             static_cast<int>(temperature),
             online ? "ONLINE" : "OFFLINE");
  } else {
    snprintf(buf, sizeof(buf), "Monitor 0x%02X [ID:%u] Model=%s FW=%s Temp=%d C %s",
             address, static_cast<unsigned>(unique_id),
             model.empty() ? "?" : model.c_str(),
             firmware_version.empty() ? "?" : firmware_version.c_str(),
             static_cast<int>(temperature),
             online ? "ONLINE" : "OFFLINE");
  }
  return std::string(buf);
}

bool parse_device_info(const uint8_t *data, size_t len, GenSAMMonitor &monitor) {
  if (data == nullptr || len == 0) {
    return false;
  }

  // Find end of string if null-terminated
  size_t actual_len = len;
  for (size_t i = 0; i < len; i++) {
    if (data[i] == '\0') {
      actual_len = i;
      break;
    }
  }

  std::string s(reinterpret_cast<const char *>(data), actual_len);
  monitor.raw_device_info = s;

  // 1. Semicolon-delimited tokens (e.g. "c-1;model-7350A;ver-1.6.2.3733;hw-0.0.0;build-...")
  if (s.find(';') != std::string::npos) {
    std::istringstream stream(s);
    std::string token;
    while (std::getline(stream, token, ';')) {
      // Strip leading/trailing whitespace
      size_t first = token.find_first_not_of(" \t\r\n");
      if (first == std::string::npos) continue;
      size_t last = token.find_last_not_of(" \t\r\n");
      token = token.substr(first, (last - first + 1));

      if (token.rfind("model-", 0) == 0) {
        monitor.model = token.substr(6);
      } else if (token.rfind("ver-", 0) == 0) {
        monitor.firmware_version = token.substr(4);
      } else if (token.rfind("hw-", 0) == 0) {
        monitor.hardware_version = token.substr(3);
      }
    }
  } else {
    // 2. Space-delimited string (e.g. "7350A 1 0000 0106 3733 " from CMD_HARDWARE_QUERY)
    std::istringstream stream(s);
    std::string model_word, hw_type, hw_rev, ver_word, build_word;
    if (stream >> model_word && model_word.length() >= 4) {
      monitor.model = model_word;
    }
    if (stream >> hw_type) {
      monitor.hardware_version = hw_type;
    }
    if (stream >> hw_rev) {
      // Hardware config/options
    }
    if (stream >> ver_word && stream >> build_word) {
      int v = 0;
      if (sscanf(ver_word.c_str(), "%d", &v) == 1 && v > 0) {
        int major = v / 100;
        int minor = v % 100;
        char fw_buf[32];
        snprintf(fw_buf, sizeof(fw_buf), "%d.%d.%s", major, minor, build_word.c_str());
        monitor.firmware_version = fw_buf;
      } else {
        monitor.firmware_version = ver_word + "." + build_word;
      }
    }
  }

  return !monitor.model.empty();
}

bool parse_barcode(const uint8_t *data, size_t len, GenSAMMonitor &monitor) {
  if (data == nullptr || len == 0) {
    return false;
  }
  // Trim trailing nulls and whitespace
  while (len > 0 && (data[len - 1] == '\0' || data[len - 1] == ' ' || data[len - 1] == '\r' || data[len - 1] == '\n')) {
    len--;
  }
  if (len == 0) {
    return false;
  }
  monitor.serial_number = std::string(reinterpret_cast<const char *>(data), len);
  return true;
}

bool parse_telemetry(const uint8_t *data, size_t len, GenSAMMonitor &monitor) {
  if (data == nullptr || len == 0) {
    return false;
  }

  // Check for Tagged TLV format (modern GLMv3-v5 monitors report ASCII-tagged records).
  //
  // A reply may carry a lone 0x06/0x07 marker byte alongside its records. It is not a power
  // state (see is_telemetry_marker() in const.h) and must never touch standby. Strip a leading
  // one so the records align at index 0. A trailing marker -- which the captures also show --
  // needs no handling here: it matches no tag, and the scan below simply steps over it. Do not
  // "improve" this into positional handling; the byte has been observed at either end.
  const uint8_t *tlv_data = data;
  size_t tlv_len = len;
  if (is_telemetry_marker(tlv_data[0])) {
    tlv_data++;
    tlv_len--;
  }

  // A marker on its own is the whole payload: the monitor answered, but reported nothing this
  // round. Update no fields. standby_known in particular is sticky -- nothing ever clears it --
  // so arming it here would enrol this monitor in MonitorRegistry::all_online_in_standby() for
  // the rest of the session on the strength of a byte that carries no power information.
  if (tlv_len == 0) {
    return false;
  }

  // Tags:
  //   0x41: Amp/DSP Temperature (°C)
  //   0x42: Input Signal Level, pre-volume (signed int8 dBFS)
  //   0x43: HF / tweeter channel output level (signed int8 dBFS)
  //   0x44: Midrange channel output level (3-way models; never yet observed)
  //   0x45: LF / woofer channel output level (signed int8 dBFS)
  //   0x46: Subwoofer-only meter, exact meaning unresolved (signed int8 dBFS)
  //   0x47: Power State (0x01 = Active, 0x02 = Standby / ISS)
  //
  // 0x43 is HF and 0x45 is LF, not the other way round. The tags are consecutive and it is
  // tempting to read them as ascending driver order, which is how they were originally labelled
  // here -- but a 7350A subwoofer, which has no tweeter, reports 0x43 pinned to the 0x80 floor in
  // every single frame (164/164 with music playing) while 0x45 swings across 0x80-0xF1. Both are
  // active on a two-way. HLM reached the same assignment independently from a frequency sweep.
  //
  // 0x46 is emitted only by the subwoofer, but it is not simply "the subwoofer's output": it has
  // been seen dominating while 0x45 sat at the floor, and floored while 0x45 swung widely, in
  // different sessions. Left deliberately vague rather than guessed at.
  bool found_tag = false;
  bool found_output = false;
  int8_t max_output_db = -128;
  for (size_t i = 0; i < tlv_len; i++) {
    uint8_t tag = tlv_data[i];
    if (tag == 0x41 && i + 1 < tlv_len) {  // 'A' = Temperature
      monitor.temperature = static_cast<int8_t>(tlv_data[i + 1]);
      found_tag = true;
      i++;
    } else if (tag == 0x42 && i + 1 < tlv_len) {  // 'B' = Input level
      monitor.input_db = static_cast<int8_t>(tlv_data[i + 1]);
      found_tag = true;
      i++;
    } else if ((tag == 0x43 || tag == 0x44 || tag == 0x45 || tag == 0x46) && i + 1 < tlv_len) {
      // Per-channel output meters (see the tag table above). The monitor's overall output level
      // tracks the peak across whichever channels this model reports, so the individual
      // assignments do not affect it -- they matter only for reading the raw frames.
      int8_t level = static_cast<int8_t>(tlv_data[i + 1]);
      if (!found_output || level > max_output_db) {
        max_output_db = level;
      }
      found_output = true;
      found_tag = true;
      i++;
    } else if (tag == 0x47 && i + 1 < tlv_len &&
               (tlv_data[i + 1] == 0x01 || tlv_data[i + 1] == 0x02)) {
      // 'G' = Power state, and the sole authority on it (0x01 = Active, 0x02 = Standby / ISS).
      //
      // The operand is validated in the branch condition rather than the body because this loop
      // is a scan, not a length-driven parse: a 0x47 that is really someone else's operand (a
      // temperature of 71 degC, say) can reach here. Rejecting it in the condition means it is
      // treated as the coincidental data byte it almost certainly is -- no byte consumed, no
      // found_tag, no suppression of the Format A fallback -- rather than arming standby_known,
      // which is sticky and would make this monitor a permanent standby voter.
      monitor.standby = (tlv_data[i + 1] == 0x02);
      monitor.standby_known = true;
      found_tag = true;
      i++;
    } else if ((tag & 0xF0) == 0x80 && i + 2 < tlv_len) {
      // Multi-byte extended record (e.g. 0x81, 0x83, 0x84 followed by 2 payload bytes)
      i += 2;
    }
  }

  if (found_output) {
    monitor.output_db = max_output_db;
  }

  if (found_tag) {
    return true;
  }

  // Format A: Fixed byte offsets from CID_POLL (genlc / standard RACE)
  // Byte 1: Temperature (deg C)
  // Byte 6: Input level (dBFS)
  // Byte 12: Output level (dBFS)
  //
  // Deliberately reads raw data/len, not tlv_data/tlv_len: these offsets are absolute, so the
  // marker strip above must not shift them. The two pointers look interchangeable here and are
  // not. Format A carries no power field, which is why standby is left untouched below.
  if (len >= 7) {
    monitor.temperature = static_cast<int8_t>(data[1]);
    monitor.input_db = static_cast<int8_t>(data[6]);
  }
  if (len >= 13) {
    monitor.output_db = static_cast<int8_t>(data[12]);
  }

  return true;
}

}  // namespace gensam
}  // namespace esphome

