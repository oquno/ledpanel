#include "config_store.hpp"
#include <cctype>
#include <cstring>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <mbedtls/pkcs5.h>
#include <cJSON.h>

namespace ledpanel {
namespace {
constexpr size_t kMaxConfigBytes = 4096;
constexpr int kPasswordIterations = 100000;
constexpr size_t kSaltBytes = 16;
constexpr size_t kVerifierBytes = 32;

bool is_hex(const char* value, size_t length) {
  if (!value || std::strlen(value) != length) return false;
  for (size_t i = 0; i < length; ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}

bool valid_master_address(const char* value) {
  if (!value) return false;
  const size_t length = std::strlen(value);
  if (length == 0 || length > 64) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (!std::isalnum(c) && c != '.' && c != '-' && c != ':') return false;
  }
  return true;
}

void wipe(char* data, size_t length) {
  volatile char* p = data;
  while (length--) *p++ = 0;
}

void wipe_config_secrets(cJSON* root) {
  for (const char* name : {"adminPassword", "adminSalt", "adminVerifier", "apPsk",
                           "groupKey", "stationSsid", "stationPassword"}) {
    cJSON* field = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(field) && field->valuestring)
      wipe(field->valuestring, std::strlen(field->valuestring));
  }
}

struct JsonSecretGuard {
  cJSON* root;
  ~JsonSecretGuard() { wipe_config_secrets(root); }
};

struct StringSecretGuard {
  std::string& value;
  ~StringSecretGuard() {
    if (!value.empty()) wipe(value.data(), value.size());
  }
};

bool safe_wifi_text(const char* value, size_t minimum, size_t maximum) {
  if (!value) return false;
  const size_t length = std::strlen(value);
  if (length < minimum || length > maximum) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

bool derive_password(const std::string& password, const uint8_t salt[kSaltBytes],
                     uint8_t verifier[kVerifierBytes]) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(
             MBEDTLS_MD_SHA256,
             reinterpret_cast<const unsigned char*>(password.data()),
             password.size(), salt, kSaltBytes, kPasswordIterations,
             kVerifierBytes, verifier) == 0;
}

std::string hex_bytes(const uint8_t* bytes, size_t size) {
  static const char kHex[] = "0123456789abcdef";
  std::string result(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    result[2 * i] = kHex[bytes[i] >> 4];
    result[2 * i + 1] = kHex[bytes[i] & 15];
  }
  return result;
}

bool from_hex(const std::string& input, uint8_t* bytes, size_t size) {
  if (input.size() != size * 2) return false;
  for (size_t i = 0; i < size; ++i) {
    const auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int hi = nibble(input[i * 2]);
    const int lo = nibble(input[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool load_json(std::string& json, std::string& error) {
  nvs_handle_t handle;
  const esp_err_t opened = nvs_open("ledpanel", NVS_READONLY, &handle);
  if (opened != ESP_OK) {
    error = opened == ESP_ERR_NVS_NOT_FOUND ? "unprovisioned" : "nvs unavailable";
    return false;
  }
  size_t size = 0;
  esp_err_t err = nvs_get_blob(handle, "device", nullptr, &size);
  if (err != ESP_OK || size == 0 || size > kMaxConfigBytes) {
    nvs_close(handle);
    error = "configuration missing or invalid";
    return false;
  }
  std::unique_ptr<char[]> buffer(new (std::nothrow) char[size]);
  if (!buffer) {
    nvs_close(handle);
    error = "out of memory";
    return false;
  }
  err = nvs_get_blob(handle, "device", buffer.get(), &size);
  nvs_close(handle);
  if (err != ESP_OK) {
    error = "configuration read failed";
    return false;
  }
  json.assign(buffer.get(), size);
  wipe(buffer.get(), size);
  return true;
}

bool json_to_config(const std::string& json, NodeConfig& out, std::string& error) {
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
      cJSON_ParseWithLength(json.data(), json.size()), &cJSON_Delete);
  if (!root || !cJSON_IsObject(root.get())) {
    error = "invalid configuration JSON";
    return false;
  }
  JsonSecretGuard secret_guard{root.get()};
  const auto string_value = [&](const char* name) -> const char* {
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(root.get(), name);
    return cJSON_IsString(field) ? field->valuestring : nullptr;
  };
  const char* role = string_value("role");
  const char* mode = string_value("networkMode");
  const char* group_id = string_value("groupId");
  const char* psk = string_value("apPsk");
  const char* master_address = string_value("masterAddress");
  const char* station_ssid = string_value("stationSsid");
  const char* station_password = string_value("stationPassword");
  const char* key = string_value("groupKey");
  const char* salt = string_value("adminSalt");
  const char* verifier = string_value("adminVerifier");
  const cJSON* node = cJSON_GetObjectItemCaseSensitive(root.get(), "nodeId");
  const cJSON* master_node = cJSON_GetObjectItemCaseSensitive(root.get(), "masterNodeId");
  const cJSON* channel = cJSON_GetObjectItemCaseSensitive(root.get(), "channel");

  const bool is_master = role && std::strcmp(role, "master") == 0;
  const bool is_follower = role && std::strcmp(role, "follower") == 0;
  const bool is_softap = mode && std::strcmp(mode, "softap") == 0;
  const bool is_infrastructure = mode && std::strcmp(mode, "infrastructure") == 0;
  const size_t ssid_length = station_ssid ? std::strlen(station_ssid) : 0;
  const size_t station_password_length = station_password
      ? std::strlen(station_password) : 0;
  const bool station_credentials_valid = is_infrastructure && station_ssid &&
      ssid_length > 0 && ssid_length <= 32 && station_password &&
      station_password_length >= 8 && station_password_length <= 63 &&
      safe_wifi_text(station_ssid, 1, 32) &&
      safe_wifi_text(station_password, 8, 63);
  if ((!is_master && !is_follower) || !mode ||
      (!is_softap && !is_infrastructure) || !group_id || std::strlen(group_id) != 36 ||
      !psk || std::strlen(psk) < 12 || std::strlen(psk) > 63 || !is_hex(key, 64) ||
      !is_hex(salt, 32) || !is_hex(verifier, 64) || !cJSON_IsNumber(node) ||
      node->valuedouble < 1 || node->valuedouble > 65535 ||
      node->valuedouble != static_cast<double>(static_cast<uint16_t>(node->valuedouble)) ||
      (is_softap && (station_ssid || station_password)) ||
      (is_infrastructure && !station_credentials_valid) ||
      (is_follower && (!cJSON_IsNumber(master_node) || master_node->valueint < 1 ||
                       master_node->valueint > 65535 ||
                       master_node->valueint == node->valueint ||
                       !valid_master_address(master_address))) ||
      (is_master && master_node &&
       (!cJSON_IsNumber(master_node) || master_node->valuedouble != node->valuedouble)) ||
      (is_master && master_address && std::strlen(master_address) != 0) ||
      (channel && (!cJSON_IsNumber(channel) ||
                   (channel->valueint != 1 && channel->valueint != 6 &&
                    channel->valueint != 11)))) {
    error = "invalid fields; network mode requires matching credentials";
    return false;
  }
  const std::string id(group_id);
  for (size_t i = 0; i < id.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (id[i] != '-') {
        error = "groupId must be a UUID";
        return false;
      }
    } else if (!std::isxdigit(static_cast<unsigned char>(id[i]))) {
      error = "groupId must be a UUID";
      return false;
    }
  }

  const cJSON* version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
  const cJSON* cap = cJSON_GetObjectItemCaseSensitive(root.get(), "brightnessCap");
  if (!cJSON_IsNumber(version) || version->valueint != 2 ||
      (cap && (!cJSON_IsNumber(cap) || cap->valueint < 1 || cap->valueint > 64))) {
    error = "unsupported config version or brightnessCap";
    return false;
  }
  out.nodeId = static_cast<uint16_t>(node->valueint);
  out.masterNodeId = is_master ? out.nodeId : static_cast<uint16_t>(master_node->valueint);
  out.role = is_master ? Role::Master : Role::Follower;
  out.provisioned = true;
  out.groupId = id;
  out.networkMode = mode;
  out.masterAddress = is_follower ? master_address : "";
  out.apPsk = psk;
  out.stationSsid = station_ssid ? station_ssid : "";
  out.stationPassword = station_password ? station_password : "";
  out.groupKey = key;
  out.adminSalt = salt;
  out.adminVerifier = verifier;
  out.channel = channel ? static_cast<uint8_t>(channel->valueint) : 6;
  out.brightnessCap = cap ? static_cast<uint8_t>(cap->valueint) : 64;
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) != ESP_OK) {
    error = "hardware identity unavailable";
    return false;
  }
  char hardware_id[37];
  std::snprintf(hardware_id, sizeof(hardware_id),
                "00000000-0000-4000-8000-%02x%02x%02x%02x%02x%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  out.hardwareId = hardware_id;
  return true;
}
}  // namespace

bool config_load(NodeConfig& out, std::string& error) {
  std::string json;
  if (!load_json(json, error) || !json_to_config(json, out, error)) return false;
  nvs_handle_t handle;
  if (nvs_open("ledpanel", NVS_READONLY, &handle) == ESP_OK) {
    size_t slot_length = 0;
    if (nvs_get_str(handle, "panel_slot", nullptr, &slot_length) == ESP_OK &&
        slot_length > 1 && slot_length <= 33) {
      std::string slot(slot_length, '\0');
      if (nvs_get_str(handle, "panel_slot", slot.data(), &slot_length) == ESP_OK)
        out.slotId.assign(slot.c_str());
    }
    uint8_t rotation = 0;
    if (nvs_get_u8(handle, "panel_rot", &rotation) == ESP_OK &&
        (rotation == 0 || rotation == 180)) out.rotation = rotation;
    nvs_close(handle);
  }
  return true;
}

bool config_save(const std::string& input, NodeConfig& out, std::string& error) {
  if (input.empty() || input.size() > kMaxConfigBytes) {
    error = "configuration size is invalid";
    return false;
  }
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
      cJSON_ParseWithLength(input.data(), input.size()), &cJSON_Delete);
  if (!root || !cJSON_IsObject(root.get())) {
    error = "invalid configuration JSON";
    return false;
  }
  JsonSecretGuard secret_guard{root.get()};
  cJSON* password = cJSON_GetObjectItemCaseSensitive(root.get(), "adminPassword");
  if (!cJSON_IsString(password) || std::strlen(password->valuestring) < 12 ||
      std::strlen(password->valuestring) > 128) {
    error = "adminPassword must contain 12..128 characters";
    return false;
  }
  uint8_t salt[kSaltBytes];
  uint8_t verifier[kVerifierBytes];
  esp_fill_random(salt, sizeof(salt));
  const std::string clear_password(password->valuestring);
  const bool derived = derive_password(clear_password, salt, verifier);
  wipe(password->valuestring, std::strlen(password->valuestring));
  if (!derived) {
    error = "password derivation failed";
    return false;
  }
  cJSON_DeleteItemFromObjectCaseSensitive(root.get(), "adminPassword");
  cJSON_AddStringToObject(root.get(), "adminSalt", hex_bytes(salt, sizeof(salt)).c_str());
  cJSON_AddStringToObject(root.get(), "adminVerifier",
                          hex_bytes(verifier, sizeof(verifier)).c_str());
  std::memset(verifier, 0, sizeof(verifier));
  std::memset(salt, 0, sizeof(salt));
  const char* canonical = cJSON_PrintUnformatted(root.get());
  if (!canonical) {
    error = "configuration serialization failed";
    return false;
  }
  std::string normalized(canonical);
  cJSON_free(const_cast<char*>(canonical));
  StringSecretGuard normalized_secret_guard{normalized};
  if (!json_to_config(normalized, out, error)) return false;

