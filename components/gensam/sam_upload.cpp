/// @file sam_upload.cpp
/// @brief Device-local runtime preset upload and storage implementation.
/// See sam_upload.h for the request/loop boundary and ownership model.

#include "sam_upload.h"
#include "group_view.h"

#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static const char *const TAG = "gensam.sam_upload";

namespace esphome {
namespace gensam {
namespace {

constexpr uint32_t SLOT_SIZE = 0x11000;
constexpr uint32_t SLOT_MAGIC = 0x53414D52;
constexpr uint32_t SLOT_VERSION = 1;
constexpr uint32_t SLOT_VERSION_DEFAULT = 2;
constexpr uint32_t SLOT_VERSION_UNFILTERED = 3;
constexpr uint32_t SLOT_COMMITTED = 0xA55A3CC3;
constexpr size_t WRITE_CHUNK = 4096;

struct SlotHeader {
  uint32_t magic;
  uint32_t sequence;
  uint32_t length;
  uint32_t data_crc;
  uint32_t version;
  uint32_t committed;
};
static_assert(sizeof(SlotHeader) == 24);
static_assert(sizeof(SlotHeader) + SAM_MAX_BYTES <= SLOT_SIZE);

int slot_default(const SlotHeader &header) {
  if ((header.version & 0xFF) != SLOT_VERSION_UNFILTERED) return -1;
  return static_cast<int>((header.version >> 8) & 0xFF) - 1;
}

uint32_t slot_version(int default_index) {
  const uint32_t encoded = static_cast<uint32_t>(default_index + 1);
  return 0xA5000000U | ((encoded ^ 0xFFU) << 16) | (encoded << 8) | SLOT_VERSION_UNFILTERED;
}

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
  }
  return crc;
}

uint32_t crc32(const uint8_t *data, size_t length) {
  return ~crc32_update(0xFFFFFFFF, data, length);
}

std::string json_escape(const std::string &value) {
  std::string result;
  result.reserve(value.size() + 8);
  for (unsigned char ch : value) {
    switch (ch) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (ch < 0x20) {
          static const char HEX[] = "0123456789abcdef";
          result += "\\u00";
          result += HEX[ch >> 4];
          result += HEX[ch & 0x0F];
        } else {
          result += static_cast<char>(ch);
        }
    }
  }
  return result;
}

std::string input_summary(uint8_t source, uint8_t channel) {
  if (source == SOURCE_ANALOG) return "Analog";
  if (source != SOURCE_DIGITAL_AES3) return "Unknown";
  switch (channel) {
    case AES3_CHANNEL_A: return "AES3 A (Left)";
    case AES3_CHANNEL_B: return "AES3 B (Right)";
    case AES3_CHANNEL_SUM: return "AES3 A+B (Sum)";
    default: return "AES3 (Unknown channel)";
  }
}

std::string lfe_summary(uint8_t channel) {
  switch (channel) {
    case 0: return "none";
    case AES3_CHANNEL_A: return "AES3 A";
    case AES3_CHANNEL_B: return "AES3 B";
    case AES3_CHANNEL_SUM: return "AES3 A+B";
    default: return "Unknown";
  }
}

const char *peq_type_name(PeqType type) {
  switch (type) {
    case PeqType::BYPASS: return "Bypass";
    case PeqType::LOW_SHELF: return "Low shelf";
    case PeqType::HIGH_SHELF: return "High shelf";
    case PeqType::PEAKING: return "Peaking";
  }
  return "Unknown";
}

std::string json_float(float value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
  return buffer;
}

