#include "sync_protocol.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mbedtls/md.h>

namespace ledpanel {
namespace {
constexpr size_t kHeaderSize = 40;
constexpr size_t kMacSize = 32;
constexpr size_t kMaxPayloadSize = 1128;
constexpr size_t kMaxPacketSize = kHeaderSize + kMaxPayloadSize + kMacSize;
constexpr uint8_t kProtocolMajor = 2;
constexpr char kHexDigits[] = "0123456789abcdef";

void wipe(uint8_t* bytes, size_t length) {
  volatile uint8_t* cursor = bytes;
  while (length-- != 0) *cursor++ = 0;
}

int hex_nibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool decode_key(const std::string& text, std::array<uint8_t, 32>& key) {
  if (text.size() != key.size() * 2) return false;
  for (size_t i = 0; i < key.size(); ++i) {
    const int hi = hex_nibble(text[i * 2]);
    const int lo = hex_nibble(text[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      wipe(key.data(), key.size());
      return false;
    }
    key[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool decode_fixed_hex(const std::string& text, uint8_t* output,
                      size_t output_size) {
  if (text.size() != output_size * 2) return false;
  for (size_t i = 0; i < output_size; ++i) {
    const int hi = hex_nibble(text[i * 2]);
    const int lo = hex_nibble(text[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      wipe(output, output_size);
      return false;
    }
    output[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool decode_uuid(const std::string& text, std::array<uint8_t, 16>& uuid) {
  if (text.size() != 36) return false;
  size_t out = 0;
  for (size_t i = 0; i < text.size();) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i++] != '-') return false;
      continue;
    }
    if (i + 1 >= text.size() || out >= uuid.size()) return false;
    const int hi = hex_nibble(text[i]);
    const int lo = hex_nibble(text[i + 1]);
    if (hi < 0 || lo < 0) return false;
    uuid[out++] = static_cast<uint8_t>((hi << 4) | lo);
    i += 2;
  }
  return out == uuid.size();
}

bool valid_type(uint8_t type) {
  return (type >= 1 && type <= 6) || (type >= 10 && type <= 17);
}

void put_le16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8);
}

void put_le32(uint8_t* output, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    output[i] = static_cast<uint8_t>(value >> (i * 8));
}

void put_le64(uint8_t* output, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    output[i] = static_cast<uint8_t>(value >> (i * 8));
}

uint16_t get_le16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8);
}

uint32_t get_le32(const uint8_t* input) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i)
    value |= static_cast<uint32_t>(input[i]) << (i * 8);
  return value;
}

uint64_t get_le64(const uint8_t* input) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= static_cast<uint64_t>(input[i]) << (i * 8);
  return value;
}

std::string encode_hex(const uint8_t* bytes, size_t length) {
  std::string output(length * 2, '0');
  for (size_t i = 0; i < length; ++i) {
    output[i * 2] = kHexDigits[bytes[i] >> 4];
    output[i * 2 + 1] = kHexDigits[bytes[i] & 15];
  }
  return output;
}

bool calculate_hmac(const uint8_t* key, const uint8_t* bytes, size_t length,
                    uint8_t output[kMacSize]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return info && mbedtls_md_hmac(info, key, 32, bytes, length, output) == 0;
}

bool constant_time_equal(const uint8_t* left, const uint8_t* right,
                         size_t length) {
  uint8_t mismatch = 0;
  for (size_t i = 0; i < length; ++i) mismatch |= left[i] ^ right[i];
  return mismatch == 0;
}
}  // namespace

