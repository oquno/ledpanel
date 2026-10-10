#include "runtime_package.hpp"
#include "lpk_decoder.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <cJSON.h>
#include <mbedtls/sha256.h>

namespace ledpanel {
namespace {
bool integer(const cJSON* value, int64_t min, int64_t max, int64_t& out) {
  if (!cJSON_IsNumber(value) || value->valuedouble < min || value->valuedouble > max ||
      value->valuedouble != static_cast<double>(value->valueint)) return false;
  out = value->valueint;
  return true;
}

const char* string(const cJSON* object, const char* key) {
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(value) ? value->valuestring : nullptr;
}

bool valid_id(const char* id) {
  if (!id || id[0] < 'a' || id[0] > 'z') return false;
  size_t length = 0;
  while (id[length]) {
    const unsigned char ch = static_cast<unsigned char>(id[length]);
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
    if (++length > 32) return false;
  }
  return length > 0;
}

bool keys_within(const cJSON* object, std::initializer_list<const char*> allowed) {
  if (!cJSON_IsObject(object)) return false;
  for (const cJSON* item = object->child; item; item = item->next) {
    bool found = false;
    for (const char* key : allowed) if (item->string && std::strcmp(item->string, key) == 0) found = true;
    if (!found) return false;
  }
  return true;
}

bool validate_layout_slots(const cJSON* layout, uint32_t width, uint32_t height,
                           std::string& error) {
  const cJSON* slots = cJSON_GetObjectItemCaseSensitive(layout, "slots");
  if (!cJSON_IsArray(slots) || cJSON_GetArraySize(slots) < 1 ||
      cJSON_GetArraySize(slots) > 16) {
    error = "layout slots must contain 1..16 panels";
    return false;
  }
  struct Slot { std::string id; uint32_t x; uint32_t y; };
  std::vector<Slot> parsed;
  for (const cJSON* slot = slots->child; slot; slot = slot->next) {
    const char* id = string(slot, "id");
    int64_t x = 0, y = 0;
    if (!keys_within(slot, {"id", "x", "y"}) || !valid_id(id) ||
        !integer(cJSON_GetObjectItemCaseSensitive(slot, "x"), 0, 4095, x) ||
        !integer(cJSON_GetObjectItemCaseSensitive(slot, "y"), 0, 4095, y) ||
        static_cast<uint64_t>(x) + 64 > width ||
        static_cast<uint64_t>(y) + 32 > height) {
      error = "layout slot is invalid or outside the canvas";
      return false;
    }
    for (const auto& previous : parsed) {
      if (previous.id == id) {
        error = "layout contains duplicate slot IDs";
        return false;
      }
      const bool overlaps = static_cast<uint64_t>(x) < previous.x + 64 &&
          static_cast<uint64_t>(x) + 64 > previous.x &&
          static_cast<uint64_t>(y) < previous.y + 32 &&
          static_cast<uint64_t>(y) + 32 > previous.y;
      if (overlaps) {
        error = "layout slots overlap";
        return false;
      }
    }
    parsed.push_back({id, static_cast<uint32_t>(x), static_cast<uint32_t>(y)});
  }
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

bool hash_equals(const uint8_t* data, size_t length, const char* expected) {
  if (!expected || std::strlen(expected) != 64) return false;
  uint8_t digest[32];
  if (mbedtls_sha256(data, length, digest, 0) != 0) return false;
  static constexpr char kHex[] = "0123456789abcdef";
  uint8_t mismatch = 0;
  for (size_t i = 0; i < 32; ++i) {
    mismatch |= static_cast<uint8_t>(expected[2 * i] ^ kHex[digest[i] >> 4]);
    mismatch |= static_cast<uint8_t>(expected[2 * i + 1] ^ kHex[digest[i] & 15]);
  }
  std::memset(digest, 0, sizeof(digest));
  return mismatch == 0;
}

bool validate_manifest(const cJSON* manifest, const LpkPackage& package,
                       std::string& error) {
  int64_t value = 0;
  if (!integer(cJSON_GetObjectItemCaseSensitive(manifest, "version"), 2, 2, value) ||
      !integer(cJSON_GetObjectItemCaseSensitive(manifest, "runtimeMinor"), 0, 0, value) ||
      !integer(cJSON_GetObjectItemCaseSensitive(manifest, "fps"), 30, 30, value) ||
      std::strcmp(string(manifest, "renderer") ? string(manifest, "renderer") : "",
                  "lp-raster-1") != 0 || !valid_id(string(manifest, "defaultLayout"))) {
    error = "unsupported or malformed runtime manifest";
    return false;
  }

  const cJSON* layouts = cJSON_GetObjectItemCaseSensitive(manifest, "layouts");
  if (!cJSON_IsArray(layouts) || cJSON_GetArraySize(layouts) < 1 ||
      cJSON_GetArraySize(layouts) > 16) {
    error = "layout count must be 1..16";
    return false;
  }
  std::vector<std::string> layout_ids;
  std::set<std::string> layout_set;
  if (!keys_within(manifest, {"version", "runtimeMinor", "renderer", "fps", "projectId",
                              "layouts", "defaultLayout", "assets", "scenes", "cues"})) {
    error = "unknown runtime manifest property";
    return false;
  }
  value = 0;
  cJSON* layout = nullptr;
  cJSON_ArrayForEach(layout, layouts) {
    const char* id = string(layout, "id");
    int64_t width = 0, height = 0;
    const char* name = string(layout, "name");
    if (!keys_within(layout, {"id", "name", "width", "height", "slots"}) ||
        !name || !*name || std::strlen(name) > 320 ||
        !valid_id(id) || !layout_set.insert(id).second ||
        !integer(cJSON_GetObjectItemCaseSensitive(layout, "width"), 1, 4096, width) ||
        !integer(cJSON_GetObjectItemCaseSensitive(layout, "height"), 1, 4096, height) ||
        width * height > 262144) {
      error = "invalid layout";
      return false;
    }
    if (!validate_layout_slots(layout, static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height), error))
      return false;
    if (std::strcmp(id, string(manifest, "defaultLayout")) == 0) value = 1;
    layout_ids.emplace_back(id);
  }
  if (!value) {
    error = "default layout not found";
    return false;
  }

  const cJSON* assets = cJSON_GetObjectItemCaseSensitive(manifest, "assets");
  if (!cJSON_IsArray(assets) || cJSON_GetArraySize(assets) > 512) {
    error = "invalid asset list";
    return false;
  }
  std::set<std::string> asset_ids;
  std::vector<std::string> ordered_ids;
  std::vector<std::pair<size_t, size_t>> asset_ranges;
  uint64_t total_pixels = 0;
  cJSON* asset = nullptr;
  cJSON_ArrayForEach(asset, assets) {
    const char* id = string(asset, "id");
    const char* hash = string(asset, "sha256");
    int64_t width = 0, height = 0, offset = 0, length = 0;
    if (!keys_within(asset, {"id", "width", "height", "offset", "length", "sha256"}) ||
        !valid_id(id) || !asset_ids.insert(id).second ||
        !integer(cJSON_GetObjectItemCaseSensitive(asset, "width"), 1, 4096, width) ||
        !integer(cJSON_GetObjectItemCaseSensitive(asset, "height"), 1, 4096, height) ||
        !integer(cJSON_GetObjectItemCaseSensitive(asset, "offset"), 0, package.blobSize, offset) ||
        !integer(cJSON_GetObjectItemCaseSensitive(asset, "length"), 1, package.blobSize, length) ||
        static_cast<uint64_t>(width) * height * 3 != static_cast<uint64_t>(length) ||
        static_cast<uint64_t>(offset + length) > package.blobSize || !hash) {
      error = "invalid asset dimensions, offsets, or length";
      return false;
    }
    if (!hash_equals(package.blob + offset, length, hash)) {
      error = "asset hash mismatch";
      return false;
    }
    total_pixels += static_cast<uint64_t>(width) * height;
    if (total_pixels > 800000) {
      error = "asset pixel limit exceeded";
      return false;
    }
    asset_ranges.emplace_back(static_cast<size_t>(offset), static_cast<size_t>(length));
  }
  std::sort(asset_ranges.begin(), asset_ranges.end());
  size_t expected_offset = 0;
  for (const auto& range : asset_ranges) {
    if (range.first != expected_offset) {
      error = "asset ranges overlap or leave a gap";
      return false;
    }
    expected_offset += range.second;
  }
  if (expected_offset != package.blobSize) {
    error = "asset ranges do not consume the complete blob";
    return false;
  }

  const cJSON* scenes = cJSON_GetObjectItemCaseSensitive(manifest, "scenes");
  if (!cJSON_IsArray(scenes) || cJSON_GetArraySize(scenes) < 1 || cJSON_GetArraySize(scenes) > 64) {
    error = "scene count must be 1..64";
    return false;
  }
  std::set<std::string> scene_ids;
  cJSON* scene = nullptr;
  cJSON_ArrayForEach(scene, scenes) {
    const char* id = string(scene, "id");
    const char* playback = string(scene, "playback");
    int64_t duration = 0;
    const cJSON* variants = cJSON_GetObjectItemCaseSensitive(scene, "variants");
    if (!keys_within(scene, {"id", "name", "durationTicks", "playback", "variants"}) || !string(scene, "name") ||
        !valid_id(id) || !scene_ids.insert(id).second || !playback ||
        (std::strcmp(playback, "loop") && std::strcmp(playback, "hold")) ||
        !integer(cJSON_GetObjectItemCaseSensitive(scene, "durationTicks"), 1, 18000, duration) ||
        !cJSON_IsObject(variants) || cJSON_GetArraySize(variants) != static_cast<int>(layout_ids.size())) {
      error = "invalid scene";
      return false;
    }
    for (const auto& layout_id : layout_ids) {
      int64_t scene_keys = 0;
      const cJSON* variant = cJSON_GetObjectItemCaseSensitive(variants, layout_id.c_str());
      const cJSON* layers = cJSON_GetObjectItemCaseSensitive(variant, "layers");
      if (!keys_within(variant, {"layers"}) || !cJSON_IsArray(layers) || cJSON_GetArraySize(layers) > 16) {
        error = "invalid scene layout variant";
        return false;
      }
      cJSON* layer = nullptr;
      std::set<std::string> layer_ids;
      cJSON_ArrayForEach(layer, layers) {
        const char* kind = string(layer, "kind");
        const char* asset_id = string(layer, "assetId");
        int64_t start = 0, end = 0, x = 0, y = 0, opacity = 0;
        if (!keys_within(layer, {"id", "kind", "x", "y", "opacity", "startTick", "endTick",
                                 "tracks", "assetId", "width", "height", "color", "frames", "repeat"})) {
          error = "unknown runtime layer property";
          return false;
        }
        const cJSON* tracks = cJSON_GetObjectItemCaseSensitive(layer, "tracks");
        if (!cJSON_IsObject(tracks)) {
          error = "invalid layer tracks";
          return false;
        }
        cJSON* track = nullptr;
        cJSON_ArrayForEach(track, tracks) {
          const char* name = track->string;
          if (!name || (std::strcmp(name, "x") && std::strcmp(name, "y") && std::strcmp(name, "opacity")) ||
              !cJSON_IsArray(track) || cJSON_GetArraySize(track) < 1 || cJSON_GetArraySize(track) > 64) {
            error = "invalid animation track";
            return false;
          }
          int64_t previous_tick = -1;
          int key_index = 0;
          const int key_count = cJSON_GetArraySize(track);
          cJSON* key = nullptr;
          cJSON_ArrayForEach(key, track) {
            int64_t key_tick = 0, key_value = 0;
            const char* ease = string(key, "ease");
            const int64_t min_value = !std::strcmp(name, "opacity") ? 0 : -4096;
            const int64_t max_value = !std::strcmp(name, "opacity") ? 255 : 4096;
            if (!keys_within(key, {"tick", "value", "ease"}) ||
                !integer(cJSON_GetObjectItemCaseSensitive(key, "tick"), 0, duration, key_tick) ||
                key_tick <= previous_tick ||
                !integer(cJSON_GetObjectItemCaseSensitive(key, "value"), min_value, max_value, key_value) ||
                !ease || (std::strcmp(ease, "step") && std::strcmp(ease, "linear")) ||
                (key_index == key_count - 1 && std::strcmp(ease, "step"))) {
              error = "invalid animation key";
              return false;
            }
            previous_tick = key_tick;
            ++key_index;
          }
          scene_keys += key_count;
        }
        if (scene_keys > 512) {
          error = "animation key limit exceeded";
          return false;
        }
        const char* layer_id = string(layer, "id");
        const bool common = valid_id(layer_id) && layer_ids.insert(layer_id).second &&
            integer(cJSON_GetObjectItemCaseSensitive(layer, "startTick"), 0, duration - 1, start) &&
            integer(cJSON_GetObjectItemCaseSensitive(layer, "endTick"), 1, duration, end) && start < end &&
            integer(cJSON_GetObjectItemCaseSensitive(layer, "x"), -4096, 4096, x) &&
            integer(cJSON_GetObjectItemCaseSensitive(layer, "y"), -4096, 4096, y) &&
            integer(cJSON_GetObjectItemCaseSensitive(layer, "opacity"), 0, 255, opacity);
        if (!common || !kind) {
          error = "invalid runtime layer";
          return false;
        }
        if (!std::strcmp(kind, "sprite")) {
          if (!keys_within(layer, {"id", "kind", "x", "y", "opacity", "startTick", "endTick", "tracks", "assetId"}) ||
              !asset_id || !asset_ids.count(asset_id)) {
            error = "sprite asset missing";
            return false;
          }
        } else if (!std::strcmp(kind, "rect")) {
          int64_t w = 0, h = 0;
          const char* color = string(layer, "color");
          if (!keys_within(layer, {"id", "kind", "x", "y", "opacity", "startTick", "endTick", "tracks", "width", "height", "color"}) ||
              !integer(cJSON_GetObjectItemCaseSensitive(layer, "width"), 1, 2048, w) ||
              !integer(cJSON_GetObjectItemCaseSensitive(layer, "height"), 1, 2048, h) ||
              !color || std::strlen(color) != 7 || color[0] != '#' ||
              std::strspn(color + 1, "0123456789abcdefABCDEF") != 6) {
            error = "invalid rect layer";
            return false;
          }
        } else if (!std::strcmp(kind, "sequence")) {
          const cJSON* frames = cJSON_GetObjectItemCaseSensitive(layer, "frames");
          const char* repeat = string(layer, "repeat");
          if (!keys_within(layer, {"id", "kind", "x", "y", "opacity", "startTick", "endTick", "tracks", "frames", "repeat"}) ||
              !repeat || (std::strcmp(repeat, "loop") && std::strcmp(repeat, "hold")) ||
              !cJSON_IsArray(frames) || cJSON_GetArraySize(frames) < 1 ||
              cJSON_GetArraySize(frames) > 512) {
            error = "invalid sequence layer";
            return false;
          }
          cJSON* frame = nullptr;
          int64_t frame_ticks = 0;
          int64_t frame_width = 0, frame_height = 0;
          cJSON_ArrayForEach(frame, frames) {
            int64_t ticks = 0;
            const char* frame_asset = string(frame, "assetId");
            if (!keys_within(frame, {"assetId", "ticks"}) || !frame_asset || !asset_ids.count(frame_asset) ||
                !integer(cJSON_GetObjectItemCaseSensitive(frame, "ticks"), 1, 18000, ticks)) {
              error = "invalid sequence frame";
              return false;
            }
            const cJSON* asset_entry = nullptr;
            cJSON_ArrayForEach(asset_entry, assets) {
              if (std::strcmp(string(asset_entry, "id"), frame_asset) == 0) {
                const cJSON* w = cJSON_GetObjectItemCaseSensitive(asset_entry, "width");
                const cJSON* h = cJSON_GetObjectItemCaseSensitive(asset_entry, "height");
                if (!frame_width) { frame_width = w->valueint; frame_height = h->valueint; }
                else if (frame_width != w->valueint || frame_height != h->valueint) {
                  error = "sequence frame dimensions differ";
                  return false;
                }
                break;
              }
            }
            frame_ticks += ticks;
            if (frame_ticks > 18000) {
              error = "sequence duration limit exceeded";
              return false;
            }
          }
        } else {
          error = "unsupported runtime layer kind";
          return false;
        }
      }
    }
  }

  const cJSON* cues = cJSON_GetObjectItemCaseSensitive(manifest, "cues");
  if (!cJSON_IsArray(cues) || cJSON_GetArraySize(cues) < 1 || cJSON_GetArraySize(cues) > 128) {
    error = "cue count must be 1..128";
    return false;
  }
  std::set<std::string> cue_ids;
  cJSON* cue = nullptr;
  cJSON_ArrayForEach(cue, cues) {
    const char* id = string(cue, "id");
    const char* scene_id = string(cue, "sceneId");
    const char* advance = string(cue, "advance");
    const cJSON* hold = cJSON_GetObjectItemCaseSensitive(cue, "holdTicks");
    int64_t hold_ticks = 0;
    if (!keys_within(cue, {"id", "name", "sceneId", "advance", "holdTicks"}) || !string(cue, "name") ||
        !valid_id(id) || !cue_ids.insert(id).second || !scene_id || !scene_ids.count(scene_id) ||
        !advance || (std::strcmp(advance, "manual") && std::strcmp(advance, "auto"))) {
      error = "invalid cue";
      return false;
    }
    if ((!std::strcmp(advance, "manual") && !cJSON_IsNull(hold)) ||
        (!std::strcmp(advance, "auto") && !integer(hold, 1, 108000, hold_ticks))) {
      error = "invalid cue holdTicks";
      return false;
    }
  }
  return true;
}
}  // namespace

bool validate_runtime_package(const uint8_t* bytes, size_t length,
                              std::string& package_hash, std::string& error) {
  LpkPackage package;
  if (!decode_lpk(bytes, length, package, error)) return false;
  if (bytes[6] != 0 || bytes[7] != 0 || bytes[56] || bytes[57] || bytes[58] || bytes[59] ||
      bytes[60] || bytes[61] || bytes[62] || bytes[63]) {
    error = "LPK flags, reserved fields, or minor version are not supported";
    return false;
  }
  cJSON* manifest = cJSON_ParseWithLength(package.manifest.data(), package.manifest.size());
  if (!manifest) {
    error = "invalid runtime manifest JSON";
    return false;
  }
  const bool valid = validate_manifest(manifest, package, error);
  cJSON_Delete(manifest);
  if (!valid) return false;
  uint8_t digest[32];
  if (mbedtls_sha256(bytes, length, digest, 0) != 0) {
    error = "package hash failed";
    return false;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  package_hash.resize(64);
  for (size_t i = 0; i < 32; ++i) {
    package_hash[2 * i] = kHex[digest[i] >> 4];
    package_hash[2 * i + 1] = kHex[digest[i] & 15];
  }
  std::memset(digest, 0, sizeof(digest));
  return true;
}

bool runtime_package_get_layout(const uint8_t* bytes, size_t length,
                                const std::string& layout_id,
                                std::string& layout_json,
                                std::string& error) {
  layout_json.clear();
  std::string package_hash;
  if (!validate_runtime_package(bytes, length, package_hash, error)) return false;
  LpkPackage package;
  if (!decode_lpk(bytes, length, package, error)) return false;
  cJSON* manifest = cJSON_ParseWithLength(package.manifest.data(),
                                          package.manifest.size());
  const cJSON* layouts = manifest
      ? cJSON_GetObjectItemCaseSensitive(manifest, "layouts") : nullptr;
  const cJSON* selected = nullptr;
  for (const cJSON* item = cJSON_IsArray(layouts) ? layouts->child : nullptr;
       item; item = item->next) {
    const char* id = string(item, "id");
    if (id && id == layout_id) {
      selected = item;
      break;
    }
  }
  cJSON* canonical = canonical_clone(selected);
  cJSON_Delete(manifest);
  if (!canonical) {
    error = "requested runtime layout is unavailable";
    return false;
  }
  char* serialized = cJSON_PrintUnformatted(canonical);
  cJSON_Delete(canonical);
  if (!serialized) {
    error = "runtime layout serialization failed";
    return false;
  }
  layout_json.assign(serialized);
  cJSON_free(serialized);
  if (layout_json.size() > 4096) {
    layout_json.clear();
    error = "runtime layout exceeds topology limit";
    return false;
  }
  error.clear();
  return true;
}

bool runtime_package_get_default_layout(const uint8_t* bytes, size_t length,
                                        std::string& layout_id,
                                        std::string& error) {
  layout_id.clear();
  std::string package_hash;
  if (!validate_runtime_package(bytes, length, package_hash, error)) return false;
  LpkPackage package;
  if (!decode_lpk(bytes, length, package, error)) return false;
  cJSON* manifest = cJSON_ParseWithLength(package.manifest.data(),
                                          package.manifest.size());
  const char* value = manifest ? string(manifest, "defaultLayout") : nullptr;
  if (!value || !valid_id(value)) {
    cJSON_Delete(manifest);
    error = "runtime default layout is invalid";
    return false;
  }
  layout_id.assign(value);
  cJSON_Delete(manifest);
  error.clear();
  return true;
}

bool runtime_package_self_test(std::string& error) {
  const auto accepts = [](const char* json) {
    cJSON* root = cJSON_Parse(json);
    int64_t width = 0, height = 0;
    const cJSON* width_item = root
        ? cJSON_GetObjectItemCaseSensitive(root, "width") : nullptr;
    const cJSON* height_item = root
        ? cJSON_GetObjectItemCaseSensitive(root, "height") : nullptr;
    const bool valid = root && integer(width_item, 1, 4096, width) &&
        integer(height_item, 1, 4096, height);
    std::string reason;
    const bool slots_valid = valid && validate_layout_slots(
        root, static_cast<uint32_t>(width), static_cast<uint32_t>(height), reason);
    cJSON_Delete(root);
    return slots_valid;
  };
  const auto grid_json = [](uint32_t columns, uint32_t rows) {
    cJSON* root = cJSON_CreateObject();
    cJSON* slots = cJSON_CreateArray();
    if (!root || !slots ||
        !cJSON_AddNumberToObject(root, "width", columns * 64) ||
        !cJSON_AddNumberToObject(root, "height", rows * 32) ||
        !cJSON_AddItemToObject(root, "slots", slots)) {
      cJSON_Delete(slots);
      cJSON_Delete(root);
      return std::string{};
    }
    for (uint32_t i = 0; i < columns * rows; ++i) {
      cJSON* slot = cJSON_CreateObject();
      const std::string id = "panel_" + std::to_string(i);
      if (!slot || !cJSON_AddStringToObject(slot, "id", id.c_str()) ||
          !cJSON_AddNumberToObject(slot, "x", (i % columns) * 64) ||
          !cJSON_AddNumberToObject(slot, "y", (i / columns) * 32) ||
          !cJSON_AddItemToArray(slots, slot)) {
        cJSON_Delete(slot);
        cJSON_Delete(root);
        return std::string{};
      }
    }
    char* bytes = cJSON_PrintUnformatted(root);
    const std::string result = bytes ? bytes : "";
    cJSON_free(bytes);
    cJSON_Delete(root);
    return result;
  };
  const auto accepts_string = [&](const std::string& json) {
    return !json.empty() && accepts(json.c_str());
  };
  const std::string horizontal_eight = grid_json(8, 1);
  const std::string vertical_eight = grid_json(1, 8);
  const std::string grid_two_by_two = grid_json(2, 2);
  if (!accepts("{\"width\":128,\"height\":32,\"slots\":[{\"id\":\"panel_1\",\"x\":0,\"y\":0},{\"id\":\"panel_2\",\"x\":64,\"y\":0}]}") ||
      !accepts_string(horizontal_eight) || !accepts_string(vertical_eight) ||
      !accepts_string(grid_two_by_two) ||
      !accepts("{\"width\":160,\"height\":64,\"slots\":[{\"id\":\"panel_a\",\"x\":0,\"y\":0},{\"id\":\"panel_b\",\"x\":96,\"y\":32}]}") ||
      accepts("{\"width\":128,\"height\":32,\"slots\":[{\"id\":\"panel_1\",\"x\":0,\"y\":0},{\"id\":\"panel_2\",\"x\":32,\"y\":0}]}") ||
      accepts("{\"width\":128,\"height\":32,\"slots\":[{\"id\":\"panel_1\",\"x\":0,\"y\":0},{\"id\":\"panel_1\",\"x\":64,\"y\":0}]}") ||
      accepts("{\"width\":100,\"height\":32,\"slots\":[{\"id\":\"panel_1\",\"x\":0,\"y\":0},{\"id\":\"panel_2\",\"x\":64,\"y\":0}]}") ||
      accepts("{\"width\":128,\"height\":32}")) {
    error = "runtime package slot validation self-test failed";
    return false;
  }
  error.clear();
  return true;
}
}  // namespace ledpanel
