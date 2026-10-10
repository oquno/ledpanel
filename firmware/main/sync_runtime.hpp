#pragma once

#include "node_state.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ledpanel {

struct SyncPeerSnapshot {
  uint16_t node_id = 0;
  std::string hardware_id;
  std::string label;
  std::string health;
  std::string fault_code;
  std::string topology_hash;
  std::string active_hash;
  std::string candidate_hash;
  uint32_t revision = 0;
  uint32_t topology_revision = 0;
  int64_t last_seen_ago_ms = 0;
  int64_t clock_offset_us = 0;
  uint32_t clock_rtt_us = 0;
  int64_t clock_sample_age_ms = -1;
  bool enabled = false;
  bool online = false;
  bool authenticated = false;
  bool clock_synced = false;
};

void sync_runtime_start(NodeConfig* config, PlaybackState* state, uint64_t* boot_id);
void sync_runtime_peer_snapshot(std::vector<SyncPeerSnapshot>& peers);
void sync_runtime_broadcast_blackout(uint32_t revision, const char* reason);
bool sync_runtime_update_state(const PlaybackState& state);
bool sync_runtime_state_snapshot(PlaybackState& state);
// Playback timestamps are in the Master's clock domain on every node.
bool sync_runtime_render_snapshot(PlaybackState& state, uint64_t& master_now_us);
bool sync_runtime_apply_state(const PlaybackState& state,
                              const std::string& command_id,
                              uint32_t timeout_ms, std::string& error);
bool sync_runtime_transaction_active();
bool sync_runtime_node_http_authorize(
    uint16_t node_id, uint32_t source_ipv4, const std::string& sender_boot_id,
    uint32_t sequence, const std::string& nonce_hex,
    const std::string& method, const std::string& path,
    const std::string& declared_body_hash_hex,
    const std::string& declared_request_mac_hex,
    const uint8_t* request_body, size_t request_body_length,
    std::string& session_key, std::string& error);
bool sync_runtime_node_http_exchange(
    uint16_t node_id, uint32_t source_ipv4, const std::string& sender_boot_id,
    uint32_t sequence, const std::string& nonce_hex,
    const std::string& method, const std::string& path,
    const std::string& declared_body_hash_hex,
    const std::string& declared_request_mac_hex,
    const uint8_t* request_body, size_t request_body_length,
    uint16_t response_status, const uint8_t* response_body,
    size_t response_body_length, std::string& response_body_hash_hex,
    std::string& response_mac_hex, std::string& error);
bool sync_runtime_node_http_request(
    uint16_t remote_node_id, const std::string& method, const std::string& path,
    const uint8_t* request_body, size_t request_body_length,
    const std::string& if_candidate_hash, uint8_t* response_body,
    size_t response_capacity, size_t& response_body_length,
    uint16_t& response_status, std::string& response_candidate_hash,
    std::string& error);
bool sync_runtime_self_test(std::string& error);

}  // namespace ledpanel
