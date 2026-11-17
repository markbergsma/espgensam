// See sam_runtime.h for ownership and import rationale.
#include "sam_runtime.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>

namespace esphome {
namespace gensam {
namespace {

/// One parsed filter block before slot validation.
struct Band {
  std::string key;
  int index{0};
  std::map<std::string, std::string> fields;
};
/// One device block; discarded after conversion to owned preset storage.
struct Node {
  std::string kind;
  std::map<std::string, std::string> fields;
  std::vector<Band> bands;
};

/// Strip .sam delimiter whitespace, including CRLF endings.
std::string trim(const std::string &s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

/// Validate UTF-8, excluding overlong sequences, surrogates and out-of-range code points.
bool is_utf8(const std::string &s) {
  for (size_t i = 0; i < s.size();) {
    const auto first = static_cast<unsigned char>(s[i++]);
    if (first < 0x80) continue;
    size_t count;
    uint32_t codepoint;
    uint32_t minimum;
    if (first >= 0xc2 && first <= 0xdf) {
      count = 1; codepoint = first & 0x1f; minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      count = 2; codepoint = first & 0x0f; minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      count = 3; codepoint = first & 0x07; minimum = 0x10000;
    } else {
      return false;
    }
    if (count > s.size() - i) return false;
    for (size_t j = 0; j < count; ++j) {
      const auto next = static_cast<unsigned char>(s[i++]);
      if ((next & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if (codepoint < minimum || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
  }
  return true;
}

/// Convert legacy Latin-1 group labels for ESPHome and the web interface.
std::string latin1_to_utf8(const std::string &s) {
  std::string out;
  for (unsigned char c : s) {
    if (c < 128) out += static_cast<char>(c);
    else {
      out += static_cast<char>(0xc0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3f));
    }
  }
  return out;
}

/// Reject malformed and non-finite numeric values rather than partially parsing them.
bool number(const std::string &s, double &out) {
  if (s.empty()) return false;
  char *end = nullptr;
  errno = 0;
  out = std::strtod(s.c_str(), &end);
  return errno != ERANGE && end != s.c_str() && *end == '\0' && std::isfinite(out);
}

/// Read a numeric field, distinguishing absent values from malformed present values.
bool field(const std::map<std::string, std::string> &fields, const char *name, double fallback,
           double &out, std::string &error) {
  auto it = fields.find(name);
  if (it == fields.end()) {
    out = fallback;
    return true;
  }
  if (!number(it->second, out)) {
    error = std::string("Invalid number in ") + name;
    return false;
  }
  return true;
}

/// Validate routing, calibration and filter layout before adding a device to a group.
bool convert_node(const Node &node, double group_crossover, bool utf8_input, SamPresets::Group &group,
                  std::vector<std::string> &warnings, std::string &error) {
  double serial = 0;
  const auto serial_field = node.fields.find("Serial");
  if (serial_field == node.fields.end() || !number(serial_field->second, serial) ||
      serial <= 0 || serial > UINT32_MAX || std::floor(serial) != serial) {
    warnings.push_back("Device without a valid numeric Serial skipped");
    return true;
  }
  const auto id = static_cast<uint32_t>(serial);
  for (const auto &d : group.devices) {
    if (d.value.unique_id == id) {
      error = "Duplicate device in group";
      return false;
    }
  }
  const bool sub = node.kind == "SubwooferGen2";
  const bool threeway = node.kind == "ThreewayGen2";
  if (!sub && !threeway && node.kind != "TwowayGen2") {
    error = "Unknown device class: " + node.kind;
    return false;
  }
  SamPresets::Device dev;
  dev.value.unique_id = id;
  const uint32_t design_rate = sub ? 12000 : (threeway ? 96000 : 48000);
  dev.value.peq_design_rate_hz = design_rate;
  const auto enabled = node.fields.find("Group_ON");
  if (enabled != node.fields.end() && enabled->second != "0" && enabled->second != "1") {
    error = "Group_ON must be 0 or 1";
    return false;
  }
  dev.value.enabled = enabled == node.fields.end() || enabled->second == "1";
  double crossover, level, group_level, phase, input, lfe, lfe_level, boost;
  if (!field(node.fields, "CrossoverFrequency(Hz)", group_crossover, crossover, error) ||
      !field(node.fields, "Level_Sensitivity", 0, level, error) ||
      !field(node.fields, "Group_Sensitivity", 0, group_level, error) ||
      !field(node.fields, "Phase(degrees)", 0, phase, error) ||
      !field(node.fields, "Input", 4, input, error) ||
      !field(node.fields, "LFE_Channel", 0, lfe, error) ||
      !field(node.fields, "LFE_Level", 0, lfe_level, error) ||
      !field(node.fields, "LFE_+10", 0, boost, error)) return false;
  if (level <= -130) {
    warnings.push_back("Uncalibrated level replaced with 0 dB");
    level = 0;
  }
  level += group_level;
  if (level < -60 || level > 0) { error = "Level outside -60..0 dB"; return false; }
  const bool has_crossover = !std::isnan(crossover);
  if (has_crossover && crossover != 1 && (crossover < 50 || crossover > 120 || std::floor(crossover) != crossover ||
                          static_cast<int>(crossover) % 5 != 0)) {
    error = "Invalid crossover"; return false;
  }
  if (input < 1 || input > 4 || std::floor(input) != input || lfe < 0 || lfe > 3 || std::floor(lfe) != lfe) {
    error = "Invalid input or LFE channel"; return false;
  }
  if (input == 3 && lfe != 0) {
    if (lfe == 3) { error = "LFE sum leaves no program channel"; return false; }
    input = lfe == 1 ? 2 : 1;
  }
  lfe_level += boost != 0 ? 10 : 0;
  if (lfe_level < -128 || lfe_level > 127) { error = "LFE level out of range"; return false; }
  if (lfe && lfe_level != std::nearbyint(lfe_level)) warnings.push_back("Fractional LFE level will be rounded");
  dev.value.crossover_hz = has_crossover ? static_cast<uint16_t>(crossover) : DEFAULT_CROSSOVER_HZ;
  dev.value.level_db = static_cast<float>(level);
  dev.value.source = input == 4 ? SOURCE_ANALOG : SOURCE_DIGITAL_AES3;
  dev.value.aes3_channel = input == 4 ? AES3_CHANNEL_A : static_cast<uint8_t>(input);
  dev.value.lfe_channel = static_cast<uint8_t>(lfe);
  dev.value.lfe_level_db = static_cast<float>(lfe_level);
  double normalized = std::fmod(phase, 360.0);
  if (normalized < 0) normalized += 360;
  if ((!has_crossover || crossover == 1) && normalized != 0)
    warnings.push_back("Phase ignored without a bass-management crossover");
  double delay = !has_crossover || crossover == 1 ? 0 : std::nearbyint(normalized / 360.0 / crossover * 48000.0);
  if (delay > 9216) { error = "Delay out of range"; return false; }
  dev.value.delay_samples = static_cast<uint32_t>(delay);
  for (const char *name : {"Optional_Gain", "Time-of-flight_Compensation", "Video_Delay", "SubwooferGroupID"}) {
    double v;
    if (!field(node.fields, name, 0, v, error)) return false;
    if (v != 0) warnings.push_back(std::string(name) + " is not applied");
  }
  SamPresets::Group::DeviceLayout layout;
  const auto name = node.fields.find("Name");
  if (name != node.fields.end()) {
    layout.name = utf8_input ? name->second : latin1_to_utf8(name->second);
  }
  bool used[20]{};
  for (const auto &band : node.bands) {
    int base = 0;
    PeqType type = PeqType::PEAKING;
    if (band.key == "LP_Shelve") { base = 0; type = PeqType::LOW_SHELF; }
    else if (band.key == "HP_Shelve") { base = 2; type = PeqType::HIGH_SHELF; }
    else if (!sub) base = 4;
    if (sub && band.key != "Notch") { error = "Unexpected subwoofer shelf"; return false; }
    const int max_index = band.key != "Notch" ? 2 : (sub ? 20 : 16);
    if (band.index > max_index) { error = "Filter index exceeds class layout"; return false; }
    int slot = base + band.index - 1;
    if (slot < 0 || slot >= 20 || used[slot]) { error = "Duplicate or invalid filter slot"; return false; }
    used[slot] = true;
    double frequency, gain, q, sr;
    if (!field(band.fields, "Frequency", 0, frequency, error) ||
        !field(band.fields, "Gain", 0, gain, error) ||
        !field(band.fields, "Q_Value", 1, q, error) ||
        !field(band.fields, "SR", design_rate, sr, error)) return false;
    if (sr != design_rate) {
      error = "Incorrect filter design rate"; return false;
    }
    constexpr double max_float = std::numeric_limits<float>::max();
    if (frequency < 0 || frequency > max_float || std::fabs(gain) > 120 ||
        q > max_float || q < 0 ||
        (gain != 0 && (frequency <= 0 || frequency >= design_rate / 2.0 ||
                      (type == PeqType::PEAKING && q <= 0)))) {
      error = "Invalid filter parameters"; return false;
    }
    dev.bands[slot] = {type, static_cast<float>(frequency), static_cast<float>(gain),
                       type == PeqType::PEAKING ? static_cast<float>(q) : 0.0f};
  }
  dev.value.band_count = 20;
  group.devices.push_back(std::move(dev));
  group.layouts.push_back(std::move(layout));
  return true;
}
}  // namespace

void SamPresets::build_views() {
  views_.clear();
  for (auto &group : owned) {
    group.views.clear();
    for (auto &device : group.devices) {
      device.value.bands = device.bands.data();
      group.views.push_back(device.value);
    }
    views_.push_back({group.name.c_str(), group.views.data(), static_cast<uint8_t>(group.views.size())});
  }
}

std::map<uint32_t, std::string> SamPresets::device_names() const {
  std::map<uint32_t, std::string> names;
  for (const auto &group : owned) {
    const size_t count = std::min(group.devices.size(), group.layouts.size());
    for (size_t i = 0; i < count; i++) {
      const auto &name = group.layouts[i].name;
      if (!name.empty()) {
        names.emplace(group.devices[i].value.unique_id, name);
      }
    }
  }
  return names;
}

bool parse_sam_runtime(const std::string &text, SamPresets &result,
                       std::vector<std::string> &warnings, std::string &error) {
  warnings.clear();
  error.clear();
  if (text.empty() || text.size() > SAM_MAX_BYTES || text.find('\0') != std::string::npos) {
    error = "Empty, oversized or binary upload"; return false;
  }
  const bool utf8_input = is_utf8(text);
  SamPresets parsed;
  std::string line;
  bool header = false, inventory_open = false, magic = false, in_group = false,
       in_node = false, skip = false;
  std::map<std::string, std::map<std::string, std::string>> inventory;
  std::map<std::string, std::string> inventory_device;
  size_t device_configs = 0;
  double crossover = std::numeric_limits<double>::quiet_NaN();
  Node node;
  size_t offset = 0;
  while (offset < text.size()) {
    const size_t end = text.find('\n', offset);
    line = text.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
    offset = end == std::string::npos ? text.size() : end + 1;
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = trim(line.substr(0, colon)), value = trim(line.substr(colon + 1));
    if (key == "Magic" && value.find("GLM5") != std::string::npos) magic = true;
    if (key == "Header_End") { header = true; inventory_open = true; continue; }
    if (!header) continue;
    if (key == "Group_Name") {
      if (in_group || value.empty() || value.size() > 128 || parsed.owned.size() >= SAM_MAX_GROUPS) {
        error = "Invalid group start, name too long or more than 16 groups"; return false;
      }
      const auto name = utf8_input ? value : latin1_to_utf8(value);
      for (const auto &g : parsed.owned) if (g.name == name) { error = "Duplicate group name"; return false; }
      parsed.owned.emplace_back();
      parsed.owned.back().name = name;
      inventory_open = false;
      in_group = true; crossover = std::numeric_limits<double>::quiet_NaN(); skip = false;
      continue;
    }
    if (!in_group) {
      if (inventory_open) {
        if (key == "Model") inventory_device.clear();
        else if (key == "Data_End") {
          auto serial = inventory_device.find("Serial");
          if (serial != inventory_device.end()) inventory[serial->second] = inventory_device;
          inventory_device.clear();
        } else if (key == "Serial" || key == "Name") {
          inventory_device[key] = value;
        }
      }
      continue;
    }
    if (key == "9320DATA") { skip = true; continue; }
    if (key == "Group_Nodes") { skip = false; continue; }
    if (skip) continue;
    if (key == "Class") {
      if (in_node) { error = "Unterminated device"; return false; }
      node = Node{}; node.kind = value; in_node = true; continue;
    }
    if (key == "Data_End" && in_node) {
      auto &group = parsed.owned.back();
      if (++device_configs > SAM_MAX_DEVICE_CONFIGS) { error = "More than 64 device configurations"; return false; }
      const auto serial = node.fields.find("Serial");
      if (serial != node.fields.end()) {
        const auto device = inventory.find(serial->second);
        if (device != inventory.end()) {
          const auto device_name = device->second.find("Name");
          if (device_name != device->second.end() && node.fields.count("Name") == 0) {
            node.fields["Name"] = device_name->second;
          }
        }
      }
      if (!convert_node(node, crossover, utf8_input, group, warnings, error)) return false;
      if (group.devices.size() > 255) { error = "Too many devices"; return false; }
      in_node = false; continue;
    }
    if (key == "Group_End") {
      if (in_node) { error = "Unterminated device"; return false; }
      if (parsed.owned.back().devices.empty()) {
        warnings.push_back("Group has no usable devices; skipped");
        parsed.owned.pop_back();
      }
      in_group = false; continue;
    }
    if (!in_node) {
      if (key == "Group_Crossover" && !number(value, crossover)) { error = "Invalid group crossover"; return false; }
      continue;
    }
    if (key == "Notch" || key == "LP_Shelve" || key == "HP_Shelve") {
      double index;
      if (!number(value, index) || index < 1 || index > 20 || std::floor(index) != index || node.bands.size() >= 20) {
        error = "Invalid filter index"; return false;
      }
      node.bands.push_back({key, static_cast<int>(index), {}});
    } else if (!node.bands.empty() && (key == "Frequency" || key == "Gain" || key == "Q_Value" || key == "SR")) {
      node.bands.back().fields[key] = value;
    } else if (key != "Serial" || node.fields.count(key) == 0) {
      if (node.fields.count(key) == 0 && node.fields.size() >= 64) {
        error = "Too many fields in a device block";
        return false;
      }
      node.fields[key] = value;
    }
  }
  if (!magic) warnings.push_back("Missing GLM5 magic header");
  if (!header || in_group || in_node || parsed.owned.empty()) { error = "Incomplete or empty setup"; return false; }
  result = std::move(parsed);
  result.build_views();
  return true;
}

}  // namespace gensam
}  // namespace esphome