  nvs_handle_t handle;
  if (nvs_open("ledpanel", NVS_READWRITE, &handle) != ESP_OK) {
    error = "nvs unavailable";
    return false;
  }
  nvs_erase_key(handle, "panel_slot");
  nvs_erase_key(handle, "panel_rot");
  esp_err_t err = nvs_set_blob(handle, "device", normalized.data(), normalized.size());
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) {
    error = "configuration save failed";
    return false;
  }
  return true;
}

bool config_store_panel_binding(const std::string& slot_id, uint8_t rotation,
                                std::string& error) {
  if ((!slot_id.empty() && (slot_id.size() > 32 ||
       slot_id[0] < 'a' || slot_id[0] > 'z')) ||
      (rotation != 0 && rotation != 180)) {
    error = "panel binding is invalid";
    return false;
  }
  for (const char value : slot_id) {
    if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
          value == '_' || value == '-')) {
      error = "panel slot ID is invalid";
      return false;
    }
  }
  nvs_handle_t handle;
  if (nvs_open("ledpanel", NVS_READWRITE, &handle) != ESP_OK) {
    error = "nvs unavailable";
    return false;
  }
  esp_err_t result = slot_id.empty()
      ? nvs_erase_key(handle, "panel_slot")
      : nvs_set_str(handle, "panel_slot", slot_id.c_str());
  if (result == ESP_ERR_NVS_NOT_FOUND && slot_id.empty()) result = ESP_OK;
  if (result == ESP_OK) result = nvs_set_u8(handle, "panel_rot", rotation);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) {
    error = "panel binding save failed";
    return false;
  }
  error.clear();
  return true;
}

