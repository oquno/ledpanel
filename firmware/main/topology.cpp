#include "topology.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <vector>
#include <cJSON.h>
#include <mbedtls/sha256.h>

namespace ledpanel {
namespace {
constexpr const char* kBoardProfile = "seengreat_hub75_s3_n16r8_v1";

bool exact_keys(const cJSON* object,
                std::initializer_list<const char*> expected) {
  if (!cJSON_IsObject(object) ||
      cJSON_GetArraySize(object) != static_cast<int>(expected.size())) return false;
  for (const char* key : expected)
    if (!cJSON_GetObjectItemCaseSensitive(object, key)) return false;
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

const char* json_string(const cJSON* object, const char* key) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : nullptr;
}

bool lower_uuid(const char* value) {
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

bool lower_hash(const std::string& value) {
  if (value.size() != 64) return false;
  for (const char c : value)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

cJSON* canonical_clone(const cJSON* value) {
  if (!value) return nullptr;
  if (cJSON_IsObject(value)) {
    std::vector<const cJSON*> fields;
    for (const cJSON* field = value->child; field; field = field->next)
      fields.push_back(field);
    std::sort(fields.begin(), fields.end(), [](const cJSON* left,
                                               const cJSON* right) {
      return std::strcmp(left->string ? left->string : "",
                         right->string ? right->string : "") < 0;
    });
    cJSON* copy = cJSON_CreateObject();
    if (!copy) return nullptr;
    for (const cJSON* field : fields) {
      cJSON* child = canonical_clone(field);
      if (!child || !field->string ||
          !cJSON_AddItemToObject(copy, field->string, child)) {
        cJSON_Delete(child);
        cJSON_Delete(copy);
        return nullptr;
      }
    }
    return copy;
  }
  if (cJSON_IsArray(value)) {
    cJSON* copy = cJSON_CreateArray();
    if (!copy) return nullptr;
    for (const cJSON* item = value->child; item; item = item->next) {
      cJSON* child = canonical_clone(item);
      if (!child || !cJSON_AddItemToArray(copy, child)) {
        cJSON_Delete(child);
        cJSON_Delete(copy);
        return nullptr;
      }
    }
    return copy;
  }
  if (cJSON_IsString(value)) return cJSON_CreateString(value->valuestring);
  if (cJSON_IsNumber(value)) return cJSON_CreateNumber(value->valuedouble);
  if (cJSON_IsTrue(value)) return cJSON_CreateTrue();
  if (cJSON_IsFalse(value)) return cJSON_CreateFalse();
  if (cJSON_IsNull(value)) return cJSON_CreateNull();
  return nullptr;
}

bool normalized_layout(const std::string& source, cJSON*& result,
                      std::string& error) {
  result = nullptr;
  cJSON* parsed = cJSON_ParseWithLength(source.data(), source.size());
  if (!exact_keys(parsed, {"id", "name", "width", "height", "slots"}) ||
      !json_string(parsed, "id") || !json_string(parsed, "name")) {
    cJSON_Delete(parsed);
    error = "TOPOLOGY_LAYOUT_INVALID";
    return false;
  }
  const cJSON* slots = cJSON_GetObjectItemCaseSensitive(parsed, "slots");
  if (!cJSON_IsArray(slots) || cJSON_GetArraySize(slots) < 1 ||
      cJSON_GetArraySize(slots) > 16) {
    cJSON_Delete(parsed);
    error = "TOPOLOGY_LAYOUT_INVALID";
    return false;
  }
  std::vector<const cJSON*> ordered;
  std::set<std::string> ids;
  for (const cJSON* slot = slots->child; slot; slot = slot->next) {
    const char* id = json_string(slot, "id");
    uint32_t x = 0, y = 0;
    if (!exact_keys(slot, {"id", "x", "y"}) || !id || !*id ||
        !unsigned_integer(cJSON_GetObjectItemCaseSensitive(slot, "x"),
                          0, 4095, x) ||
        !unsigned_integer(cJSON_GetObjectItemCaseSensitive(slot, "y"),
                          0, 4095, y) || !ids.insert(id).second) {
      cJSON_Delete(parsed);
      error = "TOPOLOGY_LAYOUT_INVALID";
      return false;
    }
    ordered.push_back(slot);
  }
  std::sort(ordered.begin(), ordered.end(), [](const cJSON* left,
                                               const cJSON* right) {
    return std::strcmp(json_string(left, "id"), json_string(right, "id")) < 0;
  });
  cJSON* normalized_slots = cJSON_CreateArray();
  if (!normalized_slots) {
    cJSON_Delete(parsed);
    error = "NO_MEMORY";
    return false;
  }
  for (const cJSON* slot : ordered) {
    cJSON* copy = cJSON_Duplicate(slot, true);
    if (!copy || !cJSON_AddItemToArray(normalized_slots, copy)) {
      cJSON_Delete(copy);
      cJSON_Delete(normalized_slots);
      cJSON_Delete(parsed);
      error = "NO_MEMORY";
      return false;
    }
  }
  cJSON_ReplaceItemInObjectCaseSensitive(parsed, "slots", normalized_slots);
  result = canonical_clone(parsed);
  cJSON_Delete(parsed);
  if (!result) {
    error = "NO_MEMORY";
    return false;
  }
  error.clear();
  return true;
}

std::string print_json(const cJSON* value) {
  char* bytes = cJSON_PrintUnformatted(value);
  if (!bytes) return {};
  std::string result(bytes);
  cJSON_free(bytes);
  return result;
}

bool hash_json(const cJSON* value, std::string& canonical,
               std::string& hash) {
  cJSON* sorted = canonical_clone(value);
  if (!sorted) return false;
  canonical = print_json(sorted);
  cJSON_Delete(sorted);
  if (canonical.empty()) return false;
  uint8_t digest[32] = {};
  if (mbedtls_sha256(reinterpret_cast<const uint8_t*>(canonical.data()),
                     canonical.size(), digest, 0) != 0) return false;
  static constexpr char kHex[] = "0123456789abcdef";
  hash.resize(64);
  for (size_t i = 0; i < 32; ++i) {
    hash[2 * i] = kHex[digest[i] >> 4];
    hash[2 * i + 1] = kHex[digest[i] & 15];
  }
  std::memset(digest, 0, sizeof(digest));
  return true;
}

bool read_layout_slots(const cJSON* layout,
                       std::vector<std::string>& slot_ids) {
  slot_ids.clear();
  const cJSON* slots = cJSON_GetObjectItemCaseSensitive(layout, "slots");
  for (const cJSON* slot = cJSON_IsArray(slots) ? slots->child : nullptr;
       slot; slot = slot->next) {
    const char* id = json_string(slot, "id");
    if (!id) return false;
    slot_ids.emplace_back(id);
  }
  return !slot_ids.empty() && slot_ids.size() <= 16;
}

bool node_record_valid(const cJSON* node, uint16_t& node_id,
                       const char*& hardware_id, const char*& role) {
  uint32_t parsed_id = 0, rotation = 0, brightness = 0;
  const char* label = json_string(node, "label");
  hardware_id = json_string(node, "hardwareId");
  role = json_string(node, "role");
  const char* profile = json_string(node, "boardProfile");
  const bool plain_node = exact_keys(node, {"nodeId", "hardwareId", "label",
      "role", "enabled", "boardProfile", "rotation", "brightnessCap"});
  const bool topology_member = exact_keys(node, {"nodeId", "hardwareId",
      "label", "role", "enabled", "boardProfile", "rotation",
      "brightnessCap", "slotId"});
  if ((!plain_node && !topology_member) ||
      !unsigned_integer(cJSON_GetObjectItemCaseSensitive(node, "nodeId"),
                        1, UINT16_MAX, parsed_id) ||
      !lower_uuid(hardware_id) || !label || !*label || std::strlen(label) > 320 ||
      !role || (std::strcmp(role, "master") != 0 &&
                std::strcmp(role, "follower") != 0) ||
      !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node, "enabled")) ||
      !profile || std::strcmp(profile, kBoardProfile) != 0 ||
      !unsigned_integer(cJSON_GetObjectItemCaseSensitive(node, "rotation"),
                        0, 180, rotation) || (rotation != 0 && rotation != 180) ||
      !unsigned_integer(cJSON_GetObjectItemCaseSensitive(node, "brightnessCap"),
                        0, 255, brightness)) return false;
  node_id = static_cast<uint16_t>(parsed_id);
  return true;
}

bool topology_members_valid(const NodeConfig& config, const cJSON* topology,
                            const cJSON* layout, std::string& error) {
  const cJSON* members = cJSON_GetObjectItemCaseSensitive(topology, "members");
  std::vector<std::string> slots;
  if (!cJSON_IsArray(members) || cJSON_GetArraySize(members) < 1 ||
      cJSON_GetArraySize(members) > 16 || !read_layout_slots(layout, slots)) {
    error = "TOPOLOGY_MEMBERS_INVALID";
    return false;
  }
  std::set<std::string> assigned_slots;
  std::set<std::string> hardware_ids;
  uint32_t previous_node = 0;
  bool master_found = false, self_found = false;
  for (const cJSON* member = members->child; member; member = member->next) {
    uint16_t node_id = 0;
    const char* hardware_id = nullptr;
    const char* role = nullptr;
    if (!exact_keys(member, {"nodeId", "hardwareId", "label", "role",
                             "enabled", "boardProfile", "rotation",
                             "brightnessCap", "slotId"}) ||
        !node_record_valid(member, node_id, hardware_id, role) ||
        node_id <= previous_node || !hardware_ids.insert(hardware_id).second) {
      error = "TOPOLOGY_MEMBER_INVALID";
      return false;
    }
    previous_node = node_id;
    const cJSON* slot = cJSON_GetObjectItemCaseSensitive(member, "slotId");
    if (cJSON_IsString(slot) && slot->valuestring) {
      const std::string slot_id(slot->valuestring);
      if (std::find(slots.begin(), slots.end(), slot_id) == slots.end() ||
          !assigned_slots.insert(slot_id).second) {
        error = "TOPOLOGY_SLOT_BINDING_INVALID";
        return false;
      }
    } else if (!cJSON_IsNull(slot)) {
      error = "TOPOLOGY_SLOT_BINDING_INVALID";
      return false;
    }
    if (node_id == config.masterNodeId && std::strcmp(role, "master") == 0)
      master_found = true;
    if (node_id == config.nodeId) {
      self_found = config.role == Role::Follower &&
          std::strcmp(role, "follower") == 0 &&
          config.hardwareId == hardware_id && cJSON_IsString(slot);
    }
  }
  if (!master_found || !self_found || assigned_slots.size() != slots.size()) {
    error = "TOPOLOGY_REQUIRED_MEMBER_MISSING";
    return false;
  }
  return true;
}
}  // namespace

bool topology_build_master(const NodeConfig& config, const std::string& rig_json,
                           const std::string& layout_json,
                           const std::string& layout_id,
                           TopologySnapshot& result, std::string& error) {
  result = TopologySnapshot{};
  if (!config.provisioned || config.role != Role::Master ||
      config.nodeId == 0 || !lower_uuid(config.hardwareId.c_str())) {
    error = "MASTER_IDENTITY_INVALID";
    return false;
  }
  cJSON* normalized = nullptr;
  if (!normalized_layout(layout_json, normalized, error)) return false;
  if (layout_id != (json_string(normalized, "id")
                        ? json_string(normalized, "id") : "")) {
    cJSON_Delete(normalized);
    error = "TOPOLOGY_LAYOUT_ID_MISMATCH";
    return false;
  }
  std::vector<std::string> slot_ids;
  if (!read_layout_slots(normalized, slot_ids)) {
    cJSON_Delete(normalized);
    error = "TOPOLOGY_LAYOUT_INVALID";
    return false;
  }

  cJSON* rig = cJSON_ParseWithLength(rig_json.data(), rig_json.size());
  uint32_t master_id = 0, topology_revision = 0;
  const char* group_id = json_string(rig, "groupId");
  const cJSON* rig_nodes = rig
      ? cJSON_GetObjectItemCaseSensitive(rig, "nodes") : nullptr;
  const cJSON* rig_layouts = rig
      ? cJSON_GetObjectItemCaseSensitive(rig, "layouts") : nullptr;
  if (!exact_keys(rig, {"version", "groupId", "masterNodeId", "revision",
                        "nodes", "layouts"}) ||
      !group_id || config.groupId != group_id ||
      !unsigned_integer(cJSON_GetObjectItemCaseSensitive(rig, "masterNodeId"),
                        1, UINT16_MAX, master_id) || master_id != config.nodeId ||
      !unsigned_integer(cJSON_GetObjectItemCaseSensitive(rig, "revision"),
                        0, UINT32_MAX, topology_revision) ||
      !cJSON_IsArray(rig_nodes) || !cJSON_IsArray(rig_layouts)) {
    cJSON_Delete(normalized);
    cJSON_Delete(rig);
    error = "RIG_TOPOLOGY_SOURCE_INVALID";
    return false;
  }
  const cJSON* selected_bindings = nullptr;
  bool selected_layout_found = false;
  for (const cJSON* entry = rig_layouts->child; entry; entry = entry->next) {
    const char* id = json_string(entry, "layoutId");
    if (id && id == layout_id) {
      selected_layout_found = true;
      selected_bindings = cJSON_GetObjectItemCaseSensitive(entry, "bindings");
      break;
    }
  }
  cJSON* implicit_bindings = nullptr;
  if (!selected_layout_found && slot_ids.size() == 1) {
    // A fresh one-panel installation has no saved layout binding yet. Its
    // only display node is unambiguously the Master; multi-panel layouts
    // still require explicit Rig assignments.
    implicit_bindings = cJSON_CreateArray();
    cJSON* binding = cJSON_CreateObject();
    if (!implicit_bindings || !binding ||
        !cJSON_AddStringToObject(binding, "slotId", slot_ids.front().c_str()) ||
        !cJSON_AddNumberToObject(binding, "nodeId", config.nodeId) ||
        !cJSON_AddItemToArray(implicit_bindings, binding)) {
      cJSON_Delete(binding);
      cJSON_Delete(implicit_bindings);
      cJSON_Delete(normalized);
      cJSON_Delete(rig);
      error = "NO_MEMORY";
      return false;
    }
    selected_bindings = implicit_bindings;
  }
  if (!cJSON_IsArray(selected_bindings) ||
      cJSON_GetArraySize(selected_bindings) != static_cast<int>(slot_ids.size())) {
    cJSON_Delete(implicit_bindings);
    cJSON_Delete(normalized);
    cJSON_Delete(rig);
    error = "RIG_LAYOUT_BINDINGS_INCOMPLETE";
    return false;
  }

  struct BoundNode { uint16_t id; cJSON* member; };
  std::vector<BoundNode> members;
  std::set<std::string> bound_slots;
  std::set<uint16_t> bound_nodes;
  for (const cJSON* binding = selected_bindings->child; binding;
       binding = binding->next) {
    const char* slot_id = json_string(binding, "slotId");
    uint32_t node_id = 0;
    if (!exact_keys(binding, {"slotId", "nodeId"}) || !slot_id ||
        !unsigned_integer(cJSON_GetObjectItemCaseSensitive(binding, "nodeId"),
                          1, UINT16_MAX, node_id) ||
        std::find(slot_ids.begin(), slot_ids.end(), slot_id) == slot_ids.end() ||
        !bound_slots.insert(slot_id).second ||
        !bound_nodes.insert(static_cast<uint16_t>(node_id)).second) {
      error = "RIG_LAYOUT_BINDING_INVALID";
      goto fail;
    }
    const cJSON* record = nullptr;
    for (const cJSON* node = rig_nodes->child; node; node = node->next) {
      uint16_t parsed_id = 0;
      const char* hardware = nullptr;
      const char* role = nullptr;
      if (!node_record_valid(node, parsed_id, hardware, role)) continue;
      if (parsed_id == node_id) { record = node; break; }
    }
    if (!record) {
      error = "RIG_LAYOUT_NODE_MISSING";
      goto fail;
    }
    cJSON* member = cJSON_Duplicate(record, true);
    if (!member || !cJSON_AddItemToObject(member, "slotId",
                                           cJSON_CreateString(slot_id))) {
      cJSON_Delete(member);
      error = "NO_MEMORY";
      goto fail;
    }
    members.push_back({static_cast<uint16_t>(node_id), member});
  }
  if (bound_slots.size() != slot_ids.size()) {
    error = "RIG_LAYOUT_BINDINGS_INCOMPLETE";
    goto fail;
  }
  {
    bool master_bound = bound_nodes.count(config.nodeId) != 0;
    if (!master_bound) {
      const cJSON* master_record = nullptr;
      for (const cJSON* node = rig_nodes->child; node; node = node->next) {
        uint16_t parsed_id = 0;
        const char* hardware = nullptr;
        const char* role = nullptr;
        if (node_record_valid(node, parsed_id, hardware, role) &&
            parsed_id == config.nodeId && std::strcmp(role, "master") == 0 &&
            config.hardwareId == hardware) {
          master_record = node;
          break;
        }
      }
      if (!master_record) {
        error = "RIG_MASTER_RECORD_MISSING";
        goto fail;
      }
      cJSON* member = cJSON_Duplicate(master_record, true);
      if (!member || !cJSON_AddItemToObject(member, "slotId", cJSON_CreateNull())) {
        cJSON_Delete(member);
        error = "NO_MEMORY";
        goto fail;
      }
      members.push_back({config.nodeId, member});
    }
  }
  std::sort(members.begin(), members.end(), [](const BoundNode& left,
                                                const BoundNode& right) {
    return left.id < right.id;
  });
  {
    cJSON* topology = cJSON_CreateObject();
    cJSON* member_array = cJSON_CreateArray();
    if (!topology || !member_array) {
      cJSON_Delete(topology);
      cJSON_Delete(member_array);
      error = "NO_MEMORY";
      goto fail;
    }
    cJSON_AddStringToObject(topology, "groupId", config.groupId.c_str());
    cJSON_AddNumberToObject(topology, "masterNodeId", config.nodeId);
    cJSON_AddNumberToObject(topology, "topologyRevision", topology_revision);
    cJSON_AddItemToObject(topology, "layout", normalized);
    normalized = nullptr;
    for (auto& member : members) {
      if (!cJSON_AddItemToArray(member_array, member.member)) {
        cJSON_Delete(topology);
        cJSON_Delete(member_array);
        error = "NO_MEMORY";
        goto fail;
      }
      member.member = nullptr;
    }
    cJSON_AddItemToObject(topology, "members", member_array);
    if (!hash_json(topology, result.json, result.hash)) {
      cJSON_Delete(topology);
      error = "TOPOLOGY_HASH_FAILED";
      goto fail;
    }
    cJSON_Delete(topology);
    result.revision = topology_revision;
    result.layout_id = layout_id;
  }
  cJSON_Delete(rig);
  cJSON_Delete(implicit_bindings);
  return true;

fail:
  for (auto& member : members) cJSON_Delete(member.member);
  cJSON_Delete(implicit_bindings);
  cJSON_Delete(normalized);
  cJSON_Delete(rig);
  return false;
}

bool topology_validate_follower(const NodeConfig& config,
                                const std::string& topology_json,
                                const std::string& expected_hash,
                                const std::string& local_layout_json,
                                TopologySnapshot& result,
                                std::string& error) {
  result = TopologySnapshot{};
  if (!config.provisioned || config.role != Role::Follower ||
      config.nodeId == 0 || config.masterNodeId == 0 ||
      !lower_uuid(config.hardwareId.c_str()) || !lower_hash(expected_hash)) {
    error = "FOLLOWER_IDENTITY_INVALID";
    return false;
  }
  cJSON* topology = cJSON_ParseWithLength(topology_json.data(), topology_json.size());
  uint32_t master_id = 0, revision = 0;
  const char* group_id = json_string(topology, "groupId");
  const cJSON* layout = topology
      ? cJSON_GetObjectItemCaseSensitive(topology, "layout") : nullptr;
  const char* layout_id = json_string(layout, "id");
  cJSON* normalized = nullptr;
  std::string hash, canonical;
  if (!exact_keys(topology, {"groupId", "masterNodeId", "topologyRevision",
                             "layout", "members"}) ||
      !group_id || config.groupId != group_id ||
      !unsigned_integer(cJSON_GetObjectItemCaseSensitive(topology, "masterNodeId"),
                        1, UINT16_MAX, master_id) ||
      master_id != config.masterNodeId ||
      !unsigned_integer(cJSON_GetObjectItemCaseSensitive(topology, "topologyRevision"),
                        0, UINT32_MAX, revision) || !layout_id ||
      !normalized_layout(local_layout_json, normalized, error) ||
      std::strcmp(layout_id, json_string(normalized, "id")) != 0) {
    cJSON_Delete(normalized);
    cJSON_Delete(topology);
    if (error.empty()) error = "TOPOLOGY_HEADER_INVALID";
    return false;
  }
  std::string received_layout;
  std::string expected_layout;
  const cJSON* received_layout_object =
      cJSON_GetObjectItemCaseSensitive(topology, "layout");
  if (!exact_keys(received_layout_object, {"id", "name", "width", "height", "slots"}) ||
      !hash_json(received_layout_object, received_layout, hash) ||
      !hash_json(normalized, expected_layout, hash) ||
      received_layout != expected_layout ||
      !topology_members_valid(config, topology, normalized, error)) {
    cJSON_Delete(normalized);
    cJSON_Delete(topology);
    if (error.empty()) error = "TOPOLOGY_LAYOUT_MISMATCH";
    return false;
  }
  if (!hash_json(topology, canonical, hash) || hash != expected_hash) {
    cJSON_Delete(normalized);
    cJSON_Delete(topology);
    error = "TOPOLOGY_HASH_MISMATCH";
    return false;
  }
  result.revision = revision;
  result.hash = hash;
  result.layout_id = layout_id;
  result.json = canonical;
  cJSON_Delete(normalized);
  cJSON_Delete(topology);
  error.clear();
  return true;
}

bool topology_local_binding(const TopologySnapshot& snapshot, uint16_t node_id,
                            std::string& slot_id, uint8_t& rotation,
                            std::string& error) {
  slot_id.clear();
  rotation = 0;
  cJSON* root = cJSON_ParseWithLength(snapshot.json.data(), snapshot.json.size());
  const cJSON* members = root
      ? cJSON_GetObjectItemCaseSensitive(root, "members") : nullptr;
  for (const cJSON* member = cJSON_IsArray(members) ? members->child : nullptr;
       member; member = member->next) {
    uint32_t id = 0;
    if (!unsigned_integer(cJSON_GetObjectItemCaseSensitive(member, "nodeId"),
                          1, UINT16_MAX, id) || id != node_id) continue;
    const char* assigned = json_string(member, "slotId");
    const cJSON* angle = cJSON_GetObjectItemCaseSensitive(member, "rotation");
    if (!assigned || !cJSON_IsNumber(angle) ||
        (angle->valueint != 0 && angle->valueint != 180)) {
      cJSON_Delete(root);
      error = "LOCAL_PANEL_BINDING_INVALID";
      return false;
    }
    slot_id = assigned;
    rotation = static_cast<uint8_t>(angle->valueint);
    cJSON_Delete(root);
    error.clear();
    return true;
  }
  cJSON_Delete(root);
  error = "LOCAL_PANEL_BINDING_MISSING";
  return false;
}

bool topology_self_test(std::string& error) {
  NodeConfig master;
  master.provisioned = true;
  master.role = Role::Master;
  master.nodeId = 7;
  master.groupId = "00112233-4455-6677-8899-aabbccddeeff";
  master.hardwareId = "00000000-0000-4000-8000-000000000007";
  NodeConfig follower;
  follower.provisioned = true;
  follower.role = Role::Follower;
  follower.nodeId = 42;
  follower.masterNodeId = 7;
  follower.groupId = master.groupId;
  follower.hardwareId = "00000000-0000-4000-8000-00000000002a";
  const std::string rig = R"({"version":2,"groupId":"00112233-4455-6677-8899-aabbccddeeff","masterNodeId":7,"revision":3,"nodes":[{"nodeId":7,"hardwareId":"00000000-0000-4000-8000-000000000007","label":"Master","role":"master","enabled":true,"boardProfile":"seengreat_hub75_s3_n16r8_v1","rotation":0,"brightnessCap":64},{"nodeId":42,"hardwareId":"00000000-0000-4000-8000-00000000002a","label":"Follower","role":"follower","enabled":true,"boardProfile":"seengreat_hub75_s3_n16r8_v1","rotation":180,"brightnessCap":48}],"layouts":[{"layoutId":"horizontal","bindings":[{"slotId":"panel_1","nodeId":7},{"slotId":"panel_2","nodeId":42}]}]})";
  const std::string layout = R"({"id":"horizontal","name":"横2枚","width":128,"height":32,"slots":[{"id":"panel_2","x":64,"y":0},{"id":"panel_1","x":0,"y":0}]})";
  TopologySnapshot built, verified;
  if (!topology_build_master(master, rig, layout, "horizontal", built, error) ||
      !topology_validate_follower(follower, built.json, built.hash, layout,
                                  verified, error) ||
      verified.revision != 3 || verified.layout_id != "horizontal" ||
      verified.hash != built.hash || verified.json != built.json) {
    error = "topology build/verify self-test failed";
    return false;
  }
  if (topology_validate_follower(follower, built.json, std::string(64, '0'),
                                 layout, verified, error)) {
    error = "topology hash mismatch was accepted";
    return false;
  }
  const std::string single_panel_rig = R"({"version":2,"groupId":"00112233-4455-6677-8899-aabbccddeeff","masterNodeId":7,"revision":4,"nodes":[{"nodeId":7,"hardwareId":"00000000-0000-4000-8000-000000000007","label":"Master","role":"master","enabled":true,"boardProfile":"seengreat_hub75_s3_n16r8_v1","rotation":0,"brightnessCap":64}],"layouts":[]})";
  const std::string single_panel_layout = R"({"id":"single","name":"1枚","width":64,"height":32,"slots":[{"id":"panel_1","x":0,"y":0}]})";
  if (!topology_build_master(master, single_panel_rig, single_panel_layout,
                             "single", built, error) ||
      built.revision != 4 || built.layout_id != "single" ||
      built.json.find("\"slotId\":\"panel_1\"") == std::string::npos) {
    error = "single-panel Master fallback self-test failed";
    return false;
  }

  const uint16_t node_ids[] = {
      7, 12, 42, 100, 256, 1024, 4096, 8192,
      16384, 32768, 40000, 48000, 50000, 60000, 65534, 65535};
  const auto hardware_id = [](uint16_t node_id) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string suffix(12, '0');
    uint32_t value = node_id;
    for (int i = 11; i >= 0; --i) {
      suffix[static_cast<size_t>(i)] = kHex[value & 0xf];
      value >>= 4;
    }
    return "00000000-0000-4000-8000-" + suffix;
  };
  const auto layout_id_for = [](size_t count) {
    return std::string("layout_") + std::to_string(count);
  };
  const auto slot_id_for = [](size_t index) {
    std::string value = std::to_string(index);
    if (value.size() < 2) value.insert(value.begin(), '0');
    return std::string("panel_") + value;
  };
  const auto topology_fixture = [&](size_t count, bool omit_last_binding,
                                    bool duplicate_last_node,
                                    std::string& rig_json,
                                    std::string& layout_json,
                                    std::string& selected_layout) {
    selected_layout = layout_id_for(count);
    rig_json = "{\"version\":2,\"groupId\":\"" + master.groupId +
        "\",\"masterNodeId\":7,\"revision\":" +
        std::to_string(100 + count) + ",\"nodes\":[";
    for (size_t index = count; index > 0; --index) {
      const size_t i = index - 1;
      const uint16_t id = node_ids[i];
      if (i + 1 != count) rig_json += ',';
      rig_json += "{\"nodeId\":" + std::to_string(id) +
          ",\"hardwareId\":\"" + hardware_id(id) +
          "\",\"label\":\"Node " + std::to_string(id) +
          "\",\"role\":\"" + (i == 0 ? "master" : "follower") +
          "\",\"enabled\":true,\"boardProfile\":\"" + kBoardProfile +
          "\",\"rotation\":" + (i % 2 ? "180" : "0") +
          ",\"brightnessCap\":64}";
    }
    rig_json += "],\"layouts\":[{\"layoutId\":\"" + selected_layout +
        "\",\"bindings\":[";
    size_t emitted = 0;
    for (size_t index = count; index > 0; --index) {
      const size_t i = index - 1;
      if (omit_last_binding && i + 1 == count) continue;
      const uint16_t id = duplicate_last_node && i + 1 == count
          ? node_ids[i - 1] : node_ids[i];
      if (emitted++) rig_json += ',';
      rig_json += "{\"slotId\":\"" + slot_id_for(i) +
          "\",\"nodeId\":" + std::to_string(id) + "}";
    }
    rig_json += "]}]}";

    layout_json = "{\"id\":\"" + selected_layout +
        "\",\"name\":\"Scale test\",\"width\":" +
        std::to_string(count * 64) + ",\"height\":32,\"slots\":[";
    for (size_t index = count; index > 0; --index) {
      const size_t i = index - 1;
      if (i + 1 != count) layout_json += ',';
      layout_json += "{\"id\":\"" + slot_id_for(i) +
          "\",\"x\":" + std::to_string(i * 64) +
          ",\"y\":0}";
    }
    layout_json += "]}";
  };

