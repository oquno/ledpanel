#include "web_api.hpp"
#include "config_store.hpp"
#include "content_store.hpp"
#include "hub75_hal.hpp"
#include "lpk_decoder.hpp"
#include "runtime_package.hpp"
#include "render_runtime.hpp"
#include "rig_store.hpp"
#include "node_http_auth.hpp"
#include "sync_runtime.hpp"
#include "topology.hpp"
#include "panel_keys.hpp"
#include <nvs.h>
#include <driver/i2c_master.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_psram.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" {
extern const uint8_t _binary_lp_login_html_txt_start[];
extern const uint8_t _binary_lp_login_html_txt_end[];
extern const uint8_t _binary_lp_login_module_txt_start[];
extern const uint8_t _binary_lp_login_module_txt_end[];
extern const uint8_t _binary_lp_portal_style_txt_start[];
extern const uint8_t _binary_lp_portal_style_txt_end[];
extern const uint8_t _binary_lp_editor_html_txt_start[];
extern const uint8_t _binary_lp_editor_html_txt_end[];
extern const uint8_t _binary_lp_editor_app_txt_start[];
extern const uint8_t _binary_lp_editor_app_txt_end[];
extern const uint8_t _binary_lp_editor_style_txt_start[];
extern const uint8_t _binary_lp_editor_style_txt_end[];
extern const uint8_t _binary_lp_editor_animation_txt_start[];
extern const uint8_t _binary_lp_editor_animation_txt_end[];
extern const uint8_t _binary_lp_editor_deployment_txt_start[];
extern const uint8_t _binary_lp_editor_deployment_txt_end[];
extern const uint8_t _binary_lp_editor_media_txt_start[];
extern const uint8_t _binary_lp_editor_media_txt_end[];
extern const uint8_t _binary_lp_formats_index_txt_start[];
extern const uint8_t _binary_lp_formats_index_txt_end[];
extern const uint8_t _binary_lp_formats_browser_txt_start[];
extern const uint8_t _binary_lp_formats_browser_txt_end[];
extern const uint8_t _binary_lp_formats_crypto_txt_start[];
extern const uint8_t _binary_lp_formats_crypto_txt_end[];
extern const uint8_t _binary_lp_formats_zip_txt_start[];
extern const uint8_t _binary_lp_formats_zip_txt_end[];
extern const uint8_t _binary_lp_renderer_index_txt_start[];
extern const uint8_t _binary_lp_renderer_index_txt_end[];
}

namespace ledpanel {
namespace {
constexpr const char* kTag = "web-api";
constexpr int64_t kSessionLifetimeUs = 12LL * 60 * 60 * 1000000;
NodeConfig* g_config = nullptr;
PlaybackState* g_state = nullptr;
uint64_t* g_boot_id = nullptr;
bool g_sync_protocol_ok = false;
bool g_sync_runtime_ok = false;
std::string g_sync_runtime_error;
bool g_rig_store_ok = false;
bool g_node_http_auth_ok = false;
bool g_runtime_package_ok = false;
bool g_topology_ok = false;
std::string g_session_hash;
std::string g_csrf;
int64_t g_session_expiry = 0;
uint8_t g_login_failures = 0;
int64_t g_login_block_until = 0;
std::atomic<uint32_t> g_revision{0};
std::atomic<bool> g_restore_pending{true};
std::atomic<bool> g_keys_available{false};
std::atomic<bool> g_controls_self_test{false};
std::atomic<uint8_t> g_keys_raw{14};
// 0=normal, 1=maintenance, 2=package upload in progress.
std::atomic<uint8_t> g_maintenance_state{0};
std::atomic<bool> g_topology_dirty{true};
std::atomic<bool> g_topology_sync_busy{false};
std::vector<uint16_t> g_maintenance_members;
TopologySnapshot g_topology_snapshot;
bool g_topology_snapshot_valid = false;

struct AtomicFlagReset {
  explicit AtomicFlagReset(std::atomic<bool>& value) : flag(value) {}
  ~AtomicFlagReset() { flag.store(false, std::memory_order_release); }
  std::atomic<bool>& flag;
};

std::string bytes_hex(const uint8_t* data, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    result[2 * i] = kHex[data[i] >> 4];
    result[2 * i + 1] = kHex[data[i] & 15];
  }
  return result;
}

std::string sha256_hex(const std::string& input) {
  uint8_t digest[32];
  mbedtls_sha256(reinterpret_cast<const uint8_t*>(input.data()), input.size(), digest, 0);
  const std::string result = bytes_hex(digest, sizeof(digest));
  std::memset(digest, 0, sizeof(digest));
  return result;
}

bool constant_equal(const std::string& a, const std::string& b) {
  size_t mismatch = a.size() ^ b.size();
  const size_t length = std::max(a.size(), b.size());
  for (size_t i = 0; i < length; ++i) {
    const uint8_t x = i < a.size() ? static_cast<uint8_t>(a[i]) : 0;
    const uint8_t y = i < b.size() ? static_cast<uint8_t>(b[i]) : 0;
    mismatch |= x ^ y;
  }
  return mismatch == 0;
}

bool header(httpd_req_t* req, const char* name, std::string& value) {
  const size_t length = httpd_req_get_hdr_value_len(req, name);
  if (!length || length > 256) return false;
  std::vector<char> buffer(length + 1, 0);
  if (httpd_req_get_hdr_value_str(req, name, buffer.data(), buffer.size()) != ESP_OK)
    return false;
  value.assign(buffer.data(), length);
  return true;
}

void json_response(httpd_req_t* req, const char* status, const char* json) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

void error_response(httpd_req_t* req, const char* status, const char* code) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "error", code);
  char* json = cJSON_PrintUnformatted(root);
  if (json) json_response(req, status, json);
  cJSON_free(json);
  cJSON_Delete(root);
}

void secure_clear(std::string& value) {
  volatile char* cursor = value.empty() ? nullptr : value.data();
  for (size_t i = 0; i < value.size(); ++i) cursor[i] = 0;
  value.clear();
}

esp_err_t send_signed_node_response(httpd_req_t* req, uint16_t status_code,
    const char* status_text, const char* content_type, const std::string& path,
    uint32_t sequence, const std::string& nonce, std::string& session_key,
    const uint8_t* body, size_t body_length, const char* package_hash = nullptr) {
  std::string response_hash, response_mac, error;
  const bool signed_response = node_http_sign_response(session_key, status_code,
      path, sequence, nonce, body, body_length, response_hash, response_mac, error);
  secure_clear(session_key);
  if (!signed_response) {
    ESP_LOGE(kTag, "node HTTP response signing failed: %s", error.c_str());
    return error_response(req, "500 Internal Server Error", "NODE_RESPONSE_SIGN_FAILED"), ESP_OK;
  }
  httpd_resp_set_status(req, status_text);
  httpd_resp_set_type(req, content_type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const std::string seq = std::to_string(sequence);
  httpd_resp_set_hdr(req, "X-Node-Seq", seq.c_str());
  httpd_resp_set_hdr(req, "X-Request-Nonce", nonce.c_str());
  httpd_resp_set_hdr(req, "X-Body-SHA256", response_hash.c_str());
  httpd_resp_set_hdr(req, "X-Node-MAC", response_mac.c_str());
  if (package_hash) httpd_resp_set_hdr(req, "X-Content-SHA256", package_hash);
  static constexpr char kEmptyBody[] = "";
  return httpd_resp_send(req, body_length
      ? reinterpret_cast<const char*>(body) : kEmptyBody, body_length);
}

esp_err_t send_signed_node_error(httpd_req_t* req, uint16_t status_code,
    const char* status_text, const std::string& path, uint32_t sequence,
    const std::string& nonce, std::string& session_key, const char* code) {
  cJSON* root = cJSON_CreateObject();
  if (!root) {
    secure_clear(session_key);
    return error_response(req, "503 Service Unavailable", "NO_MEMORY"), ESP_OK;
  }
  cJSON_AddStringToObject(root, "error", code);
  char* json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) {
    secure_clear(session_key);
    return error_response(req, "503 Service Unavailable", "NO_MEMORY"), ESP_OK;
  }
  const size_t length = std::strlen(json);
  const esp_err_t result = send_signed_node_response(req, status_code,
      status_text, "application/json; charset=utf-8", path, sequence, nonce,
      session_key, reinterpret_cast<const uint8_t*>(json), length);
  cJSON_free(json);
  return result;
}

bool valid_sha256(const std::string& value);

bool send_json(httpd_req_t* req, cJSON* root) {
  char* json = cJSON_PrintUnformatted(root);
  if (!json) return false;
  json_response(req, "200 OK", json);
  cJSON_free(json);
  return true;
}

bool read_body(httpd_req_t* req, std::string& body, size_t limit) {
  if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > limit) return false;
  body.resize(req->content_len);
  size_t received = 0;
  while (received < body.size()) {
    const int n = httpd_req_recv(req, body.data() + received, body.size() - received);
    if (n <= 0) return false;
    received += n;
  }
  return true;
}

cJSON* parse_body(httpd_req_t* req, std::string& storage) {
  std::string content_type;
  if (!header(req, "Content-Type", content_type) ||
      content_type.compare(0, 16, "application/json") != 0 ||
      !read_body(req, storage, 2048))
    return nullptr;
  return cJSON_ParseWithLength(storage.data(), storage.size());
}

std::string boot_id_hex() {
  char text[17] = {};
  std::snprintf(text, sizeof(text), "%016llx",
      static_cast<unsigned long long>(g_boot_id ? *g_boot_id : 0));
  return text;
}

cJSON* parse_body_with_limit(httpd_req_t* req, std::string& storage, size_t limit) {
  std::string content_type;
  if (!header(req, "Content-Type", content_type) ||
      content_type.compare(0, 16, "application/json") != 0 ||
      !read_body(req, storage, limit))
    return nullptr;
  return cJSON_ParseWithLength(storage.data(), storage.size());
}

bool exact_object_keys(const cJSON* object,
                       std::initializer_list<const char*> expected) {
  if (!cJSON_IsObject(object) ||
      cJSON_GetArraySize(object) != static_cast<int>(expected.size())) return false;
  for (const char* key : expected)
    if (!cJSON_GetObjectItemCaseSensitive(object, key)) return false;
  return true;
}

