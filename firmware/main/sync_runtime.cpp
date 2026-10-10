#include "sync_runtime.hpp"

#include "rig_store.hpp"
#include "content_store.hpp"
#include "hub75_hal.hpp"
#include "node_http_auth.hpp"
#include "render_runtime.hpp"
#include "sync_protocol.hpp"
#include "web_api.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <strings.h>
#include <vector>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

namespace ledpanel {
namespace {
constexpr const char* kTag = "sync-runtime";
constexpr uint16_t kSyncPort = 42175;
constexpr size_t kMaxPeers = 15;  // 16 active nodes, including the Master.
constexpr size_t kMaxSoftApFollowers = 3;  // AP station slots also serve operators.
constexpr int64_t kChallengeLifetimeUs = 5000000;
constexpr int64_t kPeerTimeoutUs = 1000000;
constexpr int64_t kHeartbeatIntervalUs = 250000;
constexpr int64_t kHandshakeRetryUs = 500000;
constexpr int64_t kHelloIntervalUs = 1000000;
constexpr int64_t kNodeHttpNonceLifetimeUs = 10LL * 60 * 1000000;
constexpr size_t kNodeHttpNonceCapacity = 128;
constexpr char kZeroHash[] = "0000000000000000000000000000000000000000000000000000000000000000";

struct NodeHttpNonce {
  std::array<uint8_t, 16> value{};
  int64_t expires_us = 0;
};

struct NodeHttpReplayState {
  bool has_sequence = false;
  uint32_t sequence = 0;
  std::array<NodeHttpNonce, kNodeHttpNonceCapacity> nonces{};
};

struct PendingSession {
  bool valid = false;
  bool candidate_ready = false;
  std::string nonce_b;
  std::string nonce_a;
  std::string candidate_key;
  uint64_t remote_boot_id = 0;
  int64_t expires_us = 0;
  int64_t last_response_us = 0;
  uint32_t group_sequence = 0;
  sockaddr_in address{};
  SyncReplayWindow candidate_replay;
};

enum class FollowerTransactionPhase : uint8_t {
  None,
  Prepared,
  Committed,
  Applied,
  Rejected,
};

enum class MasterPeerTransactionPhase : uint8_t {
  None,
  WaitingReady,
  Ready,
  Committed,
  Applied,
  Rejected,
};

enum class MasterTransactionPhase : uint8_t {
  Idle,
  Preparing,
  Committing,
  WaitingApplied,
  Finished,
};

struct FollowerTransaction {
  FollowerTransactionPhase phase = FollowerTransactionPhase::None;
  std::string command_id;
  PlaybackState next_state;
  int64_t apply_at_local_us = 0;
  int64_t completed_at_us = 0;
  std::string error;
};

struct Peer {
  bool used = false;
  bool enabled = false;
  bool active = false;
  uint16_t node_id = 0;
  std::string hardware_id;
  std::string label;
  sockaddr_in address{};
  uint64_t remote_boot_id = 0;
  std::string session_key;
  uint32_t tx_sequence = 0;
  uint32_t node_http_tx_sequence = 0;
  SyncReplayWindow replay;
  int64_t last_seen_us = 0;
  int64_t last_heartbeat_us = 0;
  int64_t last_state_us = 0;
  int64_t last_time_request_us = 0;
  int64_t last_clock_adjust_us = 0;
  int64_t last_valid_clock_us = 0;
  int64_t clock_offset_us = 0;
  uint32_t clock_rtt_us = 0;
  uint64_t pending_time_t1 = 0;
  int64_t pending_time_sent_us = 0;
  std::array<int64_t, 8> clock_offsets{};
  std::array<uint32_t, 8> clock_rtts{};
  uint8_t clock_sample_count = 0;
  uint8_t clock_sample_next = 0;
  bool pending_time = false;
  bool clock_synced = false;
  uint32_t remote_revision = 0;
  uint32_t remote_topology_revision = 0;
  std::string remote_topology_hash;
  std::string remote_active_hash;
  std::string remote_candidate_hash;
  std::string remote_health = "unsynced";
  std::string remote_fault_code = "PEER_OFFLINE";
  bool remote_clock_synced = false;
  int64_t remote_clock_offset_us = 0;
  uint32_t remote_clock_rtt_us = 0;
  int64_t remote_clock_sample_age_ms = -1;
  int64_t remote_clock_status_us = 0;
  bool transaction_required = false;
  MasterPeerTransactionPhase transaction_phase = MasterPeerTransactionPhase::None;
  std::string transaction_error;
  int64_t transaction_last_send_us = 0;
  FollowerTransaction transaction;
  PendingSession pending;
};

struct BlackoutBroadcast {
  bool pending = false;
  uint32_t revision = 0;
  std::string reason;
  int64_t next_send_us = 0;
  uint8_t attempts = 0;
};

struct MasterTransaction {
  MasterTransactionPhase phase = MasterTransactionPhase::Idle;
  bool success = false;
  std::string command_id;
  std::string error;
  PlaybackState next_state;
  int64_t started_us = 0;
  int64_t last_send_us = 0;
  size_t required_count = 0;
};

NodeConfig* g_config = nullptr;
PlaybackState g_playback_snapshot;
uint64_t* g_boot_id = nullptr;
int g_socket = -1;
SemaphoreHandle_t g_mutex = nullptr;
bool g_started = false;
bool g_master_address_resolved = false;
sockaddr_in g_master_address{};
Peer g_peers[kMaxPeers];
NodeHttpReplayState g_node_http_replay[kMaxPeers];
BlackoutBroadcast g_blackout_broadcast;
MasterTransaction g_master_transaction;
uint32_t g_hello_sequence = 0;
int64_t g_last_hello_us = 0;
int64_t g_last_follower_heartbeat_us = 0;

NodeHttpReplayState* node_http_replay_state(Peer* peer) {
  for (size_t i = 0; i < kMaxPeers; ++i)
    if (&g_peers[i] == peer) return &g_node_http_replay[i];
  return nullptr;
}

void reset_node_http_replay(Peer* peer) {
  if (NodeHttpReplayState* replay = node_http_replay_state(peer))
    *replay = NodeHttpReplayState{};
  if (peer) peer->node_http_tx_sequence = 0;
}

struct NodeHttpClientCapture {
  uint8_t* body = nullptr;
  size_t capacity = 0;
  size_t length = 0;
  bool overflow = false;
  std::string sequence;
  std::string nonce;
  std::string body_hash;
  std::string mac;
  std::string candidate_hash;
};

esp_err_t node_http_client_event(esp_http_client_event_t* event) {
  if (!event || !event->user_data) return ESP_OK;
  auto& capture = *static_cast<NodeHttpClientCapture*>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_DATA) {
    if (event->data_len < 0 || (event->data_len > 0 && !event->data) ||
        static_cast<size_t>(event->data_len) > capture.capacity - capture.length) {
      capture.overflow = true;
      return ESP_FAIL;
    }
    if (event->data_len > 0) {
      std::memcpy(capture.body + capture.length, event->data,
                  static_cast<size_t>(event->data_len));
      capture.length += static_cast<size_t>(event->data_len);
    }
  } else if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
             event->header_value) {
    if (strcasecmp(event->header_key, "X-Node-Seq") == 0)
      capture.sequence = event->header_value;
    else if (strcasecmp(event->header_key, "X-Request-Nonce") == 0)
      capture.nonce = event->header_value;
    else if (strcasecmp(event->header_key, "X-Body-SHA256") == 0)
      capture.body_hash = event->header_value;
    else if (strcasecmp(event->header_key, "X-Node-MAC") == 0)
      capture.mac = event->header_value;
    else if (strcasecmp(event->header_key, "X-Content-SHA256") == 0)
      capture.candidate_hash = event->header_value;
  }
  return ESP_OK;
}

bool master_peer_is_live(const Peer& peer, const RigFollowerIdentity& follower,
                         int64_t now) {
  return peer.used && peer.enabled && peer.active &&
      peer.node_id == follower.node_id &&
      peer.hardware_id == follower.hardware_id && peer.last_seen_us > 0 &&
      now >= peer.last_seen_us && now - peer.last_seen_us <= kPeerTimeoutUs;
}

bool master_peer_matches_state(const Peer& peer, const PlaybackState& state) {
  return peer.remote_revision == state.revision &&
      peer.remote_topology_revision == state.topologyRevision &&
      peer.remote_topology_hash == state.topologyHash &&
      peer.remote_active_hash == state.packageHash;
}

void wipe(std::string& value) {
  volatile char* bytes = value.empty() ? nullptr : value.data();
  for (size_t i = 0; i < value.size(); ++i) bytes[i] = 0;
  value.clear();
}

bool lower_hex(const std::string& value, size_t expected_length) {
  if (value.size() != expected_length) return false;
  for (const unsigned char c : value)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

bool admit_node_http_replay(NodeHttpReplayState& replay, uint32_t sequence,
                            const std::string& nonce, int64_t now_us) {
  if (!lower_hex(nonce, 32) ||
      (replay.has_sequence && sequence <= replay.sequence)) return false;
  std::array<uint8_t, 16> nonce_bytes{};
  for (size_t i = 0; i < nonce_bytes.size(); ++i) {
    const auto nibble = [](char c) -> uint8_t {
      return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    nonce_bytes[i] = static_cast<uint8_t>(
        (nibble(nonce[i * 2]) << 4) | nibble(nonce[i * 2 + 1]));
  }
  NodeHttpNonce* free_entry = nullptr;
  for (auto& entry : replay.nonces) {
    if (entry.expires_us <= now_us) {
      entry.value.fill(0);
      entry.expires_us = 0;
    }
    if (entry.expires_us != 0 && entry.value == nonce_bytes) return false;
    if (!free_entry && entry.expires_us == 0) free_entry = &entry;
  }
  if (!free_entry || now_us < 0 || now_us > INT64_MAX - kNodeHttpNonceLifetimeUs)
    return false;
  free_entry->value = nonce_bytes;
  free_entry->expires_us = now_us + kNodeHttpNonceLifetimeUs;
  replay.has_sequence = true;
  replay.sequence = sequence;
  return true;
}

bool valid_uuid(const std::string& value) {
  if (value.size() != 36) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else if (!((value[i] >= '0' && value[i] <= '9') ||
                 (value[i] >= 'a' && value[i] <= 'f'))) return false;
  }
  return true;
}

std::string boot_hex(uint64_t value) {
  char text[17] = {};
  std::snprintf(text, sizeof(text), "%016llx",
                static_cast<unsigned long long>(value));
  return text;
}

std::string random_hex(size_t bytes) {
  std::vector<uint8_t> raw(bytes);
  esp_fill_random(raw.data(), raw.size());
  static constexpr char digits[] = "0123456789abcdef";
  std::string result(raw.size() * 2, '0');
  for (size_t i = 0; i < raw.size(); ++i) {
    result[i * 2] = digits[raw[i] >> 4];
    result[i * 2 + 1] = digits[raw[i] & 0x0f];
  }
  std::memset(raw.data(), 0, raw.size());
  return result;
}

cJSON* parse_payload(const std::string& payload) {
  cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
  return cJSON_IsObject(root) ? root : (cJSON_Delete(root), nullptr);
}

bool exact_keys(const cJSON* root, std::initializer_list<const char*> keys) {
  if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != static_cast<int>(keys.size()))
    return false;
  for (const char* key : keys)
    if (!cJSON_GetObjectItemCaseSensitive(root, key)) return false;
  return true;
}

bool json_uint(const cJSON* root, const char* key, uint32_t maximum,
               uint32_t& output) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
      item->valuedouble > maximum ||
      std::floor(item->valuedouble) != item->valuedouble) return false;
  output = static_cast<uint32_t>(item->valuedouble);
  return true;
}

bool json_string(const cJSON* root, const char* key, std::string& output) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsString(item) || !item->valuestring) return false;
  output = item->valuestring;
  return true;
}

std::string print_json(cJSON* root) {
  char* text = cJSON_PrintUnformatted(root);
  if (!text) return {};
  std::string result(text);
  cJSON_free(text);
  return result;
}

std::string make_hello_payload(const std::string& nonce_b,
                               const std::string& hardware_id) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "nonceB", nonce_b.c_str());
  cJSON_AddNumberToObject(root, "protocolMajor", 2);
  cJSON_AddStringToObject(root, "hardwareId", hardware_id.c_str());
  cJSON_AddStringToObject(root, "renderer", "lp-raster-1");
  cJSON_AddNullToObject(root, "activeHash");
  std::string payload = print_json(root);
  cJSON_Delete(root);
  return payload;
}

std::string handshake_payload(const std::string& nonce_b,
                              const std::string& nonce_a,
                              uint64_t master_boot,
                              uint16_t master_id,
                              uint16_t follower_id) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "nonceB", nonce_b.c_str());
  cJSON_AddStringToObject(root, "nonceA", nonce_a.c_str());
  cJSON_AddStringToObject(root, "masterBootId", boot_hex(master_boot).c_str());
  cJSON_AddNumberToObject(root, "masterNodeId", master_id);
  cJSON_AddNumberToObject(root, "followerNodeId", follower_id);
  std::string payload = print_json(root);
  cJSON_Delete(root);
  return payload;
}

bool parse_hello(const std::string& payload, std::string& nonce_b,
                 std::string& hardware_id) {
  cJSON* root = parse_payload(payload);
  if (!root) return false;
  uint32_t major = 0;
  std::string renderer;
  const cJSON* active_hash = cJSON_GetObjectItemCaseSensitive(root, "activeHash");
  const bool active_hash_valid = cJSON_IsNull(active_hash) ||
      (cJSON_IsString(active_hash) && lower_hex(active_hash->valuestring, 64));
  const bool valid = exact_keys(root, {"nonceB", "protocolMajor", "hardwareId",
                                       "renderer", "activeHash"}) &&
      json_string(root, "nonceB", nonce_b) && lower_hex(nonce_b, 32) &&
      json_uint(root, "protocolMajor", 2, major) && major == 2 &&
      json_string(root, "hardwareId", hardware_id) && valid_uuid(hardware_id) &&
      json_string(root, "renderer", renderer) && renderer == "lp-raster-1" &&
      active_hash_valid;
  cJSON_Delete(root);
  return valid;
}

bool parse_handshake(const std::string& payload, std::string& nonce_b,
                     std::string& nonce_a, std::string& master_boot_hex,
                     uint16_t& master_id, uint16_t& follower_id) {
  cJSON* root = parse_payload(payload);
  if (!root) return false;
  uint32_t master = 0, follower = 0;
  const bool valid = exact_keys(root, {"nonceB", "nonceA", "masterBootId",
                                       "masterNodeId", "followerNodeId"}) &&
      json_string(root, "nonceB", nonce_b) && lower_hex(nonce_b, 32) &&
      json_string(root, "nonceA", nonce_a) && lower_hex(nonce_a, 32) &&
      json_string(root, "masterBootId", master_boot_hex) &&
      lower_hex(master_boot_hex, 16) &&
      json_uint(root, "masterNodeId", UINT16_MAX, master) && master != 0 &&
      json_uint(root, "followerNodeId", UINT16_MAX, follower) && follower != 0;
  cJSON_Delete(root);
  if (!valid) return false;
  master_id = static_cast<uint16_t>(master);
  follower_id = static_cast<uint16_t>(follower);
  return true;
}

bool parse_signed_decimal(const cJSON* value, int64_t& output) {
  if (!cJSON_IsString(value) || !value->valuestring) return false;
  const std::string text(value->valuestring);
  if (text.empty() || text.size() > 20) return false;
  const bool negative = text[0] == '-';
  const size_t first_digit = negative ? 1 : 0;
  if (first_digit == text.size() ||
      (text.size() - first_digit > 1 && text[first_digit] == '0') ||
      (negative && text == "-0")) return false;
  const uint64_t limit = negative ? (uint64_t{1} << 63) : INT64_MAX;
  uint64_t magnitude = 0;
  for (size_t i = first_digit; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (magnitude > (limit - digit) / 10) return false;
    magnitude = magnitude * 10 + digit;
  }
  output = negative
      ? (magnitude == (uint64_t{1} << 63) ? INT64_MIN : -static_cast<int64_t>(magnitude))
      : static_cast<int64_t>(magnitude);
  return true;
}

