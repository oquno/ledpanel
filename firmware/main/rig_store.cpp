#include "rig_store.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#include <vector>

namespace ledpanel {
namespace {
constexpr size_t kHeaderSize = 40;
constexpr size_t kMaxRigBytes = 32768;

const esp_partition_t* rig_partition(uint32_t slot) {
  const auto subtype = static_cast<esp_partition_subtype_t>(slot ? 0x43 : 0x42);
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, nullptr);
}

uint32_t read_le32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) |
         (static_cast<uint32_t>(bytes[3]) << 24);
}

void write_le32(uint8_t* bytes, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    bytes[i] = static_cast<uint8_t>(value >> (8 * i));
}

bool digest(const std::string& bytes, uint8_t output[32]) {
  return mbedtls_sha256(reinterpret_cast<const uint8_t*>(bytes.data()),
                        bytes.size(), output, 0) == 0;
}

bool uuid_lowercase(const char* value) {
  if (!value || std::strlen(value) != 36) return false;
  for (size_t i = 0; i < 36; ++i) {
    const char c = value[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool valid_label(const std::string& value) {
  if (value.empty() || value.size() > 320) return false;
  size_t characters = 0;
  for (size_t i = 0; i < value.size();) {
    const uint8_t first = static_cast<uint8_t>(value[i]);
    uint32_t point = 0;
    size_t width = 0;
    if (first < 0x80) {
      point = first;
      width = 1;
    } else if (first >= 0xc2 && first <= 0xdf) {
      point = first & 0x1f;
      width = 2;
    } else if (first >= 0xe0 && first <= 0xef) {
      point = first & 0x0f;
      width = 3;
    } else if (first >= 0xf0 && first <= 0xf4) {
      point = first & 0x07;
      width = 4;
    } else {
      return false;
    }
    if (i + width > value.size()) return false;
    for (size_t offset = 1; offset < width; ++offset) {
      const uint8_t next = static_cast<uint8_t>(value[i + offset]);
      if ((next & 0xc0) != 0x80) return false;
      point = (point << 6) | (next & 0x3f);
    }
    if ((width == 2 && point < 0x80) || (width == 3 && point < 0x800) ||
        (width == 4 && point < 0x10000) || point > 0x10ffff ||
        (point >= 0xd800 && point <= 0xdfff) || point < 0x20 ||
        (point >= 0x7f && point <= 0x9f) || ++characters > 80) return false;
    i += width;
  }
  return true;
}

bool unsigned_integer(const cJSON* item, uint32_t minimum, uint32_t maximum,
                      uint32_t& result) {
  if (!cJSON_IsNumber(item) || item->valuedouble < minimum ||
      item->valuedouble > maximum ||
      std::floor(item->valuedouble) != item->valuedouble) return false;
  result = static_cast<uint32_t>(item->valuedouble);
  return true;
}

bool exact_keys(const cJSON* object,
                std::initializer_list<const char*> expected) {
  if (!cJSON_IsObject(object) ||
      cJSON_GetArraySize(object) != static_cast<int>(expected.size())) return false;
  for (const char* key : expected)
    if (!cJSON_GetObjectItemCaseSensitive(object, key)) return false;
  return true;
}

bool valid_identifier(const char* value) {
  if (!value) return false;
  const size_t length = std::strlen(value);
  if (length == 0 || length > 32 || value[0] < 'a' || value[0] > 'z') return false;
  for (size_t i = 1; i < length; ++i) {
    const char c = value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-')) return false;
  }
  return true;
}

bool parse_active_rig(const RigSlot& slot, const NodeConfig& config,
                      cJSON*& root, cJSON*& nodes, std::string& error) {
  root = nullptr;
  nodes = nullptr;
  if (!slot.valid) return false;
  root = cJSON_ParseWithLength(slot.json.data(), slot.json.size());
  if (!root || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    root = nullptr;
    error = "rig JSON is invalid";
    return false;
  }
  const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
  const cJSON* group_id = cJSON_GetObjectItemCaseSensitive(root, "groupId");
  const cJSON* master = cJSON_GetObjectItemCaseSensitive(root, "masterNodeId");
  const cJSON* revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
  nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
  const cJSON* layouts = cJSON_GetObjectItemCaseSensitive(root, "layouts");
  uint32_t parsed_version = 0, parsed_master = 0, parsed_revision = 0;
  if (!exact_keys(root, {"version", "groupId", "masterNodeId", "revision",
                         "nodes", "layouts"}) ||
      !unsigned_integer(version, 2, 2, parsed_version) ||
      !cJSON_IsString(group_id) || !group_id->valuestring ||
      !uuid_lowercase(group_id->valuestring) || config.groupId != group_id->valuestring ||
      !unsigned_integer(master, 1, UINT16_MAX, parsed_master) ||
      parsed_master != config.nodeId ||
      !unsigned_integer(revision, 0, UINT32_MAX, parsed_revision) ||
      !cJSON_IsArray(nodes) || cJSON_GetArraySize(nodes) < 1 ||
      cJSON_GetArraySize(nodes) > 64 || !cJSON_IsArray(layouts) ||
      cJSON_GetArraySize(layouts) > 16) {
    cJSON_Delete(root);
    root = nullptr;
    nodes = nullptr;
    error = "rig does not match this master or exceeds the registry limit";
    return false;
  }

  bool master_found = false;
  size_t enabled_count = 0;
  std::vector<uint16_t> node_ids;
  std::vector<std::string> hardware_ids;
  std::vector<bool> node_enabled;
  for (const cJSON* node = nodes->child; node; node = node->next) {
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(node, "nodeId");
    const cJSON* hardware_id = cJSON_GetObjectItemCaseSensitive(node, "hardwareId");
    const cJSON* role = cJSON_GetObjectItemCaseSensitive(node, "role");
    const cJSON* label = cJSON_GetObjectItemCaseSensitive(node, "label");
    const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(node, "enabled");
    const cJSON* board_profile = cJSON_GetObjectItemCaseSensitive(node, "boardProfile");
    const cJSON* rotation = cJSON_GetObjectItemCaseSensitive(node, "rotation");
    const cJSON* brightness = cJSON_GetObjectItemCaseSensitive(node, "brightnessCap");
    uint32_t node_id = 0, rotation_value = 0, brightness_value = 0;
    if (!exact_keys(node, {"nodeId", "hardwareId", "label", "role", "enabled",
                           "boardProfile", "rotation", "brightnessCap"}) ||
        !unsigned_integer(id, 1, UINT16_MAX, node_id) ||
        !cJSON_IsString(hardware_id) || !hardware_id->valuestring ||
        !uuid_lowercase(hardware_id->valuestring) ||
        !cJSON_IsString(role) ||
        (std::strcmp(role->valuestring, "master") != 0 &&
         std::strcmp(role->valuestring, "follower") != 0) ||
        !cJSON_IsString(label) || !label->valuestring || !valid_label(label->valuestring) ||
        !cJSON_IsBool(enabled) || !cJSON_IsString(board_profile) ||
        !valid_identifier(board_profile->valuestring) ||
        !unsigned_integer(rotation, 0, 180, rotation_value) ||
        (rotation_value != 0 && rotation_value != 180) ||
        !unsigned_integer(brightness, 0, 255, brightness_value)) {
      cJSON_Delete(root);
      root = nullptr;
      nodes = nullptr;
      error = "rig node record is invalid";
      return false;
    }
    const bool is_master = std::strcmp(role->valuestring, "master") == 0;
    if (is_master) {
      if (master_found || node_id != config.nodeId ||
          config.hardwareId != hardware_id->valuestring || !cJSON_IsTrue(enabled)) {
        cJSON_Delete(root);
        root = nullptr;
        nodes = nullptr;
        error = "rig master record is invalid";
        return false;
      }
      master_found = true;
    }
    if (cJSON_IsTrue(enabled)) ++enabled_count;
    for (size_t i = 0; i < node_ids.size(); ++i) {
      if (node_ids[i] == node_id || hardware_ids[i] == hardware_id->valuestring) {
        cJSON_Delete(root);
        root = nullptr;
        nodes = nullptr;
        error = "rig contains duplicate node or hardware IDs";
        return false;
      }
    }
    node_ids.push_back(static_cast<uint16_t>(node_id));
    hardware_ids.emplace_back(hardware_id->valuestring);
    node_enabled.push_back(cJSON_IsTrue(enabled));
  }
  if (!master_found || enabled_count > 16) {
    cJSON_Delete(root);
    root = nullptr;
    nodes = nullptr;
    error = "rig master hardware identity does not match this device";
    return false;
  }

  std::vector<std::string> layout_ids;
  for (const cJSON* layout = layouts->child; layout; layout = layout->next) {
    const cJSON* layout_id = cJSON_GetObjectItemCaseSensitive(layout, "layoutId");
    const cJSON* bindings = cJSON_GetObjectItemCaseSensitive(layout, "bindings");
    if (!exact_keys(layout, {"layoutId", "bindings"}) ||
        !cJSON_IsString(layout_id) || !valid_identifier(layout_id->valuestring) ||
        !cJSON_IsArray(bindings) || cJSON_GetArraySize(bindings) > 16) {
      cJSON_Delete(root);
      root = nullptr;
      nodes = nullptr;
      error = "rig layout record is invalid";
      return false;
    }
    for (const auto& previous : layout_ids) {
      if (previous == layout_id->valuestring) {
        cJSON_Delete(root);
        root = nullptr;
        nodes = nullptr;
        error = "rig contains duplicate layout IDs";
        return false;
      }
    }
    layout_ids.emplace_back(layout_id->valuestring);
    std::vector<std::string> slot_ids;
    std::vector<uint16_t> bound_nodes;
    for (const cJSON* binding = bindings->child; binding; binding = binding->next) {
      const cJSON* slot = cJSON_GetObjectItemCaseSensitive(binding, "slotId");
      const cJSON* bound_id = cJSON_GetObjectItemCaseSensitive(binding, "nodeId");
      uint32_t node_id = 0;
      if (!exact_keys(binding, {"slotId", "nodeId"}) ||
          !cJSON_IsString(slot) || !valid_identifier(slot->valuestring) ||
          !unsigned_integer(bound_id, 1, UINT16_MAX, node_id)) {
        cJSON_Delete(root);
        root = nullptr;
        nodes = nullptr;
        error = "rig binding record is invalid";
        return false;
      }
      size_t registered = node_ids.size();
      for (size_t i = 0; i < node_ids.size(); ++i)
        if (node_ids[i] == node_id) { registered = i; break; }
      if (registered == node_ids.size() || !node_enabled[registered]) {
        cJSON_Delete(root);
        root = nullptr;
        nodes = nullptr;
        error = "rig binding references an unregistered or disabled node";
        return false;
      }
      if (std::find(slot_ids.begin(), slot_ids.end(), slot->valuestring) != slot_ids.end() ||
          std::find(bound_nodes.begin(), bound_nodes.end(), static_cast<uint16_t>(node_id)) !=
              bound_nodes.end()) {
        cJSON_Delete(root);
        root = nullptr;
        nodes = nullptr;
        error = "rig layout has duplicate slot or node bindings";
        return false;
      }
      slot_ids.emplace_back(slot->valuestring);
      bound_nodes.push_back(static_cast<uint16_t>(node_id));
    }
  }
  return true;
}

bool followers_for_layout(const RigSlot& slot, const NodeConfig& config,
                          const std::string& layout_id,
                          std::vector<RigFollowerIdentity>& followers,
                          std::string& error) {
  followers.clear();
  if (!valid_identifier(layout_id.c_str())) {
    error = "layout ID is invalid";
    return false;
  }
  cJSON* root = nullptr;
  cJSON* nodes = nullptr;
  if (!parse_active_rig(slot, config, root, nodes, error)) return false;
  const cJSON* layouts = cJSON_GetObjectItemCaseSensitive(root, "layouts");
  const cJSON* selected_bindings = nullptr;
  for (const cJSON* layout = layouts->child; layout; layout = layout->next) {
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(layout, "layoutId");
    if (cJSON_IsString(id) && layout_id == id->valuestring) {
      selected_bindings = cJSON_GetObjectItemCaseSensitive(layout, "bindings");
      break;
    }
  }
  if (cJSON_IsArray(selected_bindings)) {
    for (const cJSON* binding = selected_bindings->child; binding;
         binding = binding->next) {
      const cJSON* bound_id = cJSON_GetObjectItemCaseSensitive(binding, "nodeId");
      const uint16_t node_id = static_cast<uint16_t>(bound_id->valueint);
      for (const cJSON* node = nodes->child; node; node = node->next) {
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(node, "nodeId");
        if (!cJSON_IsNumber(id) || id->valueint != node_id) continue;
        const cJSON* role = cJSON_GetObjectItemCaseSensitive(node, "role");
        const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(node, "enabled");
        const cJSON* hardware = cJSON_GetObjectItemCaseSensitive(node, "hardwareId");
        if (cJSON_IsString(role) &&
            std::strcmp(role->valuestring, "follower") == 0 &&
            cJSON_IsTrue(enabled) && cJSON_IsString(hardware) &&
            hardware->valuestring) {
          followers.push_back({node_id, hardware->valuestring});
        }
        break;
      }
    }
  }
  cJSON_Delete(root);
  std::sort(followers.begin(), followers.end(),
            [](const RigFollowerIdentity& left,
               const RigFollowerIdentity& right) {
              return left.node_id < right.node_id;
            });
  error.clear();
  return true;
}

std::string print_json(cJSON* root) {
  char* serialized = cJSON_PrintUnformatted(root);
  if (!serialized) return {};
  std::string result(serialized);
  cJSON_free(serialized);
  return result;
}
}  // namespace

bool rig_load(RigSlot& out, std::string& error) {
  nvs_handle_t handle;
  if (nvs_open("ledpanel", NVS_READONLY, &handle) != ESP_OK) {
    error = "nvs unavailable";
    return false;
  }
  uint32_t active = 0;
  nvs_get_u32(handle, "rig_slot", &active);
  nvs_close(handle);
  const esp_partition_t* partition = rig_partition(active & 1);
  if (!partition) {
    error = "rig partition missing";
    return false;
  }

  uint8_t header[kHeaderSize] = {};
  if (esp_partition_read(partition, 0, header, sizeof(header)) != ESP_OK) {
    error = "rig header read failed";
    return false;
  }
  if (std::memcmp(header, "RIG2", 4) != 0) {
    out.valid = false;
    out.json.clear();
    out.generation = 0;
    return true;
  }
  const uint32_t length = read_le32(header + 4);
  if (length == 0 || length > kMaxRigBytes || length > partition->size - kHeaderSize) {
    error = "rig JSON length is invalid";
    return false;
  }
  std::string json(length, '\0');
  if (esp_partition_read(partition, kHeaderSize, json.data(), length) != ESP_OK) {
    error = "rig read failed";
    return false;
  }
  uint8_t actual[32] = {};
  if (!digest(json, actual)) {
    error = "rig hash computation failed";
    return false;
  }
  const bool current_format = std::memcmp(header + 8, actual, sizeof(actual)) == 0;
  const bool legacy_format = std::all_of(header + 12, header + sizeof(header),
                                         [](uint8_t value) { return value == 0; });
  if (!current_format && !legacy_format) {
    error = "rig hash mismatch";
    return false;
  }
  out.valid = true;
  out.json = std::move(json);
  out.generation = current_format ? 0 : read_le32(header + 8);
  return true;
}

bool rig_commit(const NodeConfig& config, const std::string& json,
                std::string& error) {
  if (json.empty() || json.size() > kMaxRigBytes) {
    error = "rig JSON size";
    return false;
  }
  RigSlot candidate;
  candidate.valid = true;
  candidate.json = json;
  cJSON* parsed = nullptr;
  cJSON* parsed_nodes = nullptr;
  if (!parse_active_rig(candidate, config, parsed, parsed_nodes, error)) return false;
  cJSON_Delete(parsed);
  nvs_handle_t handle;
  if (nvs_open("ledpanel", NVS_READWRITE, &handle) != ESP_OK) {
    error = "nvs open";
    return false;
  }
  uint32_t old_slot = 0;
  nvs_get_u32(handle, "rig_slot", &old_slot);
  const uint32_t next_slot = (old_slot + 1) & 1;
  const esp_partition_t* partition = rig_partition(next_slot);
  if (!partition) {
    nvs_close(handle);
    error = "rig partition missing";
    return false;
  }
  uint8_t header[kHeaderSize] = {};
  uint8_t sha[32] = {};
  if (!digest(json, sha)) {
    nvs_close(handle);
    error = "rig hash computation failed";
    return false;
  }
  std::memcpy(header, "RIG2", 4);
  write_le32(header + 4, static_cast<uint32_t>(json.size()));
  std::memcpy(header + 8, sha, sizeof(sha));
  if (esp_partition_erase_range(partition, 0, partition->size) != ESP_OK ||
      esp_partition_write(partition, 0, header, sizeof(header)) != ESP_OK ||
      esp_partition_write(partition, kHeaderSize, json.data(), json.size()) != ESP_OK) {
    nvs_close(handle);
    error = "rig write failed";
    return false;
  }

  uint8_t verify_header[kHeaderSize] = {};
  std::string verify_json(json.size(), '\0');
  uint8_t verify_sha[32] = {};
  if (esp_partition_read(partition, 0, verify_header, sizeof(verify_header)) != ESP_OK ||
      esp_partition_read(partition, kHeaderSize, verify_json.data(), verify_json.size()) != ESP_OK ||
      !digest(verify_json, verify_sha) ||
      std::memcmp(verify_header, header, sizeof(header)) != 0 ||
      verify_json != json || std::memcmp(verify_sha, sha, sizeof(sha)) != 0) {
    nvs_close(handle);
    error = "rig read-back verification failed";
    return false;
  }
  esp_err_t result = nvs_set_u32(handle, "rig_slot", next_slot);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  if (result != ESP_OK) {
    error = "rig pointer commit failed";
    return false;
  }
  return true;
}

bool rig_ensure_master_registry(const NodeConfig& config, std::string& error) {
  if (!config.provisioned || config.role != Role::Master || config.nodeId == 0 ||
      !uuid_lowercase(config.hardwareId.c_str())) {
    error = "master identity is not ready";
    return false;
  }
  RigSlot slot;
  if (!rig_load(slot, error)) return false;
  if (slot.valid) {
    cJSON* root = nullptr;
    cJSON* nodes = nullptr;
    if (!parse_active_rig(slot, config, root, nodes, error)) return false;
    cJSON_Delete(root);
    return true;
  }

  cJSON* root = cJSON_CreateObject();
  cJSON* nodes = cJSON_CreateArray();
  cJSON* layouts = cJSON_CreateArray();
  cJSON* master = cJSON_CreateObject();
  if (!root || !nodes || !layouts || !master) {
    cJSON_Delete(root);
    cJSON_Delete(nodes);
    cJSON_Delete(layouts);
    cJSON_Delete(master);
    error = "rig allocation failed";
    return false;
  }
  cJSON_AddNumberToObject(root, "version", 2);
  cJSON_AddStringToObject(root, "groupId", config.groupId.c_str());
  cJSON_AddNumberToObject(root, "masterNodeId", config.nodeId);
  cJSON_AddNumberToObject(root, "revision", 0);
  cJSON_AddStringToObject(master, "hardwareId", config.hardwareId.c_str());
  cJSON_AddNumberToObject(master, "nodeId", config.nodeId);
  cJSON_AddStringToObject(master, "label", "LED Panel Master");
  cJSON_AddStringToObject(master, "role", "master");
  cJSON_AddBoolToObject(master, "enabled", true);
  cJSON_AddStringToObject(master, "boardProfile", "seengreat_hub75_s3_n16r8_v1");
  cJSON_AddNumberToObject(master, "rotation", 0);
  cJSON_AddNumberToObject(master, "brightnessCap", config.brightnessCap);
  cJSON_AddItemToArray(nodes, master);
  cJSON_AddItemToObject(root, "nodes", nodes);
  cJSON_AddItemToObject(root, "layouts", layouts);
  const std::string json = print_json(root);
  cJSON_Delete(root);
  if (json.empty()) {
    error = "rig serialization failed";
    return false;
  }
  return rig_commit(config, json, error);
}

bool rig_register_follower(const NodeConfig& config, uint16_t node_id,
                           const std::string& hardware_id, const std::string& label,
                           std::string& error) {
  if (!config.provisioned || config.role != Role::Master || node_id == 0 ||
      node_id == config.nodeId || !uuid_lowercase(hardware_id.c_str()) ||
      !valid_label(label)) {
    error = "follower registration fields are invalid";
    return false;
  }
  RigSlot slot;
  if (!rig_load(slot, error) || !slot.valid) {
    if (error.empty()) error = "master node registry is unavailable";
    return false;
  }
  cJSON* root = nullptr;
  cJSON* nodes = nullptr;
  if (!parse_active_rig(slot, config, root, nodes, error)) return false;
  for (cJSON* node = nodes->child; node; node = node->next) {
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(node, "nodeId");
    const cJSON* hardware = cJSON_GetObjectItemCaseSensitive(node, "hardwareId");
    if ((cJSON_IsNumber(id) && id->valueint == node_id) ||
        (cJSON_IsString(hardware) && hardware_id == hardware->valuestring)) {
      if (!cJSON_IsNumber(id) || id->valueint != node_id || !cJSON_IsString(hardware) ||
          hardware_id != hardware->valuestring ||
          std::strcmp(cJSON_GetObjectItemCaseSensitive(node, "role")->valuestring,
                      "follower") != 0) {
        cJSON_Delete(root);
        error = "node ID or hardware ID is already registered to another device";
        return false;
      }
      cJSON* old_label = cJSON_GetObjectItemCaseSensitive(node, "label");
      if (cJSON_IsString(old_label) && label == old_label->valuestring) {
        cJSON_Delete(root);
        return true;
      }
      cJSON* revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
      if (!cJSON_IsNumber(revision) || revision->valuedouble >= UINT32_MAX) {
        cJSON_Delete(root);
        error = "rig revision is invalid or exhausted";
        return false;
      }
      cJSON_ReplaceItemInObjectCaseSensitive(node, "label", cJSON_CreateString(label.c_str()));
      cJSON_SetNumberValue(revision, revision->valuedouble + 1);
      const std::string json = print_json(root);
      cJSON_Delete(root);
      if (json.empty()) {
        error = "rig serialization failed";
        return false;
      }
      return rig_commit(config, json, error);
    }
  }

  if (cJSON_GetArraySize(nodes) >= 64) {
    cJSON_Delete(root);
    error = "node registry is full";
    return false;
  }
  cJSON* follower = cJSON_CreateObject();
  if (!follower) {
    cJSON_Delete(root);
    error = "rig allocation failed";
    return false;
  }
  cJSON_AddNumberToObject(follower, "nodeId", node_id);
  cJSON_AddStringToObject(follower, "hardwareId", hardware_id.c_str());
  cJSON_AddStringToObject(follower, "label", label.c_str());
  cJSON_AddStringToObject(follower, "role", "follower");
  cJSON_AddBoolToObject(follower, "enabled", false);
  cJSON_AddStringToObject(follower, "boardProfile", "seengreat_hub75_s3_n16r8_v1");
  cJSON_AddNumberToObject(follower, "rotation", 0);
  cJSON_AddNumberToObject(follower, "brightnessCap", 64);
  cJSON_AddItemToArray(nodes, follower);
  cJSON* revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
  if (!cJSON_IsNumber(revision) || revision->valuedouble >= UINT32_MAX) {
    cJSON_Delete(root);
    error = "rig revision is invalid or exhausted";
    return false;
  }
  cJSON_SetNumberValue(revision, revision->valuedouble + 1);
  const std::string json = print_json(root);
  cJSON_Delete(root);
  if (json.empty()) {
    error = "rig serialization failed";
    return false;
  }
  return rig_commit(config, json, error);
}

bool rig_lookup_follower(const NodeConfig& config, uint16_t node_id,
                         const std::string& hardware_id, bool& enabled,
                         std::string& label, std::string& error) {
  enabled = false;
  label.clear();
  RigSlot slot;
  if (!rig_load(slot, error) || !slot.valid) {
    if (error.empty()) error = "master node registry is unavailable";
    return false;
  }
  cJSON* root = nullptr;
  cJSON* nodes = nullptr;
  if (!parse_active_rig(slot, config, root, nodes, error)) return false;
  for (const cJSON* node = nodes->child; node; node = node->next) {
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(node, "nodeId");
    const cJSON* hardware = cJSON_GetObjectItemCaseSensitive(node, "hardwareId");
    const cJSON* role = cJSON_GetObjectItemCaseSensitive(node, "role");
    const cJSON* is_enabled = cJSON_GetObjectItemCaseSensitive(node, "enabled");
    const cJSON* node_label = cJSON_GetObjectItemCaseSensitive(node, "label");
    if (cJSON_IsNumber(id) && id->valueint == node_id &&
        cJSON_IsString(hardware) && hardware_id == hardware->valuestring &&
        cJSON_IsString(role) && std::strcmp(role->valuestring, "follower") == 0 &&
        cJSON_IsBool(is_enabled) && cJSON_IsString(node_label)) {
      enabled = cJSON_IsTrue(is_enabled);
      label = node_label->valuestring;
      cJSON_Delete(root);
      return true;
    }
  }
  cJSON_Delete(root);
  error = "follower is not registered";
  return false;
}

bool rig_enabled_followers(const NodeConfig& config,
                           std::vector<RigFollowerIdentity>& followers,
                           std::string& error) {
  followers.clear();
  RigSlot slot;
  if (!rig_load(slot, error) || !slot.valid) {
    if (error.empty()) error = "master node registry is unavailable";
    return false;
  }
  cJSON* root = nullptr;
  cJSON* nodes = nullptr;
  if (!parse_active_rig(slot, config, root, nodes, error)) return false;
  for (const cJSON* node = nodes->child; node; node = node->next) {
    const cJSON* role = cJSON_GetObjectItemCaseSensitive(node, "role");
    const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(node, "enabled");
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(node, "nodeId");
    const cJSON* hardware = cJSON_GetObjectItemCaseSensitive(node, "hardwareId");
    if (cJSON_IsString(role) && std::strcmp(role->valuestring, "follower") == 0 &&
        cJSON_IsTrue(enabled) && cJSON_IsNumber(id) &&
        cJSON_IsString(hardware) && hardware->valuestring) {
      followers.push_back({static_cast<uint16_t>(id->valueint), hardware->valuestring});
    }
  }
  cJSON_Delete(root);
  error.clear();
  return true;
}

bool rig_display_followers(const NodeConfig& config,
                           const std::string& layout_id,
                           std::vector<RigFollowerIdentity>& followers,
                           std::string& error) {
  RigSlot slot;
  if (!rig_load(slot, error) || !slot.valid) {
    if (error.empty()) error = "master node registry is unavailable";
    return false;
  }
  return followers_for_layout(slot, config, layout_id, followers, error);
}

bool rig_store_self_test(std::string& error) {
  NodeConfig config;
  config.nodeId = 7;
  config.groupId = "00112233-4455-6677-8899-aabbccddeeff";
  config.hardwareId = "00000000-0000-4000-8000-000000000007";
  const std::string valid = R"({"version":2,"groupId":"00112233-4455-6677-8899-aabbccddeeff","masterNodeId":7,"revision":0,"nodes":[{"nodeId":7,"hardwareId":"00000000-0000-4000-8000-000000000007","label":"Master","role":"master","enabled":true,"boardProfile":"seengreat_hub75_s3_n16r8_v1","rotation":0,"brightnessCap":64},{"nodeId":42,"hardwareId":"00000000-0000-4000-8000-00000000002a","label":"Follower","role":"follower","enabled":true,"boardProfile":"seengreat_hub75_s3_n16r8_v1","rotation":180,"brightnessCap":64},{"nodeId":12,"hardwareId":"00000000-0000-0000-0000-00000000000c","label":"Standby","role":"follower","enabled":true,"boardProfile":"seengreat_hub75_s3_n16r8_v1","rotation":0,"brightnessCap":64}],"layouts":[]})";
  const auto accepted = [&](const std::string& json) {
    RigSlot slot;
    slot.valid = true;
    slot.json = json;
    cJSON* root = nullptr;
    cJSON* nodes = nullptr;
    std::string reason;
    const bool ok = parse_active_rig(slot, config, root, nodes, reason);
    cJSON_Delete(root);
    return ok;
  };
  if (!accepted(valid)) {
    error = "valid rig was rejected by the schema validator";
    return false;
  }
  std::string valid_binding = valid;
  const std::string empty_layouts = "\"layouts\":[]";
  const size_t valid_layouts_pos = valid_binding.find(empty_layouts);
  valid_binding.replace(valid_layouts_pos, empty_layouts.size(),
      "\"layouts\":[{\"layoutId\":\"horizontal\",\"bindings\":["
      "{\"slotId\":\"panel_1\",\"nodeId\":7},"
      "{\"slotId\":\"panel_2\",\"nodeId\":42}]}]");
  if (!accepted(valid_binding)) {
    error = "valid rig slot bindings were rejected by the schema validator";
    return false;
  }
  RigSlot selected_layout;
  selected_layout.valid = true;
  selected_layout.json = valid_binding;
  std::vector<RigFollowerIdentity> required_followers;
  if (!followers_for_layout(selected_layout, config, "horizontal",
                            required_followers, error) ||
      required_followers.size() != 1 ||
      required_followers[0].node_id != 42 ||
      required_followers[0].hardware_id !=
          "00000000-0000-4000-8000-00000000002a") {
    error = "selected layout did not isolate its assigned follower from standby nodes";
    return false;
  }
  if (!followers_for_layout(selected_layout, config, "vertical",
                            required_followers, error) ||
      !required_followers.empty()) {
    error = "unassigned layout incorrectly required a standby follower";
    return false;
  }
  std::string invalid = valid;
  invalid.insert(invalid.size() - 1, ",\"unexpected\":true");
  if (accepted(invalid)) {
    error = "rig with an unknown top-level property was accepted";
    return false;
  }
  invalid = valid;
  const size_t layouts_pos = invalid.find(empty_layouts);
  invalid.replace(layouts_pos, empty_layouts.size(),
      "\"layouts\":[{\"layoutId\":\"horizontal\",\"bindings\":["
      "{\"slotId\":\"panel_1\",\"nodeId\":7},"
      "{\"slotId\":\"panel_1\",\"nodeId\":42}]}]");
  if (accepted(invalid)) {
    error = "rig with duplicate slot bindings was accepted";
    return false;
  }
  invalid = valid;
  const size_t empty_layouts_again = invalid.find(empty_layouts);
  invalid.replace(empty_layouts_again, empty_layouts.size(),
      "\"layouts\":[{\"layoutId\":\"horizontal\",\"bindings\":["
      "{\"slotId\":\"panel_1\",\"nodeId\":99}]}]");
  if (accepted(invalid)) {
    error = "rig binding to an unregistered node was accepted";
    return false;
  }
  invalid = valid;
  const size_t rotation_pos = invalid.find("\"rotation\":180");
  invalid.replace(rotation_pos, std::strlen("\"rotation\":180"), "\"rotation\":90");
  if (accepted(invalid)) {
    error = "rig with unsupported panel rotation was accepted";
    return false;
  }
  error.clear();
  return true;
}
}  // namespace ledpanel
