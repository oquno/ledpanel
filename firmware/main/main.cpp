#include "config_store.hpp"
#include "content_store.hpp"
#include "hub75_hal.hpp"
#include "node_state.hpp"
#include "lpk_decoder.hpp"
#include "node_http_auth.hpp"
#include "node_distribution.hpp"
#include "rig_store.hpp"
#include "runtime_package.hpp"
#include "render_runtime.hpp"
#include "topology.hpp"
#include "sync_protocol.hpp"
#include "sync_runtime.hpp"
#include "frame_clock.hpp"
#include "network_diagnostics.hpp"
#include "usb_setup.hpp"
#include "web_api.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cJSON.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_event.h>
#include <esp_efuse.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

using namespace ledpanel;
namespace {
const char* kTag = "ledpanel";
NodeConfig config;
PlaybackState state;
uint64_t boot_id = 0;
bool sync_protocol_ok = false;
bool sync_runtime_ok = false;
std::string sync_runtime_error;
bool rig_store_ok = false;
bool node_http_auth_ok = false;
bool runtime_package_ok = false;
bool topology_ok = false;
std::atomic<bool> station_connected{false};
std::atomic<uint32_t> network_recoveries{0};
std::atomic<uint64_t> network_loop_us{0};

void* cjson_psram_alloc(size_t size) {
  void* pointer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return pointer ? pointer : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void cjson_psram_free(void* pointer) {
  heap_caps_free(pointer);
}

void local_panel_binding(const PlaybackState& playback, std::string& slot_id,
                         uint8_t& rotation) {
  slot_id = config.slotId;
  rotation = config.rotation;
  if (config.role != Role::Master) {
    if (slot_id.empty()) slot_id = "__unassigned__";
    return;
  }
  RigSlot rig;
  std::string error;
  if (!rig_load(rig, error) || !rig.valid) return;
  cJSON* root = cJSON_ParseWithLength(rig.json.data(), rig.json.size());
  const cJSON* layouts = root
      ? cJSON_GetObjectItemCaseSensitive(root, "layouts") : nullptr;
  const cJSON* selected = nullptr;
  for (const cJSON* item = cJSON_IsArray(layouts) ? layouts->child : nullptr;
       item; item = item->next) {
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "layoutId");
    if (cJSON_IsString(id) && playback.layoutId == id->valuestring) {
      selected = item;
      break;
    }
  }
  const cJSON* bindings = selected
      ? cJSON_GetObjectItemCaseSensitive(selected, "bindings") : nullptr;
  for (const cJSON* item = cJSON_IsArray(bindings) ? bindings->child : nullptr;
       item; item = item->next) {
    const cJSON* node = cJSON_GetObjectItemCaseSensitive(item, "nodeId");
    const cJSON* slot = cJSON_GetObjectItemCaseSensitive(item, "slotId");
    if (cJSON_IsNumber(node) && node->valueint == config.nodeId &&
        cJSON_IsString(slot)) {
      slot_id = slot->valuestring;
      break;
    }
  }
  const cJSON* nodes = root ? cJSON_GetObjectItemCaseSensitive(root, "nodes") : nullptr;
  for (const cJSON* item = cJSON_IsArray(nodes) ? nodes->child : nullptr;
       item; item = item->next) {
    const cJSON* node = cJSON_GetObjectItemCaseSensitive(item, "nodeId");
    const cJSON* angle = cJSON_GetObjectItemCaseSensitive(item, "rotation");
    if (cJSON_IsNumber(node) && node->valueint == config.nodeId &&
        cJSON_IsNumber(angle) && (angle->valueint == 0 || angle->valueint == 180)) {
      rotation = static_cast<uint8_t>(angle->valueint);
      break;
    }
  }
  cJSON_Delete(root);
  // A single-slot layout is unambiguous on a fresh one-node installation.
  if (slot_id.empty()) slot_id = "__auto_single_slot__";
}

void render_task(void*) {
  esp_timer_handle_t frame_timer = nullptr;
  esp_timer_create_args_t timer_args{};
  timer_args.callback = [](void* task) {
    xTaskNotifyGive(static_cast<TaskHandle_t>(task));
  };
  timer_args.arg = xTaskGetCurrentTaskHandle();
  timer_args.name = "frame_boundary";
  const bool precise_timer = esp_timer_create(&timer_args, &frame_timer) == ESP_OK;
  auto* frame = static_cast<uint8_t*>(heap_caps_malloc(
      64 * 32 * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  std::string loaded_hash;
  std::string last_fault;
  for (;;) {
    if (hub75_diagnostic_active()) {
      vTaskDelay(pdMS_TO_TICKS(33));
      continue;
    }
    PlaybackState playback;
    uint64_t master_now_us = 0;
    if (!sync_runtime_render_snapshot(playback, master_now_us)) {
      hub75_black();
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (playback.packageHash != loaded_hash) {
      if (!playback.packageHash.empty()) {
        ContentState active;
        std::string error;
        if (content_get_state(active, error) && active.has_active &&
            active.active_hash == playback.packageHash) {
          auto* package = static_cast<uint8_t*>(heap_caps_malloc(
              kMaxPackageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
          size_t length = 0;
          std::string actual_hash;
          if (package && content_read_active(package, kMaxPackageBytes, length,
                                             actual_hash, error) &&
              actual_hash == playback.packageHash &&
              render_runtime_load_owned(package, length, playback.packageHash,
                                        error)) {
            loaded_hash = playback.packageHash;
            package = nullptr;  // The runtime owns the active package buffer.
            last_fault.clear();
            ESP_LOGI(kTag, "active LPK loaded into 30 fps renderer (%u bytes)",
                     static_cast<unsigned>(length));
          }
          if (package) {
            if (length) std::memset(package, 0, length);
            heap_caps_free(package);
          }
          if (loaded_hash != playback.packageHash) {
            ESP_LOGE(kTag, "active LPK renderer load failed: %s", error.c_str());
            render_runtime_unload();
            loaded_hash.clear();
          }
        }
      } else {
        render_runtime_unload();
        loaded_hash.clear();
      }
    }
    std::string reason;
    if (!frame || !hub75_diagnostic_supported() ||
        !state_can_render(config, playback, reason) ||
        !render_runtime_is_loaded(playback.packageHash)) {
      hub75_black();
      if (reason.empty()) reason = !frame ? "FRAMEBUFFER_UNAVAILABLE" :
          !hub75_diagnostic_supported() ? "PANEL_DMA_UNAVAILABLE" : "PACKAGE_NOT_READY";
    } else {
      if (!sync_runtime_render_snapshot(playback, master_now_us)) {
        hub75_black();
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      std::string slot_id;
      uint8_t rotation = 0;
      local_panel_binding(playback, slot_id, rotation);
      if (slot_id == "__auto_single_slot__") slot_id.clear();
      if (slot_id == "__unassigned__") reason = "NODE_HAS_NO_LAYOUT_SLOT";
      if (!reason.empty() || !render_runtime_frame(playback, slot_id, rotation,
              master_now_us, frame, 64 * 32 * 3, reason)) {
        hub75_black();
      } else {
        hub75_present(frame, 64, 32,
            std::min<uint8_t>(playback.userBrightness, config.brightnessCap));
      }
    }
    if (reason != last_fault) {
      if (!reason.empty()) ESP_LOGW(kTag, "panel output held black: %s", reason.c_str());
      last_fault = reason;
    }
    // Wake on the next shared playback-frame boundary, rather than sleeping
    // 33 ms after each node finishes drawing (which gives independent phases).
    if (precise_timer && sync_runtime_render_snapshot(playback, master_now_us)) {
      const uint64_t epoch = playback.running ? playback.anchorUs : 0;
      if (esp_timer_start_once(frame_timer,
              frame_wake_delay(epoch, master_now_us)) == ESP_OK) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        continue;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void start_http() {
  httpd_config_t http = HTTPD_DEFAULT_CONFIG();
  http.server_port = 80;
  // Fleet activation performs authenticated HTTP client calls from the handler.
  http.stack_size = 12288;
  http.max_uri_handlers = 40;
  http.max_open_sockets = 4;
  http.lru_purge_enable = true;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &http) == ESP_OK) {
  web_api_start(server, &config, &state, &boot_id, sync_protocol_ok,
                sync_runtime_ok, rig_store_ok, node_http_auth_ok,
                runtime_package_ok, topology_ok, sync_runtime_error.c_str());
  } else {
    ESP_LOGE(kTag, "HTTP server failed to start");
  }
}

bool start_master_ap(const NodeConfig& cfg) {
  if (!cfg.provisioned || cfg.role != Role::Master || cfg.networkMode != "softap")
    return false;
  esp_netif_create_default_wifi_ap();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  wifi_country_t country = {};
  std::memcpy(country.cc, "JP", 2);
  country.schan = 1;
  country.nchan = 13;
  country.policy = WIFI_COUNTRY_POLICY_MANUAL;
  ESP_ERROR_CHECK(esp_wifi_set_country(&country));
  wifi_config_t ap = {};
  const std::string ssid = "LEDPANEL-" + cfg.groupId.substr(cfg.groupId.size() - 6);
  std::memcpy(ap.ap.ssid, ssid.data(), ssid.size());
  ap.ap.ssid_len = ssid.size();
  std::memcpy(ap.ap.password, cfg.apPsk.data(), cfg.apPsk.size());
  ap.ap.channel = cfg.channel;
  ap.ap.max_connection = 6;
  ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap.ap.pmf_cfg.capable = true;
  ap.ap.pmf_cfg.required = false;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  ESP_LOGI(kTag, "master AP started; SSID=%s address=192.168.4.1", ssid.c_str());
  return true;
}

void station_event(void*, esp_event_base_t, int32_t event_id, void*) {
  if (event_id == WIFI_EVENT_STA_DISCONNECTED) station_connected.store(false);
}

void station_ip_event(void*, esp_event_base_t, int32_t, void* event_data) {
  station_connected.store(true);
  const auto* event = static_cast<const ip_event_got_ip_t*>(event_data);
  if (!event) return;
  char address[16] = {};
  std::snprintf(address, sizeof(address), IPSTR, IP2STR(&event->ip_info.ip));
  ESP_LOGI(kTag, "%s joined Wi-Fi; address=%s",
           config.role == Role::Master ? "master" : "follower", address);
}

void follower_reconnect_task(void*) {
  uint32_t delay_ms = 1000;
  int64_t master_missing_since=0;
  for (;;) {
    network_loop_us.store(esp_timer_get_time());
    // Static-IP stations do not necessarily emit a new DHCP GOT_IP event.
    // Use the driver's actual association rather than a cached IP-event flag.
    wifi_ap_record_t associated_ap{};
    station_connected.store(esp_wifi_sta_get_ap_info(&associated_ap)==ESP_OK);
    if (station_connected.load()) {
      if (config.role==Role::Follower && config.networkMode=="softap") {
        std::vector<SyncPeerSnapshot> peers;sync_runtime_peer_snapshot(peers);
        const bool master_live=std::any_of(peers.begin(),peers.end(),[](const SyncPeerSnapshot& peer){return peer.online && peer.authenticated;});
        if (master_live) {master_missing_since=0;}
        else {
          const int64_t now=esp_timer_get_time();
          if (!master_missing_since) master_missing_since=now;
          if (now-master_missing_since>=10000000) {
            // An AP reboot can leave STA "connected" with an obsolete WPA
            // association. Reassociate when the authenticated parent is lost.
            station_connected.store(false);master_missing_since=0;
            network_recoveries.fetch_add(1);
            esp_wifi_stop();esp_wifi_start();delay_ms=1000;
            continue;
          }
        }
      }
      delay_ms = 1000;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    const uint32_t jitter_span = std::max<uint32_t>(1, delay_ms * 2 / 5);
    const int32_t jitter = static_cast<int32_t>(esp_random() % jitter_span) -
                           static_cast<int32_t>(jitter_span / 2);
    vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(static_cast<int32_t>(delay_ms) + jitter)));
    if (!station_connected.load()) esp_wifi_connect();
    delay_ms = std::min<uint32_t>(delay_ms * 2, 30000);
  }
}

bool start_station(const NodeConfig& cfg, const std::string& ssid,
                   const std::string& password) {
  if (ssid.empty() || ssid.size() > 32 || password.size() < 8 ||
      password.size() > 63) return false;
  esp_netif_t* station_netif = esp_netif_create_default_wifi_sta();
  if (!station_netif) {
    ESP_LOGE(kTag, "station network interface creation failed");
    return false;
  }
  // Reserve stable addresses outside the default SoftAP DHCP client pool.
  // Small node IDs are the normal provisioner's fleet numbering (2..62).
  // This prevents a PC taking a follower's cached DHCP lease after AP reboot.
  if (cfg.role==Role::Follower && cfg.networkMode=="softap" && cfg.nodeId>=2 && cfg.nodeId<=62) {
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(station_netif));
    esp_netif_ip_info_t address{};
    address.ip.addr=ESP_IP4TOADDR(192,168,4,192+cfg.nodeId);
    address.gw.addr=ESP_IP4TOADDR(192,168,4,1);
    address.netmask.addr=ESP_IP4TOADDR(255,255,255,0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(station_netif,&address));
  }
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  wifi_country_t country = {};
  std::memcpy(country.cc, "JP", 2);
  country.schan = 1;
  country.nchan = 13;
  country.policy = WIFI_COUNTRY_POLICY_MANUAL;
  ESP_ERROR_CHECK(esp_wifi_set_country(&country));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                              station_event, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              station_ip_event, nullptr));
  wifi_config_t sta = {};
  std::memcpy(sta.sta.ssid, ssid.data(), ssid.size());
  std::memcpy(sta.sta.password, password.data(), password.size());
  sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  sta.sta.pmf_cfg.capable = true;
  sta.sta.pmf_cfg.required = false;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  if (xTaskCreate(follower_reconnect_task, "follower_wifi", 3072, nullptr, 4,
                  nullptr) != pdPASS) {
    ESP_LOGE(kTag, "station reconnect task could not start");
    return false;
  }
  ESP_LOGI(kTag, "%s joining Wi-Fi SSID=%s",
           cfg.role == Role::Master ? "master" : "follower", ssid.c_str());
  return true;
}

bool start_follower_station(const NodeConfig& cfg) {
  if (!cfg.provisioned || cfg.role != Role::Follower || cfg.networkMode != "softap")
    return false;
  const std::string ssid = "LEDPANEL-" + cfg.groupId.substr(cfg.groupId.size() - 6);
  return start_station(cfg, ssid, cfg.apPsk);
}

bool start_infrastructure_station(const NodeConfig& cfg) {
  if (!cfg.provisioned || cfg.networkMode != "infrastructure") return false;
  return start_station(cfg, cfg.stationSsid, cfg.stationPassword);
}
}  // namespace

namespace ledpanel {
void network_recovery_diagnostics(uint32_t& count,uint64_t& last_loop_us) {
  count=network_recoveries.load();last_loop_us=network_loop_us.load();
}
}

extern "C" void app_main() {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  usb_serial_jtag_driver_config_t usb_console = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  // A provisioning request includes the WPA passphrase, admin password, and
  // group key; keep the whole JSON line in the driver RX ring while the task
  // drains it in chunks.
  usb_console.rx_buffer_size = 1024;
  ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_console));
  usb_serial_jtag_vfs_use_driver();
#endif
  cJSON_Hooks json_hooks = {cjson_psram_alloc, cjson_psram_free};
  cJSON_InitHooks(&json_hooks);
  if (!render_runtime_initialize())
    ESP_LOGE(kTag, "runtime renderer lock could not be initialized");
  esp_fill_random(&boot_id, sizeof(boot_id));
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs);
  std::string protocol_error;
  sync_protocol_ok = sync_protocol_self_test(protocol_error);
  if (!sync_protocol_ok) {
    ESP_LOGE(kTag, "LPS2 protocol self-test failed: %s", protocol_error.c_str());
  } else {
    ESP_LOGI(kTag, "LPS2 authenticated packet codec self-test passed");
  }
  sync_runtime_ok = sync_runtime_self_test(sync_runtime_error);
  if (!sync_runtime_ok)
    ESP_LOGE(kTag, "LPS2 runtime transaction self-test failed: %s", sync_runtime_error.c_str());
  else
    ESP_LOGI(kTag, "LPS2 blackout and transaction state-machine self-tests passed");
  std::string rig_error;
  rig_store_ok = rig_store_self_test(rig_error);
  if (!rig_store_ok)
    ESP_LOGE(kTag, "rig validation self-test failed: %s", rig_error.c_str());
  else
    ESP_LOGI(kTag, "rig validation self-test passed");
  std::string package_error;
  runtime_package_ok = runtime_package_self_test(package_error);
  if (!runtime_package_ok)
    ESP_LOGE(kTag, "runtime package layout self-test failed: %s", package_error.c_str());
  else
    ESP_LOGI(kTag, "runtime package layout self-test passed");
  std::string topology_error;
  topology_ok = topology_self_test(topology_error);
  if (!topology_ok)
    ESP_LOGE(kTag, "topology build/verify self-test failed: %s", topology_error.c_str());
  else
    ESP_LOGI(kTag, "topology build/verify self-test passed");
  std::string node_http_error;
  node_http_auth_ok = node_http_auth_self_test(node_http_error);
  if (!node_http_auth_ok)
    ESP_LOGE(kTag, "node HTTP auth self-test failed: %s", node_http_error.c_str());
  else
    ESP_LOGI(kTag, "node HTTP auth self-test passed");
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  Hub75Config panel;
  panel.validated = false;
  hub75_init(panel);
  hub75_black();
  std::string error;
  if (!config_load(config, error)) {
    config = NodeConfig{};
    ESP_LOGI(kTag, "UNPROVISIONED: %s", error.c_str());
  }
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) == ESP_OK) {
    char hardware_id[37];
    std::snprintf(hardware_id, sizeof(hardware_id), "00000000-0000-4000-8000-%02x%02x%02x%02x%02x%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    config.hardwareId = hardware_id;
  }

  if (esp_psram_get_size() != 8 * 1024 * 1024) {
    ESP_LOGE(kTag, "board profile requires 8MB PSRAM; refusing normal operation");
    config.provisioned = false;
  }
  state.layoutId = "panel_64x32";
  if (config.topologyHash.size() == 64)
    state.topologyHash = config.topologyHash;
  ContentState content_state;
  std::string content_error;
  if (content_get_state(content_state, content_error) && content_state.has_active)
    state.packageHash = content_state.active_hash;
  if (config.provisioned) {
    if (config.role == Role::Master) {
      std::string registry_error;
      if (!rig_ensure_master_registry(config, registry_error))
        ESP_LOGE(kTag, "node registry unavailable: %s", registry_error.c_str());
    }
    if (config.networkMode == "infrastructure")
      start_infrastructure_station(config);
    else if (config.role == Role::Master)
      start_master_ap(config);
    else
      start_follower_station(config);
    sync_runtime_start(&config, &state, &boot_id);
    node_distribution_start(&config);
  }
  start_http();
  xTaskCreatePinnedToCore(render_task, "render", 8192, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(usb_setup_task, "usb_setup", 6144, &config, 4, nullptr, 0);
  ESP_LOGI(kTag, "app_main stack high-water mark=%u bytes",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  ESP_LOGI(kTag, "ready; awaiting a valid package and an explicit playback command");
}