bool parse_heartbeat(const std::string& payload, SyncPeerSnapshot& snapshot) {
  cJSON* root = parse_payload(payload);
  if (!root) return false;
  uint32_t revision = 0, topology_revision = 0;
  std::string topology_hash, now_us, health, fault_code;
  const cJSON* active_hash = cJSON_GetObjectItemCaseSensitive(root, "activeHash");
  const cJSON* candidate_hash = cJSON_GetObjectItemCaseSensitive(root, "candidateHash");
  const cJSON* fault = cJSON_GetObjectItemCaseSensitive(root, "faultCode");
  const cJSON* clock_synced = cJSON_GetObjectItemCaseSensitive(root, "clockSynced");
  const cJSON* clock_offset = cJSON_GetObjectItemCaseSensitive(root, "clockOffsetUs");
  const cJSON* clock_rtt = cJSON_GetObjectItemCaseSensitive(root, "clockRttUs");
  const cJSON* clock_age = cJSON_GetObjectItemCaseSensitive(root, "clockSampleAgeMs");
  const bool has_active_hash = cJSON_IsString(active_hash);
  if (has_active_hash) snapshot.active_hash = active_hash->valuestring;
  bool decimal_time = false;
  if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "nowUs")) &&
      json_string(root, "nowUs", now_us) && !now_us.empty() && now_us.size() <= 20) {
    decimal_time = true;
    for (const char c : now_us) decimal_time &= c >= '0' && c <= '9';
  }
  const bool valid_fault = cJSON_IsNull(fault) || cJSON_IsString(fault);
  const bool has_clock_metrics = clock_synced || clock_offset || clock_rtt || clock_age;
  const bool legacy_shape = exact_keys(root, {"revision", "topologyRevision", "topologyHash",
      "activeHash", "nowUs", "health", "faultCode"});
  const bool extended_shape = exact_keys(root, {"revision", "topologyRevision", "topologyHash",
      "activeHash", "nowUs", "health", "faultCode", "clockSynced", "clockOffsetUs",
      "clockRttUs", "clockSampleAgeMs"});
  const bool candidate_shape = exact_keys(root, {"revision", "topologyRevision", "topologyHash",
      "activeHash", "candidateHash", "nowUs", "health", "faultCode", "clockSynced",
      "clockOffsetUs", "clockRttUs", "clockSampleAgeMs"});
  const bool candidate_valid = !candidate_hash || cJSON_IsNull(candidate_hash) ||
      (cJSON_IsString(candidate_hash) && lower_hex(candidate_hash->valuestring, 64));
  int64_t parsed_clock_offset = 0;
  uint32_t parsed_clock_rtt = 0;
  int64_t parsed_clock_age = -1;
  const bool parsed_clock_synced = cJSON_IsTrue(clock_synced);
  const bool clock_metrics_valid = !has_clock_metrics ||
      ((extended_shape || candidate_shape) && cJSON_IsBool(clock_synced) &&
       parse_signed_decimal(clock_offset, parsed_clock_offset) &&
       json_uint(root, "clockRttUs", UINT32_MAX, parsed_clock_rtt) &&
       (cJSON_IsNull(clock_age) ||
        (cJSON_IsNumber(clock_age) && clock_age->valuedouble >= 0 &&
         clock_age->valuedouble <= UINT32_MAX &&
         std::floor(clock_age->valuedouble) == clock_age->valuedouble)) &&
       (!parsed_clock_synced ||
        (parsed_clock_rtt <= 20000 && cJSON_IsNumber(clock_age) &&
         clock_age->valuedouble < 5000)));
  if (cJSON_IsNumber(clock_age))
    parsed_clock_age = static_cast<int64_t>(clock_age->valuedouble);
  const bool valid = (legacy_shape || extended_shape || candidate_shape) &&
      candidate_valid && clock_metrics_valid &&
      json_uint(root, "revision", UINT32_MAX, revision) &&
      json_uint(root, "topologyRevision", UINT32_MAX, topology_revision) &&
      json_string(root, "topologyHash", topology_hash) && lower_hex(topology_hash, 64) &&
      (cJSON_IsNull(active_hash) ||
       (cJSON_IsString(active_hash) && lower_hex(active_hash->valuestring, 64))) &&
      decimal_time && json_string(root, "health", health) &&
      (health == "ready" || health == "unsynced" || health == "fault") && valid_fault;
  if (valid && cJSON_IsString(fault)) fault_code = fault->valuestring;
  const std::string parsed_candidate_hash = cJSON_IsString(candidate_hash)
      ? candidate_hash->valuestring : "";
  cJSON_Delete(root);
  if (!valid) return false;
  snapshot.revision = revision;
  snapshot.topology_revision = topology_revision;
  snapshot.health = health;
  snapshot.fault_code = fault_code;
  snapshot.topology_hash = topology_hash;
  if (!has_active_hash) snapshot.active_hash.clear();
  snapshot.candidate_hash = parsed_candidate_hash;
  snapshot.clock_synced = parsed_clock_synced;
  snapshot.clock_offset_us = parsed_clock_offset;
  snapshot.clock_rtt_us = parsed_clock_rtt;
  snapshot.clock_sample_age_ms = parsed_clock_age;
  return true;
}

bool valid_id(const std::string& value) {
  if (value.empty() || value.size() > 32 || value[0] < 'a' || value[0] > 'z')
    return false;
  for (const unsigned char c : value)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-')) return false;
  return true;
}

bool follower_session_type(uint8_t type) {
  return type == static_cast<uint8_t>(SyncMessageType::Heartbeat) ||
         type == static_cast<uint8_t>(SyncMessageType::Blackout) ||
         type == static_cast<uint8_t>(SyncMessageType::State) ||
         type == static_cast<uint8_t>(SyncMessageType::TimeResponse) ||
         type == static_cast<uint8_t>(SyncMessageType::Prepare) ||
         type == static_cast<uint8_t>(SyncMessageType::Commit) ||
         type == static_cast<uint8_t>(SyncMessageType::Abort);
}

bool decimal_u64(const cJSON* item, uint64_t& output) {
  if (!cJSON_IsString(item) || !item->valuestring) return false;
  const std::string value = item->valuestring;
  if (value.empty() || value.size() > 20 ||
      (value.size() > 1 && value[0] == '0')) return false;
  uint64_t parsed = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') return false;
    const uint8_t digit = static_cast<uint8_t>(c - '0');
    if (parsed > (UINT64_MAX - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  output = parsed;
  return true;
}

std::string time_request_payload(uint64_t t1) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return {};
  cJSON_AddStringToObject(root, "t1", std::to_string(t1).c_str());
  const std::string payload = print_json(root);
  cJSON_Delete(root);
  return payload;
}

bool parse_time_request(const std::string& payload, uint64_t& t1) {
  cJSON* root = parse_payload(payload);
  if (!root) return false;
  const bool valid = exact_keys(root, {"t1"}) &&
      decimal_u64(cJSON_GetObjectItemCaseSensitive(root, "t1"), t1) &&
      t1 <= static_cast<uint64_t>(INT64_MAX);
  cJSON_Delete(root);
  return valid;
}

std::string time_response_payload(uint64_t t1, uint64_t t2, uint64_t t3) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return {};
  cJSON_AddStringToObject(root, "t1", std::to_string(t1).c_str());
  cJSON_AddStringToObject(root, "t2", std::to_string(t2).c_str());
  cJSON_AddStringToObject(root, "t3", std::to_string(t3).c_str());
  const std::string payload = print_json(root);
  cJSON_Delete(root);
  return payload;
}

bool parse_time_response(const std::string& payload, uint64_t& t1,
                         uint64_t& t2, uint64_t& t3) {
  cJSON* root = parse_payload(payload);
  if (!root) return false;
  const bool valid = exact_keys(root, {"t1", "t2", "t3"}) &&
      decimal_u64(cJSON_GetObjectItemCaseSensitive(root, "t1"), t1) &&
      decimal_u64(cJSON_GetObjectItemCaseSensitive(root, "t2"), t2) &&
      decimal_u64(cJSON_GetObjectItemCaseSensitive(root, "t3"), t3) &&
      t1 <= static_cast<uint64_t>(INT64_MAX) &&
      t2 <= static_cast<uint64_t>(INT64_MAX) &&
      t3 <= static_cast<uint64_t>(INT64_MAX);
  cJSON_Delete(root);
  return valid;
}

bool estimate_clock_sample(uint64_t t1, uint64_t t2, uint64_t t3, uint64_t t4,
                           int64_t& offset_us, uint32_t& rtt_us) {
  if (t1 > static_cast<uint64_t>(INT64_MAX) ||
      t2 > static_cast<uint64_t>(INT64_MAX) ||
      t3 > static_cast<uint64_t>(INT64_MAX) ||
      t4 > static_cast<uint64_t>(INT64_MAX) || t4 < t1 || t3 < t2 ||
      t4 - t1 >= 500000 || t4 - t1 < t3 - t2) return false;
  const uint64_t rtt = (t4 - t1) - (t3 - t2);
  if (rtt > 20000) return false;
  const int64_t outbound = static_cast<int64_t>(t2) - static_cast<int64_t>(t1);
  const int64_t inbound = static_cast<int64_t>(t3) - static_cast<int64_t>(t4);
  offset_us = outbound / 2 + inbound / 2 + (outbound % 2 + inbound % 2) / 2;
  rtt_us = static_cast<uint32_t>(rtt);
  return true;
}

uint64_t signed_distance(int64_t high, int64_t low) {
  return static_cast<uint64_t>(high) - static_cast<uint64_t>(low);
}

void clear_clock_estimate(Peer& peer) {
  peer.clock_synced = false;
  peer.clock_offset_us = 0;
  peer.clock_rtt_us = 0;
  peer.last_clock_adjust_us = 0;
  peer.last_valid_clock_us = 0;
  peer.clock_sample_count = 0;
  peer.clock_sample_next = 0;
  peer.clock_offsets.fill(0);
  peer.clock_rtts.fill(0);
}

void record_clock_sample(Peer& peer, int64_t measured_offset, uint32_t rtt,
                         int64_t now) {
  const uint64_t difference = measured_offset >= peer.clock_offset_us
      ? signed_distance(measured_offset, peer.clock_offset_us)
      : signed_distance(peer.clock_offset_us, measured_offset);
  if (peer.clock_synced && difference > 20000) {
    clear_clock_estimate(peer);
  }
  peer.clock_offsets[peer.clock_sample_next] = measured_offset;
  peer.clock_rtts[peer.clock_sample_next] = rtt;
  peer.clock_sample_next = static_cast<uint8_t>((peer.clock_sample_next + 1) % 8);
  if (peer.clock_sample_count < 8) ++peer.clock_sample_count;
  peer.last_valid_clock_us = now;

  if (!peer.clock_synced) {
    if (peer.clock_sample_count < 3) return;
    std::array<size_t, 8> order{};
    for (size_t i = 0; i < peer.clock_sample_count; ++i) order[i] = i;
    std::sort(order.begin(), order.begin() + peer.clock_sample_count,
        [&](size_t left, size_t right) {
          return peer.clock_rtts[left] < peer.clock_rtts[right];
        });
    std::array<int64_t, 3> best_offsets{
        peer.clock_offsets[order[0]], peer.clock_offsets[order[1]],
        peer.clock_offsets[order[2]]};
    std::sort(best_offsets.begin(), best_offsets.end());
    peer.clock_offset_us = best_offsets[1];
    std::array<uint32_t, 3> best_rtts{
        peer.clock_rtts[order[0]], peer.clock_rtts[order[1]],
        peer.clock_rtts[order[2]]};
    std::sort(best_rtts.begin(), best_rtts.end());
    peer.clock_rtt_us = best_rtts[1];
    peer.clock_synced = true;
    peer.last_clock_adjust_us = now;
    return;
  }

  const int64_t elapsed = std::max<int64_t>(0, now - peer.last_clock_adjust_us);
  const int64_t max_step = elapsed / 1000;
  if (measured_offset > peer.clock_offset_us) {
    const uint64_t gap = signed_distance(measured_offset, peer.clock_offset_us);
    peer.clock_offset_us += static_cast<int64_t>(std::min<uint64_t>(gap, max_step));
  } else if (measured_offset < peer.clock_offset_us) {
    const uint64_t gap = signed_distance(peer.clock_offset_us, measured_offset);
    peer.clock_offset_us -= static_cast<int64_t>(std::min<uint64_t>(gap, max_step));
  }
  peer.clock_rtt_us = rtt;
  peer.last_clock_adjust_us = now;
}

std::string state_payload(const PlaybackState& state) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return {};
  cJSON_AddNumberToObject(root, "revision", state.revision);
  cJSON_AddNumberToObject(root, "topologyRevision", state.topologyRevision);
  const std::string topology_hash = lower_hex(state.topologyHash, 64)
      ? state.topologyHash : kZeroHash;
  cJSON_AddStringToObject(root, "topologyHash", topology_hash.c_str());
  if (lower_hex(state.packageHash, 64))
    cJSON_AddStringToObject(root, "packageHash", state.packageHash.c_str());
  else
    cJSON_AddNullToObject(root, "packageHash");
  const std::string layout = valid_id(state.layoutId) ? state.layoutId : "panel_64x32";
  cJSON_AddStringToObject(root, "layoutId", layout.c_str());
  const std::string mode = state.mode == "scene" || state.mode == "cue"
      ? state.mode : "stopped";
  cJSON_AddStringToObject(root, "mode", mode.c_str());
  if (valid_id(state.sceneId)) cJSON_AddStringToObject(root, "sceneId", state.sceneId.c_str());
  else cJSON_AddNullToObject(root, "sceneId");
  if (valid_id(state.cueId)) cJSON_AddStringToObject(root, "cueId", state.cueId.c_str());
  else cJSON_AddNullToObject(root, "cueId");
  cJSON_AddBoolToObject(root, "running", state.running && mode != "stopped");
  cJSON_AddStringToObject(root, "positionTicks",
                          std::to_string(state.positionTicks).c_str());
  cJSON_AddStringToObject(root, "anchorUs", std::to_string(state.anchorUs).c_str());
  cJSON_AddStringToObject(root, "effectiveAtUs",
                          std::to_string(state.effectiveAtUs).c_str());
  cJSON_AddBoolToObject(root, "blackout", state.blackout);
  cJSON_AddNumberToObject(root, "userBrightness", state.userBrightness);
  std::string payload = print_json(root);
  cJSON_Delete(root);
  return payload;
}

bool parse_state(const std::string& payload, PlaybackState& state) {
  cJSON* root = parse_payload(payload);
  if (!root) return false;
  PlaybackState parsed;
  uint32_t brightness = 0;
  const cJSON* package_hash = cJSON_GetObjectItemCaseSensitive(root, "packageHash");
  const cJSON* scene_id = cJSON_GetObjectItemCaseSensitive(root, "sceneId");
  const cJSON* cue_id = cJSON_GetObjectItemCaseSensitive(root, "cueId");
  std::string mode, layout_id;
  const bool scene_valid = cJSON_IsNull(scene_id) ||
      (cJSON_IsString(scene_id) && scene_id->valuestring && valid_id(scene_id->valuestring));
  const bool cue_valid = cJSON_IsNull(cue_id) ||
      (cJSON_IsString(cue_id) && cue_id->valuestring && valid_id(cue_id->valuestring));
  const bool package_valid = cJSON_IsNull(package_hash) ||
      (cJSON_IsString(package_hash) && lower_hex(package_hash->valuestring, 64));
  uint64_t effective_at = 0;
  bool valid = exact_keys(root, {"revision", "topologyRevision", "topologyHash",
                                 "packageHash", "layoutId", "mode", "sceneId",
                                 "cueId", "running", "positionTicks", "anchorUs",
                                 "effectiveAtUs", "blackout", "userBrightness"}) &&
      json_uint(root, "revision", UINT32_MAX, parsed.revision) &&
      json_uint(root, "topologyRevision", UINT32_MAX, parsed.topologyRevision) &&
      json_string(root, "topologyHash", parsed.topologyHash) &&
      lower_hex(parsed.topologyHash, 64) && package_valid &&
      json_string(root, "layoutId", layout_id) && valid_id(layout_id) &&
      json_string(root, "mode", mode) &&
      (mode == "stopped" || mode == "scene" || mode == "cue") &&
      scene_valid && cue_valid && cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "running")) &&
      decimal_u64(cJSON_GetObjectItemCaseSensitive(root, "positionTicks"), parsed.positionTicks) &&
      decimal_u64(cJSON_GetObjectItemCaseSensitive(root, "anchorUs"), parsed.anchorUs) &&
      decimal_u64(cJSON_GetObjectItemCaseSensitive(root, "effectiveAtUs"), effective_at) &&
      cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "blackout")) &&
      json_uint(root, "userBrightness", 255, brightness);
  if (valid) {
    parsed.layoutId = layout_id;
    parsed.mode = mode;
    parsed.running = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "running"));
    parsed.blackout = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(root, "blackout"));
    parsed.userBrightness = static_cast<uint8_t>(brightness);
    parsed.effectiveAtUs = effective_at;
    if (cJSON_IsString(package_hash)) parsed.packageHash = package_hash->valuestring;
    if (cJSON_IsString(scene_id)) parsed.sceneId = scene_id->valuestring;
    if (cJSON_IsString(cue_id)) parsed.cueId = cue_id->valuestring;
    valid = (mode == "stopped" && parsed.sceneId.empty() && parsed.cueId.empty() &&
             !parsed.running && parsed.positionTicks == 0) ||
            (mode == "scene" && !parsed.sceneId.empty() && parsed.cueId.empty() &&
             !parsed.packageHash.empty()) ||
            (mode == "cue" && parsed.sceneId.empty() && !parsed.cueId.empty() &&
             !parsed.packageHash.empty());
  }
  cJSON_Delete(root);
  if (!valid) return false;
  state = std::move(parsed);
  return true;
}

bool parse_transaction_state(const std::string& payload, std::string& command_id,
                             PlaybackState& next_state) {
  cJSON* root = parse_payload(payload);
  if (!root) return false;
  const cJSON* next = cJSON_GetObjectItemCaseSensitive(root, "nextState");
  std::string state_json;
  const bool shape_valid = exact_keys(root, {"commandId", "nextState"}) &&
      json_string(root, "commandId", command_id) && valid_uuid(command_id) &&
      cJSON_IsObject(next);
  if (shape_valid) {
    cJSON* duplicate = cJSON_Duplicate(next, true);
    if (duplicate) {
      state_json = print_json(duplicate);
      cJSON_Delete(duplicate);
    }
  }
  cJSON_Delete(root);
  if (!shape_valid || state_json.empty()) return false;
  return parse_state(state_json, next_state);
}

bool parse_transaction_abort(const std::string& payload, std::string& command_id,
                             uint32_t& revision) {
  cJSON* root = parse_payload(payload);
  if (!root) return false;
  const bool valid = exact_keys(root, {"commandId", "revision"}) &&
      json_string(root, "commandId", command_id) && valid_uuid(command_id) &&
      json_uint(root, "revision", UINT32_MAX, revision);
  cJSON_Delete(root);
  return valid;
}

std::string transaction_state_payload(const std::string& command_id,
                                      const PlaybackState& next_state) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return {};
  cJSON_AddStringToObject(root, "commandId", command_id.c_str());
  cJSON* state = cJSON_Parse(state_payload(next_state).c_str());
  if (!state || !cJSON_AddItemToObject(root, "nextState", state)) {
    cJSON_Delete(state);
    cJSON_Delete(root);
    return {};
  }
  const std::string payload = print_json(root);
  cJSON_Delete(root);
  return payload;
}

std::string transaction_ready_payload(const std::string& command_id,
                                      const PlaybackState& state, bool ok,
                                      const char* error) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return {};
  cJSON_AddStringToObject(root, "commandId", command_id.c_str());
  cJSON_AddNumberToObject(root, "revision", state.revision);
  cJSON_AddNumberToObject(root, "topologyRevision", state.topologyRevision);
  cJSON_AddStringToObject(root, "topologyHash", state.topologyHash.c_str());
  cJSON_AddBoolToObject(root, "ok", ok);
  if (error) cJSON_AddStringToObject(root, "error", error);
  else cJSON_AddNullToObject(root, "error");
  const std::string payload = print_json(root);
  cJSON_Delete(root);
  return payload;
}

