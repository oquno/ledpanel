#pragma once

#include "node_state.hpp"
#include <cstdint>
#include <string>

namespace ledpanel {

struct TopologySnapshot {
  uint32_t revision = 0;
  std::string hash;
  std::string layout_id;
  std::string json;
};

bool topology_build_master(const NodeConfig& config, const std::string& rig_json,
                           const std::string& layout_json,
                           const std::string& layout_id,
                           TopologySnapshot& result, std::string& error);
bool topology_validate_follower(const NodeConfig& config,
                                const std::string& topology_json,
                                const std::string& expected_hash,
                                const std::string& local_layout_json,
                                TopologySnapshot& result,
                                std::string& error);
bool topology_local_binding(const TopologySnapshot& snapshot, uint16_t node_id,
                            std::string& slot_id, uint8_t& rotation,
                            std::string& error);
bool topology_self_test(std::string& error);

}  // namespace ledpanel