std::string setup_summary_json(const std::vector<GroupPreset> &groups,
                               const SamPresets *metadata = nullptr, size_t metadata_offset = 0) {
  std::string result = "[";
  bool first_group = true;
  for (size_t group_index = 0; group_index < groups.size(); group_index++) {
    const auto &group = groups[group_index];
    if (!first_group) result += ",";
    first_group = false;
    result += "{\"name\":\"" + json_escape(group.name != nullptr ? group.name : "") + "\",\"devices\":[";
    for (uint8_t i = 0; group.devices != nullptr && i < group.device_count; i++) {
      if (i != 0) result += ",";
      const GroupDevice &device = group.devices[i];
      uint8_t active_bands = 0;
      for (uint8_t band = 0; device.bands != nullptr && band < device.band_count; band++) {
        if (device.bands[band].type != PeqType::BYPASS && device.bands[band].gain_db != 0.0f) {
          active_bands++;
        }
      }
      const std::string crossover = device.crossover_hz == CROSSOVER_FULL_BAND
                                        ? "Full band"
                                        : std::to_string(device.crossover_hz) + " Hz";
      const SamPresets::Group::DeviceLayout *layout = nullptr;
      if (metadata != nullptr && metadata_offset + group_index < metadata->owned.size() &&
          i < metadata->owned[metadata_offset + group_index].layouts.size()) {
        layout = &metadata->owned[metadata_offset + group_index].layouts[i];
      }
      result += "{\"id\":" + std::to_string(device.unique_id) +
                ",\"enabled\":" + (device.enabled ? "true" : "false") +
                ",\"name\":\"" +
                json_escape(layout != nullptr ? layout->name : std::string()) + "\"" +
                ",\"input\":\"" + input_summary(device.source, device.aes3_channel) +
                "\",\"crossover\":\"" + crossover +
                "\",\"level_db\":" + std::to_string(device.level_db) +
                ",\"delay_samples\":" + std::to_string(device.delay_samples) +
                ",\"delay_ms\":" +
                std::to_string(static_cast<double>(device.delay_samples) / 48.0) +
                ",\"lfe\":\"" + lfe_summary(device.lfe_channel) +
                "\",\"lfe_level_db\":" + std::to_string(device.lfe_level_db) +
                ",\"peq_active\":" + std::to_string(active_bands) +
                ",\"peq_design_rate_hz\":" + std::to_string(device.peq_design_rate_hz) +
                ",\"peq_bands\":[";
      for (uint8_t band = 0; device.bands != nullptr && band < device.band_count; band++) {
        if (band != 0) result += ",";
        const PeqBand &filter = device.bands[band];
        if (filter.type == PeqType::BYPASS) {
          result += "null";
          continue;
        }
        result += "{\"type\":\"" + std::string(peq_type_name(filter.type)) +
                  "\",\"frequency_hz\":" + json_float(filter.frequency_hz) +
                  ",\"gain_db\":" + json_float(filter.gain_db) +
                  ",\"q\":" + json_float(filter.q);
        if (filter.type == PeqType::LOW_SHELF || filter.type == PeqType::HIGH_SHELF) {
          result += ",\"effective_q\":" + json_float(filter.type == PeqType::LOW_SHELF
                                                         ? SHELF_Q_LOW : SHELF_Q_HIGH);
        }
        result += "}";
      }
      result += "]}";
    }
    result += "]}";
  }
  result += "]";
  return result;
}

bool valid_slot(const esp_partition_t *partition, SlotHeader &header, std::string *data = nullptr) {
  if (partition == nullptr || esp_partition_read(partition, 0, &header, sizeof(header)) != ESP_OK) {
    return false;
  }
  const uint32_t encoded = (header.version >> 8) & 0xFF;
  const bool valid_version = header.version == SLOT_VERSION ||
      ((header.version & 0xFF000000U) == 0xA5000000U &&
       ((header.version & 0xFF) == SLOT_VERSION_DEFAULT ||
        (header.version & 0xFF) == SLOT_VERSION_UNFILTERED) &&
       ((header.version >> 16) & 0xFF) == (encoded ^ 0xFFU) && encoded <= SAM_MAX_GROUPS);
  if (header.magic != SLOT_MAGIC || !valid_version ||
      header.committed != SLOT_COMMITTED || header.length == 0 ||
      header.length > SAM_MAX_BYTES || sizeof(header) + header.length > partition->size) {
    return false;
  }
  std::string contents(header.length, '\0');
  if (esp_partition_read(partition, sizeof(header), contents.data(), contents.size()) != ESP_OK ||
      (crc32(reinterpret_cast<const uint8_t *>(contents.data()), contents.size()) ^
       (header.version == SLOT_VERSION ? 0U : header.version)) != header.data_crc) {
    return false;
  }
  if (data != nullptr) {
    *data = std::move(contents);
  }
  return true;
}