bool config_reset(std::string& error) {
  nvs_handle_t display;
  if (nvs_open("display", NVS_READWRITE, &display) == ESP_OK) {
    nvs_erase_all(display); nvs_commit(display); nvs_close(display);
  }

  nvs_handle_t handle;
  if (nvs_open("ledpanel", NVS_READWRITE, &handle) != ESP_OK) {
    error = "nvs unavailable";
    return false;
  }
  esp_err_t err = nvs_erase_key(handle, "device");
  if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
  const esp_err_t slot_error = nvs_erase_key(handle, "panel_slot");
  const esp_err_t rotation_error = nvs_erase_key(handle, "panel_rot");
  if (err == ESP_OK && slot_error != ESP_OK && slot_error != ESP_ERR_NVS_NOT_FOUND)
    err = slot_error;
  if (err == ESP_OK && rotation_error != ESP_OK && rotation_error != ESP_ERR_NVS_NOT_FOUND)
    err = rotation_error;
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) {
    error = "configuration reset failed";
    return false;
  }
  return true;
}

bool config_verify_password(const NodeConfig& config, const std::string& password) {
  uint8_t salt[kSaltBytes];
  uint8_t expected[kVerifierBytes];
  uint8_t actual[kVerifierBytes];
  const bool valid = from_hex(config.adminSalt, salt, sizeof(salt)) &&
                     from_hex(config.adminVerifier, expected, sizeof(expected)) &&
                     derive_password(password, salt, actual);
  uint8_t mismatch = valid ? 0 : 1;
  if (valid) {
    for (size_t i = 0; i < sizeof(actual); ++i) mismatch |= actual[i] ^ expected[i];
  }
  std::memset(salt, 0, sizeof(salt));
  std::memset(expected, 0, sizeof(expected));
  std::memset(actual, 0, sizeof(actual));
  return mismatch == 0;
}
}  // namespace ledpanel
