#include "usb_setup.hpp"
#include "config_store.hpp"
#include "hub75_hal.hpp"
#include "rig_store.hpp"
#include "web_api.hpp"
#include "render_runtime.hpp"
#include "sync_runtime.hpp"
#include "network_diagnostics.hpp"
#include <esp_wifi.h>
#include <esp_netif.h>
#include <cJSON.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include "esp_rom_sys.h"

namespace ledpanel {
namespace {
constexpr gpio_num_t kBootPin = GPIO_NUM_0;
constexpr size_t kMaxLineBytes = 4096;
constexpr int64_t kButtonHoldUs = 3000000;
constexpr int64_t kSessionTimeoutUs = 60000000;
constexpr const char* kTag = "usb-setup";

void reply(int id, bool ok, const char* code = nullptr, bool reboot = false) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return;
  cJSON_AddNumberToObject(root, "id", id);
  cJSON_AddBoolToObject(root, "ok", ok);
  if (code) cJSON_AddStringToObject(root, ok ? "result" : "error", code);
  if (reboot) cJSON_AddBoolToObject(root, "rebootRequired", true);
  char* json = cJSON_PrintUnformatted(root);
  if (json) {
    std::printf("%s\n", json);
    std::fflush(stdout);
    cJSON_free(json);
  }
  cJSON_Delete(root);
}

void reply_panel_test(int id) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return;
  cJSON_AddNumberToObject(root, "id", id);
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "status", "diagnostic_started");
  cJSON_AddStringToObject(root, "pattern", "RGB_BARS");
  cJSON_AddNumberToObject(root, "durationMs", 30000);
  cJSON_AddNumberToObject(root, "panelCoveragePercent", 100);
  cJSON_AddNumberToObject(root, "brightness", 16);
  char* json = cJSON_PrintUnformatted(root);
  if (json) {
    std::printf("%s\n", json);
    std::fflush(stdout);
    cJSON_free(json);
  }
  cJSON_Delete(root);
}

void emit_session(bool opened) {
  std::printf("{\"event\":\"usbConfig%s\"}\n", opened ? "Opened" : "Closed");
  std::fflush(stdout);
}

int request_id(const cJSON* root) {
  const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "id");
  return cJSON_IsNumber(id) && id->valueint >= 0 ? id->valueint : 0;
}