std::string transaction_ack_payload(const std::string& command_id,
                                    const PlaybackState& state,
                                    const char* phase, bool ok,
                                    const char* error) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return {};
  cJSON_AddStringToObject(root, "commandId", command_id.c_str());
  cJSON_AddNumberToObject(root, "revision", state.revision);
  cJSON_AddNumberToObject(root, "topologyRevision", state.topologyRevision);
  cJSON_AddStringToObject(root, "topologyHash", state.topologyHash.c_str());
  cJSON_AddStringToObject(root, "phase", phase);
  cJSON_AddBoolToObject(root, "ok", ok);
  if (error) cJSON_AddStringToObject(root, "error", error);
  else cJSON_AddNullToObject(root, "error");
  const std::string payload = print_json(root);
  cJSON_Delete(root);
  return payload;
}

struct TransactionReply {
  std::string command_id;
  std::string topology_hash;
  std::string phase;
  std::string error;
  uint32_t revision = 0;
  uint32_t topology_revision = 0;
  bool ok = false;
};

bool parse_transaction_reply(const std::string& payload, bool is_ack,
                             TransactionReply& reply) {
  cJSON* root = parse_payload(payload);
  if (!root) return false;
  const cJSON* error_item = cJSON_GetObjectItemCaseSensitive(root, "error");
  const cJSON* phase_item = cJSON_GetObjectItemCaseSensitive(root, "phase");
  const bool error_valid = cJSON_IsNull(error_item) ||
      (cJSON_IsString(error_item) && error_item->valuestring &&
       std::strlen(error_item->valuestring) <= 48);
  const bool phase_valid = !is_ack ||
      (cJSON_IsString(phase_item) && phase_item->valuestring &&
       (std::strcmp(phase_item->valuestring, "committed") == 0 ||
        std::strcmp(phase_item->valuestring, "applied") == 0));
  const bool valid = exact_keys(root, is_ack
          ? std::initializer_list<const char*>({"commandId", "revision",
                "topologyRevision", "topologyHash", "phase", "ok", "error"})
          : std::initializer_list<const char*>({"commandId", "revision",
                "topologyRevision", "topologyHash", "ok", "error"})) &&
      json_string(root, "commandId", reply.command_id) &&
      valid_uuid(reply.command_id) &&
      json_uint(root, "revision", UINT32_MAX, reply.revision) &&
      json_uint(root, "topologyRevision", UINT32_MAX, reply.topology_revision) &&
      json_string(root, "topologyHash", reply.topology_hash) &&
      lower_hex(reply.topology_hash, 64) &&
      cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "ok")) &&
      error_valid && phase_valid;
  if (valid) {
    reply.ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"));
    if (cJSON_IsString(error_item)) reply.error = error_item->valuestring;
    if (is_ack) reply.phase = phase_item->valuestring;
  }
  cJSON_Delete(root);
  return valid;
}

std::string transaction_abort_payload(const std::string& command_id,
                                      uint32_t revision) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return {};
  cJSON_AddStringToObject(root, "commandId", command_id.c_str());
  cJSON_AddNumberToObject(root, "revision", revision);
  const std::string payload = print_json(root);
  cJSON_Delete(root);
  return payload;
}

bool same_state(const PlaybackState& left, const PlaybackState& right);

bool to_master_time(uint64_t local_us, int64_t offset_us, uint64_t& result) {
  if (offset_us >= 0) {
    const uint64_t offset = static_cast<uint64_t>(offset_us);
    if (local_us > UINT64_MAX - offset) return false;
    result = local_us + offset;
  } else {
    const uint64_t offset = static_cast<uint64_t>(-(offset_us + 1)) + 1;
    if (local_us < offset) return false;
    result = local_us - offset;
  }
  return true;
}

bool same_transaction_target(const std::string& command_id,
                             const PlaybackState& state,
                             const FollowerTransaction& transaction) {
  return command_id == transaction.command_id &&
      same_state(state, transaction.next_state);
}

bool same_state(const PlaybackState& left, const PlaybackState& right) {
  return left.revision == right.revision &&
      left.topologyRevision == right.topologyRevision &&
      left.topologyHash == right.topologyHash && left.packageHash == right.packageHash &&
      left.layoutId == right.layoutId && left.mode == right.mode &&
      left.sceneId == right.sceneId && left.cueId == right.cueId &&
      left.running == right.running && left.positionTicks == right.positionTicks &&
      left.anchorUs == right.anchorUs && left.effectiveAtUs == right.effectiveAtUs &&
      left.blackout == right.blackout && left.userBrightness == right.userBrightness;
}

enum class StateAdmission { Accept, Stale, TopologyMismatch, ContentMismatch, RevisionConflict };

StateAdmission admit_state(const PlaybackState& current, const PlaybackState& next) {
  if (next.revision < current.revision) return StateAdmission::Stale;
  if (next.topologyRevision != current.topologyRevision ||
      next.topologyHash != current.topologyHash) return StateAdmission::TopologyMismatch;
  if (next.packageHash != current.packageHash) return StateAdmission::ContentMismatch;
  if (next.revision == current.revision && !same_state(next, current))
    return StateAdmission::RevisionConflict;
  return StateAdmission::Accept;
}

bool parse_blackout(const std::string& payload, uint32_t& revision,
                    std::string& reason) {
  cJSON* root = parse_payload(payload);
  if (!root) return false;
  std::string now_us;
  const bool valid = exact_keys(root, {"revision", "reason", "nowUs"}) &&
      json_uint(root, "revision", UINT32_MAX, revision) &&
      json_string(root, "reason", reason) &&
      (reason == "user" || reason == "fault") &&
      json_string(root, "nowUs", now_us) && !now_us.empty() && now_us.size() <= 20 &&
      std::all_of(now_us.begin(), now_us.end(), [](char c) { return c >= '0' && c <= '9'; });
  cJSON_Delete(root);
  return valid;
}

bool same_endpoint(const sockaddr_in& left, const sockaddr_in& right) {
  return left.sin_family == right.sin_family &&
         left.sin_port == right.sin_port && left.sin_addr.s_addr == right.sin_addr.s_addr;
}

void set_port(sockaddr_in& address) {
  address.sin_port = htons(kSyncPort);
}

bool send_packet(SyncMessageType type, uint16_t sender_id, uint64_t sender_boot,
                 uint32_t sequence, const std::string& payload,
                 const std::string& key, const sockaddr_in& address) {
  SyncPacket packet;
  packet.type = type;
  packet.sender_node_id = sender_id;
  packet.sender_boot_id = sender_boot;
  packet.sequence = sequence;
  packet.payload = payload;
  std::vector<uint8_t> encoded;
  std::string error;
  if (!sync_packet_encode(packet, g_config->groupId, key, encoded, error)) return false;
  const int sent = sendto(g_socket, encoded.data(), encoded.size(), 0,
                          reinterpret_cast<const sockaddr*>(&address), sizeof(address));
  return sent == static_cast<int>(encoded.size());
}

void send_follower_ready(Peer& peer, const std::string& command_id,
                         const PlaybackState& state, bool ok,
                         const char* error) {
  const std::string payload = transaction_ready_payload(command_id, state, ok, error);
  if (!payload.empty())
    send_packet(SyncMessageType::Ready, g_config->nodeId, *g_boot_id,
                peer.tx_sequence++, payload, peer.session_key, peer.address);
}

void send_follower_ack(Peer& peer, const FollowerTransaction& transaction,
                       const char* phase, bool ok, const char* error) {
  const std::string payload = transaction_ack_payload(transaction.command_id,
      transaction.next_state, phase, ok, error);
  if (!payload.empty())
    send_packet(SyncMessageType::Ack, g_config->nodeId, *g_boot_id,
                peer.tx_sequence++, payload, peer.session_key, peer.address);
}

const char* state_admission_error(StateAdmission admission) {
  switch (admission) {
    case StateAdmission::Stale: return "STALE_REVISION";
    case StateAdmission::TopologyMismatch: return "TOPOLOGY_MISMATCH";
    case StateAdmission::ContentMismatch: return "CONTENT_MISMATCH";
    case StateAdmission::RevisionConflict: return "REVISION_CONFLICT";
    case StateAdmission::Accept: return nullptr;
  }
  return "STATE_REJECTED";
}

void reject_follower_transaction(Peer& peer, const std::string& command_id,
                                 const PlaybackState& target, const char* error,
                                 int64_t now) {
  FollowerTransaction& transaction = peer.transaction;
  transaction.phase = FollowerTransactionPhase::Rejected;
  transaction.command_id = command_id;
  transaction.next_state = target;
  transaction.completed_at_us = now;
  transaction.error = error;
  send_follower_ready(peer, command_id, target, false, error);
  peer.remote_health = "fault";
  peer.remote_fault_code = error;
}

void process_follower_prepare(Peer& peer, const SyncPacket& packet, int64_t now) {
  std::string command_id;
  PlaybackState next;
  if (!parse_transaction_state(packet.payload, command_id, next)) return;
  FollowerTransaction& transaction = peer.transaction;
  if (transaction.phase != FollowerTransactionPhase::None &&
      transaction.command_id == command_id) {
    if (!same_transaction_target(command_id, next, transaction)) {
      send_follower_ready(peer, command_id, next, false, "IDEMPOTENCY_CONFLICT");
      peer.last_seen_us = now;
      return;
    }
    const char* error = transaction.phase == FollowerTransactionPhase::Rejected
        ? transaction.error.c_str() : nullptr;
    send_follower_ready(peer, command_id, transaction.next_state,
                        transaction.phase != FollowerTransactionPhase::Rejected,
                        error);
    if (transaction.phase == FollowerTransactionPhase::Committed)
      send_follower_ack(peer, transaction, "committed", true, nullptr);
    else if (transaction.phase == FollowerTransactionPhase::Applied)
      send_follower_ack(peer, transaction, "applied", true, nullptr);
    peer.last_seen_us = now;
    return;
  }
  if (transaction.phase == FollowerTransactionPhase::Prepared ||
      transaction.phase == FollowerTransactionPhase::Committed) {
    send_follower_ready(peer, command_id, next, false, "BUSY");
    peer.last_seen_us = now;
    return;
  }
  // Completed commands must not block the next revision. The revision gate
  // below rejects stale targets; packet sequences reject replayed datagrams.

  const char* admission_error = state_admission_error(
      admit_state(g_playback_snapshot, next));
  if (admission_error) {
    reject_follower_transaction(peer, command_id, next, admission_error, now);
    return;
  }
  if (g_playback_snapshot.revision == UINT32_MAX ||
      next.revision != g_playback_snapshot.revision + 1) {
    reject_follower_transaction(peer, command_id, next, "REVISION_CONFLICT", now);
    return;
  }
  if (next.userBrightness > g_config->brightnessCap) {
    reject_follower_transaction(peer, command_id, next, "BRIGHTNESS_CAP", now);
    return;
  }
  if (!peer.clock_synced || now - peer.last_valid_clock_us >= 5000000) {
    reject_follower_transaction(peer, command_id, next, "CLOCK_UNSYNCED", now);
    return;
  }
  if (next.effectiveAtUs > static_cast<uint64_t>(INT64_MAX)) {
    reject_follower_transaction(peer, command_id, next, "INVALID_SCHEDULE", now);
    return;
  }
  const int64_t master_time = static_cast<int64_t>(next.effectiveAtUs);
  if ((peer.clock_offset_us > 0 &&
       master_time < INT64_MIN + peer.clock_offset_us) ||
      (peer.clock_offset_us < 0 &&
       master_time > INT64_MAX + peer.clock_offset_us)) {
    reject_follower_transaction(peer, command_id, next, "INVALID_SCHEDULE", now);
    return;
  }
  const int64_t local_apply_at = master_time - peer.clock_offset_us;
  if (local_apply_at < now || local_apply_at - now < 500000) {
    reject_follower_transaction(peer, command_id, next, "LATE_PREPARE", now);
    return;
  }

  transaction = FollowerTransaction{};
  transaction.phase = FollowerTransactionPhase::Prepared;
  transaction.command_id = command_id;
  transaction.next_state = std::move(next);
  transaction.apply_at_local_us = local_apply_at;
  send_follower_ready(peer, command_id, transaction.next_state, true, nullptr);
  peer.last_seen_us = now;
}

void process_follower_commit(Peer& peer, const SyncPacket& packet, int64_t now) {
  std::string command_id;
  PlaybackState next;
  if (!parse_transaction_state(packet.payload, command_id, next)) return;
  FollowerTransaction& transaction = peer.transaction;
  if (!same_transaction_target(command_id, next, transaction)) {
    FollowerTransaction unprepared;
    unprepared.command_id = command_id;
    unprepared.next_state = next;
    send_follower_ack(peer, unprepared, "committed", false, "NOT_PREPARED");
    peer.last_seen_us = now;
    return;
  }
  if (transaction.phase == FollowerTransactionPhase::Committed) {
    send_follower_ack(peer, transaction, "committed", true, nullptr);
    peer.last_seen_us = now;
    return;
  }
  if (transaction.phase == FollowerTransactionPhase::Applied) {
    send_follower_ack(peer, transaction, "applied", true, nullptr);
    peer.last_seen_us = now;
    return;
  }
  if (transaction.phase != FollowerTransactionPhase::Prepared) {
    send_follower_ack(peer, transaction, "committed", false,
                      transaction.error.empty() ? "NOT_PREPARED" :
                                                  transaction.error.c_str());
    peer.last_seen_us = now;
    return;
  }
  if (transaction.apply_at_local_us - now < 150000) {
    transaction.phase = FollowerTransactionPhase::Rejected;
    transaction.completed_at_us = now;
    transaction.error = "DEADLINE_MISSED";
    send_follower_ack(peer, transaction, "committed", false,
                      transaction.error.c_str());
    peer.remote_health = "fault";
    peer.remote_fault_code = transaction.error;
    peer.last_seen_us = now;
    return;
  }
  transaction.phase = FollowerTransactionPhase::Committed;
  send_follower_ack(peer, transaction, "committed", true, nullptr);
  peer.last_seen_us = now;
}

void process_follower_abort(Peer& peer, const SyncPacket& packet, int64_t now) {
  std::string command_id;
  uint32_t revision = 0;
  if (!parse_transaction_abort(packet.payload, command_id, revision)) return;
  FollowerTransaction& transaction = peer.transaction;
  if (transaction.command_id == command_id &&
      transaction.next_state.revision == revision &&
      (transaction.phase == FollowerTransactionPhase::Prepared ||
       transaction.phase == FollowerTransactionPhase::Committed)) {
    transaction = FollowerTransaction{};
  }
  peer.last_seen_us = now;
}

void tick_follower_transaction(Peer& peer, int64_t now) {
  FollowerTransaction& transaction = peer.transaction;
  if (transaction.phase == FollowerTransactionPhase::Prepared &&
      now >= transaction.apply_at_local_us - 150000) {
    transaction = FollowerTransaction{};
    return;
  }
  if (transaction.phase != FollowerTransactionPhase::Committed ||
      now < transaction.apply_at_local_us) return;
  if (now - transaction.apply_at_local_us > 100000) {
    transaction.phase = FollowerTransactionPhase::Rejected;
    transaction.completed_at_us = now;
    transaction.error = "LATE_COMMAND";
    send_follower_ack(peer, transaction, "applied", false,
                      transaction.error.c_str());
    peer.remote_health = "fault";
    peer.remote_fault_code = transaction.error;
    return;
  }
  g_playback_snapshot = transaction.next_state;
  web_api_apply_remote_state(transaction.next_state);
  transaction.phase = FollowerTransactionPhase::Applied;
  transaction.completed_at_us = now;
  peer.remote_revision = transaction.next_state.revision;
  peer.remote_topology_revision = transaction.next_state.topologyRevision;
  peer.remote_topology_hash = transaction.next_state.topologyHash;
  peer.remote_active_hash = transaction.next_state.packageHash;
  send_follower_ack(peer, transaction, "applied", true, nullptr);
}

bool decode_group(const uint8_t* bytes, size_t length, SyncPacket& packet) {
  std::string error;
  return sync_packet_decode(bytes, length, g_config->groupId, g_config->groupKey,
                            packet, error);
}

bool decode_session(const uint8_t* bytes, size_t length, const std::string& key,
                    SyncPacket& packet) {
  std::string error;
  return sync_packet_decode(bytes, length, g_config->groupId, key, packet, error);
}

void clear_pending(PendingSession& pending) {
  wipe(pending.candidate_key);
  pending.valid = false;
  pending.candidate_ready = false;
  pending.nonce_b.clear();
  pending.nonce_a.clear();
  pending.remote_boot_id = 0;
  pending.expires_us = 0;
  pending.last_response_us = 0;
  pending.group_sequence = 0;
  pending.address = {};
  pending.candidate_replay.reset();
}

Peer* find_peer(uint16_t node_id) {
  for (auto& peer : g_peers)
    if (peer.used && peer.node_id == node_id) return &peer;
  return nullptr;
}

Peer* allocate_peer(uint16_t node_id, const std::string& hardware_id,
                    const std::string& label, bool enabled) {
  if (Peer* existing = find_peer(node_id)) {
    if (existing->hardware_id != hardware_id) return nullptr;
    existing->label = label;
    existing->enabled = enabled;
    return existing;
  }
  for (auto& peer : g_peers) {
    if (peer.used) continue;
    peer = Peer{};
    peer.used = true;
    peer.node_id = node_id;
    peer.hardware_id = hardware_id;
    peer.label = label;
    peer.enabled = enabled;
    reset_node_http_replay(&peer);
    return &peer;
  }
  return nullptr;
}

bool valid_source_port(const sockaddr_in& source) {
  return source.sin_family == AF_INET && ntohs(source.sin_port) == kSyncPort;
}

void send_offer(Peer& peer, int64_t now) {
  const std::string payload = handshake_payload(peer.pending.nonce_b,
      peer.pending.nonce_a, *g_boot_id, g_config->nodeId, peer.node_id);
  send_packet(SyncMessageType::Offer, g_config->nodeId, *g_boot_id,
              peer.pending.group_sequence++, payload, g_config->groupKey,
              peer.pending.address);
  peer.pending.last_response_us = now;
}