bool is_same_origin(AsyncWebServerRequest *request) {
  const auto origin = request->get_header("Origin");
  const auto host = request->get_header("Host");
  if (!origin.has_value() || !host.has_value()) {
    return false;
  }
  const std::string &value = origin.value();
  constexpr char PREFIX[] = "http://";
  if (value.compare(0, sizeof(PREFIX) - 1, PREFIX) != 0) {
    return false;
  }
  const size_t authority_start = sizeof(PREFIX) - 1;
  const size_t path_start = value.find('/', authority_start);
  std::string authority = value.substr(authority_start, path_start - authority_start);
  std::string request_host = host.value();
  auto lowercase = [](std::string &text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  };
  lowercase(authority);
  lowercase(request_host);
  return !authority.empty() && authority == request_host;
}

void send_json(AsyncWebServerRequest *request, const char *status, const char *body) {
  auto *response = request->beginResponse(200, "application/json", std::string(body));
  httpd_resp_set_status(*request, status);
  request->send(response);
}

const char SAM_INDEX_HTML[] = R"HTML(<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Upload Genelec SAM setup</title>
<style>
body{font:16px system-ui,sans-serif;max-width:48rem;margin:2rem auto;padding:0 1rem;background:#111827;color:#f3f4f6}
section{background:#1f2937;padding:1rem 1.25rem;border-radius:.7rem;margin:1rem 0}button,input{font:inherit;padding:.55rem .8rem}
button,summary{cursor:pointer}select{font:inherit;padding:.55rem;min-width:16rem}#message{white-space:pre-wrap}
details{margin:.65rem 0;padding:.65rem;border:1px solid #4b5563;border-radius:.4rem}summary{font-weight:600}
dl{display:grid;grid-template-columns:max-content 1fr;gap:.35rem 1rem}dt{color:#d1d5db}dd{margin:0}
.table-wrap{overflow-x:auto}table{border-collapse:collapse;width:100%;text-align:left;font-size:.9rem}
th,td{padding:.35rem .5rem;border-bottom:1px solid #4b5563;white-space:nowrap}
</style>
<h1>Upload Genelec SAM setup</h1>
<p><a href="/">Back to device controls</a></p>
<section><h2>1. Upload a .sam setup</h2><p>Choose a file and validate it. Changing available groups requires a new upload.</p>
<input id="file" type="file" accept=".sam,text/plain"><button id="upload">Validate</button>
<div id="feedback" hidden><p id="message" role="status"></p><ul id="warnings"></ul></div></section>
<section id="details" hidden><h2>New setup details</h2><p>Preview of the newly validated file, not the current speaker settings.</p><div id="summary"></div></section>
<section><h2>2. Save and restart</h2><p>A new upload starts with its first group. Later group selections are remembered.</p>
<button id="apply" disabled>Save and restart</button></section>
<script src="/group-view.js"></script>
<script>
const message=document.getElementById('message'), warnings=document.getElementById('warnings'), summary=document.getElementById('summary');
const feedback=document.getElementById('feedback'), details=document.getElementById('details');
const apply=document.getElementById('apply'), upload=document.getElementById('upload');
let summaryKey=null, requestError=null, busy=false, attempted=false;
function node(tag,text){const e=document.createElement(tag);if(text!==undefined)e.textContent=text;return e}
function report(text){feedback.hidden=false;message.textContent=text}
function renderSummary(data){
const key=JSON.stringify(data);if(key===summaryKey)return;summaryKey=key;
details.hidden=!data.length;
summary.replaceChildren(...data.map(g=>{
const group=node('details');group.open=true;group.append(node('summary',g.name+' ('+g.devices.length+' speakers)'));
group.append(window.gensamRenderGroup(g));return group}))
}
async function refresh(){try{const status=await fetch('/upload/status');if(!status.ok)throw new Error('Status request failed (HTTP '+status.status+')');const s=await status.json();
const staged=s.state==='staged'||s.state==='saving'||s.state==='restarting', relevant=attempted||staged||s.state==='queued';
message.textContent=requestError||s.message||s.state;warnings.replaceChildren(...(relevant&&s.warnings||[]).map(w=>{const li=document.createElement('li');li.textContent='Warning: '+w;return li}));
feedback.hidden=!(relevant||requestError);
renderSummary(staged?s.summary||[]:[]);
apply.disabled=busy||s.state!=='staged';upload.disabled=busy||s.state==='queued'||s.state==='saving'||s.state==='restarting';
}catch(e){apply.disabled=true;report(requestError||'Could not reach the device: '+e)}}
async function mutate(url,options){requestError=null;attempted=true;busy=true;apply.disabled=upload.disabled=true;try{const r=await fetch(url,options);const x=await r.json();if(!r.ok)throw new Error(x.error||'HTTP '+r.status);report(x.message);busy=false;setTimeout(refresh,500)}catch(e){busy=false;upload.disabled=false;requestError='Request failed: '+e;report(requestError);await refresh()}}
document.getElementById('upload').onclick=async()=>{const f=document.getElementById('file').files[0];if(!f){requestError='Choose a .sam file first.';report(requestError);return}
try{await mutate('/upload/file',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:await f.arrayBuffer()})}catch(e){requestError='Upload failed: '+e;report(requestError)}};
document.getElementById('apply').onclick=async()=>{await mutate('/upload/commit',{method:'POST',headers:{'Content-Type':'application/octet-stream'}})};
refresh();setInterval(refresh,1500);
</script></html>)HTML";

class SamHttpHandler : public AsyncWebHandler {
 public:
  explicit SamHttpHandler(SamUploadComponent *parent) : parent_(parent) {}

  bool canHandle(AsyncWebServerRequest *request) const override {
    char url[AsyncWebServerRequest::URL_BUF_SIZE];
    const std::string path(request->url_to(url).c_str());
    if (path == "/upload" || path == "/upload/") return request->method() == HTTP_GET;
    if (path == "/group-view" || path == "/group-view.js") return request->method() == HTTP_GET;
    if (path == "/upload/status" || path == "/upload/groups") return request->method() == HTTP_GET;
    if (path == "/upload/file" || path == "/upload/commit") return request->method() == HTTP_POST;
    return false;
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    char url[AsyncWebServerRequest::URL_BUF_SIZE];
    const std::string path(request->url_to(url).c_str());
    if (path == "/upload" || path == "/upload/") {
      request->send(200, "text/html; charset=utf-8", SAM_INDEX_HTML);
      return;
    }
    if (path == "/group-view.js") {
      request->send(200, "application/javascript; charset=utf-8", GROUP_VIEW_JS);
      return;
    }
    if (path == "/group-view") {
      const auto body = parent_->active_group_json();
      request->send(200, "application/json; charset=utf-8", body.c_str());
      return;
    }
    if (path == "/upload/status") {
      const auto body = parent_->status_json();
      request->send(200, "application/json; charset=utf-8", body.c_str());
      return;
    }
    if (path == "/upload/groups") {
      const auto body = parent_->groups_json();
      request->send(200, "application/json; charset=utf-8", body.c_str());
      return;
    }
    if (!is_same_origin(request)) {
      send_json(request, "403 Forbidden", "{\"error\":\"Same-origin request required\"}");
      return;
    }
    if (path == "/upload/commit") {
      if (!parent_->queue_commit()) {
        send_json(request, "409 Conflict", "{\"error\":\"No staged setup or request already in progress\"}");
        return;
      }
      send_json(request, "202 Accepted", "{\"message\":\"Saving setup and restarting\"}");
      return;
    }
    if (path == "/upload/file") {
      // ESPHome's IDF wrapper has no disconnect callback or raw-body dispatch. Keep the
      // receive buffer request-local: every return (including disconnect) destroys it.
      const auto content_type = request->get_header("Content-Type");
      if (!content_type.has_value() || content_type.value() != "application/octet-stream") {
        send_json(request, "415 Unsupported Media Type", "{\"error\":\"Use application/octet-stream\"}");
        return;
      }
      const size_t total = request->contentLength();
      if (total == 0 || total > SAM_MAX_BYTES) {
        send_json(request, total == 0 ? "400 Bad Request" : "413 Payload Too Large",
                  "{\"error\":\"Upload must contain 1 to 65536 bytes\"}");
        return;
      }
      std::string body;
      body.resize(total);
      for (size_t offset = 0; offset < total;) {
        const int received = httpd_req_recv(*request, body.data() + offset,
                                            std::min(WRITE_CHUNK, total - offset));
        if (received <= 0) {
          send_json(request, received == HTTPD_SOCK_ERR_TIMEOUT ? "408 Request Timeout" : "400 Bad Request",
                    "{\"error\":\"Upload interrupted; the last valid setup was kept\"}");
          return;
        }
        offset += static_cast<size_t>(received);
      }
      if (!parent_->queue_upload(std::move(body))) {
        send_json(request, "409 Conflict", "{\"error\":\"A request is already being processed\"}");
        return;
      }
      send_json(request, "202 Accepted", "{\"message\":\"Upload accepted for validation\"}");
      return;
    }
    request->send(404);
  }

  bool isRequestHandlerTrivial() const override { return false; }

 private:
  SamUploadComponent *parent_;
};

}  // namespace

void SamUploadComponent::setup() {
  if (hub_ == nullptr) {
    ESP_LOGE(TAG, "No hub configured; runtime uploads are disabled");
    this->mark_failed();
    return;
  }
  auto *base = web_server_base::global_web_server_base;
  if (base == nullptr) {
    ESP_LOGE(TAG, "ESPHome web_server_base is unavailable; enable web_server to use /upload");
    this->mark_failed();
    return;
  }
  base->add_handler(new SamHttpHandler(this));
  if (!this->initialize_storage_()) {
    this->set_result_("unavailable", "Runtime preset flash partitions are unavailable; verify the generated partition table");
    this->update_snapshot_();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    storage_ready_ = true;
  }
  this->restore_stored_();
  this->update_snapshot_();
}

void SamUploadComponent::loop() {
  std::string upload;
  bool has_upload = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (upload_queued_) {
      upload = std::move(queued_upload_);
      queued_upload_.clear();
      upload_queued_ = false;
      upload_processing_ = true;
      has_upload = true;
    }
  }
  if (has_upload) {
    pending_presets_.reset();
    staged_upload_.clear();
    table_revision_++;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      table_pending_ = false;
    }
    this->process_upload_(std::move(upload));
  }

  bool commit = false;
  int default_index = -1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (commit_queued_ && !hub_->is_group_apply_in_progress()) {
      commit_queued_ = false;
      upload_processing_ = true;
      commit = true;
      default_index = commit_index_;
    }
  }
  if (commit) {
    if (pending_presets_ == nullptr || default_index < 0 ||
        static_cast<size_t>(default_index) >= pending_presets_->groups().size()) {
      this->set_result_("error", "Invalid default group; the last saved setup was kept.", warnings_);
    } else if (!this->persist_(staged_upload_, default_index)) {
      this->set_result_("error", "Could not save setup to flash; the last valid setup was kept.", warnings_);
    } else {
      this->set_result_("restarting", "Setup saved. Restarting the device.", warnings_);
      this->update_snapshot_();
      this->set_timeout("sam_restart", 1000, [this]() { this->restart_when_safe_(); });
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    upload_processing_ = false;
  }

  this->update_snapshot_();
}

