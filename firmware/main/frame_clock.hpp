#pragma once
#include <cstdint>
namespace ledpanel {
// Bound future anchors to one frame so a new master clock cannot leave the
// rendering task sleeping until an obsolete timestamp from the previous boot.
constexpr uint64_t frame_wake_delay(uint64_t epoch, uint64_t now) {
  if (now < epoch) return epoch-now > 33334 ? 33334 : epoch-now;
  const uint64_t phase = ((now-epoch)%1000000)*30%1000000;
  return (1000000-phase+29)/30;
}
constexpr bool frame_clock_self_test() {
  const uint64_t clocks[]={0,1,33333,33334,1000000,600000000};
  const uint64_t anchors[]={0,1000,600000000};
  for (const auto now:clocks) for (const auto epoch:anchors) {
    const uint64_t delay=frame_wake_delay(epoch,now);
    if (!delay || delay>33334) return false;
    if (now>=epoch && ((now+delay-epoch)%1000000)*30%1000000>=30) return false;
  }
  return frame_wake_delay(600000000,1)==33334 && frame_wake_delay(0,0)==33334 &&
      frame_wake_delay(0,33334)==33333 && frame_wake_delay(1,0)==1;
}
static_assert(frame_clock_self_test(),"Clock reset must not stall rendering");
}
