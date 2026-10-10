#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ledpanel {

enum class SyncMessageType : uint8_t {
  Hello = 1,
  Offer = 2,
  Accept = 3,
  SessionOk = 4,
  TimeRequest = 5,
  TimeResponse = 6,
  Heartbeat = 10,
  Prepare = 11,
  Ready = 12,
  Commit = 13,
  Ack = 14,
  Abort = 15,
  State = 16,
  Blackout = 17,
};

struct SyncPacket {
  SyncMessageType type = SyncMessageType::Hello;
  uint16_t sender_node_id = 0;
  uint64_t sender_boot_id = 0;
  uint32_t sequence = 0;
  std::string payload;
};

// LPS2 wire codec. `group_id` is the usual hyphenated UUID and `key_hex` is a
// 32-byte key encoded as 64 hexadecimal characters. The caller selects the
// group key for handshake packets and the per-peer session key afterwards.
bool sync_packet_encode(const SyncPacket& packet, const std::string& group_id,
                        const std::string& key_hex,
                        std::vector<uint8_t>& output, std::string& error);
bool sync_packet_decode(const uint8_t* bytes, size_t length,
                        const std::string& expected_group_id,
                        const std::string& key_hex, SyncPacket& packet,
                        std::string& error);
bool sync_protocol_self_test(std::string& error);
bool sync_derive_session_key(const std::string& group_key_hex,
                             const std::string& nonce_b_hex,
                             const std::string& nonce_a_hex,
                             uint64_t master_boot_id,
                             uint64_t follower_boot_id,
                             std::string& session_key_hex,
                             std::string& error);

class SyncReplayWindow {
 public:
  bool accept(uint32_t sequence);
  void reset();

 private:
  uint32_t highest_sequence_ = 0;
  uint64_t received_ = 0;
  bool initialized_ = false;
};

}  // namespace ledpanel