void SamUploadComponent::restart_when_safe_() {
  if (hub_->is_group_apply_in_progress()) {
    this->set_timeout("sam_restart", 1000, [this]() { this->restart_when_safe_(); });
    return;
  }
  App.safe_reboot();
}

bool SamUploadComponent::queue_upload(std::string &&text) {
  if (text.empty() || text.size() > SAM_MAX_BYTES) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!storage_ready_ || upload_queued_ || upload_processing_ || commit_queued_) return false;
  queued_upload_ = std::move(text);
  upload_queued_ = true;
  state_ = "queued";
  message_ = "Upload accepted; validating the complete setup.";
  return true;
}

bool SamUploadComponent::queue_commit() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!storage_ready_ || !table_pending_ ||
      upload_queued_ || upload_processing_ ||
      commit_queued_) return false;
  commit_index_ = 0;
  commit_queued_ = true;
  state_ = "saving";
  message_ = "Saving setup and waiting for any group application to finish.";
  return true;
}

std::string SamUploadComponent::status_json() {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_json_;
}

std::string SamUploadComponent::groups_json() {
  std::lock_guard<std::mutex> lock(mutex_);
  return groups_json_;
}

std::string SamUploadComponent::active_group_json() {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_group_json_;
}

bool SamUploadComponent::initialize_storage_() {
  const auto subtype = static_cast<esp_partition_subtype_t>(0x40);
  partitions_[0] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, "sam_slot_a");
  partitions_[1] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, "sam_slot_b");
  if (partitions_[0] == nullptr || partitions_[1] == nullptr ||
      partitions_[0]->size < SLOT_SIZE || partitions_[1]->size < SLOT_SIZE) {
    ESP_LOGE(TAG, "Two 0x%X-byte SAM data partitions are required", SLOT_SIZE);
    return false;
  }
  ESP_LOGI(TAG, "Transactional SAM storage: %u bytes in each of two slots",
           static_cast<unsigned>(SLOT_SIZE));
  return true;
}

