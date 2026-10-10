#include "node_http_auth.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

namespace ledpanel {
namespace {
constexpr char kHex[] = "0123456789abcdef";

void wipe(uint8_t* bytes, size_t length) {
  volatile uint8_t* cursor = bytes;
  while (length-- != 0) *cursor++ = 0;
}

int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool lower_hex(const std::string& value, size_t digits) {
  if (value.size() != digits) return false;
  for (const char c : value) if (nibble(c) < 0) return false;
  return true;
}

bool decode_key(const std::string& text, std::array<uint8_t, 32>& key) {
  if (!lower_hex(text, 64)) return false;
  for (size_t i = 0; i < key.size(); ++i)
    key[i] = static_cast<uint8_t>((nibble(text[i * 2]) << 4) |
                                  nibble(text[i * 2 + 1]));
  return true;
}

bool valid_uuid(const std::string& value) {
  if (value.size() != 36) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else if (nibble(value[i]) < 0) {
      return false;
    }
  }
  return true;
}

bool valid_path(const std::string& path) {
  if (path.size() < 10 || path.size() > 128 ||
      path.compare(0, 9, "/node/v2/") != 0) return false;
  for (const unsigned char c : path)
    if (c <= 0x20 || c >= 0x7f || c == '\\' || c == '?') return false;
  return true;
}

bool valid_method(const std::string& method) {
  return method == "GET" || method == "POST" || method == "PUT" ||
         method == "PATCH" || method == "DELETE";
}

bool valid_boot_id(const std::string& value) {
  return lower_hex(value, 16);
}

bool valid_nonce(const std::string& value) {
  return lower_hex(value, 32);
}

std::string hex(const uint8_t* bytes, size_t length) {
  std::string value(length * 2, '0');
  for (size_t i = 0; i < length; ++i) {
    value[i * 2] = kHex[bytes[i] >> 4];
    value[i * 2 + 1] = kHex[bytes[i] & 0x0f];
  }
  return value;
}

bool digest(const uint8_t* body, size_t length, std::string& output) {
  static constexpr uint8_t kEmpty = 0;
  uint8_t value[32] = {};
  const uint8_t* input = length == 0 ? &kEmpty : body;
  if ((!input && length != 0) ||
      mbedtls_sha256(input, length, value, 0) != 0) {
    wipe(value, sizeof(value));
    return false;
  }
  output = hex(value, sizeof(value));
  wipe(value, sizeof(value));
  return true;
}

bool hmac(const std::array<uint8_t, 32>& key, const std::string& message,
          std::string& output) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  uint8_t value[32] = {};
  const bool ok = info && mbedtls_md_hmac(
      info, key.data(), key.size(),
      reinterpret_cast<const uint8_t*>(message.data()), message.size(), value) == 0;
  if (ok) output = hex(value, sizeof(value));
  wipe(value, sizeof(value));
  return ok;
}

bool equal_hex(const std::string& left, const std::string& right) {
  size_t difference = left.size() ^ right.size();
  const size_t count = left.size() > right.size() ? left.size() : right.size();
  for (size_t i = 0; i < count; ++i) {
    const uint8_t a = i < left.size() ? static_cast<uint8_t>(left[i]) : 0;
    const uint8_t b = i < right.size() ? static_cast<uint8_t>(right[i]) : 0;
    difference |= a ^ b;
  }
  return difference == 0;
}

bool parse_sequence(uint32_t sequence, std::string& value) {
  value = std::to_string(sequence);
  return true;
}

bool sign(const std::string& session_key_hex, const std::string& message,
          std::string& mac_hex, std::string& error) {
  std::array<uint8_t, 32> key{};
  if (!decode_key(session_key_hex, key)) {
    error = "invalid session key";
    return false;
  }
  const bool ok = hmac(key, message, mac_hex);
  wipe(key.data(), key.size());
  if (!ok) {
    error = "node HTTP HMAC failed";
    return false;
  }
  error.clear();
  return true;
}
}  // namespace

bool node_http_sign_request(const std::string& session_key_hex,
                            const std::string& method,
                            const std::string& path,
                            const std::string& group_id,
                            const std::string& sender_boot_id,
                            const std::string& receiver_boot_id,
                            uint32_t sequence,
                            const std::string& nonce_hex,
                            const uint8_t* body, size_t body_length,
                            std::string& body_hash_hex,
                            std::string& mac_hex, std::string& error) {
  std::string seq;
  if (!valid_method(method) || !valid_path(path) || !valid_uuid(group_id) ||
      !valid_boot_id(sender_boot_id) || !valid_boot_id(receiver_boot_id) ||
      !parse_sequence(sequence, seq) || !valid_nonce(nonce_hex) ||
      (body_length != 0 && !body) || body_length > 2621440) {
    error = "invalid node HTTP request fields";
    return false;
  }
  if (!digest(body, body_length, body_hash_hex)) {
    error = "node HTTP body hash failed";
    return false;
  }
  const std::string message = method + "\n" + path + "\n" + group_id + "\n" +
      sender_boot_id + "\n" + receiver_boot_id + "\n" + seq + "\n" +
      nonce_hex + "\n" + body_hash_hex;
  return sign(session_key_hex, message, mac_hex, error);
}

