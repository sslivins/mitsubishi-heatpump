/// @file diag.cpp
/// @brief Boot / brownout / power-sag diagnostics implementation.

#include "diag.h"

#include <mutex>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"

namespace {
const char* TAG = "diag";
constexpr const char* kNs = "diag";

std::mutex   g_mtx;
diag::Snapshot g_snap;

const char* reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

// Read a u32 from NVS, defaulting to 0 if absent.
uint32_t nvs_get_u32_default(nvs_handle_t h, const char* key) {
    uint32_t v = 0;
    nvs_get_u32(h, key, &v);
    return v;
}

}  // namespace

namespace diag {

void init() {
    esp_reset_reason_t rr = esp_reset_reason();
    const bool brownout = (rr == ESP_RST_BROWNOUT);

    uint32_t boot_count = 0, brownout_count = 0, vin_min_ever = 0;

    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) == ESP_OK) {
        boot_count     = nvs_get_u32_default(h, "boot_cnt") + 1;
        brownout_count = nvs_get_u32_default(h, "bo_cnt") + (brownout ? 1 : 0);
        vin_min_ever   = nvs_get_u32_default(h, "vin_min");

        nvs_set_u32(h, "boot_cnt", boot_count);
        if (brownout) nvs_set_u32(h, "bo_cnt", brownout_count);
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "nvs_open failed; diagnostics not persisted this boot");
    }

    std::lock_guard<std::mutex> lock(g_mtx);
    g_snap.boot_count        = boot_count;
    g_snap.brownout_count    = brownout_count;
    g_snap.vin_min_ever_mv   = static_cast<uint16_t>(vin_min_ever);
    g_snap.reset_reason      = reset_reason_str(rr);
    g_snap.last_was_brownout = brownout;
    g_snap.vin_min_mv        = 0;  // set on first note_vin
    g_snap.vin_sag_count     = 0;

    ESP_LOGI(TAG, "boot #%lu reset=%s%s vin_min_ever=%umV",
             (unsigned long)boot_count, g_snap.reset_reason,
             brownout ? " (BROWNOUT!)" : "", (unsigned)vin_min_ever);
}

void note_vin(uint16_t input_mv, uint16_t floor_mv) {
    if (input_mv == 0) return;  // ignore invalid/absent reads

    bool new_all_time_low = false;
    uint16_t all_time_low = 0;
    {
        std::lock_guard<std::mutex> lock(g_mtx);

        // Edge-triggered sag: count only on the transition into the sag region.
        static bool below = false;
        if (input_mv < floor_mv) {
            if (!below) { g_snap.vin_sag_count++; below = true; }
        } else {
            below = false;
        }

        if (g_snap.vin_min_mv == 0 || input_mv < g_snap.vin_min_mv) {
            g_snap.vin_min_mv = input_mv;
        }
        if (g_snap.vin_min_ever_mv == 0 || input_mv < g_snap.vin_min_ever_mv) {
            g_snap.vin_min_ever_mv = input_mv;
            new_all_time_low = true;
            all_time_low = input_mv;
        }
    }

    // Persist a new record low outside the lock; rare, so flash wear is bounded.
    if (new_all_time_low) {
        nvs_handle_t h;
        if (nvs_open(kNs, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u32(h, "vin_min", all_time_low);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

Snapshot get() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_snap;
}

}  // namespace diag