void send_accept(Peer& peer, int64_t now) {
  const std::string payload = handshake_payload(peer.pending.nonce_b,
      peer.pending.nonce_a, peer.pending.remote_boot_id, g_config->masterNodeId,
      g_config->nodeId);
  send_packet(SyncMessageType::Accept, g_config->nodeId, *g_boot_id,
              peer.pending.group_sequence++, payload, g_config->groupKey,
              peer.pending.address);
  peer.pending.last_response_us = now;
}

void send_session_ok(Peer& peer, int64_t now) {
  const std::string payload = handshake_payload(peer.pending.nonce_b,
      peer.pending.nonce_a, *g_boot_id, g_config->nodeId, peer.node_id);
  send_packet(SyncMessageType::SessionOk, g_config->nodeId, *g_boot_id,
              peer.pending.group_sequence++, payload, g_config->groupKey,
              peer.pending.address);
  peer.pending.last_response_us = now;
}

void maybe_start_master_challenge(const SyncPacket& packet,
                                  const sockaddr_in& source, int64_t now) {
  std::string nonce_b, hardware_id;
  if (packet.type != SyncMessageType::Hello || !valid_source_port(source) ||
      packet.sender_node_id == g_config->nodeId ||
      !parse_hello(packet.payload, nonce_b, hardware_id)) return;
  bool enabled = false;
  std::string label, registry_error;
  if (!rig_lookup_follower(*g_config, packet.sender_node_id, hardware_id,
                           enabled, label, registry_error)) return;
  Peer* peer = allocate_peer(packet.sender_node_id, hardware_id, label, enabled);
  if (!peer) return;
  PendingSession& pending = peer->pending;
  if (pending.valid && now < pending.expires_us) {
    if (pending.nonce_b == nonce_b && pending.remote_boot_id == packet.sender_boot_id &&
        same_endpoint(pending.address, source) &&
        now - pending.last_response_us >= kHandshakeRetryUs)
      send_offer(*peer, now);
    return;
  }
  clear_pending(pending);
  pending.valid = true;
  pending.nonce_b = nonce_b;
  pending.nonce_a = random_hex(16);
  pending.remote_boot_id = packet.sender_boot_id;
  pending.expires_us = now + kChallengeLifetimeUs;
  pending.address = source;
  send_offer(*peer, now);
}

bool matches_handshake(const SyncPacket& packet, const Peer& peer,
                       const sockaddr_in& source, bool incoming_accept,
                       std::string& nonce_b, std::string& nonce_a) {
  std::string master_boot_hex;
  uint16_t master_id = 0, follower_id = 0;
  if (!parse_handshake(packet.payload, nonce_b, nonce_a, master_boot_hex,
                       master_id, follower_id)) return false;
  const uint16_t expected_master = g_config->nodeId;
  const uint16_t expected_follower = peer.node_id;
  const uint64_t expected_master_boot = *g_boot_id;
  return incoming_accept && packet.type == SyncMessageType::Accept &&
      packet.sender_node_id == expected_follower &&
      packet.sender_boot_id == peer.pending.remote_boot_id &&
      same_endpoint(source, peer.pending.address) &&
      nonce_b == peer.pending.nonce_b && nonce_a == peer.pending.nonce_a &&
      master_boot_hex == boot_hex(expected_master_boot) &&
      master_id == expected_master && follower_id == expected_follower;
}

bool matches_follower_handshake(const SyncPacket& packet, const Peer& peer,
                                SyncMessageType expected_type, std::string& nonce_b,
                                std::string& nonce_a) {
  std::string master_boot_hex;
  uint16_t master_id = 0, follower_id = 0;
  if (!parse_handshake(packet.payload, nonce_b, nonce_a, master_boot_hex,
                       master_id, follower_id)) return false;
  const uint16_t expected_master = g_config->masterNodeId;
  const uint16_t expected_follower = g_config->nodeId;
  return packet.type == expected_type &&
      packet.sender_node_id == expected_master &&
      packet.sender_boot_id != 0 &&
      packet.sender_boot_id == std::strtoull(master_boot_hex.c_str(), nullptr, 16) &&
      nonce_b == peer.pending.nonce_b &&
      master_id == expected_master && follower_id == expected_follower;
}

void process_heartbeat(Peer& peer, const SyncPacket& packet, int64_t now,
                       bool new_session) {
  SyncPeerSnapshot snapshot;
  if (!parse_heartbeat(packet.payload, snapshot)) return;
  if (new_session) {
    if (!peer.pending.candidate_replay.accept(packet.sequence)) return;
    peer.replay = peer.pending.candidate_replay;
    wipe(peer.session_key);
    peer.session_key = std::move(peer.pending.candidate_key);
    reset_node_http_replay(&peer);
    peer.remote_boot_id = peer.pending.remote_boot_id;
    peer.address = peer.pending.address;
    peer.active = true;
    peer.tx_sequence = 0;
    clear_pending(peer.pending);
  } else if (!peer.replay.accept(packet.sequence)) {
    return;
  }
  peer.last_seen_us = now;
  peer.remote_revision = snapshot.revision;
  peer.remote_topology_revision = snapshot.topology_revision;
  peer.remote_topology_hash = snapshot.topology_hash;
  peer.remote_active_hash = snapshot.active_hash;
  peer.remote_candidate_hash = snapshot.candidate_hash;
  peer.remote_health = snapshot.health;
  peer.remote_fault_code = snapshot.fault_code.empty() ? "" : snapshot.fault_code;
  peer.remote_clock_synced = snapshot.clock_synced;
  peer.remote_clock_offset_us = snapshot.clock_offset_us;
  peer.remote_clock_rtt_us = snapshot.clock_rtt_us;
  peer.remote_clock_sample_age_ms = snapshot.clock_sample_age_ms;
  peer.remote_clock_status_us = now;
}

void process_master_accept(const SyncPacket& packet,
                           const sockaddr_in& source, int64_t now) {
  Peer* peer = find_peer(packet.sender_node_id);
  if (!peer || !peer->pending.valid || now >= peer->pending.expires_us) return;
  std::string nonce_b, nonce_a;
  if (!matches_handshake(packet, *peer, source, true, nonce_b, nonce_a)) return;
  if (!peer->pending.candidate_ready) {
    std::string error;
    if (!sync_derive_session_key(g_config->groupKey, nonce_b, nonce_a,
                                 *g_boot_id, peer->pending.remote_boot_id,
                                 peer->pending.candidate_key, error)) return;
    peer->pending.candidate_ready = true;
    peer->pending.candidate_replay.reset();
  }
  if (now - peer->pending.last_response_us >= 100000 ||
      peer->pending.last_response_us == 0) send_session_ok(*peer, now);
}

uint32_t packet_sender_id(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[24]) |
         (static_cast<uint32_t>(bytes[25]) << 8) |
         (static_cast<uint32_t>(bytes[26]) << 16) |
         (static_cast<uint32_t>(bytes[27]) << 24);
}

uint64_t packet_sender_boot(const uint8_t* bytes) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= static_cast<uint64_t>(bytes[28 + i]) << (8 * i);
  return value;
}

void process_master_transaction_reply(Peer& peer, const SyncPacket& packet,
                                      int64_t now) {
  const bool is_ack = packet.type == SyncMessageType::Ack;
  TransactionReply reply;
  if (!parse_transaction_reply(packet.payload, is_ack, reply) ||
      !peer.replay.accept(packet.sequence)) return;
  peer.last_seen_us = now;
  if (!peer.transaction_required ||
      g_master_transaction.phase == MasterTransactionPhase::Idle ||
      g_master_transaction.phase == MasterTransactionPhase::Finished ||
      reply.command_id != g_master_transaction.command_id ||
      reply.revision != g_master_transaction.next_state.revision ||
      reply.topology_revision != g_master_transaction.next_state.topologyRevision ||
      reply.topology_hash != g_master_transaction.next_state.topologyHash) return;
  if (!reply.ok) {
    peer.transaction_phase = MasterPeerTransactionPhase::Rejected;
    peer.transaction_error = reply.error.empty() ? "FOLLOWER_REJECTED" : reply.error;
    return;
  }
  if (!is_ack) {
    if (g_master_transaction.phase == MasterTransactionPhase::Preparing &&
        peer.transaction_phase == MasterPeerTransactionPhase::WaitingReady)
      peer.transaction_phase = MasterPeerTransactionPhase::Ready;
    return;
  }
  if (reply.phase == "committed") {
    if (peer.transaction_phase != MasterPeerTransactionPhase::Applied)
      peer.transaction_phase = MasterPeerTransactionPhase::Committed;
  } else {
    peer.transaction_phase = MasterPeerTransactionPhase::Applied;
    // An authenticated applied ACK confirms the new state immediately. Do
    // not make the next command wait for an unrelated periodic heartbeat.
    const PlaybackState& applied = g_master_transaction.next_state;
    peer.remote_revision = applied.revision;
    peer.remote_topology_revision = applied.topologyRevision;
    peer.remote_topology_hash = applied.topologyHash;
    peer.remote_active_hash = applied.packageHash;
  }
}

void process_master_session_packet(const uint8_t* bytes, size_t length,
                                   const sockaddr_in& source, int64_t now) {
  if (!bytes || length < 40 ||
      (bytes[5] != static_cast<uint8_t>(SyncMessageType::Heartbeat) &&
       bytes[5] != static_cast<uint8_t>(SyncMessageType::TimeRequest) &&
       bytes[5] != static_cast<uint8_t>(SyncMessageType::Ready) &&
       bytes[5] != static_cast<uint8_t>(SyncMessageType::Ack)) ||
      !valid_source_port(source)) return;
  const uint32_t sender_id = packet_sender_id(bytes);
  if (sender_id == 0 || sender_id > UINT16_MAX) return;
  Peer* peer = find_peer(static_cast<uint16_t>(sender_id));
  if (!peer) return;
  const uint64_t sender_boot = packet_sender_boot(bytes);
  SyncPacket decoded;
  if (peer->pending.valid && peer->pending.candidate_ready &&
      sender_boot == peer->pending.remote_boot_id &&
      same_endpoint(source, peer->pending.address) &&
      decode_session(bytes, length, peer->pending.candidate_key, decoded) &&
      decoded.type == SyncMessageType::Heartbeat) {
    process_heartbeat(*peer, decoded, now, true);
    return;
  }
  if (!peer->active || sender_boot != peer->remote_boot_id ||
      !same_endpoint(source, peer->address) ||
      !decode_session(bytes, length, peer->session_key, decoded)) return;
  if (decoded.type == SyncMessageType::Ready ||
      decoded.type == SyncMessageType::Ack) {
    process_master_transaction_reply(*peer, decoded, now);
    return;
  }
  if (decoded.type == SyncMessageType::TimeRequest) {
    uint64_t t1 = 0;
    if (!parse_time_request(decoded.payload, t1) ||
        !peer->replay.accept(decoded.sequence)) return;
    peer->last_seen_us = now;
    const uint64_t t2 = static_cast<uint64_t>(std::max<int64_t>(0, now));
    const uint64_t t3 = static_cast<uint64_t>(std::max<int64_t>(0, esp_timer_get_time()));
    const std::string payload = time_response_payload(t1, t2, t3);
    if (!payload.empty())
      send_packet(SyncMessageType::TimeResponse, g_config->nodeId, *g_boot_id,
                  peer->tx_sequence++, payload, peer->session_key, peer->address);
    return;
  }
  if (decoded.type == SyncMessageType::Heartbeat)
    process_heartbeat(*peer, decoded, now, false);
}

void process_follower_session_packet(const uint8_t* bytes, size_t length,
                                    const sockaddr_in& source, int64_t now,
                                    Peer& peer) {
  if (!bytes || length < 40 ||
      !follower_session_type(bytes[5]) ||
      packet_sender_id(bytes) != g_config->masterNodeId ||
      packet_sender_boot(bytes) != peer.remote_boot_id ||
      !same_endpoint(source, g_master_address) || !peer.active) return;
  SyncPacket decoded;
  if (!decode_session(bytes, length, peer.session_key, decoded) ||
      !follower_session_type(static_cast<uint8_t>(decoded.type))) return;
  if (decoded.type == SyncMessageType::TimeResponse) {
    uint64_t t1 = 0, t2 = 0, t3 = 0;
    if (!peer.pending_time ||
        !parse_time_response(decoded.payload, t1, t2, t3) ||
        t1 != peer.pending_time_t1 || !peer.replay.accept(decoded.sequence)) return;
    peer.pending_time = false;
    peer.last_seen_us = now;
    int64_t offset = 0;
    uint32_t rtt = 0;
    if (!estimate_clock_sample(t1, t2, t3,
                               static_cast<uint64_t>(std::max<int64_t>(0, now)),
                               offset, rtt)) return;
    record_clock_sample(peer, offset, rtt, now);
    return;
  }
  if (decoded.type == SyncMessageType::Prepare ||
      decoded.type == SyncMessageType::Commit ||
      decoded.type == SyncMessageType::Abort) {
    if (!peer.replay.accept(decoded.sequence)) return;
    if (decoded.type == SyncMessageType::Prepare)
      process_follower_prepare(peer, decoded, now);
    else if (decoded.type == SyncMessageType::Commit)
      process_follower_commit(peer, decoded, now);
    else
      process_follower_abort(peer, decoded, now);
    return;
  }
  if (decoded.type == SyncMessageType::Blackout) {
    uint32_t revision = 0;
    std::string reason;
    if (!parse_blackout(decoded.payload, revision, reason) ||
        !peer.replay.accept(decoded.sequence)) return;
    // Retries of an older blackout can arrive after PREPARE or application
    // of a newer command. They must not cancel that command.
    if (revision < g_playback_snapshot.revision ||
        ((peer.transaction.phase == FollowerTransactionPhase::Prepared ||
          peer.transaction.phase == FollowerTransactionPhase::Committed) &&
         revision < peer.transaction.next_state.revision)) {
      peer.last_seen_us = now;
      return;
    }
    web_api_apply_remote_blackout(revision);
    peer.transaction = FollowerTransaction{};
    g_playback_snapshot.revision = std::max(g_playback_snapshot.revision, revision);
    g_playback_snapshot.blackout = true;
    peer.last_seen_us = now;
    peer.remote_revision = std::max(peer.remote_revision, revision);
    peer.remote_health = "fault";
    peer.remote_fault_code = reason == "fault" ? "REMOTE_FAULT" : "USER_BLACKOUT";
    return;
  }
  if (decoded.type == SyncMessageType::State) {
    PlaybackState next;
    if (!parse_state(decoded.payload, next) || !peer.replay.accept(decoded.sequence)) return;
    peer.last_seen_us = now;
    const StateAdmission admission = admit_state(g_playback_snapshot, next);
    if (admission == StateAdmission::Stale) return;
    peer.remote_revision = next.revision;
    peer.remote_topology_revision = next.topologyRevision;
    peer.remote_topology_hash = next.topologyHash;
    peer.remote_active_hash = next.packageHash;
    if (admission != StateAdmission::Accept) {
      peer.remote_health = "fault";
      switch (admission) {
        case StateAdmission::TopologyMismatch: peer.remote_fault_code = "TOPOLOGY_MISMATCH"; break;
        case StateAdmission::ContentMismatch: peer.remote_fault_code = "CONTENT_MISMATCH"; break;
        case StateAdmission::RevisionConflict: peer.remote_fault_code = "REVISION_CONFLICT"; break;
        default: peer.remote_fault_code = "STATE_REJECTED"; break;
      }
      return;
    }
    g_playback_snapshot = std::move(next);
    web_api_apply_remote_state(g_playback_snapshot);
    return;
  }
  SyncPeerSnapshot snapshot;
  if (!parse_heartbeat(decoded.payload, snapshot) ||
      !peer.replay.accept(decoded.sequence)) return;
  peer.last_seen_us = now;
  peer.remote_revision = snapshot.revision;
  peer.remote_topology_revision = snapshot.topology_revision;
  peer.remote_topology_hash = snapshot.topology_hash;
  peer.remote_active_hash = snapshot.active_hash;
  peer.remote_candidate_hash = snapshot.candidate_hash;
  peer.remote_health = snapshot.health;
  peer.remote_fault_code = snapshot.fault_code;
  peer.remote_clock_synced = snapshot.clock_synced;
  peer.remote_clock_offset_us = snapshot.clock_offset_us;
  peer.remote_clock_rtt_us = snapshot.clock_rtt_us;
  peer.remote_clock_sample_age_ms = snapshot.clock_sample_age_ms;
  peer.remote_clock_status_us = now;
}

void process_follower_offer(const SyncPacket& packet,
                            const sockaddr_in& source, int64_t now, Peer& peer) {
  if (!peer.pending.valid || now >= peer.pending.expires_us ||
      !same_endpoint(source, g_master_address)) return;
  std::string nonce_b, nonce_a;
  if (!matches_follower_handshake(packet, peer, SyncMessageType::Offer,
                                  nonce_b, nonce_a)) return;
  if (peer.pending.nonce_a == nonce_a && peer.pending.candidate_ready) {
    if (now - peer.pending.last_response_us >= kHandshakeRetryUs) send_accept(peer, now);
    return;
  }
  wipe(peer.pending.candidate_key);
  std::string error;
  if (!sync_derive_session_key(g_config->groupKey, nonce_b, nonce_a,
                               packet.sender_boot_id, *g_boot_id,
                               peer.pending.candidate_key, error)) return;
  peer.pending.nonce_a = nonce_a;
  peer.pending.remote_boot_id = packet.sender_boot_id;
  peer.pending.address = source;
  peer.pending.candidate_ready = true;
  peer.pending.candidate_replay.reset();
  send_accept(peer, now);
}

void process_follower_session_ok(const SyncPacket& packet,
                                 const sockaddr_in& source, int64_t now,
                                 Peer& peer) {
  if (!peer.pending.valid || !peer.pending.candidate_ready ||
      now >= peer.pending.expires_us || !same_endpoint(source, g_master_address)) return;
  std::string nonce_b, nonce_a;
  if (!matches_follower_handshake(packet, peer, SyncMessageType::SessionOk,
                                  nonce_b, nonce_a) || nonce_a != peer.pending.nonce_a ||
      packet.sender_boot_id != peer.pending.remote_boot_id ||
      !same_endpoint(source, peer.pending.address)) return;
  wipe(peer.session_key);
  peer.session_key = std::move(peer.pending.candidate_key);
  reset_node_http_replay(&peer);
  peer.remote_boot_id = packet.sender_boot_id;
  peer.address = source;
  peer.active = true;
  peer.tx_sequence = 0;
  peer.replay.reset();
  clear_clock_estimate(peer);
  peer.pending_time = false;
  peer.last_time_request_us = 0;
  peer.last_seen_us = now;
  peer.pending.valid = false;
  peer.pending.candidate_ready = false;
  peer.pending.nonce_b.clear();
  peer.pending.nonce_a.clear();
  peer.pending.expires_us = 0;
  peer.pending.last_response_us = 0;
}

