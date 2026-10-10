#pragma once
#include "node_state.hpp"
#include <string>
namespace ledpanel {
bool config_load(NodeConfig& out, std::string& error);
bool config_save(const std::string& input, NodeConfig& out, std::string& error);
bool config_store_panel_binding(const std::string& slot_id, uint8_t rotation,
                                std::string& error);
bool config_reset(std::string& error);
bool config_verify_password(const NodeConfig& config, const std::string& password);
}