bool decimal_header(const std::string& value, uint32_t maximum,
                    uint32_t& output) {
  if (value.empty() || value.size() > 10 ||
      (value.size() > 1 && value[0] == '0')) return false;
  uint32_t parsed = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(c - '0');
    if (parsed > (maximum - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  output = parsed;
  return true;
}

bool json_uint32(const cJSON* value, uint32_t& result) {
  if (!cJSON_IsNumber(value) || value->valuedouble < 0 ||
      value->valuedouble > UINT32_MAX ||
      std::floor(value->valuedouble) != value->valuedouble) return false;
  result = static_cast<uint32_t>(value->valuedouble);
  return true;
}

const char* json_string(const cJSON* object, const char* key) {
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(value) ? value->valuestring : nullptr;
}

bool valid_request_uuid(const cJSON* value) {
  if (!cJSON_IsString(value) || !value->valuestring ||
      std::strlen(value->valuestring) != 36) return false;
  for (size_t i = 0; i < 36; ++i) {
    const char c = value->valuestring[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

struct AuthorizedNodeJsonRequest {
  uint16_t node_id = 0;
  uint32_t sequence = 0;
  std::string nonce;
  std::string session_key;
  std::string body;
};

bool ipv4_peer_address(const sockaddr* source, socklen_t length,
                       uint32_t& address) {
  address = 0;
  if (!source || length < sizeof(sockaddr)) return false;
  if (source->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
    address = reinterpret_cast<const sockaddr_in*>(source)->sin_addr.s_addr;
    return true;
  }
#if CONFIG_LWIP_IPV6
  if (source->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(source);
    // ESP-IDF's dual-stack HTTP listener reports IPv4 clients as ::ffff:a.b.c.d.
    // Keep authentication tied to that same IPv4 address used by UDP sessions.
    const uint8_t* bytes = ipv6->sin6_addr.s6_addr;
    for (size_t i = 0; i < 10; ++i) if (bytes[i] != 0) return false;
    if (bytes[10] != 0xff || bytes[11] != 0xff) return false;
    std::memcpy(&address, bytes + 12, sizeof(address));
    return true;
  }
#endif
  return false;
}

bool node_http_ipv4_source(httpd_req_t* req, uint32_t& address) {
  sockaddr_storage source{};
  socklen_t length = sizeof(source);
  const int socket = httpd_req_to_sockfd(req);
  return socket >= 0 &&
      getpeername(socket, reinterpret_cast<sockaddr*>(&source), &length) == 0 &&
      ipv4_peer_address(reinterpret_cast<const sockaddr*>(&source), length, address);
}

bool ipv4_peer_address_self_test() {
  sockaddr_in ipv4{};
  ipv4.sin_family = AF_INET;
  ipv4.sin_addr.s_addr = htonl(0xc0a80403);
  uint32_t address = 0;
  if (!ipv4_peer_address(reinterpret_cast<sockaddr*>(&ipv4), sizeof(ipv4), address) ||
      address != ipv4.sin_addr.s_addr ||
      ipv4_peer_address(reinterpret_cast<sockaddr*>(&ipv4), sizeof(ipv4) - 1, address))
    return false;
#if CONFIG_LWIP_IPV6
  sockaddr_in6 mapped{};
  mapped.sin6_family = AF_INET6;
  mapped.sin6_addr.s6_addr[10] = mapped.sin6_addr.s6_addr[11] = 0xff;
  std::memcpy(mapped.sin6_addr.s6_addr + 12, &ipv4.sin_addr.s_addr, sizeof(address));
  if (!ipv4_peer_address(reinterpret_cast<sockaddr*>(&mapped), sizeof(mapped), address) ||
      address != ipv4.sin_addr.s_addr ||
      ipv4_peer_address(reinterpret_cast<sockaddr*>(&mapped), sizeof(mapped) - 1, address))
    return false;
  mapped.sin6_addr.s6_addr[0] = 0x20;
  if (ipv4_peer_address(reinterpret_cast<sockaddr*>(&mapped), sizeof(mapped), address))
    return false;
#endif
  return true;
}

bool authorize_node_json_request(httpd_req_t* req, const char* method,
                                const char* path, size_t body_limit,
                                AuthorizedNodeJsonRequest& request,
                                std::string& error) {
  request = AuthorizedNodeJsonRequest{};
  if (!g_node_http_auth_ok || !req || req->method != HTTP_POST ||
      req->content_len <= 0 || static_cast<size_t>(req->content_len) > body_limit ||
      std::strcmp(req->uri, path) != 0) {
    error = "NODE_REQUEST_INVALID";
    return false;
  }
  std::string content_type, node_id_text, boot_id, sequence_text;
  std::string body_hash, request_mac;
  if (!header(req, "Content-Type", content_type) ||
      content_type.compare(0, 16, "application/json") != 0 ||
      !read_body(req, request.body, body_limit) ||
      !header(req, "X-Node-Id", node_id_text) ||
      !header(req, "X-Boot-Id", boot_id) ||
      !header(req, "X-Node-Seq", sequence_text) ||
      !header(req, "X-Request-Nonce", request.nonce) ||
      !header(req, "X-Body-SHA256", body_hash) ||
      !header(req, "X-Node-MAC", request_mac)) {
    error = "NODE_AUTH_REQUIRED";
    return false;
  }
  uint32_t node_id = 0;
  if (!decimal_header(node_id_text, UINT16_MAX, node_id) || node_id == 0 ||
      !decimal_header(sequence_text, UINT32_MAX, request.sequence)) {
    error = "NODE_AUTH_REQUIRED";
    return false;
  }
  uint32_t source_address = 0;
  if (!node_http_ipv4_source(req, source_address)) {
    error = "NODE_AUTH_REQUIRED";
    return false;
  }
  if (!sync_runtime_node_http_authorize(static_cast<uint16_t>(node_id),
          source_address, boot_id, request.sequence, request.nonce,
          method, path, body_hash, request_mac,
          reinterpret_cast<const uint8_t*>(request.body.data()),
          request.body.size(), request.session_key, error)) return false;
  request.node_id = static_cast<uint16_t>(node_id);
  return true;
}

esp_err_t send_signed_node_object(httpd_req_t* req, uint16_t status_code,
    const char* status_text, const std::string& path,
    const AuthorizedNodeJsonRequest& request, std::string& session_key,
    cJSON* body) {
  char* json = body ? cJSON_PrintUnformatted(body) : nullptr;
  if (!json) {
    secure_clear(session_key);
    return error_response(req, "503 Service Unavailable", "NO_MEMORY"), ESP_OK;
  }
  const size_t length = std::strlen(json);
  const esp_err_t result = send_signed_node_response(req, status_code,
      status_text, "application/json; charset=utf-8", path,
      request.sequence, request.nonce, session_key,
      reinterpret_cast<const uint8_t*>(json), length);
  cJSON_free(json);
  return result;
}

bool node_http_post_json(uint16_t node_id, const std::string& path,
                         cJSON* request, cJSON*& response,
                         std::string& error) {
  response = nullptr;
  char* json = request ? cJSON_PrintUnformatted(request) : nullptr;
  if (!json) {
    error = "NODE_REQUEST_SERIALIZATION_FAILED";
    return false;
  }
  const size_t request_length = std::strlen(json);
  constexpr size_t kResponseCapacity = 2048;
  std::vector<uint8_t> response_bytes(kResponseCapacity, 0);
  size_t response_length = 0;
  uint16_t response_status = 0;
  std::string response_hash;
  const bool exchanged = sync_runtime_node_http_request(node_id, "POST", path,
      reinterpret_cast<const uint8_t*>(json), request_length, "",
      response_bytes.data(), response_bytes.size(), response_length,
      response_status, response_hash, error);
  std::memset(json, 0, request_length);
  cJSON_free(json);
  if (!exchanged) return false;
  response = response_length
      ? cJSON_ParseWithLength(reinterpret_cast<const char*>(response_bytes.data()),
                              response_length)
      : nullptr;
  std::memset(response_bytes.data(), 0, response_bytes.size());
  if (response_status < 200 || response_status >= 300 ||
      !cJSON_IsObject(response)) {
    const cJSON* error_field = response
        ? cJSON_GetObjectItemCaseSensitive(response, "error") : nullptr;
    error = cJSON_IsString(error_field) && error_field->valuestring
        ? error_field->valuestring : "NODE_RESPONSE_INVALID";
    cJSON_Delete(response);
    response = nullptr;
    return false;
  }
  error.clear();
  return true;
}

bool node_http_get_json(uint16_t node_id, const std::string& path,
                        cJSON*& response, std::string& error) {
  response = nullptr;
  constexpr size_t kResponseCapacity = 2048;
  std::vector<uint8_t> response_bytes(kResponseCapacity, 0);
  size_t response_length = 0;
  uint16_t response_status = 0;
  std::string package_hash;
  const bool exchanged = sync_runtime_node_http_request(node_id, "GET", path,
      nullptr, 0, "", response_bytes.data(), response_bytes.size(),
      response_length, response_status, package_hash, error);
  if (!exchanged) {
    std::memset(response_bytes.data(), 0, response_bytes.size());
    return false;
  }
  response = response_length
      ? cJSON_ParseWithLength(reinterpret_cast<const char*>(response_bytes.data()),
                              response_length)
      : nullptr;
  std::memset(response_bytes.data(), 0, response_bytes.size());
  if (response_status != 200 || !cJSON_IsObject(response)) {
    const cJSON* error_field = response
        ? cJSON_GetObjectItemCaseSensitive(response, "error") : nullptr;
    error = cJSON_IsString(error_field) && error_field->valuestring
        ? error_field->valuestring : "NODE_RESPONSE_INVALID";
    cJSON_Delete(response);
    response = nullptr;
    return false;
  }
  error.clear();
  return true;
}

struct NodeContentMetadata {
  uint32_t generation = 0;
  std::string active_hash;
  std::string candidate_hash;
  bool maintenance = false;
};

bool follower_candidate_ready(const NodeContentMetadata& remote,
                              const std::string& requested_hash,
                              const std::string& previous_hash,
                              bool revision_current) {
  const bool first_package = remote.active_hash.empty() && remote.generation == 0;
  return remote.maintenance && (revision_current || first_package) &&
      remote.candidate_hash == requested_hash &&
      (remote.active_hash == previous_hash || first_package);
}

bool follower_activation_self_test() {
  const std::string previous(64, 'a'), requested(64, 'b');
  NodeContentMetadata remote;
  remote.candidate_hash = requested;
  remote.maintenance = true;
  if (!follower_candidate_ready(remote, requested, previous, true) ||
      !follower_candidate_ready(remote, requested, previous, false)) return false;
  remote.generation = 1;
  if (follower_candidate_ready(remote, requested, previous, true)) return false;
  remote.active_hash = previous;
  if (!follower_candidate_ready(remote, requested, previous, true) ||
      follower_candidate_ready(remote, requested, previous, false)) return false;
  remote.active_hash = std::string(64, 'c');
  if (follower_candidate_ready(remote, requested, previous, true)) return false;
  remote.active_hash = previous;
  remote.candidate_hash.clear();
  if (follower_candidate_ready(remote, requested, previous, true)) return false;
  remote.candidate_hash = requested;
  remote.maintenance = false;
  return !follower_candidate_ready(remote, requested, previous, true);
}

bool get_remote_content_metadata(const RigFollowerIdentity& follower,
                                 NodeContentMetadata& metadata,
                                 std::string& error) {
  cJSON* response = nullptr;
  if (!node_http_get_json(follower.node_id, "/node/v2/health", response, error))
    return false;
  uint32_t remote_node_id = 0;
  const cJSON* ok = cJSON_GetObjectItemCaseSensitive(response, "ok");
  const cJSON* hardware_id = cJSON_GetObjectItemCaseSensitive(response, "hardwareId");
  const cJSON* role = cJSON_GetObjectItemCaseSensitive(response, "role");
  const cJSON* remote_maintenance =
      cJSON_GetObjectItemCaseSensitive(response, "maintenance");
  const cJSON* active = cJSON_GetObjectItemCaseSensitive(response, "activeHash");
  const cJSON* candidate = cJSON_GetObjectItemCaseSensitive(response, "candidateHash");
  const bool active_valid = cJSON_IsNull(active) ||
      (cJSON_IsString(active) && active->valuestring &&
       valid_sha256(active->valuestring));
  const bool candidate_valid = cJSON_IsNull(candidate) ||
      (cJSON_IsString(candidate) && candidate->valuestring &&
       valid_sha256(candidate->valuestring));
  const bool valid = cJSON_IsTrue(ok) &&
      json_uint32(cJSON_GetObjectItemCaseSensitive(response, "nodeId"), remote_node_id) &&
      remote_node_id == follower.node_id &&
      cJSON_IsString(hardware_id) && hardware_id->valuestring &&
      follower.hardware_id == hardware_id->valuestring &&
      cJSON_IsString(role) && role->valuestring &&
      std::strcmp(role->valuestring, "follower") == 0 &&
      json_uint32(cJSON_GetObjectItemCaseSensitive(response, "contentGeneration"),
                  metadata.generation) &&
      cJSON_IsBool(remote_maintenance) && active_valid && candidate_valid;
  if (valid) {
    metadata.maintenance = cJSON_IsTrue(remote_maintenance);
    metadata.active_hash = cJSON_IsString(active) ? active->valuestring : "";
    metadata.candidate_hash = cJSON_IsString(candidate) ? candidate->valuestring : "";
  }
  cJSON_Delete(response);
  if (!valid) error = "NODE_HEALTH_INVALID";
  else if (!metadata.maintenance) error = "MAINTENANCE_REQUIRED";
  else error.clear();
  return valid && metadata.maintenance;
}

bool set_remote_maintenance(uint16_t node_id, bool enabled,
                            std::string& error) {
  cJSON* request = cJSON_CreateObject();
  if (!request) {
    error = "OUT_OF_MEMORY";
    return false;
  }
  cJSON_AddBoolToObject(request, "enabled", enabled);
  cJSON* response = nullptr;
  const bool exchanged = node_http_post_json(node_id,
      "/node/v2/maintenance", request, response, error);
  cJSON_Delete(request);
  const cJSON* remote_enabled = response
      ? cJSON_GetObjectItemCaseSensitive(response, "maintenance") : nullptr;
  const bool confirmed = exchanged && cJSON_IsBool(remote_enabled) &&
      cJSON_IsTrue(remote_enabled) == enabled;
  cJSON_Delete(response);
  if (!confirmed && exchanged) error = "NODE_MAINTENANCE_UNCONFIRMED";
  return confirmed;
}

void remember_maintenance_member(uint16_t node_id) {
  if (std::find(g_maintenance_members.begin(), g_maintenance_members.end(),
                node_id) == g_maintenance_members.end())
    g_maintenance_members.push_back(node_id);
}

bool activate_remote_candidate(uint16_t node_id, const std::string& hash,
                               uint32_t generation, uint32_t state_revision,
                               std::string& error) {
  cJSON* request = cJSON_CreateObject();
  if (!request) {
    error = "OUT_OF_MEMORY";
    return false;
  }
  cJSON_AddStringToObject(request, "packageHash", hash.c_str());
  cJSON_AddNumberToObject(request, "generation", generation);
  cJSON_AddNumberToObject(request, "stateRevision", state_revision);
  cJSON* response = nullptr;
  const bool exchanged = node_http_post_json(node_id, "/node/v2/activate",
      request, response, error);
  cJSON_Delete(request);
  const cJSON* response_hash = response
      ? cJSON_GetObjectItemCaseSensitive(response, "packageHash") : nullptr;
  uint32_t response_generation = 0;
  uint32_t response_revision = 0;
  const bool confirmed = exchanged && cJSON_IsString(response_hash) &&
      response_hash->valuestring && hash == response_hash->valuestring &&
      json_uint32(cJSON_GetObjectItemCaseSensitive(response, "generation"),
                  response_generation) && response_generation == generation &&
      json_uint32(cJSON_GetObjectItemCaseSensitive(response, "stateRevision"),
                  response_revision) && response_revision == state_revision;
  cJSON_Delete(response);
  if (!confirmed && exchanged) error = "NODE_ACTIVATION_UNCONFIRMED";
  return confirmed;
}

bool build_current_topology(TopologySnapshot& result, std::string& error) {
  result = TopologySnapshot{};
  if (!g_config || g_config->role != Role::Master) {
    error = "MASTER_REQUIRED";
    return false;
  }
  RigSlot rig;
  if (!rig_load(rig, error) || !rig.valid) {
    if (error.empty()) error = "RIG_UNAVAILABLE";
    return false;
  }
  ContentState content;
  if (!content_get_state(content, error)) return false;
  uint8_t* package = static_cast<uint8_t*>(heap_caps_malloc(
      kMaxPackageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!package) {
    error = "PACKAGE_BUFFER_UNAVAILABLE";
    return false;
  }
  bool built = false;
  std::string last_error;
  const auto try_slot = [&](bool candidate) {
    size_t length = 0;
    std::string package_hash, layout_id, layout_json, slot_error;
    const bool read = candidate
        ? content_read_candidate(package, kMaxPackageBytes, length,
                                 package_hash, slot_error)
        : content_read_active(package, kMaxPackageBytes, length,
                              package_hash, slot_error);
    if (!read) {
      last_error = slot_error;
      return false;
    }
    const bool ok = runtime_package_get_default_layout(package, length,
            layout_id, slot_error) &&
        runtime_package_get_layout(package, length, layout_id,
                                   layout_json, slot_error) &&
        topology_build_master(*g_config, rig.json, layout_json, layout_id,
                              result, slot_error);
    std::memset(package, 0, length);
    if (!ok) last_error = slot_error;
    return ok;
  };
  const bool maintenance =
      g_maintenance_state.load(std::memory_order_relaxed) == 1;
  if (maintenance && content.has_candidate) {
    // A staged package is the topology candidate. Never report the old
    // active layout as synchronized when the candidate layout is invalid.
    built = try_slot(true);
  } else {
    if (content.has_active) built = try_slot(false);
    if (!built && content.has_candidate) built = try_slot(true);
  }
  heap_caps_free(package);
  if (!built) {
    error = last_error.empty() ? "RUNTIME_LAYOUT_UNAVAILABLE" : last_error;
    return false;
  }
  error.clear();
  return true;
}

bool build_candidate_topology(TopologySnapshot& result, std::string& error) {
  result = TopologySnapshot{};
  if (!g_config || g_config->role != Role::Master) {
    error = "MASTER_REQUIRED";
    return false;
  }
  RigSlot rig;
  if (!rig_load(rig, error) || !rig.valid) {
    if (error.empty()) error = "RIG_UNAVAILABLE";
    return false;
  }
  auto* package = static_cast<uint8_t*>(heap_caps_malloc(
      kMaxPackageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!package) {
    error = "PACKAGE_BUFFER_UNAVAILABLE";
    return false;
  }
  size_t length = 0;
  std::string package_hash;
  bool built = content_read_candidate(package, kMaxPackageBytes, length,
                                      package_hash, error);
  if (built) {
    std::string layout_id, layout_json;
    built = runtime_package_get_default_layout(package, length, layout_id,
                error) &&
        runtime_package_get_layout(package, length, layout_id, layout_json,
                                   error) &&
        topology_build_master(*g_config, rig.json, layout_json, layout_id,
                              result, error);
  }
  if (length) std::memset(package, 0, length);
  heap_caps_free(package);
  if (!built) {
    result = TopologySnapshot{};
    if (error.empty()) error = "CANDIDATE_TOPOLOGY_UNAVAILABLE";
    return false;
  }
  error.clear();
  return true;
}

bool apply_topology_snapshot(TopologySnapshot& snapshot, std::string& error) {
  if (!g_config || !g_state ||
      snapshot.hash.size() != 64 || snapshot.layout_id.empty()) {
    error = "TOPOLOGY_SNAPSHOT_INVALID";
    return false;
  }
  if (g_config->topologyHash == snapshot.hash &&
      g_state->topologyHash == snapshot.hash &&
      g_state->topologyRevision == snapshot.revision &&
      g_state->layoutId == snapshot.layout_id) {
    error.clear();
    return true;
  }
  if (g_state->revision == UINT32_MAX) {
    error = "STATE_REVISION_EXHAUSTED";
    return false;
  }
  ++g_state->revision;
  g_state->topologyRevision = snapshot.revision;
  g_state->topologyHash = snapshot.hash;
  g_state->layoutId = snapshot.layout_id;
  g_state->mode = "stopped";
  g_state->running = false;
  g_state->sceneId.clear();
  g_state->cueId.clear();
  g_state->positionTicks = 0;
  g_state->anchorUs = 0;
  g_state->effectiveAtUs = 0;
  g_state->blackout = true;
  g_config->topologyHash = snapshot.hash;
  if (!sync_runtime_update_state(*g_state)) {
    error = "SYNC_STATE_UPDATE_FAILED";
    return false;
  }
  error.clear();
  return true;
}

bool push_topology_to_follower(uint16_t node_id,
                               const TopologySnapshot& snapshot,
                               std::string& error) {
  cJSON* request = cJSON_CreateObject();
  cJSON* topology = cJSON_ParseWithLength(snapshot.json.data(), snapshot.json.size());
  if (!request || !topology) {
    cJSON_Delete(request);
    cJSON_Delete(topology);
    error = "NO_MEMORY";
    return false;
  }
  cJSON_AddItemToObject(request, "topology", topology);
  cJSON_AddStringToObject(request, "topologyHash", snapshot.hash.c_str());
  cJSON* response = nullptr;
  const bool exchanged = node_http_post_json(node_id, "/node/v2/topology",
                                              request, response, error);
  cJSON_Delete(request);
  const char* response_hash = response
      ? json_string(response, "topologyHash") : nullptr;
  uint32_t response_revision = 0;
  const bool confirmed = exchanged && response_hash &&
      snapshot.hash == response_hash &&
      json_uint32(cJSON_GetObjectItemCaseSensitive(response, "topologyRevision"),
                  response_revision) && response_revision == snapshot.revision;
  cJSON_Delete(response);
  if (!confirmed && exchanged) error = "NODE_TOPOLOGY_UNCONFIRMED";
  return confirmed;
}

void topology_sync_task(void*) {
  for (;;) {
    const auto safe_to_sync = [] {
      return g_config && g_state && g_topology_ok &&
          g_config->role == Role::Master && !g_state->running &&
          g_state->mode == "stopped" && g_state->blackout &&
          g_maintenance_state.load(std::memory_order_relaxed) != 2 &&
          !sync_runtime_transaction_active();
    };
    bool expected_busy = false;
    if (safe_to_sync() && g_topology_sync_busy.compare_exchange_strong(
            expected_busy, true, std::memory_order_acq_rel)) {
      AtomicFlagReset busy_reset(g_topology_sync_busy);
      if (safe_to_sync()) {
        bool topology_ready = g_topology_snapshot_valid &&
            !g_topology_dirty.load(std::memory_order_relaxed);
        if (g_topology_dirty.load(std::memory_order_relaxed) ||
            !g_topology_snapshot_valid) {
          TopologySnapshot next;
          std::string error;
          if (build_current_topology(next, error)) {
            g_topology_snapshot = std::move(next);
            g_topology_snapshot_valid = true;
            g_topology_dirty.store(false, std::memory_order_relaxed);
            topology_ready = true;
          } else {
            ESP_LOGD(kTag, "topology is not ready yet: %s", error.c_str());
          }
        }
        if (topology_ready && g_topology_snapshot_valid && safe_to_sync()) {
          std::string error;
          const bool maintenance =
              g_maintenance_state.load(std::memory_order_relaxed) == 1;
          if (!maintenance) {
            // Reconstruct the active package's local topology after reboot so
            // maintenance and layout queries use the durable Rig. Remote nodes
            // are updated only during an explicit fleet maintenance window.
            if (!apply_topology_snapshot(g_topology_snapshot, error))
              ESP_LOGW(kTag, "local topology apply deferred: %s", error.c_str());
          } else {
            std::vector<RigFollowerIdentity> followers;
            if (rig_display_followers(*g_config, g_topology_snapshot.layout_id,
                                      followers, error)) {
              std::vector<SyncPeerSnapshot> peers;
              sync_runtime_peer_snapshot(peers);
              bool all_ready = true;
              for (const auto& follower : followers) {
                const auto peer = std::find_if(peers.begin(), peers.end(),
                    [&follower](const SyncPeerSnapshot& item) {
                      return item.node_id == follower.node_id &&
                          item.hardware_id == follower.hardware_id && item.online &&
                          item.authenticated;
                    });
                if (peer == peers.end()) {
                  all_ready = false;
                  continue;
                }
                if (std::find(g_maintenance_members.begin(),
                              g_maintenance_members.end(), follower.node_id) ==
                    g_maintenance_members.end()) {
                  if (!set_remote_maintenance(follower.node_id, true, error)) {
                    all_ready = false;
                    ESP_LOGD(kTag, "maintenance enable for node %u deferred: %s",
                             follower.node_id, error.c_str());
                    continue;
                  }
                  remember_maintenance_member(follower.node_id);
                }
                if (peer->topology_revision == g_topology_snapshot.revision &&
                    peer->topology_hash == g_topology_snapshot.hash) continue;
                if (!push_topology_to_follower(follower.node_id,
                        g_topology_snapshot, error)) {
                  all_ready = false;
                  ESP_LOGD(kTag, "topology delivery to node %u deferred: %s",
                           follower.node_id, error.c_str());
                }
              }
              if (all_ready && safe_to_sync() &&
                  !apply_topology_snapshot(g_topology_snapshot, error))
                ESP_LOGW(kTag, "local topology apply deferred: %s", error.c_str());
            }
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

std::string new_operation_id() {
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
  char value[37] = {};
  std::snprintf(value, sizeof(value),
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
      bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  std::memset(bytes, 0, sizeof(bytes));
  return value;
}

bool required_follower_registered(const std::string& layout_id) {
  if (!g_config || g_config->role != Role::Master) return false;
  std::vector<RigFollowerIdentity> followers;
  std::string error;
  if (!rig_display_followers(*g_config, layout_id, followers, error)) return true;
  return !followers.empty();
}

bool ensure_active_renderer(const std::string& expected_hash,
                            std::string& error) {
  if (expected_hash.empty()) {
    error = "ACTIVE_PACKAGE_REQUIRED";
    return false;
  }
  if (render_runtime_is_loaded(expected_hash)) {
    error.clear();
    return true;
  }
  ContentState content;
  if (!content_get_state(content, error) || !content.has_active ||
      content.active_hash != expected_hash) {
    error = "ACTIVE_PACKAGE_NOT_FOUND";
    return false;
  }
  auto* package = static_cast<uint8_t*>(heap_caps_malloc(
      kMaxPackageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!package) {
    error = "PACKAGE_BUFFER_UNAVAILABLE";
    return false;
  }
  size_t length = 0;
  std::string actual_hash;
  const bool read = content_read_active(package, kMaxPackageBytes, length,
                                        actual_hash, error);
  const bool loaded = read && actual_hash == expected_hash &&
      render_runtime_load_owned(package, length, expected_hash, error);
  if (loaded) {
    package = nullptr;
  } else if (read && actual_hash != expected_hash) {
    error = "ACTIVE_PACKAGE_HASH_MISMATCH";
  }
  if (package) {
    if (length) std::memset(package, 0, length);
    heap_caps_free(package);
  }
  return loaded;
}

std::string deployment_scope(const std::string& layout_id) {
  if (!g_config || g_config->role != Role::Master) return "follower";
  std::vector<RigFollowerIdentity> followers;
  std::string error;
  if (!rig_display_followers(*g_config, layout_id, followers, error))
    return "local_only";
  if (followers.empty()) return "single_node";
  std::vector<SyncPeerSnapshot> peers;
  sync_runtime_peer_snapshot(peers);
  for (const auto& follower : followers) {
    const auto peer = std::find_if(peers.begin(), peers.end(),
        [&follower](const SyncPeerSnapshot& item) {
          return item.node_id == follower.node_id &&
              item.hardware_id == follower.hardware_id && item.enabled &&
              item.online && item.authenticated;
        });
    if (peer == peers.end()) return "local_only";
  }
  return "fleet";
}

bool origin_matches_host(httpd_req_t* req) {
  std::string host;
  std::string origin;
  return header(req, "Host", host) && header(req, "Origin", origin) &&
         constant_equal(origin, std::string("http://") + host);
}

bool token_cookie(httpd_req_t* req, std::string& token) {
  std::string cookies;
  if (!header(req, "Cookie", cookies)) return false;
  constexpr const char* prefix = "lpsid=";
  size_t start = 0;
  while (start < cookies.size()) {
    while (start < cookies.size() && (cookies[start] == ' ' || cookies[start] == ';')) ++start;
    const size_t end = cookies.find(';', start);
    const std::string part = cookies.substr(start, end == std::string::npos ? end : end - start);
    if (part.compare(0, 6, prefix) == 0) {
      token = part.substr(6);
      return token.size() == 64;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

bool authenticated(httpd_req_t* req, bool mutation) {
  if (esp_timer_get_time() >= g_session_expiry || g_session_hash.empty()) return false;
  std::string token;
  if (!token_cookie(req, token) || !constant_equal(sha256_hex(token), g_session_hash))
    return false;
  if (!mutation) return true;
  std::string csrf;
  return header(req, "X-CSRF-Token", csrf) && constant_equal(csrf, g_csrf) &&
         origin_matches_host(req);
}

bool master_only_mutation(httpd_req_t* req);

esp_err_t health(httpd_req_t* req) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "bootId", boot_id_hex().c_str());
  cJSON_AddNumberToObject(root, "apiMajor", 2);
  cJSON_AddBoolToObject(root, "syncProtocolCodecSelfTest", g_sync_protocol_ok);
  cJSON_AddBoolToObject(root, "syncRuntimeBlackoutSelfTest", g_sync_runtime_ok);
  cJSON_AddBoolToObject(root, "syncRuntimeTransactionSelfTest", g_sync_runtime_ok);
  if (!g_sync_runtime_error.empty())
    cJSON_AddStringToObject(root, "syncRuntimeSelfTestError", g_sync_runtime_error.c_str());
  cJSON_AddBoolToObject(root, "rigStoreValidationSelfTest", g_rig_store_ok);
  cJSON_AddBoolToObject(root, "nodeHttpAuthSelfTest", g_node_http_auth_ok);
  cJSON_AddBoolToObject(root, "runtimePackageLayoutSelfTest", g_runtime_package_ok);
  cJSON_AddBoolToObject(root, "topologyBuildVerifySelfTest", g_topology_ok);
  send_json(req, root);
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t provisioning_state(httpd_req_t* req) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "provisioned", g_config->provisioned);
  cJSON_AddStringToObject(root, "setup", g_config->provisioned ? "login" : "usb");
  if (g_config->provisioned)
    cJSON_AddStringToObject(root, "networkMode", g_config->networkMode.c_str());
  send_json(req, root);
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t login(httpd_req_t* req) {
  if (!origin_matches_host(req)) return error_response(req, "403 Forbidden", "ORIGIN_FORBIDDEN"), ESP_OK;
  if (!g_config->provisioned) return error_response(req, "503 Service Unavailable", "UNPROVISIONED"), ESP_OK;
  if (esp_timer_get_time() < g_login_block_until)
    return error_response(req, "429 Too Many Requests", "LOGIN_RATE_LIMIT"), ESP_OK;
  std::string body;
  cJSON* root = parse_body(req, body);
  cJSON* password = root ? cJSON_GetObjectItemCaseSensitive(root, "password") : nullptr;
  if (!cJSON_IsString(password) || std::strlen(password->valuestring) < 1 ||
      std::strlen(password->valuestring) > 128) {
    cJSON_Delete(root);
    return error_response(req, "400 Bad Request", "INVALID_REQUEST"), ESP_OK;
  }
  const std::string supplied(password->valuestring);
  std::memset(password->valuestring, 0, std::strlen(password->valuestring));
  cJSON_Delete(root);
  if (!config_verify_password(*g_config, supplied)) {
    ++g_login_failures;
    if (g_login_failures >= 5) {
      g_login_failures = 0;
      g_login_block_until = esp_timer_get_time() + 60LL * 1000000;
    }
    return error_response(req, "401 Unauthorized", "INVALID_CREDENTIALS"), ESP_OK;
  }
  g_login_failures = 0;
  uint8_t token[32];
  uint8_t csrf[16];
  esp_fill_random(token, sizeof(token));
  esp_fill_random(csrf, sizeof(csrf));
  const std::string clear_token = bytes_hex(token, sizeof(token));
  g_session_hash = sha256_hex(clear_token);
  g_csrf = bytes_hex(csrf, sizeof(csrf));
  g_session_expiry = esp_timer_get_time() + kSessionLifetimeUs;
  std::memset(token, 0, sizeof(token));
  std::memset(csrf, 0, sizeof(csrf));
  cJSON* response = cJSON_CreateObject();
  cJSON_AddStringToObject(response, "csrfToken", g_csrf.c_str());
  cJSON_AddNumberToObject(response, "expiresInSec", 43200);
  char cookie[160];
  std::snprintf(cookie, sizeof(cookie), "lpsid=%s; Max-Age=43200; HttpOnly; SameSite=Strict; Path=/",
                clear_token.c_str());
  httpd_resp_set_hdr(req, "Set-Cookie", cookie);
  send_json(req, response);
  cJSON_Delete(response);
  return ESP_OK;
}

esp_err_t logout(httpd_req_t* req) {
  if (!authenticated(req, true)) return error_response(req, "403 Forbidden", "AUTH_OR_CSRF_REQUIRED"), ESP_OK;
  std::string body;
  cJSON* request = parse_body(req, body);
  if (!request) return error_response(req, "400 Bad Request", "INVALID_REQUEST"), ESP_OK;
  cJSON_Delete(request);
  g_session_hash.clear();
  g_csrf.clear();
  g_session_expiry = 0;
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_set_hdr(req, "Set-Cookie", "lpsid=; Max-Age=0; HttpOnly; SameSite=Strict; Path=/");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

esp_err_t csrf(httpd_req_t* req) {
  if (!authenticated(req, false)) return error_response(req, "401 Unauthorized", "UNAUTHENTICATED"), ESP_OK;
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "csrfToken", g_csrf.c_str());
  cJSON_AddNumberToObject(root, "expiresInSec", std::max<int64_t>(0, (g_session_expiry - esp_timer_get_time()) / 1000000));
  send_json(req, root);
  cJSON_Delete(root);
  return ESP_OK;
}

cJSON* make_status() {
  PlaybackState mirrored_state;
  const PlaybackState* current_state = g_state;
  if (g_config->role == Role::Follower &&
      sync_runtime_state_snapshot(mirrored_state)) current_state = &mirrored_state;
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "role",
                          g_config->role == Role::Master ? "master" : "follower");
  cJSON_AddNumberToObject(root, "masterNodeId", g_config->masterNodeId);
  cJSON_AddStringToObject(root, "bootId", boot_id_hex().c_str());
  cJSON_AddNumberToObject(root, "revision",
                          g_revision.load(std::memory_order_relaxed));
  uint32_t rig_revision = 0;
  cJSON_AddStringToObject(root, "panelOutput",
      hub75_nightlight_active() ? "dim_nightlight" :
      hub75_diagnostic_active() ? "diagnostic_rgb_bars" :
      hub75_diagnostic_supported() && current_state->running && !current_state->blackout
          ? "content_playback" : "disabled_black");
  cJSON_AddStringToObject(root, "panelOutputReason",
      hub75_nightlight_active() ? "DIM_NIGHTLIGHT_8_OF_255" :
      hub75_diagnostic_active() ? "LOW_BRIGHTNESS_PANEL_DIAGNOSTIC" :
      current_state->blackout ? "BLACKOUT_LATCHED" :
      current_state->running ? "CONTENT_PLAYBACK" : "STOPPED_WAITING_FOR_GO");
  cJSON* state = cJSON_AddObjectToObject(root, "state");
  cJSON_AddBoolToObject(state, "running", current_state->running);
  cJSON_AddNumberToObject(state, "revision", current_state->revision);
  cJSON_AddBoolToObject(state, "blackout", current_state->blackout);
  cJSON_AddStringToObject(state, "mode", current_state->running ? current_state->mode.c_str() : "stopped");
  cJSON_AddNumberToObject(state, "userBrightness", current_state->userBrightness);
  cJSON_AddNumberToObject(state, "brightnessCap", g_config->brightnessCap);
  cJSON_AddStringToObject(state, "sceneId", current_state->sceneId.c_str());
  cJSON_AddStringToObject(state, "cueId", current_state->cueId.c_str());
  cJSON_AddStringToObject(state, "packageHash", current_state->packageHash.c_str());
  ContentState content;
  std::string content_error;
  const bool content_ok = content_get_state(content, content_error);
  if (content_ok) {
    cJSON_AddNumberToObject(state, "contentGeneration", content.generation);
    if (content.has_active) cJSON_AddStringToObject(state, "activePackageHash", content.active_hash.c_str());
    else cJSON_AddNullToObject(state, "activePackageHash");
    if (content.has_candidate) cJSON_AddStringToObject(state, "candidatePackageHash", content.candidate_hash.c_str());
    else cJSON_AddNullToObject(state, "candidatePackageHash");
    cJSON_AddNumberToObject(state, "activePackageBytes", content.active_length);
    cJSON_AddNumberToObject(state, "candidatePackageBytes", content.candidate_length);
  } else {
    cJSON_AddStringToObject(state, "contentFault", "CONTENT_METADATA_INVALID");
  }
  cJSON_AddBoolToObject(root, "maintenance", g_maintenance_state.load() != 0);
  if (g_config->role == Role::Master)
    cJSON_AddStringToObject(root, "maintenanceScope",
        deployment_scope(current_state->layoutId).c_str());
  else
    cJSON_AddStringToObject(root, "maintenanceScope", "follower");
  cJSON_AddBoolToObject(root, "faultLatch", false);
  cJSON_AddNullToObject(root, "pendingOperationId");
  cJSON* nodes = cJSON_AddArrayToObject(root, "nodes");
  std::vector<SyncPeerSnapshot> live_peers;
  sync_runtime_peer_snapshot(live_peers);
  const auto append_node = [&](uint16_t node_id, const char* role,
                               const char* hardware_id, const char* label,
                               bool enabled, bool local,
                               const SyncPeerSnapshot* live_peer) {
    cJSON* node = cJSON_CreateObject();
    cJSON_AddNumberToObject(node, "nodeId", node_id);
    cJSON_AddStringToObject(node, "role", role);
    const bool online = local || (live_peer && live_peer->online);
    cJSON_AddBoolToObject(node, "online", online);
    cJSON_AddBoolToObject(node, "enabled", enabled);
    const bool state_matches = live_peer &&
        live_peer->revision == current_state->revision &&
        live_peer->topology_revision == current_state->topologyRevision &&
        live_peer->topology_hash == current_state->topologyHash &&
        live_peer->active_hash == current_state->packageHash;
    const bool local_content_matches = content_ok && content.has_active &&
        content.active_hash == current_state->packageHash;
    const char* local_fault = !hub75_diagnostic_supported() ? "PANEL_DMA_UNAVAILABLE" :
        !content_ok ? "CONTENT_METADATA_INVALID" :
        current_state->running && !local_content_matches ? "ACTIVE_CONTENT_MISMATCH" :
        current_state->running && !render_runtime_is_loaded(current_state->packageHash)
            ? "PACKAGE_NOT_READY" : nullptr;
    const bool local_panel_ready = local_fault == nullptr;
    const bool ready = local ? local_panel_ready :
        enabled && online && live_peer && live_peer->authenticated &&
        live_peer->clock_synced && state_matches && live_peer->health == "ready";
    cJSON_AddBoolToObject(node, "ready", ready);
    cJSON_AddStringToObject(node, "label", label);
    cJSON_AddStringToObject(node, "hardwareId", hardware_id);
    if (local) cJSON_AddStringToObject(node, "firmwareVersion", "2.0.0-dev");
    else cJSON_AddNullToObject(node, "firmwareVersion");
    if (local && !g_config->slotId.empty())
      cJSON_AddStringToObject(node, "slotId", g_config->slotId.c_str());
    else
      cJSON_AddNullToObject(node, "slotId");
    if (local && content.has_active)
      cJSON_AddStringToObject(node, "activeHash", content.active_hash.c_str());
    else if (live_peer && !live_peer->active_hash.empty())
      cJSON_AddStringToObject(node, "activeHash", live_peer->active_hash.c_str());
    else
      cJSON_AddNullToObject(node, "activeHash");
    if (local && content.has_candidate)
      cJSON_AddStringToObject(node, "candidateHash", content.candidate_hash.c_str());
    else if (live_peer && !live_peer->candidate_hash.empty())
      cJSON_AddStringToObject(node, "candidateHash", live_peer->candidate_hash.c_str());
    else
      cJSON_AddNullToObject(node, "candidateHash");
    const char* sync_state = local ?
        (ready ? (current_state->blackout ? "local_blackout" : "ready") : "fault") :
        !enabled ? "standby" :
        !live_peer || !live_peer->authenticated ? "offline" :
        !online ? "peer_lost" : ready ? "ready" :
        live_peer->health == "fault" ? "fault" : "authenticated_unsynced";
    const char* fault_code = local ?
        (ready ? (current_state->blackout ? "BLACKOUT_LATCHED" : nullptr) : local_fault) :
        !enabled ? "STANDBY" :
        !live_peer || !live_peer->authenticated ? "PEER_OFFLINE" :
        !online ? "PEER_LOST" :
        ready ? nullptr :
        live_peer->health == "fault" && !live_peer->fault_code.empty()
            ? live_peer->fault_code.c_str() :
        !live_peer->clock_synced ? "CLOCK_UNSYNCED" :
        !state_matches ? "PEER_STATE_MISMATCH" :
        "PEER_UNSYNCED";
    cJSON_AddStringToObject(node, "syncState", sync_state);
    if (fault_code) cJSON_AddStringToObject(node, "faultCode", fault_code);
    else cJSON_AddNullToObject(node, "faultCode");
    if (local && g_config->role == Role::Master) {
      cJSON_AddBoolToObject(node, "clockSynced", true);
      cJSON_AddNumberToObject(node, "clockOffsetUs", 0);
      cJSON_AddNumberToObject(node, "clockRttUs", 0);
      cJSON_AddNumberToObject(node, "clockSampleAgeMs", 0);
    } else if (live_peer) {
      cJSON_AddBoolToObject(node, "clockSynced", live_peer->clock_synced);
      cJSON_AddNumberToObject(node, "clockOffsetUs", live_peer->clock_offset_us);
      cJSON_AddNumberToObject(node, "clockRttUs", live_peer->clock_rtt_us);
      if (live_peer->clock_sample_age_ms >= 0)
        cJSON_AddNumberToObject(node, "clockSampleAgeMs", live_peer->clock_sample_age_ms);
      else
        cJSON_AddNullToObject(node, "clockSampleAgeMs");
    } else {
      cJSON_AddBoolToObject(node, "clockSynced", false);
      cJSON_AddNullToObject(node, "clockOffsetUs");
      cJSON_AddNullToObject(node, "clockRttUs");
      cJSON_AddNullToObject(node, "clockSampleAgeMs");
    }
    if (live_peer && live_peer->last_seen_ago_ms >= 0)
      cJSON_AddNumberToObject(node, "lastSeenAgoMs", live_peer->last_seen_ago_ms);
    else
      cJSON_AddNullToObject(node, "lastSeenAgoMs");
    cJSON_AddNumberToObject(node, "revision", live_peer ? live_peer->revision : 0);
    if (live_peer)
      cJSON_AddNumberToObject(node, "topologyRevision", live_peer->topology_revision);
    else if (local)
      cJSON_AddNumberToObject(node, "topologyRevision", current_state->topologyRevision);
    else
      cJSON_AddNullToObject(node, "topologyRevision");
    if (live_peer && !live_peer->topology_hash.empty())
      cJSON_AddStringToObject(node, "topologyHash", live_peer->topology_hash.c_str());
    else if (local && current_state->topologyHash.size() == 64)
      cJSON_AddStringToObject(node, "topologyHash", current_state->topologyHash.c_str());
    else
      cJSON_AddNullToObject(node, "topologyHash");
    cJSON_AddItemToArray(nodes, node);
  };
  bool registry_reported = false;
  if (g_config->role == Role::Master) {
    RigSlot rig;
    std::string rig_error;
    if (rig_load(rig, rig_error) && rig.valid) {
      cJSON* rig_json = cJSON_ParseWithLength(rig.json.data(), rig.json.size());
      cJSON* records = rig_json ? cJSON_GetObjectItemCaseSensitive(rig_json, "nodes") : nullptr;
      cJSON* revision = rig_json ? cJSON_GetObjectItemCaseSensitive(rig_json, "revision") : nullptr;
      if (cJSON_IsArray(records) && cJSON_GetArraySize(records) <= 64) {
        std::vector<const cJSON*> sorted;
        for (const cJSON* item = records->child; item; item = item->next) sorted.push_back(item);
        std::sort(sorted.begin(), sorted.end(), [](const cJSON* left, const cJSON* right) {
          const cJSON* left_id = cJSON_GetObjectItemCaseSensitive(left, "nodeId");
          const cJSON* right_id = cJSON_GetObjectItemCaseSensitive(right, "nodeId");
          return cJSON_IsNumber(left_id) && cJSON_IsNumber(right_id) &&
                 left_id->valueint < right_id->valueint;
        });
        for (const cJSON* item : sorted) {
          const cJSON* node_id = cJSON_GetObjectItemCaseSensitive(item, "nodeId");
          const cJSON* role = cJSON_GetObjectItemCaseSensitive(item, "role");
          const cJSON* hardware_id = cJSON_GetObjectItemCaseSensitive(item, "hardwareId");
          const cJSON* label = cJSON_GetObjectItemCaseSensitive(item, "label");
          const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
          if (!cJSON_IsNumber(node_id) || !cJSON_IsString(role) ||
              !cJSON_IsString(hardware_id) || !cJSON_IsString(label) ||
              !cJSON_IsBool(enabled)) continue;
          const bool local = static_cast<uint16_t>(node_id->valueint) == g_config->nodeId &&
                             std::strcmp(role->valuestring, "master") == 0;
          const SyncPeerSnapshot* live_peer = nullptr;
          for (const auto& candidate : live_peers) {
            if (candidate.node_id == static_cast<uint16_t>(node_id->valueint) &&
                candidate.hardware_id == hardware_id->valuestring) {
              live_peer = &candidate;
              break;
            }
          }
          append_node(static_cast<uint16_t>(node_id->valueint), role->valuestring,
                      hardware_id->valuestring, label->valuestring,
                      cJSON_IsTrue(enabled), local, live_peer);
        }
        if (cJSON_IsNumber(revision) && revision->valuedouble >= 0 &&
            revision->valuedouble <= UINT32_MAX &&
            std::floor(revision->valuedouble) == revision->valuedouble)
          rig_revision = static_cast<uint32_t>(revision->valuedouble);
        registry_reported = cJSON_GetArraySize(nodes) > 0;
      }
      cJSON_Delete(rig_json);
    }
    if (!registry_reported) {
      append_node(g_config->nodeId, "master", g_config->hardwareId.c_str(),
                  "LED Panel Master", true, true, nullptr);
      cJSON_AddStringToObject(root, "registryFault", "NODE_REGISTRY_UNAVAILABLE");
    }
  } else {
    append_node(g_config->nodeId, "follower", g_config->hardwareId.c_str(),
                "LED Panel Follower", true, true, nullptr);
  }
  cJSON_AddNumberToObject(root, "topologyRevision", current_state->topologyRevision);
  cJSON_AddNumberToObject(root, "rigRevision", rig_revision);
  if (current_state->topologyHash.size() == 64)
    cJSON_AddStringToObject(root, "topologyHash", current_state->topologyHash.c_str());
  else
    cJSON_AddNullToObject(root, "topologyHash");
  return root;
}

esp_err_t status(httpd_req_t* req) {
  if (!authenticated(req, false)) return error_response(req, "401 Unauthorized", "UNAUTHENTICATED"), ESP_OK;
  cJSON* root = make_status();
  send_json(req, root);
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t nodes(httpd_req_t* req) {
  if (!authenticated(req, false)) return error_response(req, "401 Unauthorized", "UNAUTHENTICATED"), ESP_OK;
  cJSON* all = make_status();
  cJSON* result = cJSON_CreateObject();
  cJSON* topology_revision = cJSON_GetObjectItemCaseSensitive(all, "topologyRevision");
  cJSON_AddNumberToObject(result, "topologyRevision",
                          cJSON_IsNumber(topology_revision) ? topology_revision->valueint : 0);
  cJSON_AddItemToObject(result, "nodes", cJSON_DetachItemFromObjectCaseSensitive(all, "nodes"));
  send_json(req, result);
  cJSON_Delete(result);
  cJSON_Delete(all);
  return ESP_OK;
}

esp_err_t capabilities(httpd_req_t* req) {
  if (!authenticated(req, false)) return error_response(req, "401 Unauthorized", "UNAUTHENTICATED"), ESP_OK;
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "profile", "seengreat_hub75_s3_n16r8_v1");
  cJSON_AddStringToObject(root, "firmwareVersion", "2.0.0-dev");
  cJSON_AddStringToObject(root, "renderer", "lp-raster-1");
  cJSON_AddBoolToObject(root, "audioInput", false);
  const bool panel_ready = hub75_diagnostic_supported();
  cJSON_AddBoolToObject(root, "panelReady", panel_ready);
  if (panel_ready) cJSON_AddNullToObject(root, "panelFault");
  else cJSON_AddStringToObject(root, "panelFault", "PANEL_DMA_UNAVAILABLE");
  cJSON_AddStringToObject(root, "panelOutput",
      hub75_nightlight_active() ? "dim_nightlight" :
      hub75_diagnostic_active() ? "diagnostic_rgb_bars" :
      g_state->running && !g_state->blackout ? "content_playback" : "disabled_black");
  cJSON_AddNumberToObject(root, "outputEnableGpio", 13);
  cJSON_AddNumberToObject(root, "outputEnableLevel", hub75_output_enable_level());
  cJSON* limits = cJSON_AddObjectToObject(root, "limits");
  cJSON_AddNumberToObject(limits, "maxPackageBytes", 2621440);
  cJSON_AddNumberToObject(limits, "maxNodes",
                          g_config->networkMode == "softap" ? 4 : 16);
  cJSON_AddNumberToObject(limits, "maxLayouts", 16);
  cJSON_AddNumberToObject(limits, "width", 64);
  cJSON_AddNumberToObject(limits, "height", 32);
  cJSON* network_modes = cJSON_AddArrayToObject(root, "networkModes");
  cJSON_AddItemToArray(network_modes, cJSON_CreateString("softap"));
  cJSON_AddItemToArray(network_modes, cJSON_CreateString("infrastructure"));
  cJSON* features = cJSON_AddObjectToObject(root, "features");
  cJSON_AddBoolToObject(features, "usbProvisioning", true);
  cJSON_AddBoolToObject(features, "packageStage", true);
  cJSON_AddBoolToObject(features, "packageActivation", true);
  cJSON_AddBoolToObject(features, "multiNodeSync", g_sync_runtime_ok);
  cJSON_AddBoolToObject(features, "safeStateTransactions", g_sync_runtime_ok);
  cJSON_AddBoolToObject(features, "topologyDistribution",
      g_topology_ok && g_config->role == Role::Master && g_sync_runtime_ok &&
          g_node_http_auth_ok);
  cJSON_AddBoolToObject(features, "packageDistribution",
      g_config->role == Role::Master && g_sync_runtime_ok && g_node_http_auth_ok);
  cJSON_AddBoolToObject(features, "panelChainOutput", false);
  cJSON_AddBoolToObject(features, "panelDiagnosticTest",
      g_config->role == Role::Master && hub75_diagnostic_supported() &&
          !hub75_diagnostic_active());
  cJSON_AddBoolToObject(features, "playback", panel_ready && g_runtime_package_ok);
  cJSON* layouts = cJSON_AddArrayToObject(root, "layouts");
  cJSON* single_layout = cJSON_CreateObject();
  cJSON_AddStringToObject(single_layout, "id", "panel_64x32");
  cJSON_AddNumberToObject(single_layout, "width", 64);
  cJSON_AddNumberToObject(single_layout, "height", 32);
  cJSON_AddItemToArray(layouts, single_layout);
  send_json(req, root);
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t diagnostics(httpd_req_t* req) {
  if (!authenticated(req, false)) return error_response(req, "401 Unauthorized", "UNAUTHENTICATED"), ESP_OK;
  cJSON* root = cJSON_CreateObject();
  const bool clock_authority = g_config->role == Role::Master;
  const uint16_t authority_node_id = clock_authority
      ? g_config->nodeId : g_config->masterNodeId;
  SyncPeerSnapshot clock_peer;
  bool has_clock_peer = false;
  if (!clock_authority) {
    std::vector<SyncPeerSnapshot> peers;
    sync_runtime_peer_snapshot(peers);
    for (const auto& peer : peers) {
      if (peer.node_id == authority_node_id) {
        clock_peer = peer;
        has_clock_peer = true;
        break;
      }
    }
  }
  cJSON_AddStringToObject(root, "boardProfile", "seengreat_hub75_s3_n16r8_v1");
  cJSON_AddStringToObject(root, "firmwareVersion", "2.0.0-dev");
  cJSON_AddStringToObject(root, "clockRole", clock_authority ? "authority" : "follower");
  cJSON_AddNumberToObject(root, "clockAuthorityNodeId", authority_node_id);
  cJSON_AddBoolToObject(root, "clockSynced",
                        clock_authority || (has_clock_peer && clock_peer.clock_synced));
  if (clock_authority) {
    cJSON_AddNumberToObject(root, "clockOffsetUs", 0);
    cJSON_AddNumberToObject(root, "clockRttUs", 0);
    cJSON_AddNullToObject(root, "clockSampleAgeMs");
  } else if (has_clock_peer) {
    cJSON_AddNumberToObject(root, "clockOffsetUs", clock_peer.clock_offset_us);
    cJSON_AddNumberToObject(root, "clockRttUs", clock_peer.clock_rtt_us);
    if (clock_peer.clock_sample_age_ms >= 0)
      cJSON_AddNumberToObject(root, "clockSampleAgeMs", clock_peer.clock_sample_age_ms);
    else
      cJSON_AddNullToObject(root, "clockSampleAgeMs");
  } else {
    cJSON_AddNullToObject(root, "clockOffsetUs");
    cJSON_AddNullToObject(root, "clockRttUs");
    cJSON_AddNullToObject(root, "clockSampleAgeMs");
  }
  cJSON_AddBoolToObject(root, "unsyncedForcesBlackout", true);
  cJSON_AddNumberToObject(root, "freeInternalHeap", esp_get_free_internal_heap_size());
  cJSON_AddNumberToObject(root, "freePsram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  cJSON_AddNumberToObject(root, "psramBytes", esp_psram_get_size());
  cJSON_AddNullToObject(root, "renderFps");
  cJSON_AddNullToObject(root, "scanRefreshHz");
  cJSON_AddBoolToObject(root, "automaticRestorePending", g_restore_pending.load());
  cJSON_AddBoolToObject(root, "panelSwitchAvailable", g_keys_available.load());
  cJSON_AddBoolToObject(root, "panelControlsSelfTest", g_controls_self_test.load());
  cJSON_AddNumberToObject(root, "panelSwitchRaw", g_keys_raw.load());
  cJSON_AddBoolToObject(root, "panelReady", hub75_diagnostic_supported());
  if (hub75_diagnostic_supported()) cJSON_AddNullToObject(root, "faultCode");
  else cJSON_AddStringToObject(root, "faultCode", "PANEL_DMA_UNAVAILABLE");
  cJSON_AddBoolToObject(root, "blackout", g_state ? g_state->blackout : true);
  cJSON_AddStringToObject(root, "panelOutput",
      hub75_nightlight_active() ? "dim_nightlight" :
      hub75_diagnostic_active() ? "diagnostic_rgb_bars" :
      g_state && g_state->running && !g_state->blackout ? "content_playback" :
      "disabled_black");
  cJSON_AddNumberToObject(root, "outputEnableGpio", 13);
  cJSON_AddNumberToObject(root, "outputEnableLevel", hub75_output_enable_level());
  send_json(req, root);
  cJSON_Delete(root);
  return ESP_OK;
}

esp_err_t command(httpd_req_t* req) {
  if (!authenticated(req, true)) return error_response(req, "403 Forbidden", "AUTH_OR_CSRF_REQUIRED"), ESP_OK;
  if (!master_only_mutation(req)) return ESP_OK;
  if (g_maintenance_state.load(std::memory_order_relaxed) == 2)
    return error_response(req, "409 Conflict", "DEPLOYMENT_BUSY"), ESP_OK;
  bool topology_busy_expected = false;
  if (!g_topology_sync_busy.compare_exchange_strong(
          topology_busy_expected, true, std::memory_order_acq_rel))
    return error_response(req, "409 Conflict", "TOPOLOGY_SYNC_BUSY"), ESP_OK;
  AtomicFlagReset topology_busy_reset(g_topology_sync_busy);
  if (g_maintenance_state.load(std::memory_order_relaxed) == 2)
    return error_response(req, "409 Conflict", "DEPLOYMENT_BUSY"), ESP_OK;
  std::string body;
  cJSON* root = parse_body(req, body);
  uint32_t expected_revision = 0;
  if (!root || !json_uint32(
          cJSON_GetObjectItemCaseSensitive(root, "expectedRevision"),
          expected_revision)) {
    cJSON_Delete(root);
    return error_response(req, "400 Bad Request", "INVALID_REQUEST"), ESP_OK;
  }
  if (expected_revision != g_revision.load(std::memory_order_relaxed)) {
    cJSON_Delete(root);
    return error_response(req, "409 Conflict", "REVISION_CONFLICT"), ESP_OK;
  }
  cJSON* op_field = root ? cJSON_GetObjectItemCaseSensitive(root, "op") : nullptr;
  const char* op = cJSON_IsString(op_field) ? op_field->valuestring : "";
  const cJSON* args = root ? cJSON_GetObjectItemCaseSensitive(root, "args") : nullptr;
  const bool playback_operation = std::strcmp(op, "go") == 0 ||
      std::strcmp(op, "play_scene") == 0 || std::strcmp(op, "next_scene") == 0 || std::strcmp(op, "next") == 0 ||
      std::strcmp(op, "prev") == 0 || std::strcmp(op, "pause") == 0 ||
      std::strcmp(op, "resume") == 0;
  if (playback_operation) {
    std::string load_error;
    if (!hub75_diagnostic_supported() ||
        !ensure_active_renderer(g_state->packageHash, load_error)) {
      cJSON_Delete(root);
      const char* code = load_error.empty() ? "PANEL_DMA_UNAVAILABLE" :
                                                   load_error.c_str();
      return error_response(req, "503 Service Unavailable", code), ESP_OK;
    }
  }
  // The USB nightlight uses an independent HUB75 task. Cancel it as part of
  // the normal stop transition so a one-click content update can take over
  // without a separate user-facing blackout/nightlight step.
  if (std::strcmp(op, "stop") == 0 && hub75_nightlight_active())
    hub75_stop_nightlight();
  bool broadcast_blackout = false;
  bool distributed = false;
  PlaybackState next_state;
  if (expected_revision == UINT32_MAX) {
    cJSON_Delete(root);
    return error_response(req, "409 Conflict", "REVISION_EXHAUSTED"), ESP_OK;
  }
  std::string command_error;
  if (!render_runtime_command(op, args, *g_state,
          static_cast<uint64_t>(esp_timer_get_time()), next_state,
          command_error)) {
    cJSON_Delete(root);
    const bool invalid = command_error == "INVALID_ARGUMENT" ||
        command_error == "UNSUPPORTED_OPERATION";
    return error_response(req, invalid ? "400 Bad Request" : "409 Conflict",
                          command_error.empty() ? "COMMAND_REJECTED" :
                              command_error.c_str()), ESP_OK;
  }
  if (std::strcmp(op, "brightness") == 0)
    next_state.userBrightness = std::min<uint8_t>(next_state.userBrightness,
                                                  g_config->brightnessCap);
  broadcast_blackout = next_state.blackout &&
      (std::strcmp(op, "blackout") == 0 || std::strcmp(op, "stop") == 0);
  if (g_state->revision == UINT32_MAX) {
    cJSON_Delete(root);
    return error_response(req, "409 Conflict", "STATE_REVISION_EXHAUSTED"), ESP_OK;
  }
  next_state.revision = g_state->revision + 1;
  const bool needs_followers = required_follower_registered(next_state.layoutId);
  // After reboot the stopped, black fleet has not reconstructed its remote
  // topology yet. Allow local preparation settings; maintenance and package
  // activation establish the shared topology before any visible playback.
  const bool stopped_preparation = !g_state->running &&
      g_state->mode == "stopped" && g_state->blackout &&
      (std::strcmp(op, "stop") == 0 || std::strcmp(op, "brightness") == 0);
  distributed = needs_followers && !stopped_preparation;
  if (distributed) {
    const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t scheduled_at = now_us + 1500000;
    // Commands that preserve playback time continue advancing until the
    // synchronized adoption boundary; cue/scene transitions start at tick 0.
    const bool starts_target = std::strcmp(op, "go") == 0 ||
        std::strcmp(op, "play_scene") == 0 || std::strcmp(op, "next_scene") == 0 || std::strcmp(op, "next") == 0 ||
        std::strcmp(op, "prev") == 0 || std::strcmp(op, "stop") == 0;
    if (!starts_target && g_state->running && scheduled_at > now_us) {
      next_state.positionTicks += ((scheduled_at - now_us) * 30) / 1000000;
    }
    next_state.effectiveAtUs = scheduled_at;
    next_state.anchorUs = scheduled_at;
  }
  cJSON_Delete(root);
  uint32_t accepted_revision = 0;
  std::string operation_id;
  if (distributed) {
    operation_id = new_operation_id();
    std::string sync_error;
    if (!sync_runtime_apply_state(next_state, operation_id, 5000, sync_error))
      return error_response(req, "503 Service Unavailable", sync_error.c_str()), ESP_OK;
    accepted_revision = expected_revision + 1;
    g_revision.store(accepted_revision, std::memory_order_relaxed);
  } else {
    accepted_revision = expected_revision + 1;
    *g_state = next_state;
    g_revision.store(accepted_revision, std::memory_order_relaxed);
    sync_runtime_update_state(*g_state);
  }
  if (broadcast_blackout && !distributed)
    sync_runtime_broadcast_blackout(g_state->revision, "user");
  g_restore_pending.store(false);
  cJSON* response = cJSON_CreateObject();
  cJSON_AddNumberToObject(response, "acceptedRevision", accepted_revision);
  cJSON_AddStringToObject(response, "status", "applied");
  if (distributed) cJSON_AddStringToObject(response, "operationId", operation_id.c_str());
  send_json(req, response);
  cJSON_Delete(response);
  return ESP_OK;
}

bool start_panel_diagnostic(std::string& error) {
  if (!g_config || g_config->role != Role::Master) {
    error = "MASTER_REQUIRED";
    return false;
  }
  if (!g_state || g_maintenance_state.load(std::memory_order_acquire) != 0 ||
      sync_runtime_transaction_active() || g_topology_sync_busy.load(
          std::memory_order_acquire) || g_state->running ||
      g_state->mode != "stopped" || !g_state->blackout) {
    error = "PANEL_TEST_REQUIRES_STOPPED_BLACKOUT";
    return false;
  }
  if (!hub75_diagnostic_supported() || !hub75_start_diagnostic()) {
    error = "PANEL_DIAGNOSTIC_UNAVAILABLE";
    return false;
  }
  error.clear();
  return true;
}

esp_err_t panel_test(httpd_req_t* req) {
  if (!authenticated(req, true))
    return error_response(req, "403 Forbidden", "AUTH_OR_CSRF_REQUIRED"), ESP_OK;
  if (!master_only_mutation(req)) return ESP_OK;

  std::string body;
  cJSON* request = parse_body(req, body);
  if (!cJSON_IsObject(request)) {
    cJSON_Delete(request);
    return error_response(req, "400 Bad Request", "INVALID_REQUEST"), ESP_OK;
  }
  cJSON_Delete(request);
  std::string diagnostic_error;
  if (!start_panel_diagnostic(diagnostic_error))
    return error_response(req,
        diagnostic_error == "PANEL_DIAGNOSTIC_UNAVAILABLE"
            ? "503 Service Unavailable" : "409 Conflict",
        diagnostic_error.c_str()), ESP_OK;

  cJSON* response = cJSON_CreateObject();
  cJSON_AddStringToObject(response, "status", "diagnostic_started");
  cJSON_AddStringToObject(response, "pattern", "RGB_BARS");
  cJSON_AddNumberToObject(response, "durationMs", 30000);
  cJSON_AddNumberToObject(response, "panelCoveragePercent", 100);
  cJSON_AddNumberToObject(response, "brightness", 96);
  send_json(req, response);
  cJSON_Delete(response);
  return ESP_OK;
}

bool master_only_mutation(httpd_req_t* req) {
  if (g_config->role == Role::Master) return true;
  error_response(req, "403 Forbidden", "FOLLOWER_READ_ONLY");
  return false;
}

esp_err_t maintenance_change(httpd_req_t* req, bool enable) {
  if (!authenticated(req, true))
    return error_response(req, "403 Forbidden", "AUTH_OR_CSRF_REQUIRED"), ESP_OK;
  if (!master_only_mutation(req)) return ESP_OK;
  std::string content_type;
  if (!header(req, "Content-Type", content_type) || content_type != "application/json")
    return error_response(req, "415 Unsupported Media Type", "JSON_REQUIRED"), ESP_OK;
  std::string body;
  cJSON* request = parse_body_with_limit(req, body, 1024);
  if (!exact_object_keys(request, {"requestId", "expectedRevision"}) ||
      !valid_request_uuid(cJSON_GetObjectItemCaseSensitive(request, "requestId"))) {
    cJSON_Delete(request);
    return error_response(req, "400 Bad Request", "INVALID_REQUEST"), ESP_OK;
  }
  uint32_t expected_revision = 0;
  if (!json_uint32(cJSON_GetObjectItemCaseSensitive(request, "expectedRevision"),
                   expected_revision)) {
    cJSON_Delete(request);
    return error_response(req, "400 Bad Request", "INVALID_REQUEST"), ESP_OK;
  }
  cJSON_Delete(request);
  if (expected_revision != g_revision.load(std::memory_order_relaxed))
    return error_response(req, "409 Conflict", "REVISION_CONFLICT"), ESP_OK;
  if (sync_runtime_transaction_active())
    return error_response(req, "409 Conflict", "SYNC_BUSY"), ESP_OK;
  if (expected_revision == UINT32_MAX)
    return error_response(req, "409 Conflict", "REVISION_EXHAUSTED"), ESP_OK;
  if (g_state->running || g_state->mode != "stopped" || !g_state->blackout)
    return error_response(req, "409 Conflict", "MAINTENANCE_REQUIRES_STOPPED_BLACKOUT"), ESP_OK;

  uint8_t expected_state = enable ? 0 : 1;
  if (!g_maintenance_state.compare_exchange_strong(expected_state, 2)) {
    const char* code = expected_state == 2 ? "DEPLOYMENT_BUSY" :
        enable ? "MAINTENANCE_ALREADY_ACTIVE" : "MAINTENANCE_NOT_ACTIVE";
    return error_response(req, "409 Conflict", code), ESP_OK;
  }
  if (g_topology_sync_busy.load(std::memory_order_acquire)) {
    g_maintenance_state.store(enable ? 0 : 1, std::memory_order_relaxed);
    return error_response(req, "409 Conflict", "TOPOLOGY_SYNC_BUSY"), ESP_OK;
  }
  std::vector<uint16_t> participants;
  std::string follower_error;
  if (enable) {
    std::vector<RigFollowerIdentity> followers;
    if (!rig_display_followers(*g_config, g_state->layoutId, followers,
                               follower_error)) {
      g_maintenance_state.store(0, std::memory_order_relaxed);
      return error_response(req, "503 Service Unavailable", "RIG_UNAVAILABLE"), ESP_OK;
    }
    for (const auto& follower : followers)
      participants.push_back(follower.node_id);
  } else {
    // Include both the previous and newly distributed topology participants.
    // A node removed from the latest Rig must still receive maintenance=false.
    participants = g_maintenance_members;
  }
  std::vector<uint16_t> updated_followers;
  for (const uint16_t node_id : participants) {
    if (set_remote_maintenance(node_id, enable, follower_error)) {
      updated_followers.push_back(node_id);
      continue;
    }
    for (const uint16_t updated_node : updated_followers) {
      std::string rollback_error;
      set_remote_maintenance(updated_node, !enable, rollback_error);
    }
    g_maintenance_state.store(enable ? 0 : 1, std::memory_order_relaxed);
    ESP_LOGW(kTag, "fleet maintenance change failed at node %u: %s",
             node_id, follower_error.c_str());
    return error_response(req, "503 Service Unavailable",
                          "FOLLOWER_MAINTENANCE_FAILED"), ESP_OK;
  }
  if (enable) g_maintenance_members = std::move(updated_followers);
  else g_maintenance_members.clear();
  g_maintenance_state.store(enable ? 1 : 0, std::memory_order_relaxed);
  const uint32_t accepted_revision = g_revision.fetch_add(1, std::memory_order_relaxed) + 1;
  bool topology_changed = true;
  if (!enable && g_topology_snapshot_valid) {
    TopologySnapshot active_topology;
    std::string topology_error;
    // Leaving update mode after activation does not invalidate the topology
    // we just confirmed. A cancelled candidate with a different layout does.
    topology_changed = !build_current_topology(active_topology, topology_error) ||
        active_topology.revision != g_topology_snapshot.revision ||
        active_topology.hash != g_topology_snapshot.hash ||
        active_topology.layout_id != g_topology_snapshot.layout_id;
  }
  g_topology_dirty.store(topology_changed, std::memory_order_relaxed);
  cJSON* response = cJSON_CreateObject();
  cJSON_AddStringToObject(response, "status", enable ? "maintenance" : "normal");
  cJSON_AddBoolToObject(response, "maintenance", enable);
  cJSON_AddStringToObject(response, "maintenanceScope",
                          deployment_scope(g_state->layoutId).c_str());
  cJSON_AddNumberToObject(response, "acceptedRevision", accepted_revision);
  send_json(req, response);
  cJSON_Delete(response);
  return ESP_OK;
}

esp_err_t node_health(httpd_req_t* req) {
  std::string node_id_text, sender_boot_id, sequence_text, nonce;
  std::string body_hash, request_mac;
  uint32_t node_id = 0, sequence = 0;
  uint32_t source_address = 0;
  if (!g_node_http_auth_ok || req->method != HTTP_GET ||
      req->content_len != 0 || req->uri[0] == '\0' ||
      !header(req, "X-Node-Id", node_id_text) ||
      !decimal_header(node_id_text, UINT16_MAX, node_id) || node_id == 0 ||
      !header(req, "X-Boot-Id", sender_boot_id) ||
      !header(req, "X-Node-Seq", sequence_text) ||
      !decimal_header(sequence_text, UINT32_MAX, sequence) ||
      !header(req, "X-Request-Nonce", nonce) ||
      !header(req, "X-Body-SHA256", body_hash) ||
      !header(req, "X-Node-MAC", request_mac) ||
      !node_http_ipv4_source(req, source_address)) {
    return error_response(req, "401 Unauthorized", "NODE_AUTH_REQUIRED"), ESP_OK;
  }

  ContentState content;
  std::string content_error;
  const bool content_ok = content_get_state(content, content_error);
  cJSON* root = cJSON_CreateObject();
  if (!root) return error_response(req, "503 Service Unavailable", "NO_MEMORY"), ESP_OK;
  cJSON_AddBoolToObject(root, "ok", content_ok);
  cJSON_AddNumberToObject(root, "nodeId", g_config->nodeId);
  if (content_ok) cJSON_AddNumberToObject(root, "contentGeneration", content.generation);
  else cJSON_AddNullToObject(root, "contentGeneration");
  cJSON_AddStringToObject(root, "hardwareId", g_config->hardwareId.c_str());
  cJSON_AddStringToObject(root, "bootId", boot_id_hex().c_str());
  cJSON_AddStringToObject(root, "role",
      g_config->role == Role::Master ? "master" : "follower");
  cJSON_AddStringToObject(root, "firmwareVersion", "2.0.0-dev");
  if (content_ok && content.has_active)
    cJSON_AddStringToObject(root, "activeHash", content.active_hash.c_str());
  else
    cJSON_AddNullToObject(root, "activeHash");
  if (content_ok && content.has_candidate)
    cJSON_AddStringToObject(root, "candidateHash", content.candidate_hash.c_str());
  else
    cJSON_AddNullToObject(root, "candidateHash");
  const uint8_t maintenance = g_maintenance_state.load(std::memory_order_relaxed);
  cJSON_AddBoolToObject(root, "maintenance", maintenance != 0);
  const bool panel_ready = hub75_diagnostic_supported();
  const bool active_content_matches = content_ok && content.has_active &&
      g_state && content.active_hash == g_state->packageHash;
  const char* panel_fault = !panel_ready ? "PANEL_DMA_UNAVAILABLE" :
      !content_ok ? "CONTENT_METADATA_INVALID" :
      g_state && g_state->running && !active_content_matches ? "ACTIVE_CONTENT_MISMATCH" :
      g_state && g_state->running &&
          !render_runtime_is_loaded(g_state->packageHash) ? "PACKAGE_NOT_READY" : nullptr;
  cJSON_AddStringToObject(root, "health", panel_fault ? "fault" : "ready");
  if (panel_fault) cJSON_AddStringToObject(root, "faultCode", panel_fault);
  else cJSON_AddNullToObject(root, "faultCode");
  cJSON_AddStringToObject(root, "panelOutput",
      hub75_nightlight_active() ? "dim_nightlight" :
      hub75_diagnostic_active() ? "diagnostic_rgb_bars" :
      g_state && g_state->running && !g_state->blackout ? "content_playback" :
      "disabled_black");
  cJSON_AddStringToObject(root, "updateStatus", "idle");
  if (!content_ok) cJSON_AddStringToObject(root, "error", "CONTENT_UNAVAILABLE");
  char* json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) return error_response(req, "503 Service Unavailable", "NO_MEMORY"), ESP_OK;
  const std::string response_body(json);
  cJSON_free(json);

  const std::string method = "GET";
  const std::string path(req->uri);
  std::string response_hash, response_mac, auth_error;
  const uint16_t status_code = content_ok ? 200 : 503;
  if (!sync_runtime_node_http_exchange(static_cast<uint16_t>(node_id),
          source_address, sender_boot_id, sequence, nonce, method, path,
          body_hash, request_mac, nullptr, 0, status_code,
          reinterpret_cast<const uint8_t*>(response_body.data()),
          response_body.size(), response_hash, response_mac, auth_error)) {
    ESP_LOGW(kTag, "node health request rejected: %s", auth_error.c_str());
    return error_response(req, "401 Unauthorized", "NODE_AUTH_FAILED"), ESP_OK;
  }
  httpd_resp_set_status(req, content_ok ? "200 OK" : "503 Service Unavailable");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  const std::string seq = std::to_string(sequence);
  httpd_resp_set_hdr(req, "X-Node-Seq", seq.c_str());
  httpd_resp_set_hdr(req, "X-Request-Nonce", nonce.c_str());
  httpd_resp_set_hdr(req, "X-Body-SHA256", response_hash.c_str());
  httpd_resp_set_hdr(req, "X-Node-MAC", response_mac.c_str());
  const esp_err_t sent = httpd_resp_send(req, response_body.data(), response_body.size());
  return sent;
}

esp_err_t node_candidate(httpd_req_t* req) {
  std::string node_id_text, sender_boot_id, sequence_text, nonce;
  std::string body_hash, request_mac;
  uint32_t node_id = 0, sequence = 0;
  uint32_t source_address = 0;
  if (!g_node_http_auth_ok || g_config->role != Role::Master ||
      req->method != HTTP_GET || req->content_len != 0 ||
      std::strcmp(req->uri, "/node/v2/candidate") != 0 ||
      !header(req, "X-Node-Id", node_id_text) ||
      !decimal_header(node_id_text, UINT16_MAX, node_id) || node_id == 0 ||
      !header(req, "X-Boot-Id", sender_boot_id) ||
      !header(req, "X-Node-Seq", sequence_text) ||
      !decimal_header(sequence_text, UINT32_MAX, sequence) ||
      !header(req, "X-Request-Nonce", nonce) ||
      !header(req, "X-Body-SHA256", body_hash) ||
      !header(req, "X-Node-MAC", request_mac) ||
      !node_http_ipv4_source(req, source_address)) {
    return error_response(req, "401 Unauthorized", "NODE_AUTH_REQUIRED"), ESP_OK;
  }
  const std::string path = "/node/v2/candidate";
  std::string session_key, error;
  if (!sync_runtime_node_http_authorize(static_cast<uint16_t>(node_id),
          source_address, sender_boot_id, sequence, nonce, "GET", path,
          body_hash, request_mac, nullptr, 0, session_key, error)) {
    ESP_LOGW(kTag, "candidate request rejected: %s", error.c_str());
    return error_response(req, "401 Unauthorized", "NODE_AUTH_FAILED"), ESP_OK;
  }
  const auto reject = [&](uint16_t status, const char* status_text, const char* code) {
    return send_signed_node_error(req, status, status_text, path, sequence,
                                  nonce, session_key, code);
  };
  if (g_maintenance_state.load(std::memory_order_relaxed) != 1 ||
      g_state->running || g_state->mode != "stopped" || !g_state->blackout)
    return reject(409, "409 Conflict", "MAINTENANCE_STOPPED_BLACKOUT_REQUIRED");

  std::vector<RigFollowerIdentity> enabled_followers;
  if (!rig_enabled_followers(*g_config, enabled_followers, error))
    return reject(503, "503 Service Unavailable", "RIG_UNAVAILABLE");
  const bool registered = std::any_of(enabled_followers.begin(),
      enabled_followers.end(), [node_id](const RigFollowerIdentity& follower) {
        return follower.node_id == node_id;
      });
  if (!registered)
    return reject(403, "403 Forbidden", "NODE_NOT_REGISTERED");

  ContentState content;
  if (!content_get_state(content, error) || !content.has_candidate ||
      content.candidate_length == 0)
    return reject(404, "404 Not Found", "CANDIDATE_NOT_FOUND");
  std::string if_candidate_hash;
  if (header(req, "X-Current-Candidate-SHA256", if_candidate_hash) &&
      !valid_sha256(if_candidate_hash))
    return reject(400, "400 Bad Request", "INVALID_CURRENT_CANDIDATE_HASH");
  if (!if_candidate_hash.empty() &&
      constant_equal(if_candidate_hash, content.candidate_hash))
    return send_signed_node_response(req, 304, "304 Not Modified",
        "application/vnd.ledpanel.package", path, sequence, nonce, session_key,
        nullptr, 0, content.candidate_hash.c_str());
  uint8_t* package = static_cast<uint8_t*>(heap_caps_malloc(
      content.candidate_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!package)
    return reject(503, "503 Service Unavailable", "NO_MEMORY");
  size_t package_length = 0;
  std::string package_hash;
  if (!content_read_candidate(package, content.candidate_length, package_length,
                              package_hash, error) ||
      package_length != content.candidate_length ||
      !constant_equal(package_hash, content.candidate_hash)) {
    std::memset(package, 0, content.candidate_length);
    heap_caps_free(package);
    ESP_LOGE(kTag, "candidate read failed: %s", error.c_str());
    return reject(503, "503 Service Unavailable", "CANDIDATE_UNAVAILABLE");
  }

  const esp_err_t sent = send_signed_node_response(req, 200, "200 OK",
      "application/vnd.ledpanel.package", path, sequence, nonce, session_key,
      package, package_length, package_hash.c_str());
  std::memset(package, 0, package_length);
  heap_caps_free(package);
  return sent;
}

esp_err_t node_topology(httpd_req_t* req) {
  AuthorizedNodeJsonRequest node_request;
  std::string auth_error;
  if (!authorize_node_json_request(req, "POST", "/node/v2/topology",
          16 * 1024, node_request, auth_error))
    return error_response(req, "401 Unauthorized", "NODE_AUTH_FAILED"), ESP_OK;
  const std::string path = "/node/v2/topology";
  const auto reject = [&](uint16_t status, const char* status_text,
                          const char* code) {
    return send_signed_node_error(req, status, status_text, path,
        node_request.sequence, node_request.nonce, node_request.session_key,
        code);
  };
  if (!g_topology_ok || g_config->role != Role::Follower ||
      node_request.node_id != g_config->masterNodeId)
    return reject(403, "403 Forbidden", "MASTER_REQUIRED");
  if (g_state->running || g_state->mode != "stopped" || !g_state->blackout)
    return reject(409, "409 Conflict", "TOPOLOGY_REQUIRES_STOPPED_BLACKOUT");

  cJSON* request = cJSON_ParseWithLength(node_request.body.data(),
                                         node_request.body.size());
  const cJSON* topology = request
      ? cJSON_GetObjectItemCaseSensitive(request, "topology") : nullptr;
  const char* requested_hash_value = json_string(request, "topologyHash");
  const cJSON* topology_layout = topology
      ? cJSON_GetObjectItemCaseSensitive(topology, "layout") : nullptr;
  const char* requested_layout_value = json_string(topology_layout, "id");
  if (!exact_object_keys(request, {"topology", "topologyHash"}) ||
      !cJSON_IsObject(topology) || !requested_hash_value ||
      !valid_sha256(requested_hash_value) || !requested_layout_value) {
    cJSON_Delete(request);
    return reject(400, "400 Bad Request", "TOPOLOGY_REQUEST_INVALID");
  }
  const std::string requested_hash(requested_hash_value);
  const std::string requested_layout(requested_layout_value);
  char* topology_text = cJSON_PrintUnformatted(topology);
  if (!topology_text) {
    cJSON_Delete(request);
    return reject(503, "503 Service Unavailable", "NO_MEMORY");
  }
  const std::string topology_json(topology_text);
  cJSON_free(topology_text);
  cJSON_Delete(request);

  ContentState content;
  std::string error;
  if (!content_get_state(content, error))
    return reject(503, "503 Service Unavailable", "CONTENT_UNAVAILABLE");
  uint8_t* package = static_cast<uint8_t*>(heap_caps_malloc(
      kMaxPackageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!package) return reject(503, "503 Service Unavailable", "NO_MEMORY");
  size_t package_length = 0;
  std::string package_hash, layout_json;
  bool package_ready = content.has_candidate &&
      content_read_candidate(package, kMaxPackageBytes, package_length,
                             package_hash, error) &&
      runtime_package_get_layout(package, package_length, requested_layout,
                                 layout_json, error);
  if (!package_ready && content.has_active) {
    package_ready = content_read_active(package, kMaxPackageBytes,
        package_length, package_hash, error) &&
        runtime_package_get_layout(package, package_length, requested_layout,
                                   layout_json, error);
  }
  if (package_length) std::memset(package, 0, package_length);
  heap_caps_free(package);
  if (!package_ready)
    return reject(409, "409 Conflict", "TOPOLOGY_LAYOUT_PACKAGE_MISSING");

  TopologySnapshot snapshot;
  if (!topology_validate_follower(*g_config, topology_json, requested_hash,
                                 layout_json, snapshot, error)) {
    ESP_LOGW(kTag, "topology rejected from node %u: %s",
             node_request.node_id, error.c_str());
    return reject(422, "422 Unprocessable Entity", "TOPOLOGY_INVALID");
  }
  std::string local_slot;
  uint8_t local_rotation = 0;
  if (!topology_local_binding(snapshot, g_config->nodeId, local_slot,
                              local_rotation, error) ||
      !config_store_panel_binding(local_slot, local_rotation, error))
    return reject(503, "503 Service Unavailable", "LOCAL_PANEL_BINDING_SAVE_FAILED");
  g_config->slotId = local_slot;
  g_config->rotation = local_rotation;
  if (snapshot.revision < g_state->topologyRevision)
    return reject(409, "409 Conflict", "TOPOLOGY_REVISION_STALE");
  if (!apply_topology_snapshot(snapshot, error))
    return reject(503, "503 Service Unavailable", "TOPOLOGY_APPLY_FAILED");

  cJSON* response = cJSON_CreateObject();
  if (!response) return reject(503, "503 Service Unavailable", "NO_MEMORY");
  cJSON_AddStringToObject(response, "status", "topology_applied");
  cJSON_AddStringToObject(response, "topologyHash", snapshot.hash.c_str());
  cJSON_AddNumberToObject(response, "topologyRevision", snapshot.revision);
  cJSON_AddStringToObject(response, "layoutId", snapshot.layout_id.c_str());
  const esp_err_t result = send_signed_node_object(req, 200, "200 OK", path,
      node_request, node_request.session_key, response);
  cJSON_Delete(response);
  return result;
}

esp_err_t node_maintenance(httpd_req_t* req) {
  AuthorizedNodeJsonRequest node_request;
  std::string auth_error;
  if (!authorize_node_json_request(req, "POST", "/node/v2/maintenance",
          256, node_request, auth_error))
    return error_response(req, "401 Unauthorized", "NODE_AUTH_FAILED"), ESP_OK;
  const std::string path = "/node/v2/maintenance";
  const auto reject = [&](uint16_t status, const char* status_text,
                          const char* code) {
    return send_signed_node_error(req, status, status_text, path,
        node_request.sequence, node_request.nonce, node_request.session_key,
        code);
  };
  if (g_config->role != Role::Follower ||
      node_request.node_id != g_config->masterNodeId)
    return reject(403, "403 Forbidden", "MASTER_REQUIRED");
  cJSON* body = cJSON_ParseWithLength(node_request.body.data(),
                                      node_request.body.size());
  if (!exact_object_keys(body, {"enabled"}) ||
      !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(body, "enabled"))) {
    cJSON_Delete(body);
    return reject(400, "400 Bad Request", "INVALID_REQUEST");
  }
  const bool enable = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "enabled"));
  cJSON_Delete(body);
  if (sync_runtime_transaction_active())
    return reject(409, "409 Conflict", "SYNC_BUSY");
  if (enable) {
    PlaybackState stopped;
    if (!sync_runtime_state_snapshot(stopped)) stopped = *g_state;
    stopped.running = false; stopped.mode = "stopped"; stopped.blackout = true;
    stopped.positionTicks = 0; stopped.anchorUs = 0; stopped.effectiveAtUs = 0;
    *g_state = stopped;
    if (!sync_runtime_update_state(stopped))
      return reject(503, "503 Service Unavailable", "SYNC_STATE_UNAVAILABLE");
    hub75_black();
  } else if (g_state->running || g_state->mode != "stopped" || !g_state->blackout)
    return reject(409, "409 Conflict", "MAINTENANCE_REQUIRES_STOPPED_BLACKOUT");
  uint8_t current = g_maintenance_state.load(std::memory_order_relaxed);
  if (current == 2)
    return reject(409, "409 Conflict", "DEPLOYMENT_BUSY");
  if ((enable && current == 0) || (!enable && current == 1)) {
    uint8_t expected = current;
    if (!g_maintenance_state.compare_exchange_strong(
            expected, enable ? 1 : 0,
            std::memory_order_relaxed))
      return reject(409, "409 Conflict", "MAINTENANCE_BUSY");
  }
  cJSON* response = cJSON_CreateObject();
  if (!response) {
    return reject(503, "503 Service Unavailable", "NO_MEMORY");
  }
  cJSON_AddStringToObject(response, "status", enable ? "maintenance" : "normal");
  cJSON_AddBoolToObject(response, "maintenance", enable);
  const esp_err_t result = send_signed_node_object(req, 200, "200 OK", path,
      node_request, node_request.session_key, response);
  cJSON_Delete(response);
  return result;
}

esp_err_t node_activate_candidate(httpd_req_t* req) {
  AuthorizedNodeJsonRequest node_request;
  std::string auth_error;
  if (!authorize_node_json_request(req, "POST", "/node/v2/activate",
          512, node_request, auth_error))
    return error_response(req, "401 Unauthorized", "NODE_AUTH_FAILED"), ESP_OK;
  const std::string path = "/node/v2/activate";
  const auto reject = [&](uint16_t status, const char* status_text,
                          const char* code) {
    return send_signed_node_error(req, status, status_text, path,
        node_request.sequence, node_request.nonce, node_request.session_key,
        code);
  };
  if (g_config->role != Role::Follower ||
      node_request.node_id != g_config->masterNodeId)
    return reject(403, "403 Forbidden", "MASTER_REQUIRED");
  cJSON* body = cJSON_ParseWithLength(node_request.body.data(),
                                      node_request.body.size());
  const cJSON* hash_field = body
      ? cJSON_GetObjectItemCaseSensitive(body, "packageHash") : nullptr;
  uint32_t target_generation = 0, target_revision = 0;
  if (!exact_object_keys(body, {"packageHash", "generation", "stateRevision"}) ||
      !cJSON_IsString(hash_field) || !valid_sha256(hash_field->valuestring) ||
      !json_uint32(cJSON_GetObjectItemCaseSensitive(body, "generation"),
                  target_generation) ||
      !json_uint32(cJSON_GetObjectItemCaseSensitive(body, "stateRevision"),
                   target_revision) || target_revision == 0) {
    cJSON_Delete(body);
    return reject(400, "400 Bad Request", "INVALID_REQUEST");
  }
  const std::string requested_hash(hash_field->valuestring);
  cJSON_Delete(body);
  if (g_state->running || g_state->mode != "stopped" || !g_state->blackout ||
      g_maintenance_state.load(std::memory_order_relaxed) != 1)
    return reject(409, "409 Conflict", "MAINTENANCE_STOPPED_BLACKOUT_REQUIRED");
  if (sync_runtime_transaction_active())
    return reject(409, "409 Conflict", "SYNC_BUSY");
  ContentState content;
  std::string error;
  if (!content_get_state(content, error))
    return reject(503, "503 Service Unavailable", "CONTENT_UNAVAILABLE");
  const bool active_matches = content.has_active &&
      content.active_hash == requested_hash;
  const bool candidate_matches = content.has_candidate &&
      content.candidate_hash == requested_hash;
  if (!active_matches && !candidate_matches)
    return reject(409, "409 Conflict", "CANDIDATE_NOT_FOUND");
  if (target_generation < content.generation)
    return reject(409, "409 Conflict", "CONTENT_GENERATION_CONFLICT");
  if (target_revision < g_state->revision)
    return reject(409, "409 Conflict", "STATE_REVISION_CONFLICT");
  const bool state_already_active = g_state->packageHash == requested_hash;
  if (candidate_matches) {
    if (target_generation == content.generation && !active_matches)
      return reject(409, "409 Conflict", "CONTENT_GENERATION_CONFLICT");
    if (!content_activate(target_generation, error))
      return reject(500, "500 Internal Server Error", "PACKAGE_ACTIVATION_FAILED");
  } else if (target_generation > content.generation &&
             !content_advance_active_generation(requested_hash,
                                                target_generation, error)) {
    return reject(500, "500 Internal Server Error", "CONTENT_GENERATION_UPDATE_FAILED");
  }
  ContentState activated;
  if (!content_get_state(activated, error) || !activated.has_active ||
      activated.active_hash != requested_hash ||
      activated.generation != target_generation)
    return reject(503, "503 Service Unavailable", "ACTIVE_CONTENT_UNVERIFIED");
  g_state->revision = target_revision;
  g_state->packageHash = requested_hash;
  g_state->running = false;
  g_state->mode = "stopped";
  g_state->blackout = true;
  bool state_updated = false;
  for (int attempt = 0; attempt < 3 && !state_updated; ++attempt)
    state_updated = sync_runtime_update_state(*g_state);
  if (!state_updated)
    return reject(503, "503 Service Unavailable", "SYNC_STATE_UNAVAILABLE");
  cJSON* response = cJSON_CreateObject();
  if (!response) return reject(503, "503 Service Unavailable", "NO_MEMORY");
  cJSON_AddStringToObject(response, "status", state_already_active
      ? "already_active_but_panel_dark" : "active_but_panel_dark");
  cJSON_AddStringToObject(response, "packageHash", requested_hash.c_str());
  cJSON_AddNumberToObject(response, "stateRevision", g_state->revision);
  cJSON_AddNumberToObject(response, "generation", activated.generation);
  const esp_err_t result = send_signed_node_object(req, 200, "200 OK", path,
      node_request, node_request.session_key, response);
  cJSON_Delete(response);
  return result;
}

esp_err_t maintenance_enter(httpd_req_t* req) { return maintenance_change(req, true); }
esp_err_t maintenance_exit(httpd_req_t* req) { return maintenance_change(req, false); }

bool send_json_status(httpd_req_t* req, const char* status, cJSON* root) {
  char* json = cJSON_PrintUnformatted(root);
  if (!json) return false;
  json_response(req, status, json);
  cJSON_free(json);
  return true;
}

bool parse_revision_header(httpd_req_t* req, uint32_t& revision) {
  std::string text;
  if (!header(req, "X-Expected-Revision", text) || text.empty() || text.size() > 10) return false;
  uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<unsigned>(c - '0');
    if (value > UINT32_MAX) return false;
  }
  revision = static_cast<uint32_t>(value);
  return true;
}

bool valid_sha256(const std::string& value) {
  if (value.size() != 64) return false;
  for (const char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

esp_err_t upload_package(httpd_req_t* req) {
  if (!authenticated(req, true)) return error_response(req, "403 Forbidden", "AUTH_OR_CSRF_REQUIRED"), ESP_OK;
  if (!master_only_mutation(req)) return ESP_OK;
  uint32_t expected_revision = 0;
  std::string content_type, declared_hash;
  if (!parse_revision_header(req, expected_revision) ||
      expected_revision != g_revision.load(std::memory_order_relaxed))
    return error_response(req, "409 Conflict", "REVISION_CONFLICT"), ESP_OK;
  if (!header(req, "Content-Type", content_type) || content_type != "application/vnd.ledpanel.package" ||
      !header(req, "X-Content-SHA256", declared_hash) || !valid_sha256(declared_hash))
    return error_response(req, "400 Bad Request", "INVALID_PACKAGE_HEADERS"), ESP_OK;
  if (req->content_len < 64 || static_cast<size_t>(req->content_len) > kMaxPackageBytes)
    return error_response(req, "413 Payload Too Large", "PACKAGE_SIZE_LIMIT"), ESP_OK;
  if (sync_runtime_transaction_active())
    return error_response(req, "409 Conflict", "SYNC_BUSY"), ESP_OK;
  uint8_t expected_state = 1;
  if (!g_maintenance_state.compare_exchange_strong(expected_state, 2)) {
    const char* code = expected_state == 0 ? "MAINTENANCE_REQUIRED" : "DEPLOYMENT_BUSY";
    return error_response(req, "409 Conflict", code), ESP_OK;
  }
  if (g_topology_sync_busy.load(std::memory_order_acquire)) {
    g_maintenance_state.store(1, std::memory_order_relaxed);
    return error_response(req, "409 Conflict", "TOPOLOGY_SYNC_BUSY"), ESP_OK;
  }
  struct UploadStateReset {
    ~UploadStateReset() { g_maintenance_state.store(1); }
  } upload_state_reset;
  auto* bytes = static_cast<uint8_t*>(heap_caps_malloc(
      req->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!bytes) return error_response(req, "503 Service Unavailable", "PACKAGE_BUFFER_UNAVAILABLE"), ESP_OK;
  size_t received = 0;
  while (received < static_cast<size_t>(req->content_len)) {
    const int n = httpd_req_recv(req, reinterpret_cast<char*>(bytes + received),
                                 static_cast<size_t>(req->content_len) - received);
    if (n <= 0) {
      std::memset(bytes, 0, received);
      heap_caps_free(bytes);
      return error_response(req, "400 Bad Request", "PACKAGE_BODY_INCOMPLETE"), ESP_OK;
    }
    received += static_cast<size_t>(n);
  }
  std::string package_hash, validation_error;
  if (!validate_runtime_package(bytes, received, package_hash, validation_error) ||
      !constant_equal(package_hash, declared_hash)) {
    std::memset(bytes, 0, received);
    heap_caps_free(bytes);
    ESP_LOGW(kTag, "runtime package rejected: %s", validation_error.c_str());
    return error_response(req, "422 Unprocessable Entity", "PACKAGE_INVALID"), ESP_OK;
  }
  std::string store_error;
  const bool stored = content_stage(bytes, received, package_hash, store_error);
  std::memset(bytes, 0, received);
  heap_caps_free(bytes);
  if (!stored) {
    ESP_LOGE(kTag, "candidate package write failed: %s", store_error.c_str());
    return error_response(req, "500 Internal Server Error", "CANDIDATE_WRITE_FAILED"), ESP_OK;
  }
  const uint32_t accepted_revision = g_revision.fetch_add(1, std::memory_order_relaxed) + 1;
  g_topology_dirty.store(true, std::memory_order_relaxed);
  cJSON* response = cJSON_CreateObject();
  cJSON_AddStringToObject(response, "status", "validated_candidate");
  cJSON_AddStringToObject(response, "packageHash", package_hash.c_str());
  cJSON_AddNumberToObject(response, "packageBytes", received);
  cJSON_AddNumberToObject(response, "acceptedRevision", accepted_revision);
  cJSON_AddStringToObject(response, "maintenanceScope",
                          deployment_scope(g_state->layoutId).c_str());
  send_json_status(req, "201 Created", response);
  cJSON_Delete(response);
  return ESP_OK;
}

esp_err_t rig_get(httpd_req_t* req) {
  if (!authenticated(req, false))
    return error_response(req, "401 Unauthorized", "UNAUTHENTICATED"), ESP_OK;
  if (g_config->role != Role::Master)
    return error_response(req, "403 Forbidden", "MASTER_REQUIRED"), ESP_OK;

  RigSlot slot;
  std::string error;
  if (!rig_load(slot, error) || !slot.valid)
    return error_response(req, "503 Service Unavailable", "RIG_UNAVAILABLE"), ESP_OK;
  cJSON* rig = cJSON_ParseWithLength(slot.json.data(), slot.json.size());
  if (!cJSON_IsObject(rig)) {
    cJSON_Delete(rig);
    return error_response(req, "503 Service Unavailable", "RIG_INVALID"), ESP_OK;
  }
  const cJSON* revision = cJSON_GetObjectItemCaseSensitive(rig, "revision");
  uint32_t rig_revision = 0;
  if (!json_uint32(revision, rig_revision)) {
    cJSON_Delete(rig);
    return error_response(req, "503 Service Unavailable", "RIG_INVALID"), ESP_OK;
  }

  bool topology_synchronized = g_topology_snapshot_valid &&
      !g_topology_dirty.load(std::memory_order_relaxed) &&
      g_topology_snapshot.hash == g_state->topologyHash &&
      g_topology_snapshot.layout_id == g_state->layoutId &&
      rig_revision == g_state->topologyRevision &&
      g_config->topologyHash.size() == 64 &&
      g_config->topologyHash == g_state->topologyHash;
  if (topology_synchronized) {
    std::vector<RigFollowerIdentity> followers;
    if (!rig_display_followers(*g_config, g_state->layoutId, followers, error)) {
      topology_synchronized = false;
    } else {
      std::vector<SyncPeerSnapshot> peers;
      sync_runtime_peer_snapshot(peers);
      for (const auto& follower : followers) {
        const auto peer = std::find_if(peers.begin(), peers.end(),
            [&follower, this_hash = g_state->topologyHash,
             this_revision = g_state->topologyRevision](
                const SyncPeerSnapshot& item) {
              return item.node_id == follower.node_id &&
                  item.hardware_id == follower.hardware_id && item.online &&
                  item.authenticated && item.topology_revision == this_revision &&
                  item.topology_hash == this_hash;
            });
        if (peer == peers.end()) {
          topology_synchronized = false;
          break;
        }
      }
    }
  }

  cJSON* response = cJSON_CreateObject();
  cJSON_AddItemToObject(response, "rig", rig);
  cJSON_AddNumberToObject(response, "controlRevision",
                          g_revision.load(std::memory_order_relaxed));
  cJSON_AddNumberToObject(response, "topologyRevision", g_state->topologyRevision);
  cJSON_AddNumberToObject(response, "rigRevision", rig_revision);
  cJSON_AddBoolToObject(response, "maintenance", g_maintenance_state.load() == 1);
  cJSON_AddStringToObject(response, "maintenanceScope",
                          deployment_scope(g_state->layoutId).c_str());
  cJSON_AddBoolToObject(response, "editable",
                        !g_state->running && g_state->mode == "stopped" &&
                            g_state->blackout && g_maintenance_state.load() == 1);
  cJSON_AddBoolToObject(response, "topologySynchronized", topology_synchronized);
  if (g_config->topologyHash.size() == 64)
    cJSON_AddStringToObject(response, "topologyHash", g_config->topologyHash.c_str());
  else
    cJSON_AddNullToObject(response, "topologyHash");
  send_json(req, response);
  cJSON_Delete(response);
  return ESP_OK;
}

esp_err_t rig_put(httpd_req_t* req) {
  if (!authenticated(req, true))
    return error_response(req, "403 Forbidden", "AUTH_OR_CSRF_REQUIRED"), ESP_OK;
  if (!master_only_mutation(req)) return ESP_OK;
  if (sync_runtime_transaction_active())
    return error_response(req, "409 Conflict", "SYNC_BUSY"), ESP_OK;
  if (!g_rig_store_ok)
    return error_response(req, "503 Service Unavailable", "RIG_VALIDATION_UNAVAILABLE"), ESP_OK;
  if (req->content_len <= 0)
    return error_response(req, "400 Bad Request", "INVALID_REQUEST"), ESP_OK;
  if (static_cast<size_t>(req->content_len) > 40 * 1024)
    return error_response(req, "413 Payload Too Large", "RIG_BODY_TOO_LARGE"), ESP_OK;
  if (g_state->running || g_state->mode != "stopped" || !g_state->blackout ||
      g_maintenance_state.load() != 1)
    return error_response(req, "409 Conflict", "RIG_REQUIRES_MAINTENANCE_STOPPED_BLACKOUT"), ESP_OK;

  std::string body;
  cJSON* request = parse_body_with_limit(req, body, 40 * 1024);
  if (!exact_object_keys(request, {"requestId", "expectedRevision",
                                   "expectedTopologyRevision", "rig"}) ||
      !valid_request_uuid(cJSON_GetObjectItemCaseSensitive(request, "requestId"))) {
    cJSON_Delete(request);
    return error_response(req, "400 Bad Request", "INVALID_REQUEST"), ESP_OK;
  }
  uint32_t expected_control_revision = 0, expected_topology_revision = 0;
  const cJSON* requested_rig = cJSON_GetObjectItemCaseSensitive(request, "rig");
  if (!json_uint32(cJSON_GetObjectItemCaseSensitive(request, "expectedRevision"),
                   expected_control_revision) ||
      !json_uint32(cJSON_GetObjectItemCaseSensitive(request, "expectedTopologyRevision"),
                   expected_topology_revision) || !cJSON_IsObject(requested_rig)) {
    cJSON_Delete(request);
    return error_response(req, "400 Bad Request", "INVALID_REQUEST"), ESP_OK;
  }
  if (expected_control_revision != g_revision.load(std::memory_order_relaxed)) {
    cJSON_Delete(request);
    return error_response(req, "409 Conflict", "REVISION_CONFLICT"), ESP_OK;
  }

  RigSlot current;
  std::string error;
  if (!rig_load(current, error) || !current.valid) {
    cJSON_Delete(request);
    return error_response(req, "503 Service Unavailable", "RIG_UNAVAILABLE"), ESP_OK;
  }
  cJSON* current_json = cJSON_ParseWithLength(current.json.data(), current.json.size());
  uint32_t current_topology_revision = 0;
  const cJSON* current_revision = current_json
      ? cJSON_GetObjectItemCaseSensitive(current_json, "revision") : nullptr;
  const bool current_revision_valid = json_uint32(current_revision, current_topology_revision);
  cJSON_Delete(current_json);
  if (!current_revision_valid) {
    cJSON_Delete(request);
    return error_response(req, "503 Service Unavailable", "RIG_INVALID"), ESP_OK;
  }
  if (expected_topology_revision != current_topology_revision) {
    cJSON_Delete(request);
    return error_response(req, "409 Conflict", "TOPOLOGY_REVISION_CONFLICT"), ESP_OK;
  }
  uint32_t submitted_revision = 0;
  if (!json_uint32(cJSON_GetObjectItemCaseSensitive(requested_rig, "revision"),
                   submitted_revision) || submitted_revision != current_topology_revision) {
    cJSON_Delete(request);
    return error_response(req, "409 Conflict", "TOPOLOGY_REVISION_CONFLICT"), ESP_OK;
  }
  if (current_topology_revision == UINT32_MAX ||
      expected_control_revision == UINT32_MAX) {
    cJSON_Delete(request);
    return error_response(req, "409 Conflict", "REVISION_EXHAUSTED"), ESP_OK;
  }

  cJSON* next_rig = cJSON_Duplicate(requested_rig, true);
  cJSON_Delete(request);
  if (!next_rig) return error_response(req, "500 Internal Server Error", "OUT_OF_MEMORY"), ESP_OK;
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(next_rig, "revision"),
                       current_topology_revision + 1);
  char* serialized = cJSON_PrintUnformatted(next_rig);
  cJSON_Delete(next_rig);
  if (!serialized)
    return error_response(req, "500 Internal Server Error", "RIG_SERIALIZATION_FAILED"), ESP_OK;
  const size_t serialized_length = std::strlen(serialized);
  if (serialized_length > 32 * 1024) {
    cJSON_free(serialized);
    return error_response(req, "413 Payload Too Large", "RIG_TOO_LARGE"), ESP_OK;
  }
  const std::string next_json(serialized, serialized_length);
  cJSON_free(serialized);
  bool topology_busy_expected = false;
  if (!g_topology_sync_busy.compare_exchange_strong(
          topology_busy_expected, true, std::memory_order_acq_rel))
    return error_response(req, "409 Conflict", "TOPOLOGY_SYNC_BUSY"), ESP_OK;
  if (g_maintenance_state.load(std::memory_order_acquire) != 1) {
    g_topology_sync_busy.store(false, std::memory_order_release);
    return error_response(req, "409 Conflict", "DEPLOYMENT_BUSY"), ESP_OK;
  }
  if (!rig_commit(*g_config, next_json, error)) {
    g_topology_sync_busy.store(false, std::memory_order_release);
    ESP_LOGW(kTag, "rig update rejected: %s", error.c_str());
    return error_response(req, "422 Unprocessable Entity", "RIG_INVALID"), ESP_OK;
  }

  const uint32_t accepted_revision = expected_control_revision + 1;
  g_revision.store(accepted_revision, std::memory_order_relaxed);
  g_topology_dirty.store(true, std::memory_order_relaxed);
  g_topology_sync_busy.store(false, std::memory_order_release);
  cJSON* response = cJSON_CreateObject();
  cJSON_AddStringToObject(response, "status", "saved_local_pending_topology");
  cJSON_AddNumberToObject(response, "acceptedRevision", accepted_revision);
  cJSON_AddNumberToObject(response, "rigRevision", current_topology_revision + 1);
  cJSON_AddNumberToObject(response, "topologyRevision", g_state->topologyRevision);
  cJSON_AddBoolToObject(response, "topologySynchronized", false);
  if (!send_json(req, response))
    error_response(req, "500 Internal Server Error", "RESPONSE_SERIALIZATION_FAILED");
  cJSON_Delete(response);
  return ESP_OK;
}

esp_err_t activate_package(httpd_req_t* req) {
  if (!authenticated(req, true)) return error_response(req, "403 Forbidden", "AUTH_OR_CSRF_REQUIRED"), ESP_OK;
  if (!master_only_mutation(req)) return ESP_OK;
  if (sync_runtime_transaction_active())
    return error_response(req, "409 Conflict", "SYNC_BUSY"), ESP_OK;
  if (g_maintenance_state.load() != 1 || g_state->running ||
      g_state->mode != "stopped" || !g_state->blackout)
    return error_response(req, "409 Conflict", "MAINTENANCE_STOPPED_BLACKOUT_REQUIRED"), ESP_OK;
  uint8_t expected_maintenance = 1;
  if (!g_maintenance_state.compare_exchange_strong(expected_maintenance, 2))
    return error_response(req, "409 Conflict", "DEPLOYMENT_BUSY"), ESP_OK;
  struct ActivationMaintenanceReset {
    std::atomic<uint8_t>& state;
    ~ActivationMaintenanceReset() { state.store(1, std::memory_order_relaxed); }
  } maintenance_reset{g_maintenance_state};
  if (g_topology_sync_busy.load(std::memory_order_acquire))
    return error_response(req, "409 Conflict", "TOPOLOGY_SYNC_BUSY"), ESP_OK;
  std::string body;
  cJSON* root = parse_body(req, body);
  const cJSON* revision = root ? cJSON_GetObjectItemCaseSensitive(root, "expectedRevision") : nullptr;
  const cJSON* hash_value = root ? cJSON_GetObjectItemCaseSensitive(root, "packageHash") : nullptr;
  if (!cJSON_IsNumber(revision) || revision->valuedouble != revision->valueint ||
      revision->valueint < 0 || static_cast<uint32_t>(revision->valueint) !=
          g_revision.load(std::memory_order_relaxed)) {
    cJSON_Delete(root);
    return error_response(req, "409 Conflict", "REVISION_CONFLICT"), ESP_OK;
  }
  if (static_cast<uint32_t>(revision->valueint) == UINT32_MAX) {
    cJSON_Delete(root);
    return error_response(req, "409 Conflict", "REVISION_EXHAUSTED"), ESP_OK;
  }
  const uint32_t expected_revision = static_cast<uint32_t>(revision->valueint);
  if (!cJSON_IsString(hash_value) || !valid_sha256(hash_value->valuestring)) {
    cJSON_Delete(root);
    return error_response(req, "400 Bad Request", "INVALID_PACKAGE_HASH"), ESP_OK;
  }
  const std::string requested_hash(hash_value->valuestring);
  cJSON_Delete(root);
  if (g_state->revision == UINT32_MAX)
    return error_response(req, "409 Conflict", "STATE_REVISION_EXHAUSTED"), ESP_OK;
  ContentState content;
  std::string error;
  if (!content_get_state(content, error) || !content.has_candidate ||
      !constant_equal(content.candidate_hash, requested_hash))
    return error_response(req, "409 Conflict", "CANDIDATE_NOT_FOUND"), ESP_OK;
  TopologySnapshot candidate_topology;
  if (!build_candidate_topology(candidate_topology, error) ||
      candidate_topology.revision != g_state->topologyRevision ||
      candidate_topology.hash != g_state->topologyHash ||
      candidate_topology.layout_id != g_state->layoutId ||
      g_config->topologyHash != candidate_topology.hash)
    return error_response(req, "409 Conflict", "TOPOLOGY_NOT_SYNCHRONIZED"), ESP_OK;
  std::vector<RigFollowerIdentity> followers;
  if (!rig_display_followers(*g_config, candidate_topology.layout_id,
                             followers, error))
    return error_response(req, "503 Service Unavailable", "RIG_UNAVAILABLE"), ESP_OK;
  const uint32_t target_state_revision = g_state->revision + 1;
  uint32_t max_content_generation = content.generation;
  std::vector<SyncPeerSnapshot> peers;
  sync_runtime_peer_snapshot(peers);
  for (const auto& follower : followers) {
    const auto peer = std::find_if(peers.begin(), peers.end(),
        [&follower](const SyncPeerSnapshot& item) {
          return item.node_id == follower.node_id &&
              item.hardware_id == follower.hardware_id && item.enabled &&
              item.online && item.authenticated;
        });
    if (peer == peers.end())
      return error_response(req, "503 Service Unavailable", "PEER_NOT_READY"), ESP_OK;
    if (peer->topology_revision != g_state->topologyRevision ||
        peer->topology_hash != g_state->topologyHash)
      return error_response(req, "409 Conflict", "TOPOLOGY_NOT_SYNCHRONIZED"), ESP_OK;
    NodeContentMetadata remote_content;
    if (!get_remote_content_metadata(follower, remote_content, error))
      return error_response(req, "503 Service Unavailable", "PEER_NOT_READY"), ESP_OK;
    max_content_generation = std::max(max_content_generation,
                                      remote_content.generation);
    const bool revision_current = peer->revision == g_state->revision;
    const bool revision_after_activation = peer->revision == target_state_revision;
    const bool already_active = remote_content.active_hash == requested_hash &&
        (revision_current || revision_after_activation);
    const bool candidate_staged = follower_candidate_ready(remote_content,
        requested_hash, g_state->packageHash, revision_current);
    if (!already_active && !candidate_staged) {
      ESP_LOGW(kTag, "follower candidate not ready: node=%u revision=%u expected=%u generation=%u",
          follower.node_id, peer->revision, g_state->revision, remote_content.generation);
      return error_response(req, "409 Conflict", "FOLLOWER_CANDIDATE_NOT_READY"), ESP_OK;
    }
  }
  if (max_content_generation == UINT32_MAX)
    return error_response(req, "409 Conflict", "CONTENT_GENERATION_EXHAUSTED"), ESP_OK;
  const uint32_t target_generation = max_content_generation + 1;
  for (const auto& follower : followers) {
    std::string follower_error;
    if (!activate_remote_candidate(follower.node_id, requested_hash,
                                   target_generation, target_state_revision,
                                   follower_error)) {
      ESP_LOGW(kTag, "package activation failed at node %u: %s",
               follower.node_id, follower_error.c_str());
      return error_response(req, "503 Service Unavailable",
                            "FOLLOWER_ACTIVATION_FAILED"), ESP_OK;
    }
  }
  ContentState current_content;
  if (!content_get_state(current_content, error) ||
      !current_content.has_candidate ||
      !constant_equal(current_content.candidate_hash, requested_hash))
    return error_response(req, "409 Conflict", "CANDIDATE_CHANGED"), ESP_OK;
  if (!content_activate(target_generation, error))
    return error_response(req, "500 Internal Server Error", "PACKAGE_ACTIVATION_FAILED"), ESP_OK;
  ContentState activated;
  if (!content_get_state(activated, error) || !activated.has_active ||
      activated.active_hash != requested_hash)
    return error_response(req, "503 Service Unavailable", "ACTIVE_CONTENT_UNVERIFIED"), ESP_OK;
  g_state->packageHash = requested_hash;
  g_state->running = false;
  g_state->mode = "stopped";
  g_state->blackout = true;
  g_state->revision = target_state_revision;
  const uint32_t accepted_revision = expected_revision + 1;
  g_revision.store(accepted_revision, std::memory_order_relaxed);
  bool state_updated = false;
  for (int attempt = 0; attempt < 3 && !state_updated; ++attempt)
    state_updated = sync_runtime_update_state(*g_state);
  if (!state_updated)
    return error_response(req, "503 Service Unavailable", "SYNC_STATE_UNAVAILABLE"), ESP_OK;
  sync_runtime_broadcast_blackout(g_state->revision, "user");
  cJSON* response = cJSON_CreateObject();
  cJSON_AddStringToObject(response, "status", "active_but_panel_dark");
  cJSON_AddStringToObject(response, "packageHash", requested_hash.c_str());
  cJSON_AddNumberToObject(response, "acceptedRevision", accepted_revision);
  cJSON_AddNumberToObject(response, "contentGeneration", activated.generation);
  if (hub75_diagnostic_supported()) cJSON_AddNullToObject(response, "panelFault");
  else cJSON_AddStringToObject(response, "panelFault", "PANEL_DMA_UNAVAILABLE");
  send_json(req, response);
  cJSON_Delete(response);
  return ESP_OK;
}

struct StaticFile {
  const char* path;
  const uint8_t* start;
  const uint8_t* end;
  const char* content_type;
};

constexpr StaticFile kStaticFiles[] = {
    {"/", _binary_lp_login_html_txt_start, _binary_lp_login_html_txt_end, "text/html; charset=utf-8"},
    {"/login.mjs", _binary_lp_login_module_txt_start, _binary_lp_login_module_txt_end, "text/javascript; charset=utf-8"},
    {"/portal.css", _binary_lp_portal_style_txt_start, _binary_lp_portal_style_txt_end, "text/css; charset=utf-8"},
    {"/editor", _binary_lp_editor_html_txt_start, _binary_lp_editor_html_txt_end, "text/html; charset=utf-8"},
    {"/app.js", _binary_lp_editor_app_txt_start, _binary_lp_editor_app_txt_end, "text/javascript; charset=utf-8"},
    {"/style.css", _binary_lp_editor_style_txt_start, _binary_lp_editor_style_txt_end, "text/css; charset=utf-8"},
    {"/apps/editor/animation.mjs", _binary_lp_editor_animation_txt_start, _binary_lp_editor_animation_txt_end, "text/javascript; charset=utf-8"},
    {"/apps/editor/deployment.mjs", _binary_lp_editor_deployment_txt_start, _binary_lp_editor_deployment_txt_end, "text/javascript; charset=utf-8"},
    {"/apps/editor/media.mjs", _binary_lp_editor_media_txt_start, _binary_lp_editor_media_txt_end, "text/javascript; charset=utf-8"},
    {"/packages/formats/index.mjs", _binary_lp_formats_index_txt_start, _binary_lp_formats_index_txt_end, "text/javascript; charset=utf-8"},
    {"/packages/formats/browser.mjs", _binary_lp_formats_browser_txt_start, _binary_lp_formats_browser_txt_end, "text/javascript; charset=utf-8"},
    {"/packages/formats/crypto.mjs", _binary_lp_formats_crypto_txt_start, _binary_lp_formats_crypto_txt_end, "text/javascript; charset=utf-8"},
    {"/packages/formats/zip.mjs", _binary_lp_formats_zip_txt_start, _binary_lp_formats_zip_txt_end, "text/javascript; charset=utf-8"},
    {"/packages/render-core/index.mjs", _binary_lp_renderer_index_txt_start, _binary_lp_renderer_index_txt_end, "text/javascript; charset=utf-8"},
};

esp_err_t static_file(httpd_req_t* req) {
  std::string_view request_path(req->uri);
  const size_t query_pos = request_path.find('?');
  if (query_pos != std::string_view::npos) request_path = request_path.substr(0, query_pos);
  if (request_path == "/editor/") request_path = "/editor";

  const StaticFile* found = nullptr;
  for (const auto& file : kStaticFiles) {
    if (request_path == file.path) {
      found = &file;
      break;
    }
  }
  if (!found) return error_response(req, "404 Not Found", "NOT_FOUND"), ESP_OK;
  if (request_path == "/editor" && !authenticated(req, false))
    return error_response(req, "401 Unauthorized", "UNAUTHENTICATED"), ESP_OK;
  httpd_resp_set_type(req, found->content_type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Content-Security-Policy",
                     "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; style-src 'self'; "
                     "connect-src 'self'; img-src 'self' blob: data:; object-src 'none'; "
                     "base-uri 'self'; frame-ancestors 'none'");
  size_t content_length = found->end - found->start;
  // ESP-IDF's target_add_binary_data(..., TEXT) includes a trailing NUL byte.
  // It terminates C strings but must not be sent as part of JavaScript modules.
  if (content_length && found->end[-1] == '\0') --content_length;
  return httpd_resp_send(req, reinterpret_cast<const char*>(found->start),
                         content_length);
}

void register_uri(httpd_handle_t server, const char* path, httpd_method_t method,
                  esp_err_t (*handler_fn)(httpd_req_t*)) {
  httpd_uri_t uri = {};
  uri.uri = path;
  uri.method = method;
  uri.handler = handler_fn;
  uri.user_ctx = nullptr;
  const esp_err_t result = httpd_register_uri_handler(server, &uri);
  if (result != ESP_OK) ESP_LOGE(kTag, "URI registration failed: %s (%s)", path,
                                 esp_err_to_name(result));
}
#include "panel_controls.inc"
}  // namespace

void web_api_apply_remote_blackout(uint32_t revision) {
  if (!g_state || revision < g_state->revision) return;
  g_state->blackout = true;
  g_state->revision = revision;
}

bool web_api_maintenance_active() {
  return g_maintenance_state.load(std::memory_order_relaxed) == 1;
}

bool web_api_usb_nightlight(bool enable, std::string& error) {
  if (!g_config || g_config->role != Role::Master) {
    error = "MASTER_REQUIRED";
    return false;
  }
  if (!enable) {
    if (hub75_nightlight_active()) hub75_stop_nightlight();
    else hub75_black();
    error.clear();
    return true;
  }
  if (hub75_nightlight_active()) {
    error.clear();
    return true;
  }
  if (!g_state || g_state->running || g_state->mode != "stopped" ||
      !g_state->blackout || g_maintenance_state.load(std::memory_order_acquire) != 0) {
    error = "NIGHTLIGHT_REQUIRES_STOPPED_BLACKOUT";
    return false;
  }
  if (!hub75_diagnostic_supported() || !hub75_start_startup_demo()) {
    error = "PANEL_NIGHTLIGHT_UNAVAILABLE";
    return false;
  }
  error.clear();
  return true;
}

bool web_api_usb_panel_test(std::string& error) {
  return start_panel_diagnostic(error);
}

void web_api_apply_remote_state(const PlaybackState& state) {
  if (!g_state || state.revision < g_state->revision) return;
  *g_state = state;
}

void web_api_start(httpd_handle_t server, NodeConfig* config, PlaybackState* state,
                   uint64_t* boot_id, bool sync_protocol_ok,
                   bool sync_runtime_ok, bool rig_store_ok,
                   bool node_http_auth_ok, bool runtime_package_ok,
                   bool topology_ok, const char* sync_runtime_error) {
  g_config = config;
  g_state = state;
  g_boot_id = boot_id;
  g_maintenance_state.store(0);
  g_restore_pending.store(config->provisioned && config->role == Role::Master);
  g_sync_protocol_ok = sync_protocol_ok;
  g_sync_runtime_ok = sync_runtime_ok;
  g_sync_runtime_error = sync_runtime_error ? sync_runtime_error : "";
  g_rig_store_ok = rig_store_ok;
  g_node_http_auth_ok = node_http_auth_ok && ipv4_peer_address_self_test();
  g_runtime_package_ok = runtime_package_ok && follower_activation_self_test();
  g_topology_ok = topology_ok;
  register_uri(server, "/health", HTTP_GET, health);
  register_uri(server, "/api/v2/health", HTTP_GET, health);
  register_uri(server, "/node/v2/health", HTTP_GET, node_health);
  register_uri(server, "/node/v2/candidate", HTTP_GET, node_candidate);
  register_uri(server, "/node/v2/topology", HTTP_POST, node_topology);
  register_uri(server, "/node/v2/maintenance", HTTP_POST, node_maintenance);
  register_uri(server, "/node/v2/activate", HTTP_POST, node_activate_candidate);
  register_uri(server, "/api/v2/provisioning", HTTP_GET, provisioning_state);
  register_uri(server, "/api/v2/auth/login", HTTP_POST, login);
  register_uri(server, "/api/v2/auth/logout", HTTP_POST, logout);
  register_uri(server, "/api/v2/auth/csrf", HTTP_GET, csrf);
  register_uri(server, "/api/v2/capabilities", HTTP_GET, capabilities);
  register_uri(server, "/api/v2/status", HTTP_GET, status);
  register_uri(server, "/api/v2/nodes", HTTP_GET, nodes);
  register_uri(server, "/api/v2/rig", HTTP_GET, rig_get);
  register_uri(server, "/api/v2/rig", HTTP_PUT, rig_put);
  register_uri(server, "/api/v2/maintenance/enter", HTTP_POST, maintenance_enter);
  register_uri(server, "/api/v2/maintenance/exit", HTTP_POST, maintenance_exit);
  register_uri(server, "/api/v2/diagnostics", HTTP_GET, diagnostics);
  register_uri(server, "/api/v2/commands", HTTP_POST, command);
  register_uri(server, "/api/v2/panel-test", HTTP_POST, panel_test);
  register_uri(server, "/api/v2/deployments", HTTP_POST, upload_package);
  register_uri(server, "/api/v2/deployments/activate", HTTP_POST, activate_package);
  register_uri(server, "/editor/", HTTP_GET, static_file);
  for (const auto& file : kStaticFiles) register_uri(server, file.path, HTTP_GET, static_file);
  if (g_topology_ok && g_config->role == Role::Master) {
    const BaseType_t created = xTaskCreatePinnedToCore(
        topology_sync_task, "topology_sync", 12288, nullptr, 4, nullptr, 0);
    if (created != pdPASS) ESP_LOGE(kTag, "topology sync task creation failed");
    if (xTaskCreatePinnedToCore(panel_controls_task,"panel_controls",12288,nullptr,3,nullptr,0)!=pdPASS)
      ESP_LOGE(kTag,"panel controls task creation failed");
  }
}
}  // namespace ledpanel