std::string heartbeat_payload(int64_t now, const Peer* follower_clock = nullptr) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return {};
  cJSON_AddNumberToObject(root, "revision", g_playback_snapshot.revision);
  cJSON_AddNumberToObject(root, "topologyRevision", g_playback_snapshot.topologyRevision);
  const std::string topology_hash = lower_hex(g_playback_snapshot.topologyHash, 64)
      ? g_playback_snapshot.topologyHash : kZeroHash;
  cJSON_AddStringToObject(root, "topologyHash", topology_hash.c_str());
  static ContentState heartbeat_content;
  static int64_t refresh_content_at_us = 0;
  const int64_t actual_now = esp_timer_get_time();
  if (actual_now >= refresh_content_at_us) {
    ContentState refreshed;
    std::string content_error;
    heartbeat_content = content_get_state(refreshed, content_error)
        ? std::move(refreshed) : ContentState{};
    refresh_content_at_us = actual_now + 250000;
  }
  if (heartbeat_content.has_active)
    cJSON_AddStringToObject(root, "activeHash", heartbeat_content.active_hash.c_str());
  else
    cJSON_AddNullToObject(root, "activeHash");
  if (heartbeat_content.has_candidate)
    cJSON_AddStringToObject(root, "candidateHash", heartbeat_content.candidate_hash.c_str());
  else
    cJSON_AddNullToObject(root, "candidateHash");
  const std::string time = std::to_string(now);
  cJSON_AddStringToObject(root, "nowUs", time.c_str());

  const bool clock_synced = follower_clock
      ? follower_clock->clock_synced && now >= follower_clock->last_valid_clock_us &&
            now - follower_clock->last_valid_clock_us < 5000000
      : true;
  ContentState local_content;
  std::string local_content_error;
  const bool local_content_ok = content_get_state(local_content, local_content_error);
  const char* local_fault = nullptr;
  if (!hub75_diagnostic_supported()) {
    local_fault = "PANEL_DMA_UNAVAILABLE";
  } else if (!local_content_ok) {
    local_fault = "CONTENT_METADATA_INVALID";
  } else if (g_playback_snapshot.running &&
             (!local_content.has_active ||
              local_content.active_hash != g_playback_snapshot.packageHash)) {
    local_fault = "ACTIVE_CONTENT_MISMATCH";
  } else if (g_playback_snapshot.running &&
             !render_runtime_is_loaded(g_playback_snapshot.packageHash)) {
    local_fault = "PACKAGE_NOT_READY";
  }
  cJSON_AddStringToObject(root, "health", local_fault ? "fault" :
      clock_synced ? "ready" : "unsynced");
  if (local_fault) cJSON_AddStringToObject(root, "faultCode", local_fault);
  else if (!clock_synced) cJSON_AddStringToObject(root, "faultCode", "CLOCK_UNSYNCED");
  else cJSON_AddNullToObject(root, "faultCode");
  cJSON_AddBoolToObject(root, "clockSynced", clock_synced);
  cJSON_AddStringToObject(root, "clockOffsetUs",
      std::to_string(follower_clock ? follower_clock->clock_offset_us : 0).c_str());
  cJSON_AddNumberToObject(root, "clockRttUs",
      follower_clock ? follower_clock->clock_rtt_us : 0);
  if (follower_clock && follower_clock->last_valid_clock_us > 0 &&
      now >= follower_clock->last_valid_clock_us)
    cJSON_AddNumberToObject(root, "clockSampleAgeMs",
        (now - follower_clock->last_valid_clock_us) / 1000);
  else
    cJSON_AddNullToObject(root, "clockSampleAgeMs");
  std::string payload = print_json(root);
  cJSON_Delete(root);
  return payload;
}

std::string blackout_payload(uint32_t revision, const std::string& reason,
                             int64_t now) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return {};
  cJSON_AddNumberToObject(root, "revision", revision);
  cJSON_AddStringToObject(root, "reason", reason.c_str());
  const std::string time = std::to_string(std::max<int64_t>(0, now));
  cJSON_AddStringToObject(root, "nowUs", time.c_str());
  const std::string payload = print_json(root);
  cJSON_Delete(root);
  return payload;
}

bool blackout_payload_self_test(std::string& error) {
  for (const char* expected_reason : {"user", "fault"}) {
    const std::string payload = blackout_payload(0x12345678U, expected_reason, 987654321);
    uint32_t revision = 0;
    std::string reason;
    if (!parse_blackout(payload, revision, reason) || revision != 0x12345678U ||
        reason != expected_reason) {
      error = "blackout payload roundtrip mismatch";
      return false;
    }
  }
  for (const char* invalid : {
           "{\"revision\":1,\"reason\":\"user\",\"nowUs\":\"1\",\"extra\":0}",
           "{\"revision\":1,\"reason\":\"unknown\",\"nowUs\":\"1\"}",
           "{\"revision\":1.5,\"reason\":\"user\",\"nowUs\":\"1\"}",
           "{\"revision\":1,\"reason\":\"fault\",\"nowUs\":\"1x\"}"}) {
    uint32_t revision = 0;
    std::string reason;
    if (parse_blackout(invalid, revision, reason)) {
      error = "invalid blackout payload was accepted";
      return false;
    }
  }
  error.clear();
  return true;
}

void send_blackout_broadcast(int64_t now) {
  if (!g_blackout_broadcast.pending || now < g_blackout_broadcast.next_send_us)
    return;
  const std::string payload = blackout_payload(g_blackout_broadcast.revision,
                                                g_blackout_broadcast.reason, now);
  for (auto& peer : g_peers) {
    if (!peer.used || !peer.active) continue;
    send_packet(SyncMessageType::Blackout, g_config->nodeId, *g_boot_id,
                peer.tx_sequence++, payload, peer.session_key, peer.address);
  }
  ++g_blackout_broadcast.attempts;
  g_blackout_broadcast.next_send_us = now + 50000;
  if (g_blackout_broadcast.attempts >= 10) {
    g_blackout_broadcast.pending = false;
    wipe(g_blackout_broadcast.reason);
  }
}

bool all_master_peers_at_least(MasterPeerTransactionPhase phase) {
  bool any = false;
  for (const auto& peer : g_peers) {
    if (!peer.transaction_required) continue;
    any = true;
    if (static_cast<uint8_t>(peer.transaction_phase) < static_cast<uint8_t>(phase))
      return false;
  }
  return any;
}

bool master_transaction_has_rejection() {
  for (const auto& peer : g_peers)
    if (peer.transaction_required &&
        peer.transaction_phase == MasterPeerTransactionPhase::Rejected) return true;
  return false;
}

void finish_master_transaction(bool success, const char* error, int64_t now) {
  MasterTransaction& operation = g_master_transaction;
  if (operation.phase == MasterTransactionPhase::Idle ||
      operation.phase == MasterTransactionPhase::Finished) return;
  if (!success) {
    const bool superseded_by_blackout = g_blackout_broadcast.pending &&
        g_blackout_broadcast.revision >= operation.next_state.revision;
    const std::string abort = transaction_abort_payload(operation.command_id,
                                                        operation.next_state.revision);
    for (auto& peer : g_peers) {
      if (!peer.transaction_required || !peer.active) continue;
      if (!abort.empty())
        send_packet(SyncMessageType::Abort, g_config->nodeId, *g_boot_id,
                    peer.tx_sequence++, abort, peer.session_key, peer.address);
      peer.remote_health = "fault";
      peer.remote_fault_code = error ? error : "SYNC_FAILED";
      peer.transaction_required = false;
    }
    if (superseded_by_blackout) {
      g_playback_snapshot.blackout = true;
      g_playback_snapshot.revision = std::max(g_playback_snapshot.revision,
                                               g_blackout_broadcast.revision);
      web_api_apply_remote_blackout(g_blackout_broadcast.revision);
    } else {
      operation.next_state.blackout = true;
      g_playback_snapshot = operation.next_state;
      web_api_apply_remote_state(g_playback_snapshot);
      web_api_apply_remote_blackout(operation.next_state.revision);
      g_blackout_broadcast.pending = true;
      g_blackout_broadcast.revision = operation.next_state.revision;
      g_blackout_broadcast.reason = "fault";
      g_blackout_broadcast.next_send_us = 0;
      g_blackout_broadcast.attempts = 0;
    }
    operation.error = error ? error : "SYNC_FAILED";
  } else {
    for (auto& peer : g_peers) {
      if (peer.transaction_required) {
        peer.transaction_required = false;
        peer.transaction_phase = MasterPeerTransactionPhase::None;
        peer.transaction_error.clear();
        peer.transaction_last_send_us = 0;
      }
    }
    operation.error.clear();
  }
  operation.success = success;
  operation.phase = MasterTransactionPhase::Finished;
  operation.last_send_us = now;
}

void tick_master_transaction(int64_t now) {
  MasterTransaction& operation = g_master_transaction;
  if (operation.phase == MasterTransactionPhase::Idle ||
      operation.phase == MasterTransactionPhase::Finished) return;
  if (g_blackout_broadcast.pending &&
      g_blackout_broadcast.revision >= operation.next_state.revision) {
    finish_master_transaction(false, "CANCELED_BY_BLACKOUT", now);
    return;
  }
  if (master_transaction_has_rejection()) {
    const char* reason = "FOLLOWER_REJECTED";
    for (const auto& peer : g_peers) {
      if (peer.transaction_required &&
          peer.transaction_phase == MasterPeerTransactionPhase::Rejected &&
          !peer.transaction_error.empty()) {
        reason = peer.transaction_error.c_str();
        break;
      }
    }
    finish_master_transaction(false, reason, now);
    return;
  }
  for (const auto& peer : g_peers) {
    if (!peer.transaction_required) continue;
    // The follower owns its offset estimate: the Master is the clock authority
    // and therefore has no local `clock_synced` sample for a remote clock.
    // PREPARE is the clock gate; a follower only sends READY after validating
    // its own fresh estimate and the scheduled apply time.
    if (!peer.active || peer.last_seen_us <= 0 || now < peer.last_seen_us ||
        now - peer.last_seen_us > kPeerTimeoutUs) {
      finish_master_transaction(false, "PEER_LOST", now);
      return;
    }
  }
  if (operation.next_state.effectiveAtUs > static_cast<uint64_t>(INT64_MAX)) {
    finish_master_transaction(false, "INVALID_SCHEDULE", now);
    return;
  }
  const int64_t apply_at = static_cast<int64_t>(operation.next_state.effectiveAtUs);
  if (operation.phase == MasterTransactionPhase::Preparing) {
    for (auto& peer : g_peers) {
      if (!peer.transaction_required ||
          peer.transaction_phase != MasterPeerTransactionPhase::WaitingReady ||
          (peer.transaction_last_send_us != 0 &&
           now - peer.transaction_last_send_us < 50000)) continue;
      const std::string payload = transaction_state_payload(operation.command_id,
                                                              operation.next_state);
      if (payload.empty() ||
          !send_packet(SyncMessageType::Prepare, g_config->nodeId, *g_boot_id,
                       peer.tx_sequence++, payload, peer.session_key, peer.address)) {
        finish_master_transaction(false, "SEND_FAILED", now);
        return;
      }
      peer.transaction_last_send_us = now;
    }
    if (all_master_peers_at_least(MasterPeerTransactionPhase::Ready))
      operation.phase = MasterTransactionPhase::Committing;
    else if (now - operation.started_us >= 100000 || apply_at - now <= 150000) {
      finish_master_transaction(false, "READY_TIMEOUT", now);
      return;
    }
  }
  if (operation.phase == MasterTransactionPhase::Committing) {
    for (auto& peer : g_peers) {
      if (!peer.transaction_required ||
          (peer.transaction_phase != MasterPeerTransactionPhase::Ready &&
           peer.transaction_phase != MasterPeerTransactionPhase::Committed) ||
          (peer.transaction_last_send_us != 0 &&
           now - peer.transaction_last_send_us < 50000)) continue;
      const std::string payload = transaction_state_payload(operation.command_id,
                                                              operation.next_state);
      if (payload.empty() ||
          !send_packet(SyncMessageType::Commit, g_config->nodeId, *g_boot_id,
                       peer.tx_sequence++, payload, peer.session_key, peer.address)) {
        finish_master_transaction(false, "SEND_FAILED", now);
        return;
      }
      peer.transaction_last_send_us = now;
    }
    if (all_master_peers_at_least(MasterPeerTransactionPhase::Committed)) {
      if (now >= apply_at) {
        g_playback_snapshot = operation.next_state;
        web_api_apply_remote_state(g_playback_snapshot);
        operation.phase = MasterTransactionPhase::WaitingApplied;
      }
    } else if (apply_at - now <= 150000) {
      finish_master_transaction(false, "COMMIT_TIMEOUT", now);
      return;
    }
  }
  if (operation.phase == MasterTransactionPhase::WaitingApplied) {
    if (all_master_peers_at_least(MasterPeerTransactionPhase::Applied)) {
      finish_master_transaction(true, nullptr, now);
      return;
    }
    if (now - apply_at > 750000) {
      finish_master_transaction(false, "APPLY_TIMEOUT", now);
      return;
    }
    for (auto& peer : g_peers) {
      if (!peer.transaction_required ||
          peer.transaction_phase == MasterPeerTransactionPhase::Applied ||
          (peer.transaction_last_send_us != 0 &&
           now - peer.transaction_last_send_us < 50000)) continue;
      const std::string payload = transaction_state_payload(operation.command_id,
                                                              operation.next_state);
      if (payload.empty() ||
          !send_packet(SyncMessageType::Commit, g_config->nodeId, *g_boot_id,
                       peer.tx_sequence++, payload, peer.session_key, peer.address)) {
        finish_master_transaction(false, "SEND_FAILED", now);
        return;
      }
      peer.transaction_last_send_us = now;
    }
  }
  if (now - operation.started_us > 5000000)
    finish_master_transaction(false, "OPERATION_TIMEOUT", now);
}

void process_datagram(const uint8_t* bytes, size_t length,
                      const sockaddr_in& source, int64_t now) {
  SyncPacket packet;
  const bool group_authenticated = decode_group(bytes, length, packet);
  if (g_config->role == Role::Master) {
    if (!group_authenticated) {
      process_master_session_packet(bytes, length, source, now);
      return;
    }
    if (packet.type == SyncMessageType::Hello) {
      maybe_start_master_challenge(packet, source, now);
    } else if (packet.type == SyncMessageType::Accept) {
      process_master_accept(packet, source, now);
    }
    return;
  }
  Peer& peer = g_peers[0];
  if (!group_authenticated) {
    process_follower_session_packet(bytes, length, source, now, peer);
    return;
  }
  if (!peer.used || packet.sender_node_id != g_config->masterNodeId ||
      !same_endpoint(source, g_master_address)) return;
  if (packet.type == SyncMessageType::Offer) {
    process_follower_offer(packet, source, now, peer);
  } else if (packet.type == SyncMessageType::SessionOk) {
    process_follower_session_ok(packet, source, now, peer);
  } else if (packet.type == SyncMessageType::Heartbeat) {
    SyncPacket decoded;
    if (!peer.active || packet.sender_boot_id != peer.remote_boot_id ||
        !decode_session(bytes, length, peer.session_key, decoded) ||
        decoded.type != SyncMessageType::Heartbeat) return;
    SyncPeerSnapshot snapshot;
    if (!parse_heartbeat(decoded.payload, snapshot) ||
        !peer.replay.accept(decoded.sequence)) return;
    peer.last_seen_us = now;
    peer.remote_revision = snapshot.revision;
    peer.remote_topology_revision = snapshot.topology_revision;
    peer.remote_topology_hash = snapshot.topology_hash;
    peer.remote_active_hash = snapshot.active_hash;
    peer.remote_candidate_hash = snapshot.candidate_hash;
    peer.remote_health = snapshot.health;
    peer.remote_fault_code = snapshot.fault_code;
  }
}

void send_master_heartbeats(int64_t now) {
  for (auto& peer : g_peers) {
    if (!peer.used) continue;
    PendingSession& pending = peer.pending;
    if (pending.valid && !pending.candidate_ready && now < pending.expires_us &&
        now - pending.last_response_us >= kHandshakeRetryUs)
      send_offer(peer, now);
    if (!peer.active || now - peer.last_heartbeat_us < kHeartbeatIntervalUs) continue;
    const std::string payload = heartbeat_payload(now);
    send_packet(SyncMessageType::Heartbeat, g_config->nodeId, *g_boot_id,
                peer.tx_sequence++, payload, peer.session_key, peer.address);
    peer.last_heartbeat_us = now;
    if (peer.enabled && now - peer.last_state_us >= kHeartbeatIntervalUs) {
      const std::string state = state_payload(g_playback_snapshot);
      if (!state.empty())
        send_packet(SyncMessageType::State, g_config->nodeId, *g_boot_id,
                    peer.tx_sequence++, state, peer.session_key, peer.address);
      peer.last_state_us = now;
    }
  }
}

bool resolve_master_address() {
  if (g_master_address_resolved) return true;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* results = nullptr;
  if (getaddrinfo(g_config->masterAddress.c_str(), nullptr, &hints, &results) != 0 ||
      !results) return false;
  const auto* address = reinterpret_cast<const sockaddr_in*>(results->ai_addr);
  g_master_address = *address;
  set_port(g_master_address);
  freeaddrinfo(results);
  g_master_address_resolved = true;
  return true;
}