bool SamUploadComponent::restore_stored_() {
  SlotHeader a{}, b{};
  const bool valid_a = valid_slot(partitions_[0], a);
  const bool valid_b = valid_slot(partitions_[1], b);
  if (!valid_a && !valid_b) {
    this->set_result_("empty", "No stored SAM setup. Upload a .sam file to add runtime presets.");
    return false;
  }
  const bool use_b = valid_b && (!valid_a || static_cast<int32_t>(b.sequence - a.sequence) > 0);
  const int newest = use_b ? 1 : 0;
  sequence_ = use_b ? b.sequence : a.sequence;
  std::string error;
  std::vector<std::string> warnings;
  for (int attempt = 0; attempt < 2; attempt++) {
    const int chosen = attempt == 0 ? newest : 1 - newest;
    SlotHeader selected{};
    std::string text;
    if (!valid_slot(partitions_[chosen], selected, &text)) continue;
    auto parsed = std::make_unique<SamPresets>();
    bool usable = parse_sam_runtime(text, *parsed, warnings, error) &&
                  !parsed->groups().empty() && parsed->groups().size() <= UINT8_MAX;
    int default_index = slot_default(selected);
    if (default_index >= 0 && static_cast<size_t>(default_index) >= parsed->groups().size()) usable = false;
    if (default_index < 0) default_index = 0;
    for (const auto &group : parsed->owned) {
      if (group.devices.empty()) usable = false;
      if (std::any_of(group.name.begin(), group.name.end(),
                      [](unsigned char ch) { return ch < 0x20 || ch == 0x7F; })) usable = false;
    }
    if (!usable) {
      // Do not log upload-controlled parser text: names and fields can contain log controls.
      ESP_LOGW(TAG, "Stored SAM slot %d is not usable", chosen);
      continue;
    }
    const auto &groups = parsed->groups();
    if (!hub_->replace_runtime_group_table(groups.data(), static_cast<uint8_t>(groups.size()))) {
      this->set_result_("error", "Stored presets could not be loaded while a group push was in progress.", warnings);
      return false;
    }
    runtime_presets_ = std::move(parsed);
    this->publish_monitor_names_();
    this->initialize_group_select_();
    hub_->set_default_group(static_cast<uint8_t>(default_index));
    hub_->set_group_generation(selected.sequence);
    table_revision_++;
    active_slot_ = chosen;
    if (attempt != 0) warnings.push_back("Newest stored setup was rejected; restored the previous valid setup.");
    if ((selected.version & 0xFF) == SLOT_VERSION_DEFAULT &&
        ((selected.version >> 8) & 0xFF) != 0) {
      warnings.push_back("Previous default could not be restored because uploaded groups are no longer filtered; the first group is now the startup default.");
    }
    this->set_result_("ready", "Stored setup restored. Select groups on the main page or in Home Assistant.", warnings);
    ESP_LOGI(TAG, "Restored %u groups from persistent SAM setup", static_cast<unsigned>(groups.size()));
    for (const auto &warning : warnings) ESP_LOGW(TAG, "Stored SAM setup warning: %s", warning.c_str());
    return true;
  }
  this->set_result_("error", "No stored SAM setup is usable with this firmware.", warnings);
  return false;
}

