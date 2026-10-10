#pragma once
#include "node_state.hpp"
#include <cstdint>
#include <string>
#include <vector>
namespace ledpanel {
struct RigSlot { uint32_t generation = 0; bool valid = false; std::string json; };
struct RigFollowerIdentity { uint16_t node_id = 0; std::string hardware_id; };
bool rig_load(RigSlot&, std::string&);
bool rig_commit(const NodeConfig&, const std::string&, std::string&);
bool rig_store_self_test(std::string&);
bool rig_ensure_master_registry(const NodeConfig&, std::string&);
bool rig_register_follower(const NodeConfig&, uint16_t node_id,
                           const std::string& hardware_id, const std::string& label,
                           std::string&);
bool rig_lookup_follower(const NodeConfig&, uint16_t node_id,
                         const std::string& hardware_id, bool& enabled,
                         std::string& label, std::string& error);
bool rig_enabled_followers(const NodeConfig&,
                           std::vector<RigFollowerIdentity>& followers,
                           std::string& error);
bool rig_display_followers(const NodeConfig&, const std::string& layout_id,
                           std::vector<RigFollowerIdentity>& followers,
                           std::string& error);
}