bool sync_packet_encode(const SyncPacket& packet, const std::string& group_id,
                        const std::string& key_hex,
                        std::vector<uint8_t>& output, std::string& error) {
  const uint8_t type = static_cast<uint8_t>(packet.type);
  if (!valid_type(type) || packet.sender_node_id == 0 ||
      packet.payload.size() > kMaxPayloadSize) {
    error = "invalid packet fields";
    return false;
  }

  std::array<uint8_t, 16> group{};
  std::array<uint8_t, 32> key{};
  if (!decode_uuid(group_id, group)) {
    error = "invalid group UUID";
    return false;
  }
  if (!decode_key(key_hex, key)) {
    error = "invalid authentication key";
    return false;
  }

  const size_t packet_size = kHeaderSize + packet.payload.size() + kMacSize;
  output.assign(packet_size, 0);
  uint8_t* header = output.data();
  std::memcpy(header, "LPS2", 4);
  header[4] = kProtocolMajor;
  header[5] = type;
  put_le16(header + 6, static_cast<uint16_t>(packet.payload.size()));
  std::memcpy(header + 8, group.data(), group.size());
  put_le32(header + 24, packet.sender_node_id);
  put_le64(header + 28, packet.sender_boot_id);
  put_le32(header + 36, packet.sequence);
  if (!packet.payload.empty())
    std::memcpy(header + kHeaderSize, packet.payload.data(), packet.payload.size());

  uint8_t digest[kMacSize] = {};
  const bool signed_ok = calculate_hmac(key.data(), header, packet_size - kMacSize,
                                        digest);
  wipe(key.data(), key.size());
  if (!signed_ok) {
    wipe(output.data(), output.size());
    output.clear();
    wipe(digest, sizeof(digest));
    error = "packet HMAC failed";
    return false;
  }
  std::memcpy(output.data() + packet_size - kMacSize, digest, sizeof(digest));
  wipe(digest, sizeof(digest));
  return true;
}

bool sync_packet_decode(const uint8_t* bytes, size_t length,
                        const std::string& expected_group_id,
                        const std::string& key_hex, SyncPacket& packet,
                        std::string& error) {
  if (!bytes || length < kHeaderSize + kMacSize || length > kMaxPacketSize) {
    error = "packet size is invalid";
    return false;
  }
  if (std::memcmp(bytes, "LPS2", 4) != 0 || bytes[4] != kProtocolMajor ||
      !valid_type(bytes[5]) || get_le16(bytes + 6) > kMaxPayloadSize ||
      length != kHeaderSize + get_le16(bytes + 6) + kMacSize ||
      get_le32(bytes + 24) == 0 || get_le32(bytes + 24) > UINT16_MAX) {
    error = "packet header is invalid";
    return false;
  }

  std::array<uint8_t, 16> expected_group{};
  std::array<uint8_t, 32> key{};
  if (!decode_uuid(expected_group_id, expected_group)) {
    error = "invalid expected group UUID";
    return false;
  }
  if (!constant_time_equal(bytes + 8, expected_group.data(), expected_group.size())) {
    error = "packet group mismatch";
    return false;
  }
  if (!decode_key(key_hex, key)) {
    error = "invalid authentication key";
    return false;
  }

  uint8_t digest[kMacSize] = {};
  const bool hmac_ok = calculate_hmac(key.data(), bytes, length - kMacSize, digest);
  wipe(key.data(), key.size());
  if (!hmac_ok || !constant_time_equal(digest, bytes + length - kMacSize,
                                        kMacSize)) {
    wipe(digest, sizeof(digest));
    error = "packet authentication failed";
    return false;
  }
  wipe(digest, sizeof(digest));

  packet.type = static_cast<SyncMessageType>(bytes[5]);
  packet.sender_node_id = static_cast<uint16_t>(get_le32(bytes + 24));
  packet.sender_boot_id = get_le64(bytes + 28);
  packet.sequence = get_le32(bytes + 36);
  const size_t payload_size = get_le16(bytes + 6);
  packet.payload.assign(reinterpret_cast<const char*>(bytes + kHeaderSize),
                        payload_size);
  return true;
}

