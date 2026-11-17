#!/usr/bin/env python3
"""Exercise production HTTP/storage snippets and the embedded UI without ESP hardware."""

from pathlib import Path
import json
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "components/gensam/sam_upload.cpp").read_text()


def main():
    assert 'path == "/group-view"' in SOURCE
    assert 'path == "/group-view.js"' in SOURCE
    assert "active_group_json()" in SOURCE
    snapshot = SOURCE[SOURCE.index("void SamUploadComponent::update_snapshot_()"):]
    assert "pending_presets_.get() : runtime_presets_.get()" not in snapshot
    assert "const SamPresets *shown = pending_presets_.get()" in snapshot
    snapshot = snapshot[:snapshot.index("\n}\n") + 2]
    helpers = SOURCE[SOURCE.index("constexpr uint32_t SLOT_SIZE"):SOURCE.index("const char SAM_INDEX_HTML")]
    handler = SOURCE[SOURCE.index("class SamHttpHandler"):SOURCE.index("}  // namespace\n")]
    restore = SOURCE[SOURCE.index("bool SamUploadComponent::restore_stored_"):SOURCE.index("void SamUploadComponent::initialize_group_select_")]
    persist = SOURCE[SOURCE.index("bool SamUploadComponent::persist_"):SOURCE.index("void SamUploadComponent::process_upload_")]
    loop = SOURCE[SOURCE.index("void SamUploadComponent::loop()"):SOURCE.index("void SamUploadComponent::restart_when_safe_()")]
    queues = SOURCE[SOURCE.index("bool SamUploadComponent::queue_upload"):SOURCE.index("std::string SamUploadComponent::status_json")]
    process = SOURCE[SOURCE.index("void SamUploadComponent::process_upload_"):SOURCE.index("void SamUploadComponent::set_result_")]
    cpp = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
