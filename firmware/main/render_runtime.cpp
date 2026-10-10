#include "render_runtime.hpp"

#include "lpk_decoder.hpp"
#include "runtime_package.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <cJSON.h>

namespace ledpanel {
namespace {
constexpr size_t kFrameBytes = 64 * 32 * 3;
uint8_t* g_package_bytes = nullptr;
size_t g_package_length = 0;
const uint8_t* g_blob = nullptr;
size_t g_blob_length = 0;
cJSON* g_manifest = nullptr;
std::string g_package_hash;
SemaphoreHandle_t g_mutex = nullptr;
RenderTelemetry g_render_telemetry;

bool lock_runtime() {
  if (!g_mutex) g_mutex = xSemaphoreCreateMutex();
  return g_mutex && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}
void unlock_runtime() { xSemaphoreGive(g_mutex); }

struct RuntimeLock {
  bool locked = lock_runtime();
  ~RuntimeLock() { if (locked) unlock_runtime(); }
};

const cJSON* field(const cJSON* object, const char* name) {
  return cJSON_GetObjectItemCaseSensitive(object, name);
}
const char* text(const cJSON* object, const char* name) {
  const cJSON* value = field(object, name);
  return cJSON_IsString(value) ? value->valuestring : nullptr;
}
bool integer(const cJSON* object, const char* name, int64_t& value) {
  const cJSON* item = field(object, name);
  if (!cJSON_IsNumber(item) || item->valuedouble != item->valueint) return false;
  value = item->valueint;
  return true;
}
const cJSON* find_by_id(const cJSON* array, const char* id) {
  if (!id || !cJSON_IsArray(array)) return nullptr;
  for (const cJSON* item = array->child; item; item = item->next) {
    const char* candidate = text(item, "id");
    if (candidate && std::strcmp(candidate, id) == 0) return item;
  }
  return nullptr;
}
const cJSON* find_cue(const char* id) {
  return find_by_id(field(g_manifest, "cues"), id);
}
const cJSON* find_scene(const char* id) {
  return find_by_id(field(g_manifest, "scenes"), id);
}

uint64_t elapsed_ticks(const PlaybackState& state, uint64_t now_us) {
  if (!state.running || now_us <= state.anchorUs) return state.positionTicks;
  const uint64_t delta = now_us - state.anchorUs;
  const uint64_t add = (delta / 1000000) * 30 + ((delta % 1000000) * 30) / 1000000;
  return add > UINT64_MAX - state.positionTicks ? UINT64_MAX
                                                 : state.positionTicks + add;
}

int64_t floor_div(int64_t numerator, int64_t denominator) {
  int64_t quotient = numerator / denominator;
  const int64_t remainder = numerator % denominator;
  if (remainder != 0 && numerator < 0) --quotient;
  return quotient;
}

int64_t track_value(const cJSON* tracks, const char* name, int64_t tick,
                    int64_t initial) {
  const cJSON* keys = field(tracks, name);
  if (!cJSON_IsArray(keys) || !keys->child) return initial;
  const cJSON* first = keys->child;
  int64_t first_tick = 0, first_value = 0;
  if (!integer(first, "tick", first_tick) || !integer(first, "value", first_value))
    return initial;
  if (tick <= first_tick) return first_value;
  const cJSON* left = first;
  for (const cJSON* right = first->next; right; left = right, right = right->next) {
    int64_t left_tick = 0, left_value = 0, right_tick = 0, right_value = 0;
    if (!integer(left, "tick", left_tick) || !integer(left, "value", left_value) ||
        !integer(right, "tick", right_tick) || !integer(right, "value", right_value))
      return initial;
    if (tick < right_tick) {
      const char* ease = text(left, "ease");
      if (ease && std::strcmp(ease, "step") == 0) return left_value;
      const int64_t numerator = (right_value - left_value) * (tick - left_tick);
      return left_value + floor_div(numerator, right_tick - left_tick);
    }
  }
  int64_t last_value = initial;
  integer(left, "value", last_value);
  return last_value;
}

bool color(const char* hex, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (!hex || hex[0] != '#' || std::strlen(hex) != 7) return false;
  uint32_t value = 0;
  for (int i = 1; i < 7; ++i) {
    const char c = hex[i];
    const int nibble = c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (nibble < 0) return false;
    value = (value << 4) | static_cast<uint32_t>(nibble);
  }
  r = static_cast<uint8_t>(value >> 16);
  g = static_cast<uint8_t>(value >> 8);
  b = static_cast<uint8_t>(value);
  return true;
}

const cJSON* find_asset(const char* id) {
  return find_by_id(field(g_manifest, "assets"), id);
}

bool asset_bytes(const cJSON* asset, const uint8_t*& bytes,
                 uint32_t& width, uint32_t& height) {
  int64_t offset = 0, length = 0, w = 0, h = 0;
  if (!asset || !integer(asset, "offset", offset) ||
      !integer(asset, "length", length) || !integer(asset, "width", w) ||
      !integer(asset, "height", h) || offset < 0 || length < 0 ||
      w < 1 || h < 1 || static_cast<uint64_t>(offset) + length > g_blob_length ||
      static_cast<uint64_t>(length) != static_cast<uint64_t>(w) * h * 3)
    return false;
  bytes = g_blob + offset;
  width = static_cast<uint32_t>(w);
  height = static_cast<uint32_t>(h);
  return true;
}

void blend_pixel(uint8_t* frame, uint32_t x, uint32_t y, uint8_t r, uint8_t g,
                 uint8_t b, uint8_t alpha) {
  if (x >= 64 || y >= 32 || !alpha) return;
  uint8_t* dst = frame + (static_cast<size_t>(y) * 64 + x) * 3;
  const uint16_t inverse = 255 - alpha;
  dst[0] = static_cast<uint8_t>((static_cast<uint16_t>(r) * alpha + dst[0] * inverse + 127) / 255);
  dst[1] = static_cast<uint8_t>((static_cast<uint16_t>(g) * alpha + dst[1] * inverse + 127) / 255);
  dst[2] = static_cast<uint8_t>((static_cast<uint16_t>(b) * alpha + dst[2] * inverse + 127) / 255);
}

bool sequence_asset(const cJSON* layer, int64_t tick, const char*& asset_id) {
  const cJSON* frames = field(layer, "frames");
  int64_t start = 0;
  if (!cJSON_IsArray(frames) || !frames->child || !integer(layer, "startTick", start))
    return false;
  int64_t total = 0;
  for (const cJSON* frame = frames->child; frame; frame = frame->next) {
    int64_t duration = 0;
    if (!integer(frame, "ticks", duration) || duration <= 0 || total > 18000 - duration)
      return false;
    total += duration;
  }
  if (total <= 0) return false;
  int64_t local = std::max<int64_t>(0, tick - start);
  const char* repeat = text(layer, "repeat");
  local = repeat && std::strcmp(repeat, "loop") == 0
      ? local % total : std::min(local, total - 1);
  for (const cJSON* item = frames->child; item; item = item->next) {
    int64_t duration = 0;
    integer(item, "ticks", duration);
    if (local < duration) {
      asset_id = text(item, "assetId");
      return asset_id != nullptr;
    }
    local -= duration;
  }
  return false;
}

bool locate_playhead(const PlaybackState& state, uint64_t now_us,
                     const cJSON*& scene, int64_t& scene_tick) {
  const cJSON* scenes = field(g_manifest, "scenes");
  const int64_t elapsed = static_cast<int64_t>(std::min<uint64_t>(
      elapsed_ticks(state, now_us), static_cast<uint64_t>(INT64_MAX)));
  if (state.mode == "scene") {
    scene = find_scene(state.sceneId.c_str());
    if (!scene) return false;
    int64_t duration = 0;
    if (!integer(scene, "durationTicks", duration) || duration <= 0) return false;
    const char* playback = text(scene, "playback");
    scene_tick = playback && std::strcmp(playback, "loop") == 0
        ? elapsed % duration : std::min<int64_t>(elapsed, duration - 1);
    return true;
  }
  if (state.mode != "cue") return false;
  const cJSON* cues = field(g_manifest, "cues");
  int cue_index = 0;
  for (const cJSON* cue = cJSON_IsArray(cues) ? cues->child : nullptr;
       cue; cue = cue->next, ++cue_index) {
    const char* id = text(cue, "id");
    if (id && state.cueId == id) break;
  }
  if (!cJSON_IsArray(cues) || cue_index >= cJSON_GetArraySize(cues)) return false;
  int64_t cue_elapsed = elapsed;
  const cJSON* cue = cues->child;
  for (int i = 0; i < cue_index; ++i) cue = cue->next;
  while (cue) {
    const char* advance = text(cue, "advance");
    int64_t hold = 0;
    if (!advance || std::strcmp(advance, "auto") != 0 ||
        !integer(cue, "holdTicks", hold) || hold <= 0 || cue_elapsed < hold)
      break;
    cue_elapsed -= hold;
    cue = cue->next;
  }
  if (!cue) return false;
  scene = find_scene(text(cue, "sceneId"));
  if (!scene) return false;
  int64_t duration = 0;
  if (!integer(scene, "durationTicks", duration) || duration <= 0) return false;
  const char* playback = text(scene, "playback");
  scene_tick = playback && std::strcmp(playback, "loop") == 0
      ? cue_elapsed % duration : std::min<int64_t>(cue_elapsed, duration - 1);
  return true;
}

bool render_locked(const PlaybackState& state, const std::string& slot_id,
                   uint8_t rotation, uint64_t now_us, uint8_t* frame) {
  const cJSON* scene = nullptr;
  int64_t scene_tick = 0;
  if (!locate_playhead(state, now_us, scene, scene_tick)) return false;
  const cJSON* layouts = field(g_manifest, "layouts");
  const cJSON* layout = find_by_id(layouts, state.layoutId.c_str());
  if (!layout) return false;
  int64_t canvas_w = 0, canvas_h = 0;
  if (!integer(layout, "width", canvas_w) || !integer(layout, "height", canvas_h))
    return false;
  const cJSON* slots = field(layout, "slots");
  const cJSON* slot = nullptr;
  for (const cJSON* candidate = cJSON_IsArray(slots) ? slots->child : nullptr;
       candidate; candidate = candidate->next) {
    const char* id = text(candidate, "id");
    if ((!slot_id.empty() && id && slot_id == id) ||
        (slot_id.empty() && cJSON_GetArraySize(slots) == 1)) {
      slot = candidate;
      break;
    }
  }
  int64_t slot_x = 0, slot_y = 0;
  if (!slot || !integer(slot, "x", slot_x) || !integer(slot, "y", slot_y))
    return false;
  const cJSON* variants = field(scene, "variants");
  const cJSON* variant = cJSON_GetObjectItemCaseSensitive(
      const_cast<cJSON*>(variants), state.layoutId.c_str());
  const cJSON* layers = field(variant, "layers");
  for (const cJSON* layer = cJSON_IsArray(layers) ? layers->child : nullptr;
       layer; layer = layer->next) {
    int64_t start = 0, end = 0, base_x = 0, base_y = 0, base_opacity = 255;
    if (!integer(layer, "startTick", start) || !integer(layer, "endTick", end) ||
        !integer(layer, "x", base_x) || !integer(layer, "y", base_y) ||
        scene_tick < start || scene_tick >= end) continue;
    integer(layer, "opacity", base_opacity);
    const cJSON* tracks = field(layer, "tracks");
    const int64_t x = track_value(tracks, "x", scene_tick, base_x);
    const int64_t y = track_value(tracks, "y", scene_tick, base_y);
    const int64_t opacity = std::clamp<int64_t>(
        track_value(tracks, "opacity", scene_tick, base_opacity), 0, 255);
    if (opacity == 0) continue;
    const char* kind = text(layer, "kind");
    if (kind && std::strcmp(kind, "rect") == 0) {
      int64_t width = 0, height = 0;
      uint8_t r = 0, g = 0, b = 0;
      if (!integer(layer, "width", width) || !integer(layer, "height", height) ||
          !color(text(layer, "color"), r, g, b)) continue;
      const int64_t left = std::max<int64_t>(x, slot_x);
      const int64_t top = std::max<int64_t>(y, slot_y);
      const int64_t right = std::min<int64_t>(x + width, slot_x + 64);
      const int64_t bottom = std::min<int64_t>(y + height, slot_y + 32);
      for (int64_t py = top; py < bottom; ++py)
        for (int64_t px = left; px < right; ++px)
          blend_pixel(frame, static_cast<uint32_t>(px - slot_x),
                      static_cast<uint32_t>(py - slot_y), r, g, b,
                      static_cast<uint8_t>(opacity));
      continue;
    }
    const char* asset_id = nullptr;
    if (kind && std::strcmp(kind, "sequence") == 0) {
      if (!sequence_asset(layer, scene_tick, asset_id)) continue;
    } else {
      asset_id = text(layer, "assetId");
    }
    const uint8_t* source = nullptr;
    uint32_t width = 0, height = 0;
    if (!asset_bytes(find_asset(asset_id), source, width, height)) continue;
    for (uint32_t sy = 0; sy < height; ++sy) {
      const int64_t py = y + sy;
      if (py < slot_y || py >= slot_y + 32) continue;
      for (uint32_t sx = 0; sx < width; ++sx) {
        const int64_t px = x + sx;
        if (px < slot_x || px >= slot_x + 64) continue;
        const uint8_t* pixel = source + (static_cast<size_t>(sy) * width + sx) * 3;
        const uint16_t packed = static_cast<uint16_t>(pixel[0]) |
                                (static_cast<uint16_t>(pixel[1]) << 8);
        const uint8_t r = static_cast<uint8_t>((((packed >> 11) & 31) << 3) |
                                               (((packed >> 11) & 31) >> 2));
        const uint8_t g = static_cast<uint8_t>((((packed >> 5) & 63) << 2) |
                                               (((packed >> 5) & 63) >> 4));
        const uint8_t b = static_cast<uint8_t>(((packed & 31) << 3) | ((packed & 31) >> 2));
        const uint8_t alpha = static_cast<uint8_t>((pixel[2] * opacity + 127) / 255);
        blend_pixel(frame, static_cast<uint32_t>(px - slot_x),
                    static_cast<uint32_t>(py - slot_y), r, g, b, alpha);
      }
    }
  }
  if (rotation == 180) {
    uint8_t rotated[kFrameBytes];
    for (uint32_t y = 0; y < 32; ++y)
      for (uint32_t x = 0; x < 64; ++x)
        std::memcpy(rotated + (static_cast<size_t>(31 - y) * 64 + (63 - x)) * 3,
                    frame + (static_cast<size_t>(y) * 64 + x) * 3, 3);
    std::memcpy(frame, rotated, kFrameBytes);
  }
  return true;
}

bool cue_index(const char* id, int& index, int& count) {
  const cJSON* cues = field(g_manifest, "cues");
  if (!cJSON_IsArray(cues)) return false;
  count = cJSON_GetArraySize(cues);
  index = -1;
  int i = 0;
  for (const cJSON* cue = cues->child; cue; cue = cue->next, ++i) {
    const char* candidate = text(cue, "id");
    if (id && candidate && std::strcmp(id, candidate) == 0) index = i;
  }
  return count > 0;
}
const cJSON* cue_at(int index) {
  const cJSON* cues = field(g_manifest, "cues");
  const cJSON* cue = cJSON_IsArray(cues) ? cues->child : nullptr;
  while (cue && index-- > 0) cue = cue->next;
  return cue;
}
bool set_cue_state(const cJSON* cue, PlaybackState& state) {
  const char* id = text(cue, "id");
  if (!id) return false;
  state.mode = "cue";
  state.sceneId.clear();
  state.cueId = id;
  state.positionTicks = 0;
  state.running = true;
  return true;
}
void set_stop(PlaybackState& state) {
  state.mode = "stopped";
  state.sceneId.clear();
  state.cueId.clear();
  state.positionTicks = 0;
  state.running = false;
  state.blackout = true;
}
}  // namespace

bool render_runtime_load_owned(uint8_t* bytes, size_t length,
                               const std::string& expected_hash,
                               std::string& error) {
  if (!bytes || length == 0) { error = "PACKAGE_BUFFER_INVALID"; return false; }
  std::string package_hash;
  if (!validate_runtime_package(bytes, length, package_hash, error)) return false;
  if (!expected_hash.empty() && package_hash != expected_hash) {
    error = "ACTIVE_PACKAGE_HASH_MISMATCH";
    return false;
  }
  LpkPackage decoded;
  if (!decode_lpk(bytes, length, decoded, error)) return false;
  cJSON* manifest = cJSON_ParseWithLength(decoded.manifest.data(), decoded.manifest.size());
  if (!manifest) { error = "RUNTIME_MANIFEST_PARSE_FAILED"; return false; }
  RuntimeLock lock;
  if (!lock.locked) { cJSON_Delete(manifest); error = "RUNTIME_LOCK_FAILED"; return false; }
  cJSON_Delete(g_manifest);
  if (g_package_bytes) {
    std::memset(g_package_bytes, 0, g_package_length);
    heap_caps_free(g_package_bytes);
  }
  g_package_bytes = bytes;
  g_package_length = length;
  g_blob = bytes + decoded.header.headerSize + decoded.header.manifestLength;
  g_blob_length = decoded.header.blobLength;
  g_manifest = manifest;
  g_package_hash = package_hash;
  error.clear();
  return true;
}

bool render_runtime_initialize() {
  if (!g_mutex) g_mutex = xSemaphoreCreateMutex();
  return g_mutex != nullptr;
}

bool render_runtime_is_loaded(const std::string& package_hash) {
  RuntimeLock lock;
  return lock.locked && g_manifest && g_package_hash == package_hash;
}

bool render_runtime_default_layout(std::string& layout_id) {
  RuntimeLock lock;
  const char* value = lock.locked ? text(g_manifest, "defaultLayout") : nullptr;
  if (!value) {
    layout_id.clear();
    return false;
  }
  layout_id = value;
  return true;
}

void render_runtime_unload() {
  RuntimeLock lock;
  if (!lock.locked) return;
  cJSON_Delete(g_manifest);
  g_manifest = nullptr;
  if (g_package_bytes) {
    std::memset(g_package_bytes, 0, g_package_length);
    heap_caps_free(g_package_bytes);
  }
  g_package_bytes = nullptr;
  g_package_length = g_blob_length = 0;
  g_blob = nullptr;
  g_package_hash.clear();
}

bool render_runtime_frame(const PlaybackState& state, const std::string& slot_id,
                          uint8_t rotation, uint64_t now_us, uint8_t* rgb,
                          size_t capacity, std::string& error) {
  if (!rgb || capacity < kFrameBytes) { error = "FRAME_BUFFER_TOO_SMALL"; return false; }
  std::memset(rgb, 0, kFrameBytes);
  RuntimeLock lock;
  if (!lock.locked || !g_manifest) { error = "RUNTIME_PACKAGE_NOT_LOADED"; return false; }
  if (!state.running || state.blackout || state.packageHash != g_package_hash ||
      (state.effectiveAtUs && now_us < state.effectiveAtUs)) {
    error.clear();
    return true;
  }
  if (!render_locked(state, slot_id, rotation, now_us, rgb)) {
    std::memset(rgb, 0, kFrameBytes);
    error = "PLAYBACK_TARGET_OR_SLOT_INVALID";
    return false;
  }
  g_render_telemetry = {now_us, elapsed_ticks(state, now_us), state.revision, true};
  error.clear();
  return true;
}

bool render_runtime_telemetry(RenderTelemetry& result) {
  RuntimeLock lock;
  if (!lock.locked) return false;
  result = g_render_telemetry;
  return result.valid;
}

bool render_runtime_command(const char* operation, const cJSON* args,
                            const PlaybackState& current, uint64_t now_us,
                            PlaybackState& next, std::string& error) {
  RuntimeLock lock;
  if (!lock.locked) { error = "RUNTIME_LOCK_FAILED"; return false; }
  next = current;
  const uint64_t position = elapsed_ticks(current, now_us);
  const auto reanchor_clock = [&]() {
    next.anchorUs = now_us;
    next.effectiveAtUs = now_us;
  };
  const auto preserve_position = [&]() {
    next.positionTicks = position;
    reanchor_clock();
  };
  if (!operation) { error = "INVALID_OPERATION"; return false; }
  if (std::strcmp(operation, "blackout") == 0) {
    const cJSON* enabled = field(args, "enabled");
    if (!cJSON_IsBool(enabled)) { error = "INVALID_ARGUMENT"; return false; }
    preserve_position();
    next.blackout = cJSON_IsTrue(enabled);
  } else if (std::strcmp(operation, "brightness") == 0) {
    int64_t value = 0;
    if (!integer(args, "value", value) || value < 0 || value > 255) {
      error = "INVALID_ARGUMENT"; return false;
    }
    preserve_position();
    next.userBrightness = static_cast<uint8_t>(value);
  } else if (std::strcmp(operation, "stop") == 0) {
    set_stop(next);
    reanchor_clock();
  } else if (std::strcmp(operation, "go") == 0) {
    const char* cue_id = text(args, "cueId");
    const cJSON* cue = find_cue(cue_id);
    if (!cue || !set_cue_state(cue, next)) { error = "CUE_NOT_FOUND"; return false; }
    reanchor_clock();
  } else if (std::strcmp(operation, "play_scene") == 0) {
    const char* scene_id = text(args, "sceneId");
    if (!find_scene(scene_id)) { error = "SCENE_NOT_FOUND"; return false; }
    next.mode = "scene";
    next.sceneId = scene_id;
    next.cueId.clear();
    next.positionTicks = 0;
    next.running = true;
    reanchor_clock();
  } else if (std::strcmp(operation, "next_scene") == 0) {
    const cJSON* scenes = field(g_manifest, "scenes");
    const int count = cJSON_GetArraySize(scenes);
    if (count < 1) { error = "SCENE_NOT_FOUND"; return false; }
    int index = -1;
    for (int i = 0; i < count; ++i)
      if (current.sceneId == text(cJSON_GetArrayItem(scenes, i), "id")) index = i;
    next.mode = "scene";
    next.sceneId = text(cJSON_GetArrayItem(scenes, (index + 1) % count), "id");
    next.cueId.clear(); next.positionTicks = 0; next.running = true;
    reanchor_clock();
  } else if (std::strcmp(operation, "next") == 0 ||
             std::strcmp(operation, "prev") == 0) {
    const bool forward = std::strcmp(operation, "next") == 0;
    int index = -1, count = 0;
    if (!cue_index(current.mode == "cue" ? current.cueId.c_str() : nullptr,
                   index, count)) { error = "CUE_LIST_EMPTY"; return false; }
    int target = 0;
    if (current.mode == "cue" && index >= 0)
      target = forward ? index + 1 : index - 1;
    else
      target = forward ? 0 : count - 1;
    if (target < 0) target = 0;
    if (target >= count) {
      set_stop(next);
    } else if (!set_cue_state(cue_at(target), next)) {
      error = "CUE_NOT_FOUND"; return false;
    }
    reanchor_clock();
  } else if (std::strcmp(operation, "pause") == 0) {
    if (!current.running || current.mode == "stopped") {
      error = "PLAYBACK_NOT_RUNNING"; return false;
    }
    next.positionTicks = position;
    next.running = false;
    next.anchorUs = now_us;
    next.effectiveAtUs = now_us;
  } else if (std::strcmp(operation, "resume") == 0) {
    if (current.running || current.mode == "stopped") {
      error = "PLAYBACK_NOT_PAUSED"; return false;
    }
    next.running = true;
    next.anchorUs = now_us;
    next.effectiveAtUs = now_us;
  } else {
    error = "UNSUPPORTED_OPERATION";
    return false;
  }
  error.clear();
  return true;
}

}  // namespace ledpanel
