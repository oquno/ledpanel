#pragma once
#include <cstdint>
namespace ledpanel {
struct Hub75Config { int width = 64, height = 32, scan = 16; bool validated = false; };
bool hub75_init(const Hub75Config&);
bool hub75_start_diagnostic();
bool hub75_start_startup_demo();
bool hub75_stop_nightlight();
bool hub75_nightlight_active();
bool hub75_diagnostic_supported();
bool hub75_diagnostic_active();
void hub75_black();
int hub75_output_enable_level();
void hub75_present(const uint8_t*, int, int, uint8_t);
}