void SamUploadComponent::publish_monitor_names_() {
  if (hub_ != nullptr && runtime_presets_ != nullptr) {
    hub_->set_sam_monitor_names(runtime_presets_->device_names());
  }
}

void SamUploadComponent::initialize_group_select_() {
  if (group_select_ == nullptr || runtime_presets_ == nullptr) return;
  const auto &groups = runtime_presets_->groups();
  FixedVector<const char *> options;
  options.init(groups.size());
  for (const auto &group : groups) options.push_back(group.name);
  group_select_->traits.set_options(options);
  group_select_->set_has_state(false);
  group_select_->set_runtime_ready();
}

bool SamUploadComponent::persist_(const std::string &text, int default_index) {
  const int target = active_slot_ == 0 ? 1 : 0;
  const esp_partition_t *partition = partitions_[target];
  if (esp_partition_erase_range(partition, 0, SLOT_SIZE) != ESP_OK) {
    ESP_LOGE(TAG, "Could not erase SAM storage slot %d", target);
    return false;
  }
  size_t offset = 0;
  std::array<uint8_t, WRITE_CHUNK> padded{};
  while (offset < text.size()) {
    const size_t chunk = std::min(WRITE_CHUNK, text.size() - offset);
    const size_t aligned = (chunk + 3U) & ~size_t(3U);
    const uint8_t *source = reinterpret_cast<const uint8_t *>(text.data() + offset);
    if (aligned != chunk) {
      padded.fill(0xFF);
      std::memcpy(padded.data(), source, chunk);
      source = padded.data();
    }
    if (esp_partition_write(partition, sizeof(SlotHeader) + offset, source, aligned) != ESP_OK) {
      ESP_LOGE(TAG, "Could not write SAM storage slot %d", target);
      return false;
    }
    offset += chunk;
  }
  const uint32_t version = slot_version(default_index);
  SlotHeader header{SLOT_MAGIC, sequence_ + 1U, static_cast<uint32_t>(text.size()),
                    crc32(reinterpret_cast<const uint8_t *>(text.data()), text.size()) ^ version,
                    version, SLOT_COMMITTED};
  uint32_t read_crc = 0xFFFFFFFF;
  for (size_t read_offset = 0; read_offset < text.size();) {
    const size_t chunk = std::min(padded.size(), text.size() - read_offset);
    if (esp_partition_read(partition, sizeof(SlotHeader) + read_offset, padded.data(), chunk) != ESP_OK) {
      ESP_LOGE(TAG, "Could not verify SAM storage slot %d", target);
      return false;
    }
    read_crc = crc32_update(read_crc, padded.data(), chunk);
    read_offset += chunk;
  }
  if ((~read_crc ^ version) != header.data_crc) {
    ESP_LOGE(TAG, "SAM storage slot %d failed readback CRC; leaving it uncommitted", target);
    return false;
  }
  constexpr size_t COMMIT_OFFSET = offsetof(SlotHeader, committed);
  SlotHeader read_header{};
  if (esp_partition_write(partition, 0, &header, COMMIT_OFFSET) != ESP_OK ||
      esp_partition_read(partition, 0, &read_header, COMMIT_OFFSET) != ESP_OK ||
      std::memcmp(&header, &read_header, COMMIT_OFFSET) != 0 ||
      esp_partition_write(partition, COMMIT_OFFSET, &header.committed, sizeof(header.committed)) != ESP_OK) {
    ESP_LOGE(TAG, "Could not commit SAM storage slot %d", target);
    return false;
  }
  active_slot_ = target;
  sequence_ = header.sequence;
  return true;
}