bool sync_derive_session_key(const std::string& group_key_hex,
                             const std::string& nonce_b_hex,
                             const std::string& nonce_a_hex,
                             uint64_t master_boot_id,
                             uint64_t follower_boot_id,
                             std::string& session_key_hex,
                             std::string& error) {
  session_key_hex.clear();
  std::array<uint8_t, 32> key{};
  std::array<uint8_t, 16> nonce_b{};
  std::array<uint8_t, 16> nonce_a{};
  if (!decode_key(group_key_hex, key)) {
    error = "invalid group key";
    return false;
  }
  if (!decode_fixed_hex(nonce_b_hex, nonce_b.data(), nonce_b.size()) ||
      !decode_fixed_hex(nonce_a_hex, nonce_a.data(), nonce_a.size())) {
    wipe(key.data(), key.size());
    wipe(nonce_b.data(), nonce_b.size());
    wipe(nonce_a.data(), nonce_a.size());
    error = "invalid handshake nonce";
    return false;
  }

  uint8_t material[60] = {};
  std::memcpy(material, "LPS2/session", 12);
  std::memcpy(material + 12, nonce_b.data(), nonce_b.size());
  std::memcpy(material + 28, nonce_a.data(), nonce_a.size());
  put_le64(material + 44, master_boot_id);
  put_le64(material + 52, follower_boot_id);
  uint8_t derived[32] = {};
  const bool ok = calculate_hmac(key.data(), material, sizeof(material), derived);
  wipe(key.data(), key.size());
  wipe(nonce_b.data(), nonce_b.size());
  wipe(nonce_a.data(), nonce_a.size());
  wipe(material, sizeof(material));
  if (!ok) {
    wipe(derived, sizeof(derived));
    error = "session key derivation failed";
    return false;
  }
  session_key_hex = encode_hex(derived, sizeof(derived));
  wipe(derived, sizeof(derived));
  error.clear();
  return true;
}

bool SyncReplayWindow::accept(uint32_t sequence) {
  if (!initialized_) {
    initialized_ = true;
    highest_sequence_ = sequence;
    received_ = 1;
    return true;
  }

  const uint32_t forward = sequence - highest_sequence_;
  if (forward != 0 && forward < 0x80000000U) {
    received_ = forward >= 64 ? 1 : (received_ << forward) | 1;
    highest_sequence_ = sequence;
    return true;
  }

  const uint32_t age = highest_sequence_ - sequence;
  if (age >= 64) return false;
  const uint64_t mask = uint64_t{1} << age;
  if ((received_ & mask) != 0) return false;
  received_ |= mask;
  return true;
}

void SyncReplayWindow::reset() {
  highest_sequence_ = 0;
  received_ = 0;
  initialized_ = false;
}

