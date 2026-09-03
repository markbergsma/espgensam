/// @file monitor.cpp
/// @brief Implementations for Genelec SAM monitor tracking and parsers.
/// See monitor.h for architectural design rationale and complete API documentation.

#include "monitor.h"
#include <cstdio>
#include <sstream>
#include <iomanip>

namespace esphome {
namespace gensam {

std::string GenSAMMonitor::unique_id_hex() const {
  if (unique_id.empty()) {
    return "(none)";
  }
  std::ostringstream oss;
  for (size_t i = 0; i < unique_id.size(); i++) {
    if (i > 0) oss << " ";
    oss << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
        << static_cast<int>(unique_id[i]);
  }
  return oss.str();
}

std::string GenSAMMonitor::to_string() const {
  char buf[160];
  if (!serial_number.empty()) {
    snprintf(buf, sizeof(buf), "Monitor 0x%02X [%s, SN:%s] Model=%s FW=%s Temp=%d C %s",
             address, unique_id_hex().c_str(), serial_number.c_str(),
             model.empty() ? "?" : model.c_str(),
             firmware_version.empty() ? "?" : firmware_version.c_str(),
             static_cast<int>(temperature),
             online ? "ONLINE" : "OFFLINE");
  } else {
    snprintf(buf, sizeof(buf), "Monitor 0x%02X [%s] Model=%s FW=%s Temp=%d C %s",
             address, unique_id_hex().c_str(),
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

  // Check for Tagged TLV format (modern GLMv3-v5 monitors report ASCII-tagged records)
  // Tags:
  //   'A' (0x41): Amp/DSP Temperature (°C)
  //   'B' (0x42): Input Signal Level
  //   'C' (0x43): Overload / Clip / Protection Status
  //   'E' (0x45): Driver Output Level (Woofer / Tweeter)
  //   'F' (0x46): Driver Output Level (Subwoofer)
  bool found_tag = false;
  for (size_t i = 0; i < len; i++) {
    uint8_t tag = data[i];
    if (tag == 0x41 && i + 1 < len) {  // 'A' = Temperature
      monitor.temperature = static_cast<int8_t>(data[i + 1]);
      found_tag = true;
      i++;
    } else if (tag == 0x42 && i + 1 < len) {  // 'B' = Input level
      monitor.input_db = static_cast<int8_t>(data[i + 1]);
      found_tag = true;
      i++;
    } else if (tag == 0x43 && i + 1 < len) {  // 'C' = Limiter / Clip gain reduction
      // Idle floor is <= -100 dBFS (0x80 = -128 dBFS, 0x8C = -116 dBFS).
      // Limiter/clip is only active when gain reduction rises significantly towards 0 dBFS.
      int8_t clip_db = static_cast<int8_t>(data[i + 1]);
      monitor.clip = (clip_db > -20);
      found_tag = true;
      i++;
    } else if ((tag == 0x45 || tag == 0x46) && i + 1 < len) {  // 'E'/'F' = Output level
      monitor.output_db = static_cast<int8_t>(data[i + 1]);
      found_tag = true;
      i++;
    } else if ((tag & 0xF0) == 0x80 && i + 2 < len) {
      // Multi-byte extended record (e.g. 0x81, 0x83, 0x84 followed by 2 payload bytes)
      i += 2;
    }
  }

  if (found_tag) {
    return true;
  }

  // Format A: Fixed byte offsets from CID_POLL (genlc / standard RACE)
  // Byte 1: Temperature (deg C)
  // Byte 6: Input level (dBFS)
  // Byte 12: Output level (dBFS)
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