void send_follower_time_request(Peer& peer, int64_t now) {
  if (peer.clock_synced && now - peer.last_valid_clock_us >= 5000000)
    clear_clock_estimate(peer);
  if (peer.pending_time) {
    if (now - peer.pending_time_sent_us < 500000) return;
    peer.pending_time = false;
  }
  const int64_t interval = peer.clock_sample_count < 8 ? 100000 : 1000000;
  if (peer.last_time_request_us != 0 &&
      now - peer.last_time_request_us < interval) return;
  const uint64_t t1 = static_cast<uint64_t>(std::max<int64_t>(0, now));
  const std::string payload = time_request_payload(t1);
  if (payload.empty() ||
      !send_packet(SyncMessageType::TimeRequest, g_config->nodeId, *g_boot_id,
                   peer.tx_sequence++, payload, peer.session_key, peer.address)) return;
  peer.pending_time_t1 = t1;
  peer.pending_time_sent_us = now;
  peer.pending_time = true;
  peer.last_time_request_us = now;
}

void begin_follower_challenge(Peer& peer, int64_t now) {
  clear_pending(peer.pending);
  peer.pending.valid = true;
  peer.pending.nonce_b = random_hex(16);
  peer.pending.expires_us = now + kChallengeLifetimeUs;
  peer.pending.address = g_master_address;
  peer.pending.last_response_us = 0;
  g_last_hello_us = 0;
}

void tick_follower(int64_t now) {
  if (!resolve_master_address()) return;
  Peer& peer = g_peers[0];
  if (!peer.used) {
    peer.used = true;
    peer.enabled = true;
    peer.node_id = g_config->masterNodeId;
    peer.label = "Master";
  }
  tick_follower_transaction(peer, now);
  const bool recent_master = peer.active && now - peer.last_seen_us <= kPeerTimeoutUs;
  if (!recent_master) {
    if (!peer.pending.valid || now >= peer.pending.expires_us)
      begin_follower_challenge(peer, now);
    if (!peer.pending.candidate_ready &&
        (g_last_hello_us == 0 || now - g_last_hello_us >= kHelloIntervalUs)) {
      const std::string payload = make_hello_payload(peer.pending.nonce_b,
                                                       g_config->hardwareId);
      send_packet(SyncMessageType::Hello, g_config->nodeId, *g_boot_id,
                  g_hello_sequence++, payload, g_config->groupKey, g_master_address);
      g_last_hello_us = now;
    }
    if (peer.pending.candidate_ready &&
        now - peer.pending.last_response_us >= kHandshakeRetryUs)
      send_accept(peer, now);
  }
  if (recent_master) {
    send_follower_time_request(peer, now);
    if (now - g_last_follower_heartbeat_us >= kHeartbeatIntervalUs) {
      const std::string payload = heartbeat_payload(now, &peer);
      send_packet(SyncMessageType::Heartbeat, g_config->nodeId, *g_boot_id,
                  peer.tx_sequence++, payload, peer.session_key, peer.address);
      g_last_follower_heartbeat_us = now;
    }
  }
}

bool open_socket() {
  const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return false;
  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  timeval timeout{};
  timeout.tv_sec = 0;
  timeout.tv_usec = 50000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(kSyncPort);
  if (bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
    closesocket(fd);
    return false;
  }
  g_socket = fd;
  return true;
}

void runtime_task(void*) {
  uint8_t buffer[1200];
  for (;;) {
    if (g_socket < 0 && !open_socket()) {
      ESP_LOGW(kTag, "UDP port %u could not be bound; retrying", kSyncPort);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    sockaddr_in source{};
    socklen_t source_length = sizeof(source);
    const int received = recvfrom(g_socket, buffer, sizeof(buffer), 0,
        reinterpret_cast<sockaddr*>(&source), &source_length);
    const int64_t now = esp_timer_get_time();
    if (g_mutex) xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (received > 0) process_datagram(buffer, received, source, now);
    if (g_config->role == Role::Master) {
      tick_master_transaction(now);
      send_master_heartbeats(now);
    }
    else tick_follower(now);
    if (g_config->role == Role::Master) send_blackout_broadcast(now);
    if (g_mutex) xSemaphoreGive(g_mutex);
  }
}

bool transaction_runtime_self_test(std::string& error) {
  if (g_started) {
    error = "transaction self-test must run before the UDP runtime starts";
    return false;
  }
  for (size_t i = 1; i < kMaxPeers; ++i) {
    if (g_peers[i].active || g_peers[i].transaction_required) {
      error = "transaction self-test could not isolate the peer table";
      return false;
    }
  }
  struct RestoreState {
    NodeConfig* config = g_config;
    uint64_t* boot_id = g_boot_id;
    int socket = g_socket;
    PlaybackState playback = g_playback_snapshot;
    Peer peer = g_peers[0];
    BlackoutBroadcast blackout = g_blackout_broadcast;
    MasterTransaction master_transaction = g_master_transaction;
    ~RestoreState() {
      g_config = config;
      g_boot_id = boot_id;
      g_socket = socket;
      g_playback_snapshot = playback;
      g_peers[0] = peer;
      g_blackout_broadcast = blackout;
      g_master_transaction = master_transaction;
    }
  } restore;

  NodeConfig config;
  config.nodeId = 1;
  config.masterNodeId = 1;
  config.role = Role::Master;
  config.groupId = "01234567-89ab-cdef-0123-456789abcdef";
  config.groupKey = std::string(64, 'a');
  config.brightnessCap = 64;
  uint64_t boot_id = 0x1234;
  g_config = &config;
  g_boot_id = &boot_id;
  g_socket = -1;

  PlaybackState baseline;
  baseline.revision = 7;
  baseline.topologyRevision = 3;
  baseline.topologyHash = std::string(64, 'b');
  baseline.packageHash = std::string(64, 'c');
  baseline.layoutId = "layout_a";
  baseline.mode = "stopped";
  baseline.running = false;
  baseline.blackout = true;
  baseline.userBrightness = 26;
  const std::string command_id = "01234567-89ab-cdef-0123-456789abcdef";
  const std::string other_command_id = "fedcba98-7654-3210-fedc-ba9876543210";
  constexpr int64_t kNow = 1000000;

  auto make_next = [&](uint32_t revision, uint64_t apply_at) {
    PlaybackState next = baseline;
    next.revision = revision;
    next.effectiveAtUs = apply_at;
    return next;
  };
  auto begin_master_case = [&](const PlaybackState& next,
                               MasterTransactionPhase phase,
                               MasterPeerTransactionPhase peer_phase,
                               int64_t started_us, int64_t last_seen_us,
                               int64_t last_send_us) {
    g_playback_snapshot = baseline;
    g_blackout_broadcast = BlackoutBroadcast{};
    g_master_transaction = MasterTransaction{};
    g_master_transaction.phase = phase;
    g_master_transaction.command_id = command_id;
    g_master_transaction.next_state = next;
    g_master_transaction.started_us = started_us;
    g_master_transaction.required_count = 1;
    g_peers[0] = Peer{};
    g_peers[0].used = true;
    g_peers[0].enabled = true;
    g_peers[0].active = true;
    g_peers[0].node_id = 2;
    g_peers[0].hardware_id = other_command_id;
    g_peers[0].last_seen_us = last_seen_us;
    g_peers[0].transaction_required = true;
    g_peers[0].transaction_phase = peer_phase;
    g_peers[0].transaction_last_send_us = last_send_us;
    g_peers[0].session_key = std::string(64, 'd');
    g_peers[0].address.sin_family = AF_INET;
    g_peers[0].address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_peers[0].address.sin_port = htons(1);
  };
  auto fail = [&](const char* message) {
    error = message;
    return false;
  };

  PlaybackState next = make_next(8, 3000000);
  begin_master_case(next, MasterTransactionPhase::Preparing,
                    MasterPeerTransactionPhase::WaitingReady,
                    kNow, kNow, kNow);
  SyncPacket reply;
  reply.type = SyncMessageType::Ready;
  reply.sequence = 0;
  reply.payload = transaction_ready_payload(other_command_id, next, true, nullptr);
  process_master_transaction_reply(g_peers[0], reply, kNow + 1);
  if (g_peers[0].transaction_phase != MasterPeerTransactionPhase::WaitingReady)
    return fail("Master accepted READY for a different command");
  PlaybackState wrong_topology = next;
  ++wrong_topology.topologyRevision;
  reply.sequence = 1;
  reply.payload = transaction_ready_payload(command_id, wrong_topology, true, nullptr);
  process_master_transaction_reply(g_peers[0], reply, kNow + 2);
  if (g_peers[0].transaction_phase != MasterPeerTransactionPhase::WaitingReady)
    return fail("Master accepted READY for a different topology");
  reply.sequence = 2;
  reply.payload = transaction_ready_payload(command_id, next, true, nullptr);
  process_master_transaction_reply(g_peers[0], reply, kNow + 3);
  if (g_peers[0].transaction_phase != MasterPeerTransactionPhase::Ready)
    return fail("Master did not accept the matching READY reply");
  process_master_transaction_reply(g_peers[0], reply, kNow + 4);
  if (g_peers[0].transaction_phase != MasterPeerTransactionPhase::Ready)
    return fail("Master accepted a duplicate READY sequence");
  reply.type = SyncMessageType::Ack;
  reply.sequence = 3;
  reply.payload = transaction_ack_payload(command_id, next, "committed", true, nullptr);
  process_master_transaction_reply(g_peers[0], reply, kNow + 5);
  if (g_peers[0].transaction_phase != MasterPeerTransactionPhase::Committed)
    return fail("Master did not accept the committed ACK");
  reply.sequence = 4;
  reply.payload = transaction_ack_payload(command_id, next, "applied", true, nullptr);
  process_master_transaction_reply(g_peers[0], reply, kNow + 6);
  if (g_peers[0].transaction_phase != MasterPeerTransactionPhase::Applied)
    return fail("Master did not accept the applied ACK");
  if (!master_peer_matches_state(g_peers[0], next))
    return fail("applied ACK did not confirm the peer state for the next command");
  reply.sequence = 5;
  reply.payload = transaction_ack_payload(command_id, next, "committed", true, nullptr);
  process_master_transaction_reply(g_peers[0], reply, kNow + 7);
  if (g_peers[0].transaction_phase != MasterPeerTransactionPhase::Applied)
    return fail("late committed ACK regressed an applied peer");

  next = make_next(8, 3000000);
  next.blackout = false;
  begin_master_case(next, MasterTransactionPhase::Committing,
                    MasterPeerTransactionPhase::Committed,
                    kNow, 3000000, 3000000);
  tick_master_transaction(3000000);
  if (g_master_transaction.phase != MasterTransactionPhase::WaitingApplied ||
      g_playback_snapshot.revision != next.revision ||
      g_playback_snapshot.blackout) {
    return fail("Master did not apply a committed state with its requested blackout flag");
  }
  reply = SyncPacket{};
  reply.type = SyncMessageType::Ack;
  reply.sequence = 0;
  reply.payload = transaction_ack_payload(command_id, next, "applied", true, nullptr);
  process_master_transaction_reply(g_peers[0], reply, 3000001);
  tick_master_transaction(3000001);
  if (g_master_transaction.phase != MasterTransactionPhase::Finished ||
      !g_master_transaction.success || g_peers[0].transaction_required) {
    return fail("Master did not finish after every required applied ACK");
  }

  next = make_next(8, 3000000);
  begin_master_case(next, MasterTransactionPhase::Preparing,
                    MasterPeerTransactionPhase::WaitingReady,
                    kNow, kNow, kNow);
  reply = SyncPacket{};
  reply.type = SyncMessageType::Ready;
  reply.sequence = 0;
  reply.payload = transaction_ready_payload(command_id, next, false,
                                             "BRIGHTNESS_CAP");
  process_master_transaction_reply(g_peers[0], reply, kNow);
  tick_master_transaction(kNow);
  if (g_master_transaction.success ||
      g_master_transaction.error != "BRIGHTNESS_CAP" ||
      !g_playback_snapshot.blackout || !g_blackout_broadcast.pending ||
      g_peers[0].transaction_required) {
    return fail("Master did not fail dark after a follower rejection");
  }

  next = make_next(8, 3000000);
  begin_master_case(next, MasterTransactionPhase::Preparing,
                    MasterPeerTransactionPhase::WaitingReady,
                    kNow, kNow - kPeerTimeoutUs - 1, kNow);
  tick_master_transaction(kNow);
  if (g_master_transaction.error != "PEER_LOST" ||
      !g_playback_snapshot.blackout || !g_blackout_broadcast.pending) {
    return fail("Master did not fail dark when a follower was lost");
  }

  next = make_next(8, 3000000);
  begin_master_case(next, MasterTransactionPhase::Preparing,
                    MasterPeerTransactionPhase::WaitingReady,
                    kNow - 100000, kNow, kNow);
  tick_master_transaction(kNow);
  if (g_master_transaction.error != "READY_TIMEOUT" ||
      !g_playback_snapshot.blackout) {
    return fail("Master did not fail dark when READY timed out");
  }

  next = make_next(8, 3000000);
  begin_master_case(next, MasterTransactionPhase::Committing,
                    MasterPeerTransactionPhase::Ready,
                    kNow, 2850000, 2850000);
  tick_master_transaction(2850000);
  if (g_master_transaction.error != "COMMIT_TIMEOUT" ||
      !g_playback_snapshot.blackout) {
    return fail("Master did not fail dark when COMMIT timed out");
  }

  next = make_next(8, 2000000);
  begin_master_case(next, MasterTransactionPhase::WaitingApplied,
                    MasterPeerTransactionPhase::Committed,
                    kNow, 2750001, 2750001);
  tick_master_transaction(2750001);
  if (g_master_transaction.error != "APPLY_TIMEOUT" ||
      !g_playback_snapshot.blackout) {
    return fail("Master did not fail dark when the applied ACK timed out");
  }

  next = make_next(8, 3000000);
  begin_master_case(next, MasterTransactionPhase::Preparing,
                    MasterPeerTransactionPhase::WaitingReady,
                    kNow, kNow, kNow);
  g_blackout_broadcast.pending = true;
  g_blackout_broadcast.revision = 9;
  g_blackout_broadcast.reason = "user";
  tick_master_transaction(kNow);
  if (g_master_transaction.error != "CANCELED_BY_BLACKOUT" ||
      !g_playback_snapshot.blackout || g_playback_snapshot.revision != 9 ||
      !g_blackout_broadcast.pending) {
    return fail("a newer blackout did not supersede the pending transaction");
  }

  config.role = Role::Follower;
  g_playback_snapshot = baseline;
  Peer follower;
  follower.active = true;
  follower.clock_synced = true;
  follower.last_valid_clock_us = kNow - 1000;
  follower.session_key = std::string(64, 'e');
  follower.address.sin_family = AF_INET;
  follower.address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  follower.address.sin_port = htons(1);
  next = make_next(8, 4000000);
  next.mode = "scene";
  next.sceneId = "scene_a";
  next.running = true;
  next.blackout = false;
  SyncPacket prepare;
  prepare.type = SyncMessageType::Prepare;
  prepare.payload = transaction_state_payload(command_id, next);
  process_follower_prepare(follower, prepare, 3000000);
  const int64_t prepared_at = follower.transaction.apply_at_local_us;
  process_follower_prepare(follower, prepare, 3010000);
  if (follower.transaction.phase != FollowerTransactionPhase::Prepared ||
      follower.transaction.apply_at_local_us != prepared_at ||
      follower.transaction.command_id != command_id) {
    return fail("duplicate PREPARE was not idempotent");
  }
  PlaybackState conflict = next;
  ++conflict.userBrightness;
  SyncPacket conflicting_prepare = prepare;
  conflicting_prepare.payload = transaction_state_payload(command_id, conflict);
  process_follower_prepare(follower, conflicting_prepare, 3020000);
  if (follower.transaction.phase != FollowerTransactionPhase::Prepared ||
      follower.transaction.next_state.userBrightness != next.userBrightness) {
    return fail("conflicting PREPARE changed an already prepared target");
  }
  SyncPacket commit;
  commit.type = SyncMessageType::Commit;
  commit.payload = transaction_state_payload(command_id, next);
  process_follower_commit(follower, commit, 3200000);
  process_follower_commit(follower, commit, 3250000);
  if (follower.transaction.phase != FollowerTransactionPhase::Committed) {
    return fail("follower did not commit or idempotently repeat COMMIT");
  }
  tick_follower_transaction(follower, 4000000);
  if (follower.transaction.phase != FollowerTransactionPhase::Applied ||
      g_playback_snapshot.revision != next.revision ||
      g_playback_snapshot.blackout || !g_playback_snapshot.running ||
      g_playback_snapshot.mode != "scene") {
    return fail("follower did not apply the scheduled running state and blackout flag");
  }
  SyncPacket abort;
  abort.type = SyncMessageType::Abort;
  abort.payload = transaction_abort_payload(command_id, next.revision);
  process_follower_abort(follower, abort, 4000001);
  if (follower.transaction.phase != FollowerTransactionPhase::Applied) {
    return fail("ABORT incorrectly undid an already applied transaction");
  }

  PlaybackState abort_target = g_playback_snapshot;
  ++abort_target.revision;
  abort_target.effectiveAtUs = 6000000;
  prepare.payload = transaction_state_payload(other_command_id, abort_target);
  process_follower_prepare(follower, prepare, 5000000);
  if (follower.transaction.phase != FollowerTransactionPhase::Prepared) {
    return fail("completed command blocked the next revision within ten seconds");
  }
  abort.payload = transaction_abort_payload(other_command_id, abort_target.revision);
  process_follower_abort(follower, abort, 5010000);
  if (follower.transaction.phase != FollowerTransactionPhase::None) {
    return fail("ABORT did not clear an uncommitted PREPARE");
  }

  PlaybackState late_target = g_playback_snapshot;
  ++late_target.revision;
  late_target.effectiveAtUs = 8000000;
  follower.last_valid_clock_us = 7000000;
  prepare.payload = transaction_state_payload(command_id, late_target);
  process_follower_prepare(follower, prepare, 7000000);
  commit.payload = transaction_state_payload(command_id, late_target);
  process_follower_commit(follower, commit, late_target.effectiveAtUs - 149999);
  if (follower.transaction.phase != FollowerTransactionPhase::Rejected ||
      follower.transaction.error != "DEADLINE_MISSED") {
    return fail("follower accepted a COMMIT after its scheduling deadline");
  }

  error.clear();
  return true;
}
}  // namespace

