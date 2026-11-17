#pragma once

/// @file sam_runtime.h
/// @brief Bounded, owned runtime representation of GLM 5 setup files.
///
/// The importer implements the sam_import.py mapping without depending on ESPHome or a
/// transport, so it can be tested on a host. ThreewayGen2 imports additionally preserve their
/// 96 kHz filter design rate. Presets own all strings and bands; the immutable GroupPreset
/// views remain valid until the owning object is destroyed. Setup files are interpreted as
/// UTF-8 when valid, otherwise as Latin-1, matching the build-time importer.

#include "groups.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace esphome {
namespace gensam {

/// Maximum accepted upload size, chosen to bound network and parser memory use.
constexpr size_t SAM_MAX_BYTES = 65536;
/// Maximum groups retained from one upload.
constexpr size_t SAM_MAX_GROUPS = 16;
/// Maximum device configurations across all groups, bounding owned filter storage.
constexpr size_t SAM_MAX_DEVICE_CONFIGS = 64;

/// @brief Owned backing storage for a set of group presets.
class SamPresets {
 public:
  SamPresets() = default;
  SamPresets(const SamPresets &) = delete;
  SamPresets &operator=(const SamPresets &) = delete;
  SamPresets(SamPresets &&) = default;
  SamPresets &operator=(SamPresets &&) = default;

  /// @brief One device and its independently owned filter slots.
  struct Device {
    GroupDevice value;
    std::array<PeqBand, 20> bands{};
  };
  /// @brief One group and its owned devices.
  struct Group {
    struct DeviceLayout {
      std::string name;
    };

    std::string name;
    std::vector<Device> devices;
    std::vector<DeviceLayout> layouts;
    std::vector<GroupDevice> views;
  };

  /// @brief Rebuild stable views after construction is complete.
  void build_views();
  /// @brief Get immutable views for the protocol hub.
  const std::vector<GroupPreset> &groups() const { return views_; }
  /// @brief Get the first non-empty SAM name for each hardware ID across all groups.
  std::map<uint32_t, std::string> device_names() const;

  std::vector<Group> owned;  ///< Parser-owned groups.

 private:
  std::vector<GroupPreset> views_;
};

/// @brief Parse and validate a complete upload, replacing result only on success.
/// @param text Raw UTF-8 or Latin-1 .sam bytes.
/// @param result Receives owned presets on success.
/// @param warnings Receives non-fatal import warnings.
/// @param error Receives the reason for rejection.
/// @return True only for a complete, usable setup.
bool parse_sam_runtime(const std::string &text, SamPresets &result,
                       std::vector<std::string> &warnings, std::string &error);

}  // namespace gensam
}  // namespace esphome
