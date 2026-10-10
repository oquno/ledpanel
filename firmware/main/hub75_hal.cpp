#include "hub75_hal.hpp"

#include "hub75.h"

#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/gpio_sig_map.h>

#include <atomic>
#include <memory>

namespace ledpanel {
namespace {
constexpr const char* kTag = "hub75";

// Pin map from Seengreat's HUB75 S3 board example. OE is active-low.
constexpr gpio_num_t kR1 = GPIO_NUM_5;
constexpr gpio_num_t kG1 = GPIO_NUM_4;
constexpr gpio_num_t kB1 = GPIO_NUM_6;
constexpr gpio_num_t kR2 = GPIO_NUM_15;
constexpr gpio_num_t kG2 = GPIO_NUM_7;
constexpr gpio_num_t kB2 = GPIO_NUM_17;
constexpr gpio_num_t kA = GPIO_NUM_8;
constexpr gpio_num_t kB = GPIO_NUM_18;
constexpr gpio_num_t kC = GPIO_NUM_10;
constexpr gpio_num_t kD = GPIO_NUM_9;
constexpr gpio_num_t kE = GPIO_NUM_16;
constexpr gpio_num_t kClk = GPIO_NUM_12;
constexpr gpio_num_t kLat = GPIO_NUM_11;
constexpr gpio_num_t kOe = GPIO_NUM_13;

constexpr gpio_num_t kPanelPins[] = {
    kR1, kG1, kB1, kR2, kG2, kB2, kA, kB, kC, kD, kE, kClk, kLat, kOe};

std::atomic<bool> g_diagnostic_active{false};
std::atomic<bool> g_diagnostic_cancel{false};
std::atomic<bool> g_diagnostic_persistent{false};
bool g_panel_io_ready = false;
bool g_test_profile_supported = false;
std::unique_ptr<Hub75Driver> g_driver;

bool restore_safe_gpio_levels() {
  uint64_t pin_mask = 0;
  for (gpio_num_t pin : kPanelPins) {
    if (pin != kOe) pin_mask |= 1ULL << pin;
  }

  // Preload the GPIO output latches before disconnecting LCD_CAM. This keeps
  // OE inactive as soon as the pins return to GPIO control.
  for (gpio_num_t pin : kPanelPins) gpio_set_level(pin, pin == kOe ? 1 : 0);
  esp_err_t reset_error = ESP_OK;
  for (gpio_num_t pin : kPanelPins) {
    const esp_err_t error = gpio_reset_pin(pin);
    if (error != ESP_OK && reset_error == ESP_OK) reset_error = error;
  }

  gpio_config_t oe_config = {};
  oe_config.pin_bit_mask = 1ULL << kOe;
  oe_config.mode = GPIO_MODE_INPUT_OUTPUT;
  oe_config.pull_up_en = GPIO_PULLUP_DISABLE;
  oe_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  oe_config.intr_type = GPIO_INTR_DISABLE;
  const esp_err_t oe_error = gpio_config(&oe_config);
  if (oe_error != ESP_OK) {
    ESP_LOGE(kTag, "could not configure OE GPIO13: %s",
             esp_err_to_name(oe_error));
    gpio_set_direction(kOe, GPIO_MODE_OUTPUT);
    gpio_set_level(kOe, 1);
    return false;
  }

  gpio_config_t data_config = {};
  data_config.pin_bit_mask = pin_mask;
  data_config.mode = GPIO_MODE_OUTPUT;
  data_config.pull_up_en = GPIO_PULLUP_DISABLE;
  data_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  data_config.intr_type = GPIO_INTR_DISABLE;
  const esp_err_t data_error = gpio_config(&data_config);
  if (data_error != ESP_OK) {
    ESP_LOGE(kTag, "could not configure HUB75 data GPIO levels: %s",
             esp_err_to_name(data_error));
    gpio_set_level(kOe, 1);
    return false;
  }

  if (reset_error != ESP_OK) {
    ESP_LOGE(kTag, "could not reset HUB75 pin routing: %s",
             esp_err_to_name(reset_error));
    gpio_set_level(kOe, 1);
    return false;
  }

  for (gpio_num_t pin : kPanelPins) {
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_level(pin, pin == kOe ? 1 : 0);
  }
  return true;
}

::Hub75Config make_dma_config() {
  ::Hub75Config config{};
  config.panel_width = 64;
  config.panel_height = 32;
  config.scan_wiring = Hub75ScanWiring::STANDARD_TWO_SCAN;
  // The Seengreat sample uses SHIFTREG (no special chip initialization).
  config.shift_driver = Hub75ShiftDriver::GENERIC;
  config.output_clock_speed = Hub75ClockSpeed::HZ_8M;
  // Favor camera-friendly scanning while keeping the proven 8 MHz wiring.
  // The driver compresses low BCM planes to meet this refresh target.
  config.min_refresh_rate = 240;
  config.brightness = 0;  // Start dark; the explicit test raises it after drawing.
  config.pins.r1 = kR1;
  config.pins.g1 = kG1;
  config.pins.b1 = kB1;
  config.pins.r2 = kR2;
  config.pins.g2 = kG2;
  config.pins.b2 = kB2;
  config.pins.a = kA;
  config.pins.b = kB;
  config.pins.c = kC;
  config.pins.d = kD;
  config.pins.e = kE;
  config.pins.lat = kLat;
  config.pins.oe = kOe;
  config.pins.clk = kClk;
  return config;
}

void draw_test_pattern(Hub75Driver& driver) {
  // Four broad horizontal bands exercise every RGB channel while making the
  // test visible even when viewed from across the room or through a camera.
  driver.fill(0, 0, 64, 8, 255, 0, 0);
  driver.fill(0, 8, 64, 8, 0, 255, 0);
  driver.fill(0, 16, 64, 8, 0, 0, 255);
  driver.fill(0, 24, 64, 8, 255, 255, 255);
}

void diagnostic_task(void*) {
  if (g_driver && g_driver->is_running()) {
    if (g_diagnostic_persistent.load(std::memory_order_acquire)) {
      ESP_LOGI(kTag, "dim amber nightlight started: 8/255 steady brightness");
      g_driver->fill(0, 0, 64, 32, 255, 64, 8);
      g_driver->set_brightness(8);
      while (!g_diagnostic_cancel.load(std::memory_order_acquire))
        vTaskDelay(pdMS_TO_TICKS(40));
    } else {
      ESP_LOGI(kTag,
               "low-brightness DMA RGB-bar diagnostic started: 64x32, standard 1/16 scan");
      draw_test_pattern(*g_driver);

      constexpr int64_t kTestDurationUs = 30000000;
      constexpr uint32_t kPulsePeriodMs = 1800;
      constexpr uint32_t kOnPeriodMs = 1350;
      constexpr uint8_t kTestBrightness = 16;
      const int64_t start_us = esp_timer_get_time();
      uint8_t last_brightness = 0xff;
      while (!g_diagnostic_cancel.load(std::memory_order_acquire) &&
             esp_timer_get_time() - start_us < kTestDurationUs) {
        const uint32_t elapsed_ms = static_cast<uint32_t>(
            (esp_timer_get_time() - start_us) / 1000);
        const uint32_t phase = elapsed_ms % kPulsePeriodMs;
        const uint8_t brightness = phase < kOnPeriodMs ? kTestBrightness : 0;
        if (brightness != last_brightness) {
          g_driver->set_brightness(brightness);
          last_brightness = brightness;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
      }
    }
    if (g_driver->get_brightness() != 0) g_driver->set_brightness(0);
    g_driver->clear();
  } else {
    ESP_LOGE(kTag, "DMA panel mode could not run; keeping panel black");
  }

  ESP_LOGI(kTag, "panel output mode ended; normal output is blank at zero brightness");
  g_diagnostic_persistent.store(false, std::memory_order_release);
  g_diagnostic_active.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}
}  // namespace

bool hub75_init(const Hub75Config& config) {
  if (!restore_safe_gpio_levels()) return false;
  g_test_profile_supported = config.width == 64 && config.height == 32 &&
      config.scan == 16;

  ESP_LOGI(kTag, "intentional blackout asserted: OE GPIO13 reads %d",
           hub75_output_enable_level());
  if (!g_test_profile_supported) {
    ESP_LOGW(kTag,
             "panel remains blank: this diagnostic only supports 64x32 standard 1/16 scan");
    return false;
  }

  ::Hub75Config dma_config = make_dma_config();
  g_driver.reset(new (std::nothrow) Hub75Driver(dma_config));
  if (!g_driver || !g_driver->begin()) {
    g_driver.reset();
    ESP_LOGE(kTag, "HUB75 DMA initialization failed; output remains black");
    return false;
  }
  g_driver->set_brightness(0);
  g_driver->clear();
  g_panel_io_ready = true;
  ESP_LOGI(kTag, "HUB75 DMA ready for 64x32 RGB888 frame output; initial brightness is zero");
  return true;
}

namespace {
bool start_diagnostic(bool persistent) {
  if (!g_panel_io_ready || !g_test_profile_supported) return false;
  bool expected = false;
  if (!g_diagnostic_active.compare_exchange_strong(expected, true,
          std::memory_order_acq_rel)) return false;
  g_diagnostic_persistent.store(persistent, std::memory_order_release);
  g_diagnostic_cancel.store(false, std::memory_order_release);
  if (xTaskCreatePinnedToCore(diagnostic_task, "hub75_test", 8192, nullptr,
                              3, nullptr, 1) != pdPASS) {
    g_diagnostic_active.store(false, std::memory_order_release);
    hub75_black();
    return false;
  }
  return true;
}
}  // namespace

bool hub75_start_diagnostic() { return start_diagnostic(false); }

bool hub75_start_startup_demo() { return start_diagnostic(true); }
bool hub75_stop_nightlight() {
  if (!g_diagnostic_active.load(std::memory_order_acquire) ||
      !g_diagnostic_persistent.load(std::memory_order_acquire)) return false;
  g_diagnostic_cancel.store(true, std::memory_order_release);
  return true;
}
bool hub75_nightlight_active() {
  return g_diagnostic_active.load(std::memory_order_acquire) &&
      g_diagnostic_persistent.load(std::memory_order_acquire);
}

bool hub75_diagnostic_supported() {
  return g_panel_io_ready && g_test_profile_supported && g_driver &&
         g_driver->is_running();
}

bool hub75_diagnostic_active() {
  return g_diagnostic_active.load(std::memory_order_acquire);
}

void hub75_black() {
  if (g_diagnostic_active.load(std::memory_order_acquire)) {
    if (!g_diagnostic_persistent.load(std::memory_order_acquire))
      g_diagnostic_cancel.store(true, std::memory_order_release);
    return;
  }
  if (g_driver && g_driver->is_running()) {
    if (g_driver->get_brightness() != 0) g_driver->set_brightness(0);
    g_driver->clear();
  } else {
    gpio_set_level(kOe, 1);
    for (gpio_num_t pin : kPanelPins) {
      if (pin != kOe) gpio_set_level(pin, 0);
    }
  }
}

int hub75_output_enable_level() { return gpio_get_level(kOe); }

void hub75_present(const uint8_t* rgb, int width, int height, uint8_t brightness) {
  if (!g_driver || !g_driver->is_running() || !rgb || width != 64 || height != 32 ||
      g_diagnostic_active.load(std::memory_order_acquire)) {
    hub75_black();
    return;
  }
  g_driver->draw_pixels(0, 0, 64, 32, rgb, Hub75PixelFormat::RGB888,
                        Hub75ColorOrder::RGB, false);
// OE updates rewrite live DMA buffers; only do this when brightness changes.
  if (g_driver->get_brightness() != brightness) g_driver->set_brightness(brightness);
}
}  // namespace ledpanel
