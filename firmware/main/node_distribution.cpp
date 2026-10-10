#include "node_distribution.hpp"

#include "content_store.hpp"
#include "lpk_decoder.hpp"
#include "runtime_package.hpp"
#include "sync_runtime.hpp"
#include "web_api.hpp"

#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace ledpanel {
namespace {
constexpr const char* kTag = "node-distribution";
constexpr uint32_t kPollIntervalMs = 5000;

void pull_task(void* context) {
  auto& config = *static_cast<NodeConfig*>(context);
  for (;;) {
    std::vector<SyncPeerSnapshot> peers;
    sync_runtime_peer_snapshot(peers);
    bool master_online = false;
    for (const auto& peer : peers) {
      if (peer.node_id == config.masterNodeId && peer.online && peer.authenticated) {
        master_online = true;
        break;
      }
    }
    if (!master_online) {
      vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
      continue;
    }

    ContentState local_content;
    std::string error;
    const bool local_content_ok = content_get_state(local_content, error);
    const std::string current_candidate = local_content_ok && local_content.has_candidate
        ? local_content.candidate_hash : std::string();
    const std::string current_active = local_content_ok && local_content.has_active
        ? local_content.active_hash : std::string();
    const std::string current_package = current_candidate.empty()
        ? current_active : current_candidate;
    uint8_t* response = static_cast<uint8_t*>(heap_caps_malloc(
        kMaxPackageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!response) {
      ESP_LOGW(kTag, "PSRAM buffer unavailable for candidate poll");
      vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
      continue;
    }

    size_t response_length = 0;
    uint16_t response_status = 0;
    std::string announced_hash;
    const bool exchanged = sync_runtime_node_http_request(
        config.masterNodeId, "GET", "/node/v2/candidate", nullptr, 0,
        current_package, response, kMaxPackageBytes, response_length,
        response_status, announced_hash, error);
    if (!exchanged) {
      ESP_LOGD(kTag, "candidate poll deferred: %s", error.c_str());
    } else if (response_status == 304) {
      if (current_package.empty() || announced_hash != current_package)
        ESP_LOGW(kTag, "ignored inconsistent not-modified candidate response");
    } else if (response_status == 200) {
      std::string actual_hash, validation_error;
      if (!response_length || response_length > kMaxPackageBytes ||
          announced_hash.size() != 64 ||
          !validate_runtime_package(response, response_length, actual_hash,
                                    validation_error) ||
          actual_hash != announced_hash) {
        ESP_LOGW(kTag, "candidate package rejected: %s",
                 validation_error.empty() ? "body hash mismatch" : validation_error.c_str());
      } else if (current_candidate == actual_hash || current_active == actual_hash) {
        ESP_LOGD(kTag, "candidate %s is already staged", actual_hash.c_str());
      } else {
        std::string store_error;
        if (content_stage(response, response_length, actual_hash, store_error))
          ESP_LOGI(kTag, "candidate staged from Master: %s (%u bytes)",
                   actual_hash.c_str(), static_cast<unsigned>(response_length));
        else
          ESP_LOGE(kTag, "candidate staging failed: %s", store_error.c_str());
      }
    } else if (response_status != 404 && response_status != 409) {
      ESP_LOGW(kTag, "Master candidate endpoint returned HTTP %u",
               static_cast<unsigned>(response_status));
    }

    if (response_length) std::memset(response, 0, response_length);
    heap_caps_free(response);
    vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
  }
}
}  // namespace

void node_distribution_start(NodeConfig* config) {
  if (!config || !config->provisioned || config->role != Role::Follower) return;
  const BaseType_t created = xTaskCreatePinnedToCore(
      pull_task, "node_candidate_pull", 9216, config, 4, nullptr, 0);
  if (created != pdPASS) ESP_LOGE(kTag, "candidate pull task creation failed");
}

}  // namespace ledpanel