void process_line(char* line, size_t length, bool session, NodeConfig& config,
                  bool& reboot_required) {
  cJSON* root = cJSON_ParseWithLength(line, length);
  if (!root || !cJSON_IsObject(root)) {
    reply(0, false, "INVALID_JSON");
    cJSON_Delete(root);
    return;
  }
  const int id = request_id(root);
  const cJSON* op_item = cJSON_GetObjectItemCaseSensitive(root, "op");
  const char* op = cJSON_IsString(op_item) ? op_item->valuestring : "";
  if (std::strcmp(op, "hello") == 0 || std::strcmp(op, "inspect") == 0) {
    cJSON* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "id", id);
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddBoolToObject(result, "provisioned", config.provisioned);
    cJSON_AddBoolToObject(result, "configurationOpen", session);
    cJSON_AddStringToObject(result, "boardProfile", "seengreat_hub75_s3_n16r8_v1");
      cJSON_AddStringToObject(result, "panelOutput",
        hub75_nightlight_active() ? "dim_nightlight" :
          hub75_diagnostic_active() ? "diagnostic_rgb_bars" : "disabled_black");
      RenderTelemetry rendered;
      if (render_runtime_telemetry(rendered)) {
        cJSON_AddStringToObject(result, "lastRenderedMasterUs",
            std::to_string(rendered.master_time_us).c_str());
        cJSON_AddStringToObject(result, "lastRenderedTick",
            std::to_string(rendered.playhead_tick).c_str());
        cJSON_AddNumberToObject(result, "lastRenderedRevision", rendered.revision);
      }
    uint32_t reconnects=0;uint64_t network_loop=0;
    network_recovery_diagnostics(reconnects,network_loop);
    cJSON_AddNumberToObject(result,"networkRecoveryCount",reconnects);
    cJSON_AddStringToObject(result,"networkTaskLastUs",std::to_string(network_loop).c_str());
    wifi_ap_record_t ap{};
    cJSON_AddBoolToObject(result,"wifiAssociated",esp_wifi_sta_get_ap_info(&ap)==ESP_OK);
    auto* netif=esp_netif_get_handle_from_ifkey(config.role==Role::Master?"WIFI_AP_DEF":"WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    if (netif && esp_netif_get_ip_info(netif,&ip)==ESP_OK) {
      char address[16];std::snprintf(address,sizeof(address),IPSTR,IP2STR(&ip.ip));
      cJSON_AddStringToObject(result,"ipv4",address);
    }
    std::vector<SyncPeerSnapshot> peers;sync_runtime_peer_snapshot(peers);
    cJSON* links=cJSON_AddArrayToObject(result,"links");
    for (const auto& peer:peers) {
      cJSON* link=cJSON_CreateObject();cJSON_AddNumberToObject(link,"nodeId",peer.node_id);
      cJSON_AddBoolToObject(link,"online",peer.online);cJSON_AddBoolToObject(link,"authenticated",peer.authenticated);
      cJSON_AddBoolToObject(link,"clockSynced",peer.clock_synced);cJSON_AddItemToArray(links,link);
    }
    cJSON_AddNumberToObject(result, "outputEnableGpio", 13);
    cJSON_AddNumberToObject(result, "outputEnableLevel", hub75_output_enable_level());
    cJSON_AddStringToObject(result, "hardwareId", config.hardwareId.c_str());
    if (config.provisioned) {
      cJSON_AddNumberToObject(result, "nodeId", config.nodeId);
      cJSON_AddNumberToObject(result, "masterNodeId", config.masterNodeId);
      cJSON_AddStringToObject(result, "role",
                              config.role == Role::Master ? "master" : "follower");
      cJSON_AddStringToObject(result, "groupId", config.groupId.c_str());
      cJSON_AddStringToObject(result, "networkMode", config.networkMode.c_str());
      cJSON_AddNumberToObject(result, "channel", config.channel);
    }
    char* json = cJSON_PrintUnformatted(result);
    if (json) {
      std::printf("%s\n", json);
      std::fflush(stdout);
      cJSON_free(json);
    }
    cJSON_Delete(result);
  } else if (std::strcmp(op, "configure") == 0) {
    if (!session) {
      reply(id, false, "PHYSICAL_SETUP_REQUIRED");
    } else {
      const cJSON* cfg = cJSON_GetObjectItemCaseSensitive(root, "config");
      char* serialized = cJSON_IsObject(cfg) ? cJSON_PrintUnformatted(cfg) : nullptr;
      if (!serialized) {
        reply(id, false, "CONFIG_REQUIRED");
      } else {
        NodeConfig next;
        std::string error;
        const bool ok = config_save(serialized, next, error);
        std::memset(serialized, 0, std::strlen(serialized));
        cJSON_free(serialized);
        if (ok) {
          config = std::move(next);
          reboot_required = true;
          reply(id, true, "configured", true);
        } else {
          reply(id, false, error.c_str());
        }
      }
    }
  } else if (std::strcmp(op, "factory_reset") == 0) {
    if (!session) {
      reply(id, false, "PHYSICAL_SETUP_REQUIRED");
    } else {
      std::string error;
      if (config_reset(error)) {
        config = NodeConfig{};
        reboot_required = true;
        reply(id, true, "factory_reset", true);
      } else {
        reply(id, false, error.c_str());
      }
    }
  } else if (std::strcmp(op, "register-node") == 0) {
    if (!session) {
      reply(id, false, "PHYSICAL_SETUP_REQUIRED");
    } else if (!config.provisioned || config.role != Role::Master) {
      reply(id, false, "MASTER_REQUIRED");
    } else {
      const cJSON* node_id_item = cJSON_GetObjectItemCaseSensitive(root, "nodeId");
      const cJSON* hardware_item = cJSON_GetObjectItemCaseSensitive(root, "hardwareId");
      const cJSON* label_item = cJSON_GetObjectItemCaseSensitive(root, "label");
      const cJSON* group_item = cJSON_GetObjectItemCaseSensitive(root, "groupId");
      if (!cJSON_IsNumber(node_id_item) || node_id_item->valueint < 1 ||
          node_id_item->valueint > UINT16_MAX ||
          node_id_item->valuedouble != static_cast<double>(node_id_item->valueint) ||
          !cJSON_IsString(hardware_item) || !cJSON_IsString(label_item) ||
          !cJSON_IsString(group_item) || config.groupId != group_item->valuestring) {
        reply(id, false, "INVALID_REGISTRATION");
      } else {
        std::string error;
        if (rig_register_follower(config, static_cast<uint16_t>(node_id_item->valueint),
                                  hardware_item->valuestring, label_item->valuestring,
                                  error)) {
          reply(id, true, "follower_registered");
        } else {
          reply(id, false, error.c_str());
        }
      }
    }
  } else if (std::strcmp(op, "nightlight_on") == 0 ||
             std::strcmp(op, "nightlight_off") == 0) {
    const bool enable = std::strcmp(op, "nightlight_on") == 0;
    std::string error;
    if (web_api_usb_nightlight(enable, error))
      reply(id, true, enable ? "nightlight_on_8_of_255" : "nightlight_off");
    else
      reply(id, false, error.c_str());
  } else if (std::strcmp(op, "panel_test") == 0) {
    std::string error;
    if (web_api_usb_panel_test(error)) reply_panel_test(id);
    else reply(id, false, error.c_str());
  } else {
    reply(id, false, "UNSUPPORTED_OPERATION");
  }
  cJSON_Delete(root);
}
}  // namespace