  for (const size_t count : {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{16}}) {
    std::string scale_rig, scale_layout, selected_layout;
    topology_fixture(count, false, false, scale_rig, scale_layout,
                     selected_layout);
    TopologySnapshot scale_built;
    if (!topology_build_master(master, scale_rig, scale_layout,
                               selected_layout, scale_built, error) ||
        scale_built.revision != 100 + count ||
        scale_built.layout_id != selected_layout) {
      error = "scaled topology build failed for " + std::to_string(count) +
          " active nodes";
      return false;
    }
    cJSON* topology = cJSON_ParseWithLength(scale_built.json.data(),
                                             scale_built.json.size());
    const cJSON* members = topology
        ? cJSON_GetObjectItemCaseSensitive(topology, "members") : nullptr;
    const cJSON* normalized_layout_object = topology
        ? cJSON_GetObjectItemCaseSensitive(topology, "layout") : nullptr;
    const cJSON* slots = normalized_layout_object
        ? cJSON_GetObjectItemCaseSensitive(normalized_layout_object, "slots")
        : nullptr;
    bool ordered = cJSON_IsArray(members) &&
        cJSON_GetArraySize(members) == static_cast<int>(count) &&
        cJSON_IsArray(slots) && cJSON_GetArraySize(slots) == static_cast<int>(count);
    uint32_t previous_id = 0;
    size_t member_index = 0;
    for (const cJSON* member = ordered ? members->child : nullptr;
         member; member = member->next, ++member_index) {
      uint32_t node_id = 0;
      const cJSON* slot = cJSON_GetObjectItemCaseSensitive(member, "slotId");
      ordered = unsigned_integer(
          cJSON_GetObjectItemCaseSensitive(member, "nodeId"), 1, UINT16_MAX,
          node_id) && node_id > previous_id && cJSON_IsString(slot) &&
          std::strcmp(slot->valuestring, slot_id_for(member_index).c_str()) == 0;
      if (!ordered) break;
      previous_id = node_id;
    }
    cJSON_Delete(topology);
    if (!ordered) {
      error = "scaled topology members or slots were incomplete or unordered";
      return false;
    }
    if (count > 1) {
      NodeConfig scaled_follower = follower;
      scaled_follower.nodeId = node_ids[count - 1];
      scaled_follower.masterNodeId = master.nodeId;
      scaled_follower.hardwareId = hardware_id(scaled_follower.nodeId);
      TopologySnapshot scale_verified;
      if (!topology_validate_follower(scaled_follower, scale_built.json,
                                      scale_built.hash, scale_layout,
                                      scale_verified, error) ||
          scale_verified.hash != scale_built.hash) {
        error = "scaled topology follower verification failed for " +
            std::to_string(count) + " active nodes";
        return false;
      }
    }
  }

  std::string invalid_rig, valid_layout, selected_layout;
  topology_fixture(4, false, true, invalid_rig, valid_layout, selected_layout);
  if (topology_build_master(master, invalid_rig, valid_layout, selected_layout,
                            built, error)) {
    error = "topology accepted one node bound to multiple slots";
    return false;
  }
  topology_fixture(4, true, false, invalid_rig, valid_layout, selected_layout);
  if (topology_build_master(master, invalid_rig, valid_layout, selected_layout,
                            built, error)) {
    error = "topology accepted an unassigned layout slot";
    return false;
  }
  error.clear();
  return true;
}

}  // namespace ledpanel
