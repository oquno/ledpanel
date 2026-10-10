# Upstream component

This vendored ESP-IDF component is based on `esphome-libs/esp-hub75` at commit
`d0bb571fdae6ec6215615de4b73a6e8ce8868a85` (MIT; see `LICENSE`).

The local change in `src/core/hub75_driver.cpp` releases a partially initialized
DMA backend when `begin()` fails, so a failed diagnostic does not leave LCD_CAM
or GDMA resources allocated.
