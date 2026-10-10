#pragma once
#include <cstdint>
namespace ledpanel {
struct PanelKeyEvents {bool darker=false,brighter=false,toggle=false,next=false;};
struct PanelKeys {
  uint8_t raw=14,stable=14;
  uint64_t changed=0,pressed=0;
  bool center_down=false;
  PanelKeyEvents update(uint8_t value,uint64_t now) {
    PanelKeyEvents event;
    value &= 14;
    if (value!=raw) {raw=value;changed=now;}
    if (raw==stable || now<changed+50000) return event;
    const uint8_t previous=stable;stable=raw;
    event.darker=(previous&2) && !(stable&2);
    event.brighter=(previous&4) && !(stable&4);
    if ((previous&8) && !(stable&8)) {center_down=true;pressed=now;}
    if (!(previous&8) && (stable&8) && center_down) {
      center_down=false;event.next=now-pressed>=800000;event.toggle=!event.next;
    }
    return event;
  }
};
inline bool panel_keys_self_test() {
  PanelKeys keys;
  if (keys.update(12,100).darker || keys.update(14,10000).darker || keys.update(14,70000).darker) return false;
  keys.update(12,100000);if (!keys.update(12,150000).darker || keys.update(12,200000).darker) return false;
  keys.update(14,210000);keys.update(14,260000);
  keys.update(6,300000);keys.update(6,350000);
  keys.update(14,450000);auto short_press=keys.update(14,500000);
  if (!short_press.toggle || short_press.next) return false;
  keys.update(6,600000);keys.update(6,650000);
  keys.update(14,1500000);auto long_press=keys.update(14,1550000);
  return long_press.next && !long_press.toggle;
}
}