template<typename... Args> void fake_log(Args&&...) {}
const char *TAG="test";
#define ESP_LOGE(...) fake_log(__VA_ARGS__)
#define ESP_LOGW(...) fake_log(__VA_ARGS__)
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
#define ESP_OK 0
constexpr size_t SAM_MAX_BYTES = 65536;
constexpr size_t SAM_MAX_GROUPS = 16;
constexpr int HTTP_GET=0, HTTP_POST=1, HTTPD_SOCK_ERR_TIMEOUT=-3;
struct esp_partition_t { std::vector<uint8_t> bytes = std::vector<uint8_t>(0x11000, 255); size_t size=0x11000; };
bool corrupt_read=false;
int esp_partition_read(const esp_partition_t *p,size_t off,void *out,size_t len) {
  memcpy(out,p->bytes.data()+off,len);
  if(corrupt_read && off>=24 && len) static_cast<uint8_t*>(out)[0]^=1;
  return 0;
}
int esp_partition_write(const esp_partition_t *p,size_t off,const void *in,size_t len) {
  memcpy(const_cast<esp_partition_t*>(p)->bytes.data()+off,in,len); return 0;
}
int esp_partition_erase_range(const esp_partition_t *p,size_t off,size_t len) {
  memset(const_cast<esp_partition_t*>(p)->bytes.data()+off,255,len); return 0;
}
struct FakeUrl { std::string text; const char *c_str() const { return text.c_str(); } };
struct AsyncWebServerResponse { std::string content; };
class AsyncWebServerRequest {
 public:
  static constexpr size_t URL_BUF_SIZE=128;
  std::string path="/upload/file", input="valid setup", response, parameter="0";
  std::map<std::string,std::string> headers{{"Origin","http://device"},{"Host","device"},{"Content-Type","application/octet-stream"}};
  int code=0, wire_code=0, verb=HTTP_POST, fail_after=-1, reads=0;
  AsyncWebServerResponse result;
  size_t offset=0, length=11;
  int method() const { return verb; }
  FakeUrl url_to(char*) const { return {path}; }
  std::optional<std::string> get_header(const char *key) const {
    auto it=headers.find(key); return it==headers.end()?std::nullopt:std::make_optional(it->second);
  }
  size_t contentLength() const { return length; }
  bool hasParam(const char*) const { return true; }
  std::string arg(const char*) const { return parameter; }
  void send(int c,const char *type=nullptr,const char *body=nullptr) {
    (void)type; code=c; wire_code=(c==200 || c==404 || c==409) ? c : 500; response=body?body:"";
  }
  AsyncWebServerResponse *beginResponse(int c,const char*,const std::string &body) {
    code=c; wire_code=(c==200 || c==404 || c==409) ? c : 500; result.content=body; return &result;
  }
  void send(AsyncWebServerResponse *r) { response=r->content; }
};
int httpd_resp_set_status(AsyncWebServerRequest &r, const char *status) {
  r.wire_code=std::stoi(status); return 0;
}
int httpd_req_recv(AsyncWebServerRequest &r,char *out,size_t len) {
  r.reads++;
  if(r.fail_after>=0 && r.offset>=static_cast<size_t>(r.fail_after)) return 0;
  len=std::min({len,size_t(4),r.input.size()-r.offset});
  memcpy(out,r.input.data()+r.offset,len); r.offset+=len; return static_cast<int>(len);
}
class AsyncWebHandler {
 public:
  virtual ~AsyncWebHandler()=default;
  virtual bool canHandle(AsyncWebServerRequest*) const=0;
  virtual void handleRequest(AsyncWebServerRequest*)=0;
  virtual bool isRequestHandlerTrivial() const { return true; }
};
struct SamPresets {
  struct Group {
    struct DeviceLayout { std::string name; };
    std::string name="A"; std::vector<int> devices{1}; std::vector<DeviceLayout> layouts;
  };
  std::vector<Group> owned{Group{}};
  std::map<uint32_t, std::string> device_names() const { return {}; }
  std::vector<int> views{1};
  const std::vector<int> &groups() const { return views; }
};
enum class PeqType { BYPASS, LOW_SHELF, HIGH_SHELF, PEAKING };
constexpr float SHELF_Q_LOW=0.3f, SHELF_Q_HIGH=0.5f;
constexpr uint8_t SOURCE_ANALOG=1, SOURCE_DIGITAL_AES3=2;
constexpr uint8_t AES3_CHANNEL_A=1, AES3_CHANNEL_B=2, AES3_CHANNEL_SUM=3;
constexpr uint16_t CROSSOVER_FULL_BAND=1;
struct PeqBand { PeqType type=PeqType::BYPASS; float frequency_hz=0, gain_db=0, q=0; };
struct GroupDevice {
  uint32_t unique_id=0;
  bool enabled=true;
  uint16_t crossover_hz=1;
  uint8_t source=SOURCE_ANALOG, aes3_channel=AES3_CHANNEL_A, lfe_channel=0;
  float level_db=0, lfe_level_db=0;
  uint32_t delay_samples=0;
  const PeqBand *bands=nullptr;
  uint8_t band_count=0;
  uint32_t peq_design_rate_hz=0;
};
struct GroupPreset {
  const char *name=nullptr;
  const GroupDevice *devices=nullptr;
  uint8_t device_count=0;
};
bool parse_sam_runtime(const std::string &text,SamPresets&,
                       std::vector<std::string> &warnings,std::string &error) {
  warnings.clear(); error.clear(); return text!="invalid";
}
struct FakeHub {
  int default_index=-1;
  bool busy=false;
  bool replace_runtime_group_table(const int*,uint8_t) { return true; }
  void set_default_group(uint8_t index) { default_index=index; }
  void set_group_generation(uint32_t) {}
  void set_sam_monitor_names(const std::map<uint32_t, std::string> &) {}
  bool is_group_apply_in_progress() const { return busy; }
};
class SamUploadComponent {
 public:
  esp_partition_t a,b;
  const esp_partition_t *partitions_[2]{&a,&b};
  int active_slot_=-1;
  uint32_t sequence_=0;
  uint32_t table_revision_=0;
  std::unique_ptr<SamPresets> runtime_presets_;
  std::unique_ptr<SamPresets> pending_presets_;
  FakeHub hub;
  FakeHub *hub_=&hub;
  std::vector<std::string> result_warnings;
  std::vector<std::string> warnings_;
  std::string state_, message_, queued_upload_, staged_upload_;
  bool storage_ready_=true, upload_queued_=false, upload_processing_=false;
  bool table_pending_=false, commit_queued_=false, restart_scheduled=false;
  int commit_index_=-1;
  std::mutex mutex_;
  void set_result_(const std::string& state,const std::string& message,const std::vector<std::string>& warnings={}) {
    state_=state; message_=message; result_warnings=warnings; warnings_=warnings;
  }
  void update_snapshot_() {}
  void restart_when_safe_() {}
  template<typename F> void set_timeout(const char*, int, F) { restart_scheduled=true; }
  std::string status_json() { return "{}"; }
  std::string groups_json() { return "{}"; }
  std::string active_group_json() { return "null"; }
  void loop();
  bool queue_commit();
  bool queue_upload(std::string &&body);
  void process_upload_(std::string text);
  void initialize_group_select_() {}
  void publish_monitor_names_();
  bool persist_(const std::string &text, int default_index=-1);
  bool restore_stored_();
};
const char SAM_INDEX_HTML[]="";
const char GROUP_VIEW_JS[]="";
'''
    checks = r'''
