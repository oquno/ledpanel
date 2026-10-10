#pragma once
#include <cstdint>
#include <string>
namespace ledpanel {
enum class Role : uint8_t { Master, Follower };
struct NodeConfig {
  uint16_t nodeId = 0;
  uint16_t masterNodeId = 0;
  std::string hardwareId;
  std::string masterAddress;
  Role role = Role::Follower;
  bool provisioned = false;
  uint8_t brightnessCap = 64;
  uint8_t rotation = 0;
  std::string topologyHash;
  std::string groupId;
  std::string networkMode;
  std::string apPsk;
  std::string stationSsid;
  std::string stationPassword;
  std::string groupKey;
  std::string slotId;
  std::string adminSalt;
  std::string adminVerifier;
  uint8_t channel = 6;
};
struct PlaybackState {
  uint32_t revision = 0, topologyRevision = 0;
  std::string topologyHash = std::string(64, '0'), packageHash, layoutId,
              mode = "stopped", sceneId, cueId;
  bool running = false;
  uint64_t positionTicks = 0, anchorUs = 0, effectiveAtUs = 0;
  bool blackout = true;
  uint8_t userBrightness = 26;
};
bool state_can_render(const NodeConfig&, const PlaybackState&, std::string&);
}  // namespace ledpanel