bool sync_protocol_self_test(std::string& error) {
  static constexpr char kExpectedHex[] =
      "4c50533202012d0000112233445566778899aabbccddeeff341200000807060504030201"
      "d0c0b0a07b226e6f6e636542223a223030303130323033303430353036303730383039"
      "306130623063306430653066227d45366d63ee26ffa5a715c9fade79a530bbf11a619a"
      "36eeb85c4e47aca8cce695";
  const std::string group_id = "00112233-4455-6677-8899-aabbccddeeff";
  std::string key_hex;
  key_hex.reserve(64);
  for (unsigned i = 0; i < 32; ++i) {
    constexpr char digits[] = "0123456789abcdef";
    key_hex.push_back(digits[i >> 4]);
    key_hex.push_back(digits[i & 15]);
  }

  SyncPacket source;
  source.type = SyncMessageType::Hello;
  source.sender_node_id = 0x1234;
  source.sender_boot_id = 0x0102030405060708ULL;
  source.sequence = 0xa0b0c0d0;
  source.payload = "{\"nonceB\":\"000102030405060708090a0b0c0d0e0f\"}";
  std::vector<uint8_t> encoded;
  if (!sync_packet_encode(source, group_id, key_hex, encoded, error)) return false;
  if (encoded.size() * 2 != sizeof(kExpectedHex) - 1) {
    error = "known-answer packet length mismatch";
    return false;
  }
  for (size_t i = 0; i < encoded.size(); ++i) {
    const int hi = hex_nibble(kExpectedHex[i * 2]);
    const int lo = hex_nibble(kExpectedHex[i * 2 + 1]);
    if (hi < 0 || lo < 0 || encoded[i] != static_cast<uint8_t>((hi << 4) | lo)) {
      error = "known-answer packet bytes mismatch";
      return false;
    }
  }

  SyncPacket decoded;
  if (!sync_packet_decode(encoded.data(), encoded.size(), group_id, key_hex,
                          decoded, error) ||
      decoded.type != source.type || decoded.sender_node_id != source.sender_node_id ||
      decoded.sender_boot_id != source.sender_boot_id ||
      decoded.sequence != source.sequence || decoded.payload != source.payload) {
    if (error.empty()) error = "known-answer packet roundtrip mismatch";
    return false;
  }

  const std::vector<uint8_t> known_packet = encoded;
  encoded[40] ^= 1;
  if (sync_packet_decode(encoded.data(), encoded.size(), group_id, key_hex,
                         decoded, error)) {
    error = "modified packet was accepted";
    return false;
  }
  error.clear();
  if (sync_packet_decode(known_packet.data(), known_packet.size(),
                         "00112233-4455-6677-8899-aabbccddeefe", key_hex,
                         decoded, error)) {
    error = "packet for another group was accepted";
    return false;
  }
  error.clear();
  if (sync_packet_decode(known_packet.data(), known_packet.size(), group_id,
                         std::string(64, '0'), decoded, error)) {
    error = "packet with the wrong key was accepted";
    return false;
  }
  error.clear();
  if (sync_packet_decode(known_packet.data(), known_packet.size() - 1, group_id, key_hex,
                         decoded, error)) {
    error = "truncated packet was accepted";
    return false;
  }
  error.clear();
  std::string session_key;
  if (!sync_derive_session_key(
          key_hex, "000102030405060708090a0b0c0d0e0f",
          "101112131415161718191a1b1c1d1e1f",
          0x0102030405060708ULL, 0x1112131415161718ULL,
          session_key, error) ||
      session_key != "c49e73f7b3de2a6a9d6e164747e0b726"
                      "a5158ad83752e3475b5904c641f4f555") {
    if (error.empty()) error = "session-key known-answer mismatch";
    return false;
  }
  if (sync_derive_session_key(key_hex, "invalid", "101112131415161718191a1b1c1d1e1f",
                              1, 2, session_key, error)) {
    error = "invalid nonce was accepted";
    return false;
  }
  error.clear();
  if (!sync_derive_session_key(
          key_hex, "000102030405060708090a0b0c0d0e0f",
          "101112131415161718191a1b1c1d1e1f",
          0x0102030405060708ULL, 0x1112131415161718ULL,
          session_key, error)) return false;
  SyncPacket blackout;
  blackout.type = SyncMessageType::Blackout;
  blackout.sender_node_id = 7;
  blackout.sender_boot_id = 0x1112131415161718ULL;
  blackout.sequence = 9;
  blackout.payload = "{\"revision\":9,\"reason\":\"user\",\"nowUs\":\"123456\"}";
  std::vector<uint8_t> blackout_wire;
  if (!sync_packet_encode(blackout, group_id, session_key, blackout_wire, error) ||
      !sync_packet_decode(blackout_wire.data(), blackout_wire.size(), group_id,
                          session_key, decoded, error) ||
      decoded.type != blackout.type || decoded.sender_node_id != blackout.sender_node_id ||
      decoded.sender_boot_id != blackout.sender_boot_id ||
      decoded.sequence != blackout.sequence || decoded.payload != blackout.payload) {
    if (error.empty()) error = "authenticated BLACKOUT packet roundtrip mismatch";
    return false;
  }
  std::fill(session_key.begin(), session_key.end(), '\0');
  session_key.clear();

  SyncReplayWindow replay;
  if (!replay.accept(10) || !replay.accept(12) || !replay.accept(11) ||
      replay.accept(11) || replay.accept(10) || !replay.accept(100) ||
      replay.accept(36) || !replay.accept(37)) {
    error = "replay-window test failed";
    return false;
  }
  return true;
}

}  // namespace ledpanel