int main() {
  PeqBand bands[]={{PeqType::PEAKING,56.2f,-1,2.5f},{PeqType::LOW_SHELF,100,-4.3e-14f,0},
                   {PeqType::HIGH_SHELF,8000,0,0},{PeqType::BYPASS,0,0,0}};
  GroupDevice devices[2]{};
  devices[0].unique_id=1654321; devices[0].source=SOURCE_DIGITAL_AES3;
  devices[0].aes3_channel=AES3_CHANNEL_B; devices[0].bands=bands; devices[0].band_count=4;
  devices[0].peq_design_rate_hz=48000;
  devices[1].unique_id=1842915; devices[1].source=SOURCE_DIGITAL_AES3;
  devices[1].aes3_channel=AES3_CHANNEL_A; devices[1].enabled=false;
  GroupPreset presets[]={{"Lähikenttä",devices,2}};
  const std::string summary=setup_summary_json(std::vector<GroupPreset>(presets,presets+1));
  assert(summary.find("\"input\":\"AES3 B (Right)\"")!=std::string::npos);
  assert(summary.find("\"input\":\"AES3 A (Left)\"")!=std::string::npos);
  assert(summary.find("\"enabled\":false")!=std::string::npos);
  assert(summary.find("\"peq_active\":2")!=std::string::npos);
  assert(summary.find("\"peq_design_rate_hz\":48000")!=std::string::npos);
  assert(summary.find("\"type\":\"Peaking\"")!=std::string::npos);
  assert(summary.find("\"type\":\"Low shelf\"")!=std::string::npos);
  assert(summary.find("\"type\":\"High shelf\"")!=std::string::npos);
  assert(summary.find(",null]")!=std::string::npos);
  assert(summary.find("\"q\":2.5")!=std::string::npos);
  assert(summary.find("\"effective_q\":0.300000012")!=std::string::npos);
  assert(summary.find("\"effective_q\":0.5")!=std::string::npos);
  assert(summary.find("e-14")!=std::string::npos);
  assert(summary.find("Lähikenttä")!=std::string::npos);
  SamPresets metadata;
  metadata.owned[0].layouts.resize(2);
  metadata.owned[0].layouts[0]={"Left"};
  metadata.owned[0].layouts[1]={"Right"};
  const std::string named_summary=setup_summary_json(std::vector<GroupPreset>(presets,presets+1),&metadata);
  assert(named_summary.find("\"name\":\"Left\"")!=std::string::npos);
  assert(named_summary.find("\"position\"")==std::string::npos);
  metadata.owned.push_back(metadata.owned[0]);
  metadata.owned[1].layouts[0].name="Second group speaker";
  const std::string second=setup_summary_json(std::vector<GroupPreset>(presets,presets+1),&metadata,1);
  assert(second.find("Second group speaker")!=std::string::npos);
  assert(second.find("\"name\":\"Left\"")==std::string::npos);

  SamUploadComponent parent; SamHttpHandler h(&parent);
  AsyncWebServerRequest page; page.path="/upload"; page.verb=HTTP_GET;
  assert(h.canHandle(&page));
  AsyncWebServerRequest active; active.path="/group-view"; active.verb=HTTP_GET;
  assert(h.canHandle(&active)); h.handleRequest(&active); assert(active.response=="null");
  active.path="/group-view.js"; assert(h.canHandle(&active));
  AsyncWebServerRequest old; old.path="/sam"; old.verb=HTTP_GET;
  assert(!h.canHandle(&old));
  AsyncWebServerRequest denied; denied.headers["Origin"]="http://elsewhere";
  h.handleRequest(&denied); assert(denied.wire_code==403 && denied.reads==0);
  AsyncWebServerRequest huge; huge.length=65537;
  h.handleRequest(&huge); assert(huge.wire_code==413 && huge.reads==0);
  AsyncWebServerRequest interrupted; interrupted.fail_after=4;
  h.handleRequest(&interrupted); assert(interrupted.wire_code==400 && parent.queued_upload_.empty());
  AsyncWebServerRequest next;
  h.handleRequest(&next); assert(parent.queued_upload_=="valid setup");
  assert(next.wire_code==202);
  parent.loop(); assert(parent.state_=="staged" && parent.active_slot_==-1);
  AsyncWebServerRequest commit_request; commit_request.path="/upload/commit";
  commit_request.parameter="ignored";  // No startup selection is required or used.
  h.handleRequest(&commit_request); assert(commit_request.wire_code==202 && parent.commit_queued_);
  parent.loop(); assert(parent.restart_scheduled && parent.active_slot_>=0);
  SamUploadComponent flow;
  assert(flow.queue_upload(std::string("valid setup")));
  flow.loop(); assert(flow.state_=="staged" && flow.active_slot_==-1);
  assert(flow.queue_commit());
  flow.hub.busy=true; flow.loop();
  assert(flow.commit_queued_ && flow.active_slot_==-1);
  flow.hub.busy=false; flow.loop();
  assert(flow.restart_scheduled && flow.active_slot_>=0 && flow.state_=="restarting");
  assert(parent.persist_("previous"));
  assert(parent.persist_("with default", 0));
  SlotHeader default_header{};
  assert(valid_slot(parent.partitions_[parent.active_slot_], default_header));
  assert(slot_default(default_header) == 0);
  assert(parent.restore_stored_());
  assert(parent.hub.default_index == 0);
  SlotHeader old_filtered = default_header;
  old_filtered.version = (old_filtered.version & ~0xFFU) | SLOT_VERSION_DEFAULT;
  old_filtered.data_crc ^= default_header.version ^ old_filtered.version;
  memcpy(const_cast<esp_partition_t *>(parent.partitions_[parent.active_slot_])->bytes.data(),
         &old_filtered, sizeof(old_filtered));
  SamUploadComponent migrated;
  migrated.a=parent.a; migrated.b=parent.b;
  assert(migrated.restore_stored_());
  assert(migrated.hub.default_index==0 && !migrated.result_warnings.empty());
  memcpy(const_cast<esp_partition_t *>(parent.partitions_[parent.active_slot_])->bytes.data(),
         &default_header, sizeof(default_header));
  auto &version_byte = const_cast<esp_partition_t *>(parent.partitions_[parent.active_slot_])->bytes[17];
  version_byte ^= 1;
  assert(!valid_slot(parent.partitions_[parent.active_slot_], default_header));
  version_byte ^= 1;
  assert(parent.persist_("without default", -1));
  assert(valid_slot(parent.partitions_[parent.active_slot_], default_header));
  assert(slot_default(default_header) == -1);
  SamUploadComponent boot;
  boot.a=parent.a; boot.b=parent.b;
  assert(boot.restore_stored_());
  assert(boot.hub.default_index==0);
  SlotHeader legacy = default_header;
  legacy.data_crc ^= legacy.version;
  legacy.version = SLOT_VERSION;
  memcpy(const_cast<esp_partition_t *>(parent.partitions_[parent.active_slot_])->bytes.data(),
         &legacy, sizeof(legacy));
  assert(valid_slot(parent.partitions_[parent.active_slot_], default_header));
  assert(slot_default(default_header) == -1);
  const int previous=parent.active_slot_; const auto seq=parent.sequence_;
  corrupt_read=true;
  assert(!parent.persist_("replacement"));
  assert(parent.active_slot_==previous && parent.sequence_==seq);
  uint32_t commit=0;
  const auto *failed=parent.partitions_[1-previous];
  memcpy(&commit,failed->bytes.data()+20,4); assert(commit==UINT32_MAX);
  corrupt_read=false;
  SlotHeader header{}; std::string restored;
  assert(valid_slot(parent.partitions_[previous],header,&restored) && restored=="without default");
  assert(parent.persist_("replacement"));
  assert(valid_slot(parent.partitions_[parent.active_slot_],header,&restored) && restored=="replacement");
  assert(parent.persist_("invalid")); // CRC-valid newest setup, rejected by the parser.
  const int rejected=parent.active_slot_; const auto newest_sequence=parent.sequence_;
  assert(parent.restore_stored_());
  assert(parent.active_slot_==1-rejected && !parent.result_warnings.empty());
  assert(parent.sequence_==newest_sequence);
  assert(parent.persist_("new valid setup"));
  assert(parent.active_slot_==rejected && parent.sequence_==newest_sequence+1);
}
'''
    javascript = re.search(r"<script>\n(.*?)</script>", SOURCE, re.S).group(1)
    ui = r'''