void usb_setup_task(void* context) {
  auto& config = *static_cast<NodeConfig*>(context);
  gpio_config_t button = {};
  button.pin_bit_mask = 1ULL << kBootPin;
  button.mode = GPIO_MODE_INPUT;
  button.pull_up_en = GPIO_PULLUP_ENABLE;
  button.pull_down_en = GPIO_PULLDOWN_DISABLE;
  button.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&button) != ESP_OK) {
    ESP_LOGE(kTag, "BOOT input could not be initialized; USB configuration is disabled");
    vTaskDelete(nullptr);
    return;
  }
  const int old_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (old_flags >= 0) fcntl(STDIN_FILENO, F_SETFL, old_flags | O_NONBLOCK);
  ESP_LOGI(kTag, "press and hold BOOT for 3 seconds to open the 60-second USB setup window");

  std::string line;
  bool session = false;
  bool overflow = false;
  bool reboot_required = false;
  int64_t pressed_since = 0;
  int64_t session_until = 0;
  while (!reboot_required) {
    const int64_t now = esp_timer_get_time();
    if (gpio_get_level(kBootPin) == 0) {
      if (!pressed_since) pressed_since = now;
      if (!session && now - pressed_since >= kButtonHoldUs) {
        session = true;
        session_until = now + kSessionTimeoutUs;
        emit_session(true);
        ESP_LOGI(kTag, "physical USB setup window opened");
      }
    } else {
      pressed_since = 0;
    }
    if (session && now >= session_until) {
      session = false;
      emit_session(false);
      ESP_LOGI(kTag, "USB setup window closed");
    }

    char chunk[128];
    const ssize_t count = read(STDIN_FILENO, chunk, sizeof(chunk));
    if (count <= 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    for (ssize_t i = 0; i < count; ++i) {
      const char ch = chunk[i];
      if (ch == '\n' || ch == '\r') {
        if (!line.empty()) {
          if (overflow) reply(0, false, "LINE_TOO_LONG");
          else process_line(line.data(), line.size(), session, config, reboot_required);
          std::memset(line.data(), 0, line.size());
          line.clear();
          overflow = false;
        }
      } else if (line.size() < kMaxLineBytes) {
        line.push_back(ch);
      } else {
        overflow = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (old_flags >= 0) fcntl(STDIN_FILENO, F_SETFL, old_flags);
  if (session) emit_session(false);
  esp_rom_delay_us(50000);
  esp_restart();
}
}  // namespace ledpanel
