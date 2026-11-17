#pragma once

/// @file sam_upload.h
/// @brief Device-local HTTP upload, storage, and selection bridge for runtime SAM presets.
///
/// The web handler only stages bounded request bytes and enqueues user intent. Parsing, flash
/// writes, and every hub mutation happen from this component's main-loop callbacks. A validated
/// setup remains staged until saved; its first group is the initial selection committed to
/// a CRC-checked flash slot. After restart the restored names back the ESPHome select for the
/// lifetime of the boot, before web and API clients can observe them. Legacy slots without a
/// usable default select the first group, since older filtered group indices may have changed.
/// The hub remembers subsequent selections separately, scoped to this upload's generation.
/// The status snapshot includes each imported PEQ slot; a null band denotes an unused bypass
/// slot. Its JSON is rebuilt only when the status or preset table changes to avoid repeatedly
/// allocating the potentially large summary in the hub's loop.
/// A separate snapshot follows the applied group and never exposes a staged upload as active.

#include "esphome/core/component.h"
#include "hub.h"
#include "sam_runtime.h"
#include "select.h"

#include <esp_partition.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace esphome {
namespace gensam {

/// @brief Serves /upload and manages the transactional runtime preset store.
class SamUploadComponent : public Component {
 public:
  void set_hub(GenSAMHub *hub) { hub_ = hub; }
  /// @brief Register the select whose options are initialized from flash at boot.
  /// @param select Runtime group selector.
  void set_group_select(GenSAMGroupSelect *select) { group_select_ = select; }

  float get_setup_priority() const override { return setup_priority::HARDWARE; }
  void setup() override;
  void loop() override;

  /// @brief Queue a complete bounded upload from the HTTP callback.
  bool queue_upload(std::string &&text);
  /// @brief Commit a staged upload, initially selecting its first group, and request a restart.
  /// @return True when the request was queued.
  bool queue_commit();

  /// @brief Return pre-built snapshots safe to read from the HTTP task.
  std::string status_json();
  std::string groups_json();
  /// @brief Return the applied group snapshot, or null when no group has been applied.
  std::string active_group_json();

 protected:
  bool initialize_storage_();
  bool restore_stored_();
  void initialize_group_select_();
  void restart_when_safe_();
  bool persist_(const std::string &text, int default_index = -1);
  void process_upload_(std::string text);
  void publish_monitor_names_();
  void update_snapshot_();
  void set_result_(const std::string &state, const std::string &message,
                   const std::vector<std::string> &warnings = {});
  GenSAMHub *hub_{nullptr};
  GenSAMGroupSelect *group_select_{nullptr};
  const esp_partition_t *partitions_[2]{nullptr, nullptr};
  int active_slot_{-1};
  uint32_t sequence_{0};
  uint32_t table_revision_{0};
  bool storage_ready_{false};
  std::unique_ptr<SamPresets> runtime_presets_;
  std::unique_ptr<SamPresets> pending_presets_;
  std::string queued_upload_;
  std::string staged_upload_;
  int commit_index_{-1};
  bool commit_queued_{false};
  bool upload_queued_{false};
  bool upload_processing_{false};
  bool table_pending_{false};

  std::mutex mutex_;
  std::string state_{"starting"};
  std::string message_{"Initializing runtime preset storage"};
  std::vector<std::string> warnings_;
  bool status_snapshot_ready_{false};
  uint32_t status_revision_{0};
  std::string status_state_;
  std::string status_message_;
  std::vector<std::string> status_warnings_;
  std::string status_json_{"{\"state\":\"starting\",\"message\":\"Initializing runtime preset storage\",\"warnings\":[],\"groups\":[]}"};
  std::string groups_json_{"{\"groups\":[]}"};
  std::string active_group_json_{"null"};
  int snapshot_active_group_{-2};
  uint32_t active_revision_{0};
};

}  // namespace gensam
}  // namespace esphome