const assert=require('assert');
const elements={};
function element(tag='div'){
  return {tag, value:'', ownText:'', items:[],style:{},attributes:{},setAttribute(k,v){this.attributes[k]=v},set textContent(v){this.ownText=v;this.items=[]},
    get textContent(){return this.ownText+this.items.map(x=>x.textContent).join('')},
    replaceChildren(...items){this.ownText='';this.items=items;this.value=items[0]?.value||''},
    append(...items){this.items.push(...items)}};
}
global.document={getElementById:id=>elements[id]||(elements[id]=Object.assign(element(),{hidden:['feedback','details'].includes(id)})),
createElement:tag=>element(tag)};
global.window=global;
global.setInterval=()=>{};
global.setTimeout=()=>{};
let revision=1;
let state='ready';
const fetched=[];
const importedSummary=[{name:'Lähikenttä <test>',devices:[{id:1654321,name:'Left',enabled:true,input:'AES3 B (Right)',crossover:'Full band',level_db:0,delay_samples:0,delay_ms:0,lfe:'none',lfe_level_db:0,peq_active:1,peq_design_rate_hz:48000,peq_bands:[{type:'Peaking',frequency_hz:56.2,gain_db:-2.3e-5,q:2.5},{type:'Low shelf',frequency_hz:100,gain_db:0,q:0,effective_q:0.300000012},null]},{id:1842915,name:'Right',enabled:true,input:'AES3 A (Left)',crossover:'Full band',level_db:0,delay_samples:0,delay_ms:0,lfe:'none',lfe_level_db:0,peq_active:0,peq_design_rate_hz:48000,peq_bands:[]}]}];
global.fetch=async url=>{fetched.push(url);return {ok:true,json:async()=>url==='/upload/status'?{state,message:'Validated. Choose a startup default, then save and restart.',warnings:['Time-of-flight_Compensation is not applied'],summary:importedSummary}:{groups:['A','B'],selected:0,revision}}};
const healthyFetch=global.fetch;
const startupFailure=process.argv[2];
if(startupFailure)global.fetch=async()=>{
  if(startupFailure==='network')throw new Error('offline');
  if(startupFailure==='http')return {ok:false,status:503};
  return {ok:true,json:async()=>{throw new SyntaxError('Invalid JSON')}};
};
'''
    ui_checks = r'''
