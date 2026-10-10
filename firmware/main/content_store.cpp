#include "content_store.hpp"
#include "lpk_decoder.hpp"
#include <algorithm>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <nvs.h>

namespace ledpanel {
namespace {
constexpr uint32_t kRecordVersion = 2;
struct ContentRecord {
  uint32_t version;
  uint32_t generation;
  uint8_t flags;
  uint8_t activeSlot;
  uint8_t candidateSlot;
  uint8_t reserved;
  uint32_t activeLength;
  uint32_t candidateLength;
  char activeHash[65];
  char candidateHash[65];
};

const esp_partition_t* partition(uint8_t slot) {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                  static_cast<esp_partition_subtype_t>(slot ? 0x41 : 0x40),
                                  nullptr);
}

bool valid_hash(const char hash[65]) {
  if (hash[64] != 0) return false;
  for (size_t i = 0; i < 64; ++i)
    if (!((hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f'))) return false;
  return true;
}

bool read_record(ContentRecord& record, std::string& error) {
  std::memset(&record, 0, sizeof(record));
  nvs_handle_t handle;
  if (nvs_open("ledpanel", NVS_READONLY, &handle) != ESP_OK) {
    error = "nvs unavailable";
    return false;
  }
  size_t size = sizeof(record);
  const esp_err_t err = nvs_get_blob(handle, "content", &record, &size);
  nvs_close(handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    record.version = kRecordVersion;
    return true;
  }
  if (err != ESP_OK || size != sizeof(record) || record.version != kRecordVersion ||
      record.activeSlot > 1 || record.candidateSlot > 1 || record.reserved != 0 ||
      (record.flags & ~3U) ||
      ((record.flags & 1U) && (record.activeLength < 64 || record.activeLength > kMaxPackageBytes)) ||
      ((record.flags & 2U) && (record.candidateLength < 64 || record.candidateLength > kMaxPackageBytes)) ||
      (!(record.flags & 1U) && record.activeLength != 0) ||
      (!(record.flags & 2U) && record.candidateLength != 0) ||
      ((record.flags & 1U) && !valid_hash(record.activeHash)) ||
      ((record.flags & 2U) && !valid_hash(record.candidateHash)) ||
      ((record.flags & 3U) == 3U && record.activeSlot == record.candidateSlot)) {
    error = "content metadata invalid";
    return false;
  }
  return true;
}

bool save_record(const ContentRecord& record, std::string& error) {
  nvs_handle_t handle;
  if (nvs_open("ledpanel", NVS_READWRITE, &handle) != ESP_OK) {
    error = "nvs unavailable";
    return false;
  }
  esp_err_t err = nvs_set_blob(handle, "content", &record, sizeof(record));
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) {
    error = "content metadata commit failed";
    return false;
  }
  return true;
}
}  // namespace

bool content_get_state(ContentState& state, std::string& error) {
  ContentRecord record{};
  if (!read_record(record, error)) return false;
  state.has_active = record.flags & 1U;
  state.has_candidate = record.flags & 2U;
  state.active_slot = record.activeSlot;
  state.candidate_slot = record.candidateSlot;
  state.generation = record.generation;
  state.active_length = record.activeLength;
  state.candidate_length = record.candidateLength;
  if (state.has_active) state.active_hash = record.activeHash;
  if (state.has_candidate) state.candidate_hash = record.candidateHash;
  return true;
}

bool content_read_candidate(uint8_t* bytes, size_t capacity, size_t& length,
                            std::string& hash, std::string& error) {
  length = 0;
  hash.clear();
  ContentRecord record{};
  if (!read_record(record, error)) return false;
  if (!(record.flags & 2U)) {
    error = "no validated candidate";
    return false;
  }
  if (!bytes || capacity < record.candidateLength) {
    error = "candidate buffer too small";
    return false;
  }
  const auto* source = partition(record.candidateSlot);
  if (!source || record.candidateLength > source->size) {
    error = "candidate partition unavailable";
    return false;
  }
  uint8_t* chunk = static_cast<uint8_t*>(
      heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!chunk) {
    error = "internal transfer buffer unavailable";
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  bool valid = mbedtls_sha256_starts(&sha, 0) == 0;
  size_t offset = 0;
  while (valid && offset < record.candidateLength) {
    const size_t count = std::min<size_t>(4096, record.candidateLength - offset);
    if (esp_partition_read(source, offset, chunk, count) != ESP_OK ||
        mbedtls_sha256_update(&sha, chunk, count) != 0) {
      valid = false;
      break;
    }
    std::memcpy(bytes + offset, chunk, count);
    offset += count;
  }
  uint8_t digest[32] = {};
  if (valid) valid = mbedtls_sha256_finish(&sha, digest) == 0;
  mbedtls_sha256_free(&sha);
  std::memset(chunk, 0, 4096);
  heap_caps_free(chunk);
  if (valid) {
    static constexpr char kHex[] = "0123456789abcdef";
    char actual_hash[65];
    for (size_t i = 0; i < 32; ++i) {
      actual_hash[2 * i] = kHex[digest[i] >> 4];
      actual_hash[2 * i + 1] = kHex[digest[i] & 15];
    }
    actual_hash[64] = 0;
    valid = std::memcmp(actual_hash, record.candidateHash, sizeof(actual_hash)) == 0;
    if (valid) hash.assign(actual_hash, 64);
  }
  std::memset(digest, 0, sizeof(digest));
  if (!valid) {
    std::memset(bytes, 0, record.candidateLength);
    error = "candidate readback hash mismatch";
    return false;
  }
  length = record.candidateLength;
  error.clear();
  return true;
}

bool content_read_active(uint8_t* bytes, size_t capacity, size_t& length,
                         std::string& hash, std::string& error) {
  length = 0;
  hash.clear();
  ContentRecord record{};
  if (!read_record(record, error)) return false;
  if (!(record.flags & 1U)) {
    error = "no active package";
    return false;
  }
  if (!bytes || capacity < record.activeLength) {
    error = "active package buffer too small";
    return false;
  }
  const auto* source = partition(record.activeSlot);
  if (!source || record.activeLength > source->size) {
    error = "active partition unavailable";
    return false;
  }
  uint8_t* chunk = static_cast<uint8_t*>(
      heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!chunk) {
    error = "internal transfer buffer unavailable";
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  bool valid = mbedtls_sha256_starts(&sha, 0) == 0;
  size_t offset = 0;
  while (valid && offset < record.activeLength) {
    const size_t count = std::min<size_t>(4096, record.activeLength - offset);
    if (esp_partition_read(source, offset, chunk, count) != ESP_OK ||
        mbedtls_sha256_update(&sha, chunk, count) != 0) {
      valid = false;
      break;
    }
    std::memcpy(bytes + offset, chunk, count);
    offset += count;
  }
  uint8_t digest[32] = {};
  if (valid) valid = mbedtls_sha256_finish(&sha, digest) == 0;
  mbedtls_sha256_free(&sha);
  std::memset(chunk, 0, 4096);
  heap_caps_free(chunk);
  if (valid) {
    static constexpr char kHex[] = "0123456789abcdef";
    char actual_hash[65];
    for (size_t i = 0; i < 32; ++i) {
      actual_hash[2 * i] = kHex[digest[i] >> 4];
      actual_hash[2 * i + 1] = kHex[digest[i] & 15];
    }
    actual_hash[64] = 0;
    valid = std::memcmp(actual_hash, record.activeHash, sizeof(actual_hash)) == 0;
    if (valid) hash.assign(actual_hash, 64);
  }
  std::memset(digest, 0, sizeof(digest));
  if (!valid) {
    std::memset(bytes, 0, record.activeLength);
    error = "active package readback hash mismatch";
    return false;
  }
  length = record.activeLength;
  error.clear();
  return true;
}

bool content_stage(const uint8_t* bytes, size_t length, const std::string& hash,
                   std::string& error) {
  if (!bytes || length < 64 || length > kMaxPackageBytes || hash.size() != 64) {
    error = "package bounds invalid";
    return false;
  }
  ContentRecord record{};
  if (!read_record(record, error)) return false;
  const uint8_t slot = record.flags & 1U ? static_cast<uint8_t>(record.activeSlot ^ 1U) : 0;
  record.flags &= static_cast<uint8_t>(~2U);
  record.candidateLength = 0;
  record.candidateHash[0] = 0;
  if (!save_record(record, error)) return false;
  const auto* target = partition(slot);
  if (!target || length > target->size) {
    error = "package exceeds content slot";
    return false;
  }
  uint8_t* io = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!io) {
    error = "internal transfer buffer unavailable";
    return false;
  }
  if (esp_partition_erase_range(target, 0, target->size) != ESP_OK) {
    heap_caps_free(io);
    error = "candidate erase failed";
    return false;
  }
  size_t offset = 0;
  while (offset < length) {
    const size_t source_length = std::min<size_t>(4096, length - offset);
    const size_t write_length = (source_length + 3U) & ~size_t(3U);
    std::memset(io, 0, write_length);
    std::memcpy(io, bytes + offset, source_length);
    if (esp_partition_write(target, offset, io, write_length) != ESP_OK) {
      heap_caps_free(io);
      error = "candidate write failed";
      return false;
    }
    offset += source_length;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  bool valid = mbedtls_sha256_starts(&sha, 0) == 0;
  size_t checked = 0;
  while (valid && checked < length) {
    const size_t source_length = std::min<size_t>(4096, length - checked);
    const size_t read_length = (source_length + 3U) & ~size_t(3U);
    if (esp_partition_read(target, checked, io, read_length) != ESP_OK ||
        mbedtls_sha256_update(&sha, io, source_length) != 0 ||
        std::memcmp(io, bytes + checked, source_length) != 0) {
      valid = false;
      break;
    }
    checked += source_length;
  }
  uint8_t digest[32];
  if (valid) valid = mbedtls_sha256_finish(&sha, digest) == 0;
  mbedtls_sha256_free(&sha);
  if (valid) {
    static constexpr char kHex[] = "0123456789abcdef";
    char readback_hash[65];
    for (size_t i = 0; i < 32; ++i) {
      readback_hash[2 * i] = kHex[digest[i] >> 4];
      readback_hash[2 * i + 1] = kHex[digest[i] & 15];
    }
    readback_hash[64] = 0;
    valid = hash == readback_hash;
  }
  std::memset(digest, 0, sizeof(digest));
  heap_caps_free(io);
  if (!valid) {
    error = "candidate readback hash mismatch";
    return false;
  }
  record.version = kRecordVersion;
  record.candidateSlot = slot;
  record.flags |= 2U;
  record.candidateLength = static_cast<uint32_t>(length);
  std::memset(record.candidateHash, 0, sizeof(record.candidateHash));
  std::memcpy(record.candidateHash, hash.data(), hash.size());
  return save_record(record, error);
}

bool content_activate(uint32_t target_generation, std::string& error) {
  ContentRecord record{};
  if (!read_record(record, error)) return false;
  if (!(record.flags & 2U)) {
    error = "no validated candidate";
    return false;
  }
  if (target_generation == record.generation && (record.flags & 1U) &&
      std::memcmp(record.activeHash, record.candidateHash,
                  sizeof(record.activeHash)) == 0) {
    record.flags &= static_cast<uint8_t>(~2U);
    record.candidateLength = 0;
    record.candidateHash[0] = 0;
    return save_record(record, error);
  }
  if (record.generation == UINT32_MAX || target_generation <= record.generation) {
    error = "content generation must increase";
    return false;
  }
  record.activeSlot = record.candidateSlot;
  record.activeLength = record.candidateLength;
  std::memcpy(record.activeHash, record.candidateHash, sizeof(record.activeHash));
  record.flags = static_cast<uint8_t>((record.flags | 1U) & ~2U);
  record.candidateLength = 0;
  record.candidateHash[0] = 0;
  record.generation = target_generation;
  return save_record(record, error);
}

bool content_advance_active_generation(const std::string& active_hash,
                                      uint32_t target_generation,
                                      std::string& error) {
  ContentRecord record{};
  if (!read_record(record, error)) return false;
  if (!(record.flags & 1U) || active_hash.size() != 64 ||
      std::memcmp(record.activeHash, active_hash.data(), active_hash.size()) != 0) {
    error = "active package does not match generation request";
    return false;
  }
  if (target_generation < record.generation) {
    error = "content generation cannot move backwards";
    return false;
  }
  if (target_generation == record.generation) {
    error.clear();
    return true;
  }
  record.generation = target_generation;
  return save_record(record, error);
}
}  // namespace ledpanel
