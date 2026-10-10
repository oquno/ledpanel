#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace ledpanel {
struct ContentState {
  bool has_active = false;
  bool has_candidate = false;
  uint8_t active_slot = 0;
  uint8_t candidate_slot = 0;
  uint32_t generation = 0;
  uint32_t active_length = 0;
  uint32_t candidate_length = 0;
  std::string active_hash;
  std::string candidate_hash;
};
bool content_get_state(ContentState& state, std::string& error);
bool content_read_candidate(uint8_t* bytes, size_t capacity, size_t& length,
                            std::string& hash, std::string& error);
bool content_read_active(uint8_t* bytes, size_t capacity, size_t& length,
                         std::string& hash, std::string& error);
bool content_stage(const uint8_t* bytes, size_t length, const std::string& hash,
                   std::string& error);
bool content_activate(uint32_t target_generation, std::string& error);
bool content_advance_active_generation(const std::string& active_hash,
                                       uint32_t target_generation,
                                       std::string& error);
}
