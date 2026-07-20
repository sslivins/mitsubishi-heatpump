/// @file status_led.cpp
/// @brief WS2812 onboard-LED driver for the group-coordination warning glyph.
#include "status_led.h"

#include <mutex>

#include "led_strip.h"
#include "esp_log.h"

namespace status_led {
namespace {

const char* TAG = "status_led";

// M5Stamp-S3 onboard WS2812 RGB LED. GPIO21 is dedicated to it on this SKU and
// is not used by anything else in this firmware.
constexpr int kGpioNum = 21;

led_strip_handle_t s_strip = nullptr;
std::mutex         s_mtx;
Level              s_cur   = Level::Off;
bool               s_have  = false;

// Modest brightness — the LED is a status glyph, not room lighting.
void apply(Level lvl) {
    if (!s_strip) return;
    uint8_t r = 0, g = 0, b = 0;
    switch (lvl) {
        case Level::Alert: r = 40; g = 0;  b = 0; break;  // red
        case Level::Warn:  r = 40; g = 20; b = 0; break;  // amber
        case Level::Off:
        default:           r = 0;  g = 0;  b = 0; break;
    }
    if (r == 0 && g == 0 && b == 0) {
        led_strip_clear(s_strip);
        return;
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

}  // namespace

esp_err_t init() {
    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num        = kGpioNum;
    strip_cfg.max_leds              = 1;
    strip_cfg.led_model             = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src          = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz    = 10 * 1000 * 1000;  // 10 MHz
    rmt_cfg.mem_block_symbols = 64;

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED unavailable (%s) — status glyph disabled",
                 esp_err_to_name(err));
        s_strip = nullptr;
        return err;
    }
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "status LED ready on GPIO%d", kGpioNum);
    return ESP_OK;
}

void set(Level lvl) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_have && lvl == s_cur) return;  // only touch HW on change
    s_cur  = lvl;
    s_have = true;
    apply(lvl);
}

}  // namespace status_led