bool sync_runtime_self_test(std::string& error) {
  uint64_t shared = 0;
  for (const uint64_t elapsed : {0ULL, 33334ULL, 12000000ULL, 3600000000ULL}) {
    if (!to_master_time(1000000 + elapsed, 15000000, shared) ||
        shared != 16000000 + elapsed ||
        !to_master_time(31000000 + elapsed, -15000000, shared) ||
        shared != 16000000 + elapsed) {
      error = "follower render clock differs from master playback timeline";
      return false;
    }
  }
  if (to_master_time(0, -1, shared) ||
      to_master_time(UINT64_MAX, 1, shared) ||
      !to_master_time(1ULL << 63, INT64_MIN, shared) || shared != 0) {
    error = "render clock overflow or negative-time validation failed";
    return false;
  }
  if (!blackout_payload_self_test(error)) return false;
  static NodeHttpReplayState http_replay;
  http_replay = NodeHttpReplayState{};
  const std::string http_nonce = "00112233445566778899aabbccddeeff";
  if (!admit_node_http_replay(http_replay, 0, http_nonce, 0) ||
      admit_node_http_replay(http_replay, 1, http_nonce, 1) ||
      admit_node_http_replay(http_replay, 0, "10112233445566778899aabbccddeeff", 1) ||
      !admit_node_http_replay(http_replay, 1,
          "10112233445566778899aabbccddeeff", 1)) {
    error = "node HTTP sequence and nonce replay checks failed";
    return false;
  }
  for (uint32_t sequence = 2; sequence < kNodeHttpNonceCapacity; ++sequence) {
    char nonce[33] = {};
    std::snprintf(nonce, sizeof(nonce), "%032x",
                  static_cast<unsigned>(sequence));
    if (!admit_node_http_replay(http_replay, sequence, nonce, 1)) {
      error = "node HTTP nonce cache rejected a valid entry";
      return false;
    }
  }
  if (admit_node_http_replay(http_replay, kNodeHttpNonceCapacity,
          "f0112233445566778899aabbccddeeff", 1) ||
      !admit_node_http_replay(http_replay, kNodeHttpNonceCapacity,
          "f0112233445566778899aabbccddeeff", kNodeHttpNonceLifetimeUs + 1)) {
    error = "node HTTP nonce cache did not fail closed or expire entries";
    return false;
  }
  if (kMaxPeers != 15 || kMaxSoftApFollowers != 3) {
    error = "active-node peer capacity does not match the supported network limits";
    return false;
  }
  Peer master_view;
  master_view.used = true;
  master_view.enabled = true;
  master_view.active = true;
  master_view.node_id = 42;
  master_view.hardware_id = "00000000-0000-4000-8000-00000000002a";
  master_view.last_seen_us = 9000000;
  master_view.remote_revision = 7;
  master_view.remote_topology_revision = 3;
  master_view.remote_topology_hash = std::string(64, 'a');
  master_view.remote_active_hash = std::string(64, 'b');
  RigFollowerIdentity registered_follower{42, master_view.hardware_id};
  PlaybackState committed_state;
  committed_state.revision = 7;
  committed_state.topologyRevision = 3;
  committed_state.topologyHash = std::string(64, 'a');
  committed_state.packageHash = std::string(64, 'b');
  if (master_view.clock_synced || master_view.last_valid_clock_us != 0 ||
      !master_peer_is_live(master_view, registered_follower, 9500000) ||
      !master_peer_matches_state(master_view, committed_state)) {
    error = "Master preflight incorrectly required a Master-side follower clock sample";
    return false;
  }
  master_view.remote_revision++;
  if (master_peer_matches_state(master_view, committed_state)) {
    error = "Master preflight accepted a stale follower state";
    return false;
  }
  master_view.remote_revision--;
  if (master_peer_is_live(master_view, registered_follower, 11000001)) {
    error = "Master preflight accepted a follower past the heartbeat deadline";
    return false;
  }
  for (const uint8_t type : {
           static_cast<uint8_t>(SyncMessageType::Heartbeat),
           static_cast<uint8_t>(SyncMessageType::Blackout),
           static_cast<uint8_t>(SyncMessageType::State),
           static_cast<uint8_t>(SyncMessageType::TimeResponse),
           static_cast<uint8_t>(SyncMessageType::Prepare),
           static_cast<uint8_t>(SyncMessageType::Commit),
           static_cast<uint8_t>(SyncMessageType::Abort)}) {
    if (!follower_session_type(type)) {
      error = "supported follower session type was filtered";
      return false;
    }
  }
  for (const uint8_t type : {
           static_cast<uint8_t>(SyncMessageType::TimeRequest),
           static_cast<uint8_t>(SyncMessageType::Ready),
           static_cast<uint8_t>(SyncMessageType::Ack),
           static_cast<uint8_t>(SyncMessageType::Hello)}) {
    if (follower_session_type(type)) {
      error = "unimplemented follower transaction type was accepted";
      return false;
    }
  }
  const std::string time_request = time_request_payload(1000000);
  uint64_t t1 = 0, t2 = 0, t3 = 0;
  if (!parse_time_request(time_request, t1) || t1 != 1000000 ||
      !parse_time_response(time_response_payload(t1, 1000100, 1000120), t1, t2, t3) ||
      t1 != 1000000 || t2 != 1000100 || t3 != 1000120) {
    error = "time exchange payload roundtrip failed";
    return false;
  }
  int64_t estimated_offset = 0;
  uint32_t estimated_rtt = 0;
  if (!estimate_clock_sample(t1, t2, t3, 1000080,
                             estimated_offset, estimated_rtt) ||
      estimated_offset != 70 || estimated_rtt != 60 ||
      estimate_clock_sample(t1, t2, t3, 1030000,
                            estimated_offset, estimated_rtt) ||
      estimate_clock_sample(t1, t2, t3, 1500000,
                            estimated_offset, estimated_rtt)) {
    error = "clock offset, RTT, or response-age validation failed";
    return false;
  }
  Peer clock_peer;
  record_clock_sample(clock_peer, 100, 500, 1000000);
  record_clock_sample(clock_peer, 120, 100, 1100000);
  record_clock_sample(clock_peer, 110, 200, 1200000);
  if (!clock_peer.clock_synced || clock_peer.clock_offset_us != 110 ||
      clock_peer.clock_rtt_us != 200 || clock_peer.clock_sample_count != 3) {
    error = "clock startup did not choose the median of three lowest-RTT samples";
    return false;
  }
  SyncPeerSnapshot heartbeat_snapshot;
  const std::string clock_heartbeat = heartbeat_payload(1250000, &clock_peer);
  if (!parse_heartbeat(clock_heartbeat, heartbeat_snapshot) ||
      !heartbeat_snapshot.clock_synced ||
      heartbeat_snapshot.clock_offset_us != 110 ||
      heartbeat_snapshot.clock_rtt_us != 200 ||
      heartbeat_snapshot.clock_sample_age_ms != 50) {
    error = "heartbeat did not report the follower's verified clock status";
    return false;
  }
  const size_t candidate_key_pos = clock_heartbeat.find("\"candidateHash\"");
  const size_t candidate_colon_pos = candidate_key_pos == std::string::npos
      ? std::string::npos : clock_heartbeat.find(':', candidate_key_pos);
  const size_t candidate_value_end = candidate_colon_pos == std::string::npos
      ? std::string::npos : clock_heartbeat.find(',', candidate_colon_pos);
  if (candidate_value_end == std::string::npos) {
    error = "candidate hash heartbeat self-test field is missing";
    return false;
  }
  std::string malformed_candidate_heartbeat = clock_heartbeat;
  malformed_candidate_heartbeat.replace(candidate_colon_pos + 1,
      candidate_value_end - candidate_colon_pos - 1, "\"invalid\"");
  if (parse_heartbeat(malformed_candidate_heartbeat, heartbeat_snapshot)) {
    error = "heartbeat accepted an invalid candidate hash";
    return false;
  }
  std::string malformed_clock_heartbeat = clock_heartbeat;
  const size_t clock_offset_pos = malformed_clock_heartbeat.find("110");
  if (clock_offset_pos == std::string::npos) {
    error = "clock heartbeat self-test vector could not be located";
    return false;
  }
  malformed_clock_heartbeat.replace(clock_offset_pos, 3, "01");
  if (parse_heartbeat(malformed_clock_heartbeat, heartbeat_snapshot)) {
    error = "heartbeat accepted a non-canonical clock offset";
    return false;
  }
  if (!parse_heartbeat(
          "{\"revision\":0,\"topologyRevision\":0,\"topologyHash\":\"" +
              std::string(kZeroHash) +
              "\",\"activeHash\":null,\"nowUs\":\"0\",\"health\":\"unsynced\",\"faultCode\":null}",
          heartbeat_snapshot) || heartbeat_snapshot.clock_synced ||
      heartbeat_snapshot.clock_sample_age_ms != -1) {
    error = "legacy heartbeat did not default to unsynchronized clock status";
    return false;
  }
  record_clock_sample(clock_peer, 1110, 100, 2200000);
  if (clock_peer.clock_offset_us != 1110 || !clock_peer.clock_synced) {
    error = "clock correction exceeded or failed the 1ms/s slew limit";
    return false;
  }
  record_clock_sample(clock_peer, 51110, 100, 3200000);
  if (clock_peer.clock_synced || clock_peer.clock_sample_count != 1) {
    error = "clock jump over 20ms did not restart synchronization";
    return false;
  }
  error.clear();
  PlaybackState source;
  source.revision = 17;
  source.topologyRevision = 4;
  source.topologyHash = std::string(64, 'a');
  source.packageHash = std::string(64, 'b');
  source.layoutId = "layout_a";
  source.mode = "scene";
  source.sceneId = "scene_7";
  source.running = true;
  source.positionTicks = UINT64_MAX;
  source.anchorUs = UINT64_MAX - 1;
  source.effectiveAtUs = UINT64_MAX;
  source.blackout = false;
  source.userBrightness = 200;
  const std::string payload = state_payload(source);
  PlaybackState decoded;
  if (!parse_state(payload, decoded) || !same_state(source, decoded)) {
    error = "STATE payload roundtrip mismatch";
    return false;
  }
  PlaybackState dark_state = source;
  dark_state.blackout = true;
  PlaybackState decoded_dark_state;
  if (!parse_state(state_payload(dark_state), decoded_dark_state) ||
      !same_state(dark_state, decoded_dark_state) ||
      !decoded_dark_state.blackout) {
    error = "STATE payload did not preserve explicit blackout";
    return false;
  }
  const std::string command_id = "01234567-89ab-cdef-0123-456789abcdef";
  std::string parsed_command_id;
  PlaybackState transaction_state;
  const std::string transaction_payload =
      transaction_state_payload(command_id, source);
  if (!parse_transaction_state(transaction_payload, parsed_command_id,
                               transaction_state) ||
      parsed_command_id != command_id || !same_state(source, transaction_state) ||
      !parse_transaction_abort("{\"commandId\":\"01234567-89ab-cdef-0123-456789abcdef\",\"revision\":17}",
                               parsed_command_id, decoded.revision) ||
      parsed_command_id != command_id || decoded.revision != source.revision) {
    error = "transaction payload roundtrip failed";
    return false;
  }
  TransactionReply transaction_reply;
  if (!parse_transaction_reply(transaction_ready_payload(command_id, source, true, nullptr),
                               false, transaction_reply) ||
      transaction_reply.command_id != command_id || !transaction_reply.ok ||
      transaction_reply.revision != source.revision ||
      transaction_reply.topology_hash != source.topologyHash ||
      !parse_transaction_reply(transaction_ack_payload(command_id, source,
                                   "committed", true, nullptr),
                               true, transaction_reply) ||
      transaction_reply.phase != "committed" || !transaction_reply.ok ||
      !parse_transaction_reply(transaction_ack_payload(command_id, source,
                                   "applied", false, "LATE_COMMAND"),
                               true, transaction_reply) ||
      transaction_reply.phase != "applied" || transaction_reply.ok ||
      transaction_reply.error != "LATE_COMMAND") {
    error = "transaction reply roundtrip failed";
    return false;
  }
  for (const char* invalid : {
           "{\"commandId\":\"01234567-89ab-cdef-0123-456789abcdef\",\"nextState\":{},\"extra\":0}",
           "{\"commandId\":\"bad\",\"nextState\":{}}"}) {
    if (parse_transaction_state(invalid, parsed_command_id, transaction_state)) {
      error = "invalid transaction payload was accepted";
      return false;
    }
  }
  if (parse_transaction_abort(
          "{\"commandId\":\"01234567-89ab-cdef-0123-456789abcdef\",\"revision\":17,\"extra\":0}",
          parsed_command_id, decoded.revision)) {
    error = "invalid ABORT payload was accepted";
    return false;
  }
  if (parse_transaction_reply(
          "{\"commandId\":\"01234567-89ab-cdef-0123-456789abcdef\",\"revision\":17,\"topologyRevision\":4,\"topologyHash\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"phase\":\"prepared\",\"ok\":true,\"error\":null}",
          true, transaction_reply)) {
    error = "invalid transaction ACK phase was accepted";
    return false;
  }
  if (admit_state(source, decoded) != StateAdmission::Accept) {
    error = "matching STATE revision was not admitted";
    return false;
  }
  PlaybackState stale = source;
  --stale.revision;
  if (admit_state(source, stale) != StateAdmission::Stale) {
    error = "stale STATE revision was not rejected";
    return false;
  }
  PlaybackState content_mismatch = source;
  content_mismatch.revision++;
  content_mismatch.packageHash.assign(64, 'c');
  if (admit_state(source, content_mismatch) != StateAdmission::ContentMismatch) {
    error = "STATE with mismatched package was not rejected";
    return false;
  }
  PlaybackState topology_mismatch = source;
  topology_mismatch.revision++;
  topology_mismatch.topologyHash.assign(64, 'd');
  if (admit_state(source, topology_mismatch) != StateAdmission::TopologyMismatch) {
    error = "STATE with mismatched topology was not rejected";
    return false;
  }
  PlaybackState revision_conflict = source;
  ++revision_conflict.userBrightness;
  if (admit_state(source, revision_conflict) != StateAdmission::RevisionConflict) {
    error = "conflicting STATE at equal revision was not rejected";
    return false;
  }
  for (const char* invalid : {
           "{\"revision\":1,\"topologyRevision\":0,\"topologyHash\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"packageHash\":null,\"layoutId\":\"panel_64x32\",\"mode\":\"stopped\",\"sceneId\":null,\"cueId\":null,\"running\":false,\"positionTicks\":\"00\",\"anchorUs\":\"0\",\"effectiveAtUs\":\"0\",\"blackout\":true,\"userBrightness\":26}",
           "{\"revision\":1,\"topologyRevision\":0,\"topologyHash\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"packageHash\":null,\"layoutId\":\"panel_64x32\",\"mode\":\"stopped\",\"sceneId\":null,\"cueId\":null,\"running\":true,\"positionTicks\":\"0\",\"anchorUs\":\"0\",\"effectiveAtUs\":\"0\",\"blackout\":true,\"userBrightness\":26}",
           "{\"revision\":1,\"topologyRevision\":0,\"topologyHash\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"packageHash\":null,\"layoutId\":\"panel_64x32\",\"mode\":\"stopped\",\"sceneId\":null,\"cueId\":null,\"running\":false,\"positionTicks\":\"18446744073709551616\",\"anchorUs\":\"0\",\"effectiveAtUs\":\"0\",\"blackout\":true,\"userBrightness\":26,\"unexpected\":false}"}) {
    if (parse_state(invalid, decoded)) {
      error = "invalid STATE payload was accepted";
      return false;
    }
  }
  if (!transaction_runtime_self_test(error)) return false;
  error.clear();
  return true;
}

void sync_runtime_start(NodeConfig* config, PlaybackState* state, uint64_t* boot_id) {
  if (g_started || !config || !state || !boot_id || !config->provisioned) return;
  g_config = config;
  g_boot_id = boot_id;
  g_mutex = xSemaphoreCreateMutex();
  if (!g_mutex) {
    ESP_LOGE(kTag, "peer mutex allocation failed");
    return;
  }
  if (config->role == Role::Follower && (!config->masterNodeId ||
      config->masterAddress.empty() || !valid_uuid(config->hardwareId))) {
    ESP_LOGE(kTag, "follower identity or master address is invalid");
    return;
  }
  g_playback_snapshot = *state;
  if (xTaskCreatePinnedToCore(runtime_task, "sync_runtime", 8192, nullptr, 5,
                              nullptr, 0) != pdPASS) {
    ESP_LOGE(kTag, "UDP sync task could not start");
    return;
  }
  g_started = true;
  ESP_LOGI(kTag, "authenticated UDP runtime listening on port %u", kSyncPort);
}