;(async()=>{
const feedback=document.getElementById('feedback'), details=document.getElementById('details');
await refresh();
if(startupFailure){
  assert.equal(feedback.hidden,false,'an initial status failure is visible before any upload');
  assert.match(message.textContent,/Could not reach the device:/);
  assert.match(message.textContent,new RegExp({network:'offline',http:'HTTP 503',json:'Invalid JSON'}[startupFailure]));
  assert.equal(apply.disabled,true,'save stays disabled when status is unavailable');
  assert.equal(details.hidden,true,'a failed initial status request shows no preview');
  global.fetch=healthyFetch;
  await refresh();
}
assert.equal(feedback.hidden,true,'no status is shown before an upload');
assert.equal(details.hidden,true,'no preview is shown before a file validates');
assert(fetched.length&&fetched.every(url=>url==='/upload/status'),'the page does not load the active group');
assert.doesNotMatch(SOURCE_HTML,/Setup status|Active group|Loading/,'idle sections are gone');
document.getElementById('file').files=[];
await document.getElementById('upload').onclick();
assert.equal(feedback.hidden,false,'a missing file is reported');
await refresh();
assert.match(message.textContent,/Choose a \.sam file/,'the hint survives a poll');
assert.equal(warnings.items.length,0,'the hint does not reveal stored setup warnings');
requestError=null;
await refresh();
assert.equal(feedback.hidden,true,'a missing file does not count as an upload');
state='staged';
await refresh();
assert.equal(feedback.hidden,false,'a validated upload shows its result');
assert.equal(details.hidden,false,'a validated upload shows its preview');
assert.equal(apply.disabled,false,'validated setup enables save');
assert(!document.getElementById('groups').items.length,'upload has no default chooser');
assert.match(warnings.items[0].textContent,/Warning: Time-of-flight/);
assert.match(summary.textContent,/AES3 B \(Right\)/,'uploaded input routing is shown');
assert.match(summary.textContent,/Lähikenttä <test>/,'group names are rendered as text');
assert.match(summary.textContent,/PEQ 1\/3 active/,'actual slot count is displayed');
assert.match(summary.textContent,/Peaking/);
assert.match(summary.textContent,/56\.2/);
assert.match(summary.textContent,/-0\.000023/,'small nonzero gains remain visible');
assert.match(summary.textContent,/2\.5/);
assert.match(summary.textContent,/Low shelf/);
assert.match(summary.textContent,/0\.3 \(fixed\)/);
assert.match(summary.textContent,/Bypass/);
assert.match(summary.textContent,/48000/);
assert.match(summary.textContent,/Left/,'SAM speaker names are shown');
const view=summary.items[0].items[1];
assert.doesNotMatch(summary.textContent,/SAM position sketch|X 1 · Y 5 · Z 5/,'speaker positions are not rendered');
assert(!view.items.some(item=>item.className==='sam-layout'),'a position diagram is not rendered');
state='ready';
await refresh();
assert.doesNotMatch(summary.textContent,/Lähikenttä/,'saved setup is not presented as an upload');
assert.equal(details.hidden,true,'the preview section disappears with the staged setup');
assert.equal(feedback.hidden,true,'a reloaded page shows no status for a stored setup');
state='staged';
await refresh();
assert.match(summary.textContent,/Lähikenttä/,'newly validated setup is presented');
const originalSummary=summary.items[0];
await refresh();
assert.strictEqual(summary.items[0],originalSummary,'poll must preserve expanded sections');
let commitUrl;
const originalFetch=global.fetch;
global.fetch=async(url,options)=>{if(options?.method==='POST')commitUrl=url;return originalFetch(url)};
await document.getElementById('apply').onclick();
assert.equal(commitUrl,'/upload/commit','commit has no group index');
global.fetch=async()=>({ok:false,status:409,json:async()=>({error:'Upload busy'})});
await mutate('/upload/file',{});
assert.match(message.textContent,/Upload busy/);
assert.equal(feedback.hidden,false,'a failed request is shown');
global.fetch=async url=>({ok:true,json:async()=>url==='/upload/status'?{state,message:'Validated. Choose a startup default, then save and restart.',warnings:[]}:{groups:['A','B'],selected:0,revision}});
await refresh();
assert.match(message.textContent,/Upload busy/,'poll must preserve HTTP error');
state='empty'; requestError=null;
await refresh();
assert.equal(apply.disabled,true,'save is unavailable without a staged setup');
assert.equal(feedback.hidden,false,'status stays visible once an upload was attempted');
global.fetch=async()=>({ok:true,status:202,json:async()=>({message:'Upload accepted for validation'})});
await mutate('/upload/file',{});
assert.match(message.textContent,/accepted for validation/,'accepted upload is visible');
state='saving';
global.fetch=async url=>({ok:true,json:async()=>url==='/upload/status'?{state,message:'Saving setup',warnings:[]}:{groups:['A','B'],revision}});
await refresh();
assert.equal(apply.disabled,true,'save stays disabled while committing');
global.fetch=async()=>{throw new Error('offline')};
await document.getElementById('apply').onclick();
assert.match(message.textContent,/offline/,'Apply catches fetch failure');
})().catch(e=>{console.error(e);process.exitCode=1});
'''
    selector_source = (ROOT / "components/gensam/select.h").read_text()
    selector = selector_source[selector_source.index("class GenSAMGroupSelect"):
                               selector_source.index("/// @brief Select entity that routes")]
    selector_test = r'''
