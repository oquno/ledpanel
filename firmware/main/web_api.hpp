#pragma once
#include "node_state.hpp"
#include "esp_http_server.h"
namespace ledpanel {
void web_api_start(httpd_handle_t, NodeConfig*, PlaybackState*, uint64_t*,
                   bool, bool, bool, bool, bool, bool, const char*);
bool web_api_maintenance_active();
bool web_api_usb_panel_test(std::string& error);
bool web_api_usb_nightlight(bool enable, std::string& error);
void web_api_apply_remote_blackout(uint32_t revision);
void web_api_apply_remote_state(const PlaybackState& state);
}