bool sync_runtime_update_state(const PlaybackState& state) {
  if (!g_started || !g_mutex ||
      xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  g_playback_snapshot = state;
  xSemaphoreGive(g_mutex);
  return true;
}

bool sync_runtime_state_snapshot(PlaybackState& state) {
  if (!g_started || !g_mutex ||
      xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  state = g_playback_snapshot;
  xSemaphoreGive(g_mutex);
  return true;
}

bool sync_runtime_render_snapshot(PlaybackState& state, uint64_t& master_now_us) {
  if (!g_started || !g_mutex || !g_config ||
      xSemaphoreTake(g_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
  state = g_playback_snapshot;
  const int64_t local_now = esp_timer_get_time();
  bool valid = local_now >= 0;
  int64_t offset = 0;
  if (g_config->role == Role::Follower) {
    const Peer& master = g_peers[0];
    valid = valid && master.active && master.clock_synced &&
        master.last_seen_us > 0 && local_now >= master.last_seen_us &&
        local_now - master.last_seen_us <= kPeerTimeoutUs &&
        local_now >= master.last_valid_clock_us &&
        local_now - master.last_valid_clock_us < 5000000;
    offset = master.clock_offset_us;
  }
  if (valid) valid = to_master_time(static_cast<uint64_t>(local_now), offset,
                                  master_now_us);
  xSemaphoreGive(g_mutex);
  return valid;
}

bool sync_runtime_apply_state(const PlaybackState& state,
                              const std::string& command_id,
                              uint32_t timeout_ms, std::string& error) {
  if (!g_started || !g_mutex || !g_config || g_config->role != Role::Master ||
      !valid_uuid(command_id) || timeout_ms < 500 || timeout_ms > 10000) {
    error = "MULTI_NODE_SYNC_UNAVAILABLE";
    return false;
  }
  std::vector<RigFollowerIdentity> followers;
  if (!rig_display_followers(*g_config, state.layoutId, followers, error)) {
    error = "RIG_UNAVAILABLE";
    return false;
  }
  if (followers.empty()) {
    error = "NO_REQUIRED_FOLLOWERS";
    return false;
  }
  const size_t follower_capacity = g_config->networkMode == "softap"
      ? kMaxSoftApFollowers : kMaxPeers;
  if (followers.size() > follower_capacity) {
    error = "PEER_CAPACITY_EXCEEDED";
    return false;
  }
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    error = "SYNC_BUSY";
    return false;
  }
  if (g_master_transaction.phase != MasterTransactionPhase::Idle) {
    xSemaphoreGive(g_mutex);
    error = "SYNC_BUSY";
    return false;
  }
  if (g_playback_snapshot.revision == UINT32_MAX ||
      state.revision != g_playback_snapshot.revision + 1 ||
      state.topologyRevision != g_playback_snapshot.topologyRevision ||
      state.topologyHash != g_playback_snapshot.topologyHash ||
      state.packageHash != g_playback_snapshot.packageHash ||
      state.userBrightness > g_config->brightnessCap) {
    xSemaphoreGive(g_mutex);
    error = "STATE_NOT_SUPPORTED";
    return false;
  }
  const int64_t now = esp_timer_get_time();
  if (state.effectiveAtUs > static_cast<uint64_t>(INT64_MAX) ||
      static_cast<int64_t>(state.effectiveAtUs) - now < 500000) {
    xSemaphoreGive(g_mutex);
    error = "INVALID_SCHEDULE";
    return false;
  }
  for (const auto& follower : followers) {
    Peer* peer = find_peer(follower.node_id);
    if (!peer || !master_peer_is_live(*peer, follower, now)) {
      xSemaphoreGive(g_mutex);
      error = "PEER_NOT_READY";
      return false;
    }
    if (!master_peer_matches_state(*peer, g_playback_snapshot) ||
        peer->remote_topology_revision != state.topologyRevision ||
        peer->remote_topology_hash != state.topologyHash ||
        peer->remote_active_hash != state.packageHash) {
      xSemaphoreGive(g_mutex);
      error = "PEER_STATE_MISMATCH";
      return false;
    }
  }
  for (auto& peer : g_peers) peer.transaction_required = false;
  for (const auto& follower : followers) {
    Peer* peer = find_peer(follower.node_id);
    peer->transaction_required = true;
    peer->transaction_phase = MasterPeerTransactionPhase::WaitingReady;
    peer->transaction_error.clear();
    peer->transaction_last_send_us = 0;
  }
  g_master_transaction = MasterTransaction{};
  g_master_transaction.phase = MasterTransactionPhase::Preparing;
  g_master_transaction.command_id = command_id;
  g_master_transaction.next_state = state;
  g_master_transaction.started_us = now;
  g_master_transaction.required_count = followers.size();
  xSemaphoreGive(g_mutex);

  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  for (;;) {
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (g_master_transaction.phase == MasterTransactionPhase::Finished) {
        const bool success = g_master_transaction.success;
        error = g_master_transaction.error;
        g_master_transaction = MasterTransaction{};
        xSemaphoreGive(g_mutex);
        return success;
      }
      if (xTaskGetTickCount() >= deadline) {
        finish_master_transaction(false, "OPERATION_TIMEOUT", esp_timer_get_time());
        error = g_master_transaction.error;
        g_master_transaction = MasterTransaction{};
        xSemaphoreGive(g_mutex);
        return false;
      }
      xSemaphoreGive(g_mutex);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

bool sync_runtime_transaction_active() {
  if (!g_started || !g_mutex) return false;
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return true;
  const bool active = g_master_transaction.phase != MasterTransactionPhase::Idle &&
      g_master_transaction.phase != MasterTransactionPhase::Finished;
  xSemaphoreGive(g_mutex);
  return active;
}

bool sync_runtime_node_http_authorize(
    uint16_t node_id, uint32_t source_ipv4, const std::string& sender_boot_id,
    uint32_t sequence, const std::string& nonce_hex,
    const std::string& method, const std::string& path,
    const std::string& declared_body_hash_hex,
    const std::string& declared_request_mac_hex,
    const uint8_t* request_body, size_t request_body_length,
    std::string& authorized_session_key, std::string& error) {
  wipe(authorized_session_key);
  if (!g_started || !g_mutex || !g_config || !g_boot_id || node_id == 0 ||
      source_ipv4 == 0 || !lower_hex(sender_boot_id, 16) ||
      (request_body_length != 0 && !request_body)) {
    error = "node HTTP session unavailable";
    return false;
  }
  std::string session_key;
  std::string group_id;
  std::string receiver_boot_id;
  Peer* peer = nullptr;
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    error = "node HTTP session busy";
    return false;
  }
  const int64_t now = esp_timer_get_time();
  peer = find_peer(node_id);
  const bool peer_live = peer && peer->active && !peer->session_key.empty() &&
      peer->remote_boot_id != 0 && sender_boot_id == boot_hex(peer->remote_boot_id) &&
      peer->address.sin_addr.s_addr == source_ipv4 && peer->last_seen_us > 0 &&
      now >= peer->last_seen_us && now - peer->last_seen_us <= kPeerTimeoutUs &&
      ((g_config->role == Role::Master && peer->node_id == node_id) ||
       (g_config->role == Role::Follower && node_id == g_config->masterNodeId));
  if (!peer_live) {
    error = "node HTTP peer is not on a live authenticated session";
  } else {
    session_key = peer->session_key;
    group_id = g_config->groupId;
    receiver_boot_id = boot_hex(*g_boot_id);
  }
  xSemaphoreGive(g_mutex);
  if (!peer_live) return false;

  if (!node_http_verify_request(session_key, method, path, group_id,
          sender_boot_id, receiver_boot_id, sequence, nonce_hex,
          declared_body_hash_hex, declared_request_mac_hex,
          request_body, request_body_length, error)) {
    wipe(session_key);
    return false;
  }
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    wipe(session_key);
    error = "node HTTP session busy";
    return false;
  }
  const int64_t admission_time = esp_timer_get_time();
  Peer* current_peer = find_peer(node_id);
  const bool same_session = current_peer == peer && current_peer->active &&
      current_peer->node_id == node_id && current_peer->remote_boot_id != 0 &&
      current_peer->session_key == session_key &&
      current_peer->address.sin_addr.s_addr == source_ipv4 &&
      sender_boot_id == boot_hex(current_peer->remote_boot_id) &&
      current_peer->last_seen_us > 0 && admission_time >= current_peer->last_seen_us &&
      admission_time - current_peer->last_seen_us <= kPeerTimeoutUs &&
      ((g_config->role == Role::Master && current_peer->node_id == node_id) ||
       (g_config->role == Role::Follower && node_id == g_config->masterNodeId));
  bool admitted = false;
  if (!same_session) {
    error = "node HTTP session changed during authentication";
  } else if (!node_http_replay_state(current_peer) ||
             !admit_node_http_replay(*node_http_replay_state(current_peer), sequence,
                                     nonce_hex, admission_time)) {
    error = "node HTTP request replay rejected";
  } else {
    admitted = true;
  }
  xSemaphoreGive(g_mutex);
  if (!admitted) {
    wipe(session_key);
    return false;
  }

  authorized_session_key = std::move(session_key);
  error.clear();
  return true;
}

bool sync_runtime_node_http_exchange(
    uint16_t node_id, uint32_t source_ipv4, const std::string& sender_boot_id,
    uint32_t sequence, const std::string& nonce_hex,
    const std::string& method, const std::string& path,
    const std::string& declared_body_hash_hex,
    const std::string& declared_request_mac_hex,
    const uint8_t* request_body, size_t request_body_length,
    uint16_t response_status, const uint8_t* response_body,
    size_t response_body_length, std::string& response_body_hash_hex,
    std::string& response_mac_hex, std::string& error) {
  std::string session_key;
  if (!sync_runtime_node_http_authorize(node_id, source_ipv4, sender_boot_id,
          sequence, nonce_hex, method, path, declared_body_hash_hex,
          declared_request_mac_hex, request_body, request_body_length,
          session_key, error)) return false;
  const bool signed_response = node_http_sign_response(session_key,
      response_status, path, sequence, nonce_hex, response_body,
      response_body_length, response_body_hash_hex, response_mac_hex, error);
  wipe(session_key);
  if (!signed_response) return false;
  error.clear();
  return true;
}

bool sync_runtime_node_http_request(
    uint16_t remote_node_id, const std::string& method, const std::string& path,
    const uint8_t* request_body, size_t request_body_length,
    const std::string& if_candidate_hash, uint8_t* response_body,
    size_t response_capacity, size_t& response_body_length,
    uint16_t& response_status, std::string& response_candidate_hash,
    std::string& error) {
  response_body_length = 0;
  response_status = 0;
  response_candidate_hash.clear();
  if (!g_started || !g_mutex || !g_config || !g_boot_id || !remote_node_id ||
      (request_body_length && !request_body) ||
      (response_capacity && !response_body) ||
      (!if_candidate_hash.empty() && !lower_hex(if_candidate_hash, 64))) {
    error = "node HTTP client arguments invalid";
    return false;
  }

  std::string session_key, group_id, sender_boot_id, receiver_boot_id;
  sockaddr_in remote_address{};
  uint32_t sequence = 0;
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    error = "node HTTP session busy";
    return false;
  }
  Peer* peer = find_peer(remote_node_id);
  const int64_t now = esp_timer_get_time();
  const bool peer_live = peer && peer->active && peer->remote_boot_id != 0 &&
      !peer->session_key.empty() && peer->address.sin_family == AF_INET &&
      peer->address.sin_addr.s_addr != 0 && peer->last_seen_us > 0 &&
      now >= peer->last_seen_us && now - peer->last_seen_us <= kPeerTimeoutUs &&
      ((g_config->role == Role::Follower && remote_node_id == g_config->masterNodeId) ||
       (g_config->role == Role::Master && peer->enabled));
  if (!peer_live) {
    xSemaphoreGive(g_mutex);
    error = "node HTTP peer is not on a live authenticated session";
    return false;
  }
  if (peer->node_http_tx_sequence == UINT32_MAX) {
    xSemaphoreGive(g_mutex);
    error = "node HTTP sequence exhausted";
    return false;
  }
  sequence = ++peer->node_http_tx_sequence;
  session_key = peer->session_key;
  group_id = g_config->groupId;
  sender_boot_id = boot_hex(*g_boot_id);
  receiver_boot_id = boot_hex(peer->remote_boot_id);
  remote_address = peer->address;
  xSemaphoreGive(g_mutex);

  char ip_text[INET_ADDRSTRLEN] = {};
  if (!inet_ntop(AF_INET, &remote_address.sin_addr, ip_text, sizeof(ip_text))) {
    wipe(session_key);
    error = "node HTTP peer address invalid";
    return false;
  }
  const std::string url = std::string("http://") + ip_text + path;
  const std::string sequence_text = std::to_string(sequence);
  const std::string nonce = random_hex(16);
  std::string request_hash, request_mac;
  if (!node_http_sign_request(session_key, method, path, group_id,
          sender_boot_id, receiver_boot_id, sequence, nonce, request_body,
          request_body_length, request_hash, request_mac, error)) {
    wipe(session_key);
    return false;
  }

  NodeHttpClientCapture capture;
  capture.body = response_body;
  capture.capacity = response_capacity;
  esp_http_client_config_t client_config = {};
  client_config.url = url.c_str();
  client_config.timeout_ms = 30000;
  client_config.disable_auto_redirect = true;
  client_config.buffer_size = 4096;
  client_config.buffer_size_tx = 1024;
  client_config.event_handler = node_http_client_event;
  client_config.user_data = &capture;
  client_config.keep_alive_enable = false;
  esp_http_client_handle_t client = esp_http_client_init(&client_config);
  if (!client) {
    wipe(session_key);
    error = "node HTTP client allocation failed";
    return false;
  }

  esp_http_client_method_t http_method = HTTP_METHOD_GET;
  bool method_valid = true;
  if (method == "GET") http_method = HTTP_METHOD_GET;
  else if (method == "POST") http_method = HTTP_METHOD_POST;
  else if (method == "PUT") http_method = HTTP_METHOD_PUT;
  else if (method == "PATCH") http_method = HTTP_METHOD_PATCH;
  else if (method == "DELETE") http_method = HTTP_METHOD_DELETE;
  else method_valid = false;
  esp_err_t result = method_valid
      ? esp_http_client_set_method(client, http_method) : ESP_ERR_INVALID_ARG;
  const std::string node_id_text = std::to_string(g_config->nodeId);
  const auto set_header = [&](const char* key, const std::string& value) {
    return esp_http_client_set_header(client, key, value.c_str()) == ESP_OK;
  };
  if (result == ESP_OK && request_body_length)
    result = esp_http_client_set_post_field(client,
        reinterpret_cast<const char*>(request_body), request_body_length);
  bool headers_ok = result == ESP_OK &&
      set_header("X-Node-Id", node_id_text) &&
      set_header("X-Boot-Id", sender_boot_id) &&
      set_header("X-Node-Seq", sequence_text) &&
      set_header("X-Request-Nonce", nonce) &&
      set_header("X-Body-SHA256", request_hash) &&
      set_header("X-Node-MAC", request_mac);
  if (headers_ok && request_body_length)
    headers_ok = set_header("Content-Type", "application/json");
  if (headers_ok && !if_candidate_hash.empty())
    headers_ok = set_header("X-Current-Candidate-SHA256", if_candidate_hash);
  if (!headers_ok) result = ESP_FAIL;
  if (result == ESP_OK) result = esp_http_client_perform(client);
  if (result == ESP_OK) response_status =
      static_cast<uint16_t>(esp_http_client_get_status_code(client));
  esp_http_client_cleanup(client);
  if (result != ESP_OK) {
    wipe(session_key);
    response_body_length = 0;
    if (response_body && capture.length) std::memset(response_body, 0, capture.length);
    error = capture.overflow ? "node HTTP response exceeds buffer"
                             : std::string("node HTTP exchange failed: ") + esp_err_to_name(result);
    return false;
  }

  response_body_length = capture.length;
  response_candidate_hash = capture.candidate_hash;
  if (response_status < 100 || response_status > 599 ||
      (!capture.candidate_hash.empty() && !lower_hex(capture.candidate_hash, 64)) ||
      capture.sequence != sequence_text || capture.nonce != nonce ||
      !node_http_verify_response(
          session_key, response_status, path, sequence, nonce, capture.body_hash,
          capture.mac, response_body, response_body_length, error)) {
    wipe(session_key);
    if (response_body && response_body_length)
      std::memset(response_body, 0, response_body_length);
    response_body_length = 0;
    response_candidate_hash.clear();
    if (error.empty()) error = "node HTTP response authentication failed";
    return false;
  }
  wipe(session_key);
  error.clear();
  return true;
}

void sync_runtime_broadcast_blackout(uint32_t revision, const char* reason) {
  if (!g_started || !g_config || g_config->role != Role::Master || !g_mutex ||
      !reason || (std::strcmp(reason, "user") != 0 &&
                  std::strcmp(reason, "fault") != 0) ||
      xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  if (!g_blackout_broadcast.pending || revision >= g_blackout_broadcast.revision) {
    g_blackout_broadcast.pending = true;
    g_blackout_broadcast.revision = revision;
    g_blackout_broadcast.reason = reason;
    g_blackout_broadcast.next_send_us = 0;
    g_blackout_broadcast.attempts = 0;
  }
  xSemaphoreGive(g_mutex);
}

void sync_runtime_peer_snapshot(std::vector<SyncPeerSnapshot>& peers) {
  peers.clear();
  if (!g_mutex || !g_started || xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return;
  const int64_t now = esp_timer_get_time();
  for (const auto& peer : g_peers) {
    if (!peer.used) continue;
    SyncPeerSnapshot snapshot;
    snapshot.node_id = peer.node_id;
    snapshot.hardware_id = peer.hardware_id;
    snapshot.label = peer.label;
    snapshot.health = peer.remote_health;
    snapshot.fault_code = peer.remote_fault_code;
    snapshot.active_hash = peer.remote_active_hash;
    snapshot.candidate_hash = peer.remote_candidate_hash;
    snapshot.revision = peer.remote_revision;
    snapshot.topology_revision = peer.remote_topology_revision;
    snapshot.topology_hash = peer.remote_topology_hash;
    snapshot.authenticated = peer.active;
    snapshot.last_seen_ago_ms = peer.last_seen_us > 0
        ? std::max<int64_t>(0, (now - peer.last_seen_us) / 1000)
        : -1;
    snapshot.online = peer.active && now - peer.last_seen_us <= kPeerTimeoutUs;
    snapshot.enabled = peer.enabled;
    if (g_config->role == Role::Master) {
      snapshot.clock_synced = peer.remote_clock_synced &&
          peer.remote_clock_status_us > 0 &&
          now - peer.remote_clock_status_us < 5000000;
      snapshot.clock_offset_us = peer.remote_clock_offset_us;
      snapshot.clock_rtt_us = peer.remote_clock_rtt_us;
      snapshot.clock_sample_age_ms = peer.remote_clock_sample_age_ms >= 0 &&
              peer.remote_clock_status_us > 0
          ? peer.remote_clock_sample_age_ms +
                std::max<int64_t>(0, (now - peer.remote_clock_status_us) / 1000)
          : -1;
    } else {
      snapshot.clock_synced = peer.clock_synced &&
          now - peer.last_valid_clock_us < 5000000;
      snapshot.clock_offset_us = peer.clock_offset_us;
      snapshot.clock_rtt_us = peer.clock_rtt_us;
      snapshot.clock_sample_age_ms = peer.last_valid_clock_us > 0
          ? std::max<int64_t>(0, (now - peer.last_valid_clock_us) / 1000)
          : -1;
    }
    peers.push_back(std::move(snapshot));
  }
  xSemaphoreGive(g_mutex);
}
}  // namespace ledpanel