bool node_http_verify_request(const std::string& session_key_hex,
                              const std::string& method,
                              const std::string& path,
                              const std::string& group_id,
                              const std::string& sender_boot_id,
                              const std::string& receiver_boot_id,
                              uint32_t sequence,
                              const std::string& nonce_hex,
                              const std::string& declared_body_hash_hex,
                              const std::string& declared_mac_hex,
                              const uint8_t* body, size_t body_length,
                              std::string& error) {
  std::string actual_body_hash, actual_mac;
  if (!lower_hex(declared_body_hash_hex, 64) ||
      !lower_hex(declared_mac_hex, 64) ||
      !node_http_sign_request(session_key_hex, method, path, group_id,
          sender_boot_id, receiver_boot_id, sequence, nonce_hex,
          body, body_length, actual_body_hash, actual_mac, error)) return false;
  if (!equal_hex(actual_body_hash, declared_body_hash_hex) ||
      !equal_hex(actual_mac, declared_mac_hex)) {
    error = "node HTTP request authentication failed";
    return false;
  }
  error.clear();
  return true;
}

bool node_http_sign_response(const std::string& session_key_hex,
                             uint16_t status_code,
                             const std::string& path,
                             uint32_t sequence,
                             const std::string& nonce_hex,
                             const uint8_t* body, size_t body_length,
                             std::string& body_hash_hex,
                             std::string& mac_hex, std::string& error) {
  std::string seq;
  if (status_code < 100 || status_code > 599 || !valid_path(path) ||
      !parse_sequence(sequence, seq) || !valid_nonce(nonce_hex) ||
      (body_length != 0 && !body) || body_length > 2621440) {
    error = "invalid node HTTP response fields";
    return false;
  }
  if (!digest(body, body_length, body_hash_hex)) {
    error = "node HTTP body hash failed";
    return false;
  }
  char status[4] = {};
  const int status_length = std::snprintf(status, sizeof(status), "%u",
                                          static_cast<unsigned>(status_code));
  if (status_length != 3) {
    error = "invalid node HTTP status";
    return false;
  }
  const std::string message = std::string(status, 3) + "\n" + path + "\n" +
      seq + "\n" + nonce_hex + "\n" + body_hash_hex;
  return sign(session_key_hex, message, mac_hex, error);
}

bool node_http_verify_response(const std::string& session_key_hex,
                               uint16_t status_code,
                               const std::string& path,
                               uint32_t sequence,
                               const std::string& nonce_hex,
                               const std::string& declared_body_hash_hex,
                               const std::string& declared_mac_hex,
                               const uint8_t* body, size_t body_length,
                               std::string& error) {
  std::string actual_body_hash, actual_mac;
  if (!lower_hex(declared_body_hash_hex, 64) ||
      !lower_hex(declared_mac_hex, 64) ||
      !node_http_sign_response(session_key_hex, status_code, path, sequence,
          nonce_hex, body, body_length, actual_body_hash, actual_mac, error)) return false;
  if (!equal_hex(actual_body_hash, declared_body_hash_hex) ||
      !equal_hex(actual_mac, declared_mac_hex)) {
    error = "node HTTP response authentication failed";
    return false;
  }
  error.clear();
  return true;
}

bool node_http_auth_self_test(std::string& error) {
  std::string key;
  for (unsigned i = 0; i < 32; ++i) {
    key.push_back(kHex[i >> 4]);
    key.push_back(kHex[i & 0x0f]);
  }
  const std::string group = "00112233-4455-6677-8899-aabbccddeeff";
  const std::string nonce = "00112233445566778899aabbccddeeff";
  const std::string path = "/node/v2/health";
  const std::string request_body = "{\"x\":1}";
  std::string body_hash, mac;
  if (!node_http_sign_request(key, "POST", path, group,
      "0000000000000001", "0000000000000002", 3, nonce,
      reinterpret_cast<const uint8_t*>(request_body.data()), request_body.size(),
      body_hash, mac, error) ||
      body_hash != "5041bf1f713df204784353e82f6a4a535931cb64f1f4b4a5aeaffcb720918b22" ||
      mac != "a0d1b3331837969851b26bce4fdd94d1e75a4e6f90fa27abfb019379f9fe67d2") {
    error = "node HTTP request MAC vector mismatch";
    return false;
  }
  if (!node_http_verify_request(key, "POST", path, group,
      "0000000000000001", "0000000000000002", 3, nonce, body_hash, mac,
      reinterpret_cast<const uint8_t*>(request_body.data()), request_body.size(), error))
    return false;
  const std::string changed_body = "{\"x\":2}";
  if (node_http_verify_request(key, "POST", path, group,
      "0000000000000001", "0000000000000002", 3, nonce, body_hash, mac,
      reinterpret_cast<const uint8_t*>(changed_body.data()), changed_body.size(), error)) {
    error = "node HTTP request accepted a changed body";
    return false;
  }
  if (!node_http_sign_response(key, 200, path, 3, nonce, nullptr, 0,
      body_hash, mac, error) ||
      body_hash != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ||
      mac != "9c3871ff450b985865d29a1896e10492cbb42a16eaad8638fd0fd0893a5c3918" ||
      !node_http_verify_response(key, 200, path, 3, nonce, body_hash, mac,
                                 nullptr, 0, error)) {
    error = "node HTTP response MAC vector mismatch";
    return false;
  }
  if (node_http_sign_request(key, "GET", "/node/v2/health?x=1", group,
      "0000000000000001", "0000000000000002", 3, nonce, nullptr, 0,
      body_hash, mac, error) ||
      node_http_sign_request(key, "get", path, group,
      "0000000000000001", "0000000000000002", 0, nonce, nullptr, 0,
      body_hash, mac, error)) {
    error = "node HTTP auth accepted a noncanonical request";
    return false;
  }
  error.clear();
  return true;
}

}  // namespace ledpanel