void SamUploadComponent::process_upload_(std::string text) {
  auto parsed = std::make_unique<SamPresets>();
  std::vector<std::string> warnings;
  std::string error;
  if (!parse_sam_runtime(text, *parsed, warnings, error)) {
    this->set_result_("error", "Setup rejected; the last valid setup was kept. " + error, warnings);
    ESP_LOGW(TAG, "SAM upload rejected; see /upload/status for validation details");
  } else if (parsed->groups().empty() || parsed->groups().size() > UINT8_MAX) {
    this->set_result_("error", "Setup rejected: it must contain 1 to 255 usable groups.", warnings);
  } else {
    bool has_device = true;
    bool safe_names = true;
    for (const auto &group : parsed->owned) {
      if (group.devices.empty()) has_device = false;
      if (std::any_of(group.name.begin(), group.name.end(),
                      [](unsigned char ch) { return ch < 0x20 || ch == 0x7F; })) safe_names = false;
    }
    if (!safe_names) {
      this->set_result_("error", "Setup rejected: group names must not contain control characters.", warnings);
    } else if (!has_device) {
      this->set_result_("error", "Setup rejected: at least one group contains no usable monitors.", warnings);
    } else {
      pending_presets_ = std::move(parsed);
      staged_upload_ = std::move(text);
      table_revision_++;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        table_pending_ = true;
      }
      this->set_result_("staged", "Setup validated. Save and restart to select its first group.", warnings);
      ESP_LOGI(TAG, "Validated SAM setup (%u bytes, %u groups)",
               static_cast<unsigned>(staged_upload_.size()),
               static_cast<unsigned>(pending_presets_->groups().size()));
      for (const auto &warning : warnings) ESP_LOGW(TAG, "SAM setup warning: %s", warning.c_str());
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    upload_processing_ = false;
  }
}