#include <cassert>
#include <string>
#define ESP_LOGW(...) ((void) 0)
namespace select {
class Select { protected: virtual void control(const std::string &) = 0; };
}
class GenSAMHub {
 public:
  bool listen_only=false;
  std::string selected;
  bool is_listen_only() const { return listen_only; }
  void set_active_group_by_name(const std::string &name) { selected=name; }
};
'''
    selector_checks = r'''
class TestSelect : public GenSAMGroupSelect {
 public: using GenSAMGroupSelect::control;
};
int main() {
  GenSAMHub hub; TestSelect select; select.set_hub(&hub);
  select.control("A"); assert(hub.selected.empty());
  select.set_runtime_ready(); hub.listen_only=true;
  select.control("A"); assert(hub.selected.empty());
  hub.listen_only=false; select.control("A"); assert(hub.selected=="A");
}
'''
    snapshot_test = r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
struct GroupPreset { const char *name; };
struct SamPresets {
  std::vector<GroupPreset> items;
  const std::vector<GroupPreset> &groups() const { return items; }
};
std::string json_escape(const std::string &text) { return text; }
std::string setup_summary_json(const std::vector<GroupPreset> &groups, const SamPresets *, size_t = 0) {
  return groups.empty() ? "[]" : "[\"" + std::string(groups[0].name) + "\"]";
}
struct Hub {
  int active{-1};
  SamPresets *runtime{nullptr};
  int get_active_group_index() const { return active; }
  const GroupPreset *get_group(uint8_t index) const {
    return runtime && index < runtime->items.size() ? &runtime->items[index] : nullptr;
  }
};
class SamUploadComponent {
 public:
  void update_snapshot_();
  std::mutex mutex_;
  std::unique_ptr<SamPresets> pending_presets_, runtime_presets_;
  uint32_t table_revision_{0}, status_revision_{0};
  bool status_snapshot_ready_{false};
  std::string groups_json_, status_json_, state_, message_, status_state_, status_message_;
  std::vector<std::string> warnings_, status_warnings_;
  Hub hub, *hub_{&hub};
  std::string active_group_json_{"null"};
  int snapshot_active_group_{-2};
  uint32_t active_revision_{0};
};
'''
    snapshot_checks = r'''
int main() {
  SamUploadComponent upload;
  upload.runtime_presets_ = std::make_unique<SamPresets>();
  upload.runtime_presets_->items.push_back({"Current"});
  upload.runtime_presets_->items.push_back({"Other"});
  upload.hub.runtime = upload.runtime_presets_.get();
  upload.hub.active = 0;
  upload.state_ = "ready";
  upload.update_snapshot_();
  assert(upload.status_json_.find("Current") == std::string::npos);
  assert(upload.groups_json_.find("Current") == std::string::npos);
  assert(upload.active_group_json_ == "\"Current\"");
  upload.pending_presets_ = std::make_unique<SamPresets>();
  upload.pending_presets_->items.push_back({"New"});
  upload.table_revision_++;
  upload.state_ = "staged";
  upload.update_snapshot_();
  assert(upload.status_json_.find("New") != std::string::npos);
  assert(upload.groups_json_.find("New") != std::string::npos);
  assert(upload.status_json_.find("Current") == std::string::npos);
  assert(upload.groups_json_.find("selected") == std::string::npos);
  assert(upload.active_group_json_ == "\"Current\"");
  upload.hub.active = 1;
  upload.update_snapshot_();
  assert(upload.active_group_json_ == "\"Other\"");
  upload.hub.active = -1;
  upload.update_snapshot_();
  assert(upload.active_group_json_ == "null");
  upload.pending_presets_.reset();
  upload.table_revision_++;
  upload.state_ = "error";
  upload.update_snapshot_();
  assert(upload.status_json_.find("New") == std::string::npos);
  assert(upload.groups_json_.find("Current") == std::string::npos);
}
'''
    with tempfile.TemporaryDirectory(prefix=".sam-upload-test-", dir=ROOT) as directory:
        directory = Path(directory)
        source = directory / "test.cpp"
        binary = directory / "test"
        source.write_text(cpp + helpers + handler + restore + persist + loop + queues + process + checks)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-o", str(binary), str(source)], check=True)
        subprocess.run([str(binary)], check=True)
        source.write_text(snapshot_test + snapshot + snapshot_checks)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-o", str(binary), str(source)], check=True)
        subprocess.run([str(binary)], check=True)
        source.write_text(selector_test + selector + selector_checks)
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-o", str(binary), str(source)], check=True)
        subprocess.run([str(binary)], check=True)
        script = directory / "test.js"
        renderer = re.search(r'R"JS\((.*?)\)JS"', (ROOT / "components/gensam/group_view.h").read_text(), re.S).group(1)
        page = re.search(r'R"HTML\((.*?)\)HTML"', SOURCE, re.S).group(1)
        script.write_text(ui + f"const SOURCE_HTML={json.dumps(page)};\n" + renderer + javascript + ui_checks)
        subprocess.run(["node", str(script)], check=True)
        for failure in ("network", "http", "json"):
            subprocess.run(["node", str(script), failure], check=True)
    print("SAM HTTP disconnect/origin/limits, transactional CRC, and UI regressions passed")


if __name__ == "__main__":
    main()
