#pragma once

#include "node_state.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

struct cJSON;

namespace ledpanel {

bool render_runtime_initialize();
// Takes ownership of `bytes` on success. The buffer must be allocated with
// heap_caps_malloc and remains resident until the next successful load.
bool render_runtime_load_owned(uint8_t* bytes, size_t length,
                               const std::string& expected_hash,
                               std::string& error);
bool render_runtime_is_loaded(const std::string& package_hash);
bool render_runtime_default_layout(std::string& layout_id);
void render_runtime_unload();
struct RenderTelemetry {
  uint64_t master_time_us = 0;
  uint64_t playhead_tick = 0;
  uint32_t revision = 0;
  bool valid = false;
};
bool render_runtime_telemetry(RenderTelemetry& result);

// Renders one node's 64x32 slot into tightly packed RGB888.
bool render_runtime_frame(const PlaybackState& state, const std::string& slot_id,
                          uint8_t rotation, uint64_t now_us, uint8_t* rgb,
                          size_t capacity, std::string& error);

// Applies a validated protocol command to a playback-state copy.
bool render_runtime_command(const char* operation, const cJSON* args,
                            const PlaybackState& current, uint64_t now_us,
                            PlaybackState& next, std::string& error);

}  // namespace ledpanel