void SamUploadComponent::set_result_(const std::string &state, const std::string &message,
                                     const std::vector<std::string> &warnings) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = state;
  message_ = message;
  warnings_ = warnings;
}

void SamUploadComponent::update_snapshot_() {
  std::lock_guard<std::mutex> lock(mutex_);
  const int active = hub_->get_active_group_index();
  if (active != snapshot_active_group_ || active_revision_ != table_revision_) {
    snapshot_active_group_ = active;
    active_revision_ = table_revision_;
    const GroupPreset *group = active >= 0 ? hub_->get_group(static_cast<uint8_t>(active)) : nullptr;
    if (group == nullptr) {
      active_group_json_ = "null";
    } else {
      const SamPresets *metadata = nullptr;
      if (runtime_presets_ != nullptr && static_cast<size_t>(active) < runtime_presets_->groups().size() &&
          group == &runtime_presets_->groups()[active]) {
        metadata = runtime_presets_.get();
      }
      const std::string array = setup_summary_json({*group}, metadata, static_cast<size_t>(active));
      active_group_json_ = array.substr(1, array.size() - 2);
    }
  }
  std::string groups = "{\"groups\":[";
  bool first = true;
  const SamPresets *shown = pending_presets_.get();
  const size_t count = shown != nullptr ? shown->groups().size() : 0;
  for (size_t i = 0; i < count; i++) {
    const GroupPreset *group = &shown->groups()[i];
    if (group == nullptr || group->name == nullptr) continue;
    const std::string escaped = json_escape(group->name);
    if (!first) groups += ",";
    first = false;
    groups += "\"" + escaped + "\"";
  }
  groups += "],\"revision\":" + std::to_string(table_revision_) + "}";
  groups_json_ = groups;

  if (status_snapshot_ready_ && status_revision_ == table_revision_ &&
      status_state_ == state_ && status_message_ == message_ && status_warnings_ == warnings_) return;
  status_revision_ = table_revision_;
  status_state_ = state_;
  status_message_ = message_;
  status_warnings_ = warnings_;
  status_snapshot_ready_ = true;
  status_json_ = "{\"state\":\"" + json_escape(state_) + "\",\"message\":\"" +
                 json_escape(message_) + "\",\"warnings\":[";
  for (size_t i = 0; i < warnings_.size(); i++) {
    if (i != 0) status_json_ += ",";
    status_json_ += "\"" + json_escape(warnings_[i]) + "\"";
  }
  const std::vector<GroupPreset> empty_groups;
  status_json_ += "],\"groups\":" + std::to_string(count) +
                  ",\"summary\":" +
                  setup_summary_json(shown != nullptr ? shown->groups() : empty_groups, shown) + "}";
}

}  // namespace gensam
}  // namespace esphome
